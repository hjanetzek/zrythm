// SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#include "controllers/clip_slot_recorder.h"
#include "structure/tracks/track_processor.h"
#include "utils/logger.h"

namespace zrythm::controllers
{

namespace
{
/** Grid size in ticks of a note-value quantization option. */
units::tick_t
note_grid (structure::tracks::ClipQuantizeOption quantization)
{
  using Q = structure::tracks::ClipQuantizeOption;
  switch (quantization)
    {
    case Q::NextEighthNote:
      return units::PPQ / 2;
    case Q::NextSixteenthNote:
      return units::PPQ / 4;
    case Q::NextThirtySecondNote:
      return units::PPQ / 8;
    default:
      return units::PPQ;
    }
}
}

ClipSlotRecorder::ClipSlotRecorder (
  RecordingMaterializer                    &materializer,
  dsp::Transport                           &transport,
  const dsp::TempoMap                      &tempo_map,
  const structure::scenes::ClipLauncher    &clip_launcher,
  const structure::tracks::TrackCollection &track_collection,
  AttachClipFunc                            attach_clip,
  QObject *                                 parent)
    : QObject (parent), materializer_ (materializer), transport_ (transport),
      tempo_map_ (tempo_map), clip_launcher_ (clip_launcher),
      track_collection_ (track_collection),
      attach_clip_ (std::move (attach_clip)),
      state_timer_ (utils::make_qobject_unique<QTimer> (this))
{
  // Check about once per displayed frame
  state_timer_->setInterval (16);
  QObject::connect (
    state_timer_.get (), &QTimer::timeout, this,
    &ClipSlotRecorder::update_recording_states);
  QObject::connect (
    &materializer_, &RecordingMaterializer::clipSlotRecordingFinished, this,
    &ClipSlotRecorder::on_clip_slot_recording_finished);
}

dsp::TimelineTickI
ClipSlotRecorder::next_quantization_tick (
  const dsp::TempoMap                  &tempo_map,
  units::precise_tick_t                 tick,
  structure::tracks::ClipQuantizeOption quantization)
{
  using Q = structure::tracks::ClipQuantizeOption;

  const auto start = au::ceil_as<int64_t> (units::ticks, tick);
  if (quantization == Q::Immediate)
    return dsp::TimelineTickI{ start };

  const auto position = tempo_map.tick_to_musical_position (start);
  const auto bar_start =
    tempo_map
      .musical_position_to_tick (
        { .bar = position.bar, .beat = 1, .sixteenth = 1, .tick = 0 })
      .asQuantity ();
  const auto next_bar_start =
    tempo_map
      .musical_position_to_tick (
        { .bar = position.bar + 1, .beat = 1, .sixteenth = 1, .tick = 0 })
      .asQuantity ();

  // Bars are grid points of every option
  const auto grid = [&] () -> units::tick_t {
    if (quantization == Q::NextBar)
      return next_bar_start - bar_start;
    if (quantization == Q::NextBeat)
      return tempo_map.time_signature_at_tick (start).ticks_per_beat ();
    return note_grid (quantization);
  }();

  const auto ticks_into_bar = (start - bar_start).in (units::ticks);
  const auto grid_ticks = grid.in (units::ticks);
  const auto grid_steps = (ticks_into_bar + grid_ticks - 1) / grid_ticks;
  return dsp::TimelineTickI{
    min (bar_start + units::ticks (grid_steps * grid_ticks), next_bar_start)
  };
}

units::sample_t
ClipSlotRecorder::next_quantization_point () const
{
  const auto tick = next_quantization_tick (
    tempo_map_, transport_.playhead ()->playhead ().position_ticks (),
    clip_launcher_.launchQuantization ());
  return tempo_map_.tick_to_samples_rounded (dsp::TimelineTick{ tick });
}

bool
ClipSlotRecorder::canRecordIntoClipSlot (
  structure::tracks::Track *    track,
  structure::scenes::ClipSlot * clipSlot) const
{
  if (track == nullptr || clipSlot == nullptr)
    return false;
  const auto * processor = track->get_track_processor ();
  return processor != nullptr && processor->is_recording_armed ()
         && clipSlot->clip () == nullptr
         && !recordings_.contains (track->get_uuid ())
         && !materializer_.is_recording_into_clip_slot (track->get_uuid ());
}

void
ClipSlotRecorder::recordIntoClipSlot (
  structure::tracks::Track *    track,
  structure::scenes::ClipSlot * clipSlot)
{
  if (!canRecordIntoClipSlot (track, clipSlot))
    {
      z_warning (
        "Clip slot recording refused: the track must be armed, the slot "
        "empty, and the track not recording into another slot");
      return;
    }

  const auto start = next_quantization_point ();
  const bool started = materializer_.begin_clip_slot_recording (
    track->get_uuid (), start,
    [this, track_id = track->get_uuid (),
     slot = QPointer<structure::scenes::ClipSlot> (clipSlot)] (
      structure::arrangement::ArrangerObjectUuidReference clip_ref) {
      if (slot.isNull () || !track_collection_.contains (track_id))
        return;
      attach_clip_ (
        *track_collection_.track_ref_at_id (track_id).get (), *slot,
        std::move (clip_ref));
    });
  if (!started)
    return;

  recordings_.emplace (
    track->get_uuid (),
    SlotRecording{ .clip_slot = clipSlot, .start_position = start });
  clipSlot->setState (structure::scenes::ClipSlot::ClipState::RecordQueued);

  // The track's playing clip gives way to the recording
  if (clip_playback_service_ != nullptr)
    {
      clip_playback_service_->stopTrackClips (
        track, clip_launcher_.launchQuantization ());
    }

  if (transport_.getPlayState () != dsp::ITransport::PlayState::Rolling)
    transport_.requestRoll ();

  state_timer_->start ();
}

void
ClipSlotRecorder::stopRecording (structure::tracks::Track * track)
{
  if (track == nullptr || !recordings_.contains (track->get_uuid ()))
    return;

  materializer_.end_clip_slot_recording (
    track->get_uuid (), next_quantization_point ());
}

void
ClipSlotRecorder::update_recording_states ()
{
  const auto playhead = tempo_map_.tick_to_samples_rounded (
    dsp::TimelineTick{ transport_.playhead ()->playhead ().position_ticks () });
  const bool rolling =
    transport_.getPlayState () == dsp::ITransport::PlayState::Rolling;

  bool waiting = false;
  for (auto &recording : recordings_ | std::views::values)
    {
      if (recording.clip_slot.isNull ())
        continue;
      if (
        recording.clip_slot->state ()
        != structure::scenes::ClipSlot::ClipState::RecordQueued)
        continue;
      if (rolling && playhead >= recording.start_position)
        {
          recording.clip_slot->setState (
            structure::scenes::ClipSlot::ClipState::Recording);
        }
      else
        {
          waiting = true;
        }
    }

  if (!waiting)
    state_timer_->stop ();
}

void
ClipSlotRecorder::on_clip_slot_recording_finished (
  structure::tracks::TrackUuid   track_id,
  structure::arrangement::Clip * clip)
{
  auto node = recordings_.extract (track_id);
  if (node.empty ())
    return;

  auto * clip_slot = node.mapped ().clip_slot.data ();
  if (clip_slot == nullptr)
    return;
  clip_slot->setState (structure::scenes::ClipSlot::ClipState::Stopped);
  Q_EMIT clipSlotRecorded (clip_slot, clip);
}

} // namespace zrythm::controllers
