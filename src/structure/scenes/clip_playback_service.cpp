// SPDX-FileCopyrightText: © 2025 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#include "structure/scenes/clip_launcher.h"
#include "structure/scenes/clip_playback_service.h"
#include "structure/tracks/clip_playback_data_provider.h"
#include "structure/tracks/track_processor.h"

namespace zrythm::structure::scenes
{

ClipPlaybackService::ClipPlaybackService (
  utils::IObjectRegistry        &registry,
  const tracks::TrackCollection &track_collection,
  QObject *                      parent)
    : QObject (parent), registry_ (registry),
      track_collection_ (track_collection),
      playback_state_monitor_ (utils::make_qobject_unique<QTimer> (this))
{
  // Check about once per displayed frame
  playback_state_monitor_->setInterval (16);
  QObject::connect (
    playback_state_monitor_.get (), &QTimer::timeout, this,
    &ClipPlaybackService::checkPlaybackState);

  // Forget clips of removed tracks
  QObject::connect (
    &track_collection_, &tracks::TrackCollection::rowsRemoved, this,
    &ClipPlaybackService::checkPlaybackState);
}

tracks::Track *
ClipPlaybackService::trackIfPresent (const tracks::Track::Uuid &track_id) const
{
  if (!track_collection_.contains (track_id))
    return nullptr;
  return track_collection_.track_ref_at_id (track_id).get ();
}

void
ClipPlaybackService::watchClipSlot (ClipSlot * clip_slot)
{
  QObject::connect (
    clip_slot, &QObject::destroyed, this,
    &ClipPlaybackService::onClipSlotDestroyed, Qt::UniqueConnection);
}

void
ClipPlaybackService::onClipSlotDestroyed (QObject * object)
{
  const auto is_destroyed_slot = [object] (const ClipSlot * clip_slot) {
    return clip_slot != nullptr
           && static_cast<const QObject *> (clip_slot) == object;
  };

  std::erase_if (track_states_, [&] (auto &entry) {
    auto &[track_uuid, track_state] = entry;
    auto * track = trackIfPresent (track_uuid);
    auto * processor =
      track != nullptr ? track->get_track_processor () : nullptr;

    if (is_destroyed_slot (track_state.queued_slot))
      {
        if (processor != nullptr)
          processor->clip_playback_data_provider ().cancel_pending_launch ();
        track_state.queued_slot = nullptr;
      }
    if (is_destroyed_slot (track_state.playing_slot))
      {
        // A queued clip still takes over at its quantization point;
        // otherwise playback stops right away
        if (processor != nullptr && track_state.queued_slot == nullptr)
          processor->clip_playback_data_provider ().queue_stop_playback (
            structure::tracks::ClipQuantizeOption::Immediate);
        track_state.playing_slot = nullptr;
        track_state.stop_requested = false;
        track_state.stop_quantized = false;
      }
    return track_state.playing_slot == nullptr
           && track_state.queued_slot == nullptr;
  });
}

void
ClipPlaybackService::launchClip (
  tracks::Track *                       track,
  ClipSlot *                            clipSlot,
  structure::tracks::ClipQuantizeOption quantize)
{
  if ((track == nullptr) || (clipSlot == nullptr))
    {
      z_warning ("Invalid track or clip slot provided to launchClip");
      return;
    }

  auto * clip = clipSlot->clip ();
  if (clip == nullptr)
    {
      z_warning ("No clip in clip slot");
      return;
    }

  // Schedule the launch with quantization
  scheduleClipLaunch (track, clipSlot, quantize);
}

void
ClipPlaybackService::launch_clip_aligned (
  tracks::Track * track,
  ClipSlot *      clipSlot,
  units::sample_t anchor_position)
{
  if (track == nullptr || clipSlot == nullptr || clipSlot->clip () == nullptr)
    {
      z_warning ("Invalid track or clip slot provided to launch_clip_aligned");
      return;
    }

  scheduleClipLaunch (
    track, clipSlot, structure::tracks::ClipQuantizeOption::Immediate,
    anchor_position);
}

void
ClipPlaybackService::stopClip (
  tracks::Track *                       track,
  ClipSlot *                            clipSlot,
  structure::tracks::ClipQuantizeOption quantize)
{
  if ((track == nullptr) || (clipSlot == nullptr))
    {
      z_warning ("Invalid track or clip slot provided to stopClip");
      return;
    }

  // Schedule the stop with quantization
  scheduleClipStop (track, clipSlot, quantize);
}

void
ClipPlaybackService::launchScene (
  Scene *                               scene,
  structure::tracks::ClipQuantizeOption quantize)
{
  if (scene == nullptr)
    {
      z_warning ("Invalid scene provided to launchScene");
      return;
    }

  // Get all clip slots in the scene
  auto * clipSlotList = scene->clipSlots ();
  if (clipSlotList == nullptr)
    {
      z_warning ("Scene has no clip slot list");
      return;
    }

  // Launch each clip that has a clip
  for (const auto &track : track_collection_.tracks ())
    {
      auto * clipSlot = clipSlotList->clipSlotForTrack (track.get ());
      if ((clipSlot != nullptr) && (clipSlot->clip () != nullptr))
        {
          launchClip (track.get (), clipSlot, quantize);
        }
    }

  Q_EMIT sceneLaunched (scene);
}

void
ClipPlaybackService::stopScene (
  Scene *                               scene,
  structure::tracks::ClipQuantizeOption quantize)
{
  if (scene == nullptr)
    {
      z_warning ("Invalid scene provided to stopScene");
      return;
    }

  // Get all clip slots in the scene
  auto * clipSlotList = scene->clipSlots ();
  if (clipSlotList == nullptr)
    {
      z_warning ("Scene has no clip slot list");
      return;
    }

  // Stop each playing clip in the scene
  for (const auto &track : track_collection_.tracks ())
    {
      auto * clipSlot = clipSlotList->clipSlotForTrack (track.get ());
      if ((clipSlot != nullptr) && isClipPlaying (clipSlot))
        {
          stopClip (track.get (), clipSlot, quantize);
        }
    }

  Q_EMIT sceneStopped (scene);
}

void
ClipPlaybackService::stopAllClips (
  structure::tracks::ClipQuantizeOption quantize)
{
  // Stopping modifies the per-track states, so collect the tracks first
  const auto track_uuids =
    track_states_ | std::views::keys | std::ranges::to<std::vector> ();
  for (const auto &track_uuid : track_uuids)
    {
      auto * track = trackIfPresent (track_uuid);
      if (track != nullptr)
        {
          stopTrackClips (track, quantize);
        }
    }
}

void
ClipPlaybackService::stopTrackClips (
  tracks::Track *                       track,
  structure::tracks::ClipQuantizeOption quantize)
{
  if (track == nullptr)
    {
      z_warning ("Invalid track provided to stopTrackClips");
      return;
    }

  const auto it = track_states_.find (track->get_uuid ());
  if (it == track_states_.end ())
    return;

  // Stopping the playing clip also discards a queued one
  auto * clip_slot =
    it->second.playing_slot != nullptr
      ? it->second.playing_slot
      : it->second.queued_slot;
  if (clip_slot != nullptr)
    stopClip (track, clip_slot, quantize);
}

bool
ClipPlaybackService::isClipPlaying (ClipSlot * clipSlot) const
{
  if (clipSlot == nullptr)
    return false;

  return clipSlot->state () == ClipSlot::ClipState::Playing
         || clipSlot->state () == ClipSlot::ClipState::PlayQueued;
}

double
ClipPlaybackService::getClipPlaybackPosition (ClipSlot * clipSlot) const
{
  if (clipSlot == nullptr)
    return -1.0;

  // Find the track that plays this clip slot
  for (const auto &[trackUuid, track_state] : track_states_)
    {
      if (track_state.playing_slot == clipSlot)
        {
          // Get the track
          auto * track = trackIfPresent (trackUuid);
          if (track == nullptr)
            return -1.0;

          // Get the track processor
          auto * processor = track->get_track_processor ();
          if (processor == nullptr)
            return -1.0;

          return processor->clip_playback_data_provider ()
            .current_playback_fraction ();
        }
    }

  // If we get here, the clip is not playing
  return -1.0;
}

QList<ClipSlot *>
ClipPlaybackService::getPlayingClips () const
{
  QList<ClipSlot *> result;

  for (const auto &track_state : track_states_ | std::views::values)
    {
      for (
        auto * clip_slot : { track_state.playing_slot, track_state.queued_slot })
        {
          if (isClipPlaying (clip_slot) && !result.contains (clip_slot))
            result.append (clip_slot);
        }
    }

  return result;
}

void
ClipPlaybackService::setTrackToClipLauncherMode (tracks::Track * track)
{
  if (track == nullptr)
    return;

  auto * processor = track->get_track_processor ();
  if (processor == nullptr)
    return;

  // Set the track to clip launcher mode
  track->setClipLauncherMode (true);
}

void
ClipPlaybackService::setTrackToTimelineMode (tracks::Track * track)
{
  if (track == nullptr)
    return;

  auto * processor = track->get_track_processor ();
  if (processor == nullptr)
    return;

  // Cancel any pending clip operations for this track
  cancelPendingClipOperations (track);

  // Stop a clip that is already playing
  const auto it = track_states_.find (track->get_uuid ());
  if (it != track_states_.end ())
    {
      auto * clip_slot = it->second.playing_slot;
      track_states_.erase (it);
      if (clip_slot != nullptr)
        {
          processor->clip_playback_data_provider ().queue_stop_playback (
            structure::tracks::ClipQuantizeOption::Immediate);
          updateClipSlotState (clip_slot, ClipSlot::ClipState::Stopped);
          Q_EMIT clipStopped (track, clip_slot);
        }
    }

  // Set the track to timeline mode
  track->setClipLauncherMode (false);
}

void
ClipPlaybackService::setAllTracksToTimelineMode ()
{
  for (const auto &track : track_collection_.tracks ())
    {
      setTrackToTimelineMode (track.get ());
    }
}

void
ClipPlaybackService::scheduleClipLaunch (
  tracks::Track *                       track,
  ClipSlot *                            clipSlot,
  structure::tracks::ClipQuantizeOption quantize,
  std::optional<units::sample_t>        anchor_position)
{
  if ((track == nullptr) || (clipSlot == nullptr))
    return;

  auto * clip = clipSlot->clip ();
  if (clip == nullptr)
    return;

  auto &track_state = track_states_[track->get_uuid ()];

  // A clip queued earlier on this track will not start
  if (
    track_state.queued_slot != nullptr && track_state.queued_slot != clipSlot
    && track_state.queued_slot != track_state.playing_slot)
    {
      updateClipSlotState (
        track_state.queued_slot, ClipSlot::ClipState::Stopped);
    }
  // The launch replaces a quantized stop that has not happened yet, so the
  // playing clip continues until the launched one takes over
  if (
    track_state.stop_requested && track_state.stop_quantized
    && track_state.playing_slot != nullptr
    && track_state.playing_slot != clipSlot)
    {
      updateClipSlotState (
        track_state.playing_slot, ClipSlot::ClipState::Playing);
    }
  track_state.queued_slot = clipSlot;
  track_state.stop_requested = false;
  track_state.stop_quantized = false;
  watchClipSlot (clipSlot);

  // Update clip slot state to queued
  updateClipSlotState (clipSlot, ClipSlot::ClipState::PlayQueued);

  // Ensure track is in clip launcher mode
  setTrackToClipLauncherMode (track);

  // Generate events using the clip launcher event provider
  // The provider handles quantization internally
  if (auto * midi_clip = dynamic_cast<arrangement::MidiClip *> (clip))
    {
      generateClipEvents (track, *midi_clip, quantize, anchor_position);
    }
  else if (auto * audio_clip = dynamic_cast<arrangement::AudioClip *> (clip))
    {
      generateClipEvents (track, *audio_clip, quantize, anchor_position);
    }

  // Start monitoring for the actual playback state change
  startPlaybackStateMonitor ();

  // Emit signal
  Q_EMIT clipLaunched (track, clipSlot);
}

void
ClipPlaybackService::scheduleClipStop (
  tracks::Track *                       track,
  ClipSlot *                            clipSlot,
  structure::tracks::ClipQuantizeOption quantize)
{
  if ((track == nullptr) || (clipSlot == nullptr))
    return;

  auto * processor = track->get_track_processor ();
  if (processor == nullptr)
    return;
  auto &provider = processor->clip_playback_data_provider ();

  auto &track_state = track_states_[track->get_uuid ()];

  // A queued clip that has not started yet is discarded right away, and
  // the playing clip continues
  if (
    clipSlot == track_state.queued_slot && clipSlot != track_state.playing_slot)
    {
      provider.cancel_pending_launch ();
      track_state.queued_slot = nullptr;
      updateClipSlotState (clipSlot, ClipSlot::ClipState::Stopped);
      Q_EMIT clipStopped (track, clipSlot);
      return;
    }

  // Stopping the playing clip also discards a queued one
  if (
    track_state.queued_slot != nullptr
    && track_state.queued_slot != track_state.playing_slot)
    {
      updateClipSlotState (
        track_state.queued_slot, ClipSlot::ClipState::Stopped);
    }
  track_state.queued_slot = nullptr;
  if (track_state.playing_slot == nullptr)
    {
      track_state.playing_slot = clipSlot;
      watchClipSlot (clipSlot);
    }
  track_state.stop_requested = true;
  track_state.stop_quantized =
    quantize != structure::tracks::ClipQuantizeOption::Immediate;
  auto * stopping_slot = track_state.playing_slot;

  // Update clip slot state to queued
  updateClipSlotState (stopping_slot, ClipSlot::ClipState::StopQueued);

  // Queue stop playback with quantization in the clip launcher event
  // provider
  provider.queue_stop_playback (quantize);

  // Start monitoring for the actual playback state change
  startPlaybackStateMonitor ();

  // Emit signal
  Q_EMIT clipStopped (track, stopping_slot);
}

void
ClipPlaybackService::updateClipSlotState (
  ClipSlot *          clipSlot,
  ClipSlot::ClipState newState)
{
  if (clipSlot == nullptr)
    return;

  clipSlot->setState (newState);
}

void
ClipPlaybackService::startPlaybackStateMonitor ()
{
  if (!playback_state_monitor_->isActive ())
    playback_state_monitor_->start ();
}

void
ClipPlaybackService::checkPlaybackState ()
{
  // Slot state changes are applied after the scan, since their signal
  // handlers may call back into this service
  std::vector<std::pair<ClipSlot *, ClipSlot::ClipState>> state_changes;
  const auto                                              record_state_change =
    [&state_changes] (ClipSlot * clip_slot, ClipSlot::ClipState new_state) {
      state_changes.emplace_back (clip_slot, new_state);
    };

  std::erase_if (track_states_, [&] (auto &entry) {
    auto &[track_uuid, track_state] = entry;
    auto * track = trackIfPresent (track_uuid);
    auto * processor =
      track != nullptr ? track->get_track_processor () : nullptr;
    if (processor == nullptr)
      {
        // The track was removed: its clips no longer play
        for (
          auto * clip_slot :
          { track_state.playing_slot, track_state.queued_slot })
          {
            if (clip_slot != nullptr)
              record_state_change (clip_slot, ClipSlot::ClipState::Stopped);
          }
        return true;
      }

    const auto &provider = processor->clip_playback_data_provider ();
    const bool  playing = provider.playing ();

    // The queued clip took over: the previously playing clip has stopped
    if (
      track_state.queued_slot != nullptr && !provider.launch_pending ()
      && playing)
      {
        if (
          track_state.playing_slot != nullptr
          && track_state.playing_slot != track_state.queued_slot)
          {
            record_state_change (
              track_state.playing_slot, ClipSlot::ClipState::Stopped);
          }
        track_state.playing_slot = track_state.queued_slot;
        track_state.queued_slot = nullptr;
        record_state_change (
          track_state.playing_slot, ClipSlot::ClipState::Playing);
      }

    // The playing clip ended (stopped or cleared)
    if (
      track_state.playing_slot != nullptr && !playing
      && (track_state.stop_requested || track_state.queued_slot == nullptr))
      {
        record_state_change (
          track_state.playing_slot, ClipSlot::ClipState::Stopped);
        track_state.playing_slot = nullptr;
        track_state.stop_requested = false;
        track_state.stop_quantized = false;
      }

    // Keep tracks that still play or wait for a change
    return track_state.queued_slot == nullptr
           && (track_state.playing_slot == nullptr || !playing);
  });

  if (
    std::ranges::none_of (track_states_ | std::views::values, [] (const auto &s) {
      return s.queued_slot != nullptr || s.stop_requested;
    }))
    {
      playback_state_monitor_->stop ();
    }

  for (const auto &[clip_slot, new_state] : state_changes)
    {
      updateClipSlotState (clip_slot, new_state);
    }
}

void
ClipPlaybackService::cancelPendingClipOperations (tracks::Track * track)
{
  if (track == nullptr)
    return;

  const auto it = track_states_.find (track->get_uuid ());
  if (it == track_states_.end ())
    return;
  auto &track_state = it->second;

  // Discard a queued clip that has not started
  if (track_state.queued_slot != nullptr)
    {
      if (auto * processor = track->get_track_processor ())
        processor->clip_playback_data_provider ().cancel_pending_launch ();
      if (track_state.queued_slot != track_state.playing_slot)
        updateClipSlotState (
          track_state.queued_slot, ClipSlot::ClipState::Stopped);
      else
        updateClipSlotState (
          track_state.queued_slot, ClipSlot::ClipState::Playing);
      track_state.queued_slot = nullptr;
    }

  if (track_state.stop_requested)
    {
      if (track_state.stop_quantized)
        {
          // Discard the queued stop: the clip keeps playing
          if (auto * processor = track->get_track_processor ())
            processor->clip_playback_data_provider ().cancel_pending_launch ();
          updateClipSlotState (
            track_state.playing_slot, ClipSlot::ClipState::Playing);
        }
      else
        {
          // An immediate stop is already in effect
          updateClipSlotState (
            track_state.playing_slot, ClipSlot::ClipState::Stopped);
          track_state.playing_slot = nullptr;
        }
      track_state.stop_requested = false;
      track_state.stop_quantized = false;
    }

  if (track_state.playing_slot == nullptr)
    track_states_.erase (it);
}
}
