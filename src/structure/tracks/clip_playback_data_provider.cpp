// SPDX-FileCopyrightText: © 2025 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#include "structure/arrangement/clip_renderer.h"
#include "structure/tracks/clip_playback_data_provider.h"
#include "utils/float_ranges.h"
#include "utils/midi.h"

namespace zrythm::structure::tracks
{

// Define to enable debug logging for clip launcher event provider
static constexpr bool CLIP_LAUNCHER_EVENT_PROVIDER_DEBUG = false;

namespace
{
/**
 * @brief Grid size in ticks of a note-value quantization option.
 */
constexpr int64_t
note_grid_ticks (structure::tracks::ClipQuantizeOption quantize_opt)
{
  const auto quarter = units::PPQ.in (units::ticks);
  switch (quantize_opt)
    {
    case structure::tracks::ClipQuantizeOption::NextEighthNote:
      return quarter / 2;
    case structure::tracks::ClipQuantizeOption::NextSixteenthNote:
      return quarter / 4;
    case structure::tracks::ClipQuantizeOption::NextThirtySecondNote:
      return quarter / 8;
    default:
      return quarter;
    }
}
}

ClipPlaybackDataProvider::ClipPlaybackDataProvider (
  const dsp::TempoMap &tempo_map)
    : tempo_map_ (tempo_map), last_seen_timeline_position_ (units::samples (0)),
      clip_launch_timeline_position_ (units::samples (0)),
      clip_launch_timeline_tick_ (units::ticks (0.0)),
      midi_playback_tick_ (units::ticks (0.0))
{
  internal_clip_buffer_position_.store (units::samples (0));
}

ClipPlaybackDataProvider::SlotState
ClipPlaybackDataProvider::retract_pending_launch ()
{
  auto state = slot_state_.load (std::memory_order_acquire);
  while (pending_slot (state) != kNoSlot)
    {
      // Fails if the audio thread switched to the queued slot meanwhile
      if (
        slot_state_.compare_exchange_weak (
          state,
          make_slot_state (current_slot (state), kNoSlot, generation (state)),
          std::memory_order_acq_rel, std::memory_order_acquire))
        {
          return make_slot_state (
            current_slot (state), kNoSlot, generation (state));
        }
    }
  return state;
}

void
ClipPlaybackDataProvider::queue_cache (PlaybackCache &&cache)
{
  // With nothing queued, only this thread changes the slot state
  const auto state = retract_pending_launch ();
  const auto target_slot = 1U - current_slot (state);
  {
    CacheSlot::ScopedAccess<farbot::ThreadType::nonRealtime> slot{
      slots_.at (target_slot)
    };
    *slot = std::move (cache);
  }
  slot_state_.store (
    make_slot_state (current_slot (state), target_slot, generation (state) + 1),
    std::memory_order_release);
}

void
ClipPlaybackDataProvider::cancel_pending_launch ()
{
  retract_pending_launch ();
}

void
ClipPlaybackDataProvider::generate_midi_events (
  const arrangement::MidiClip          &midi_clip,
  structure::tracks::ClipQuantizeOption quantize_option,
  std::optional<units::sample_t>        anchor_position)
{
  // The intro runs from the clip start to the first loop end; one full loop
  // follows it, also when the clip start lies after the loop start
  const auto clip_start = midi_clip.clipStartPosition ()->asTick ();
  const auto loop_start = midi_clip.loopStartPosition ()->asTick ();
  const auto loop_end = midi_clip.loopEndPosition ()->asTick ();
  const auto intro_length = (loop_end - clip_start).asQuantity ();
  const auto loop_length = (loop_end - loop_start).asQuantity ();
  assert (intro_length > units::ticks (0.0) && loop_length > units::ticks (0.0));

  // Serialize clip (timings in ticks from the clip start)
  juce::MidiMessageSequence clip_seq;
  arrangement::ClipRenderer::serialize_to_sequence (
    midi_clip, clip_seq, std::nullopt,
    dsp::ContentTick{ intro_length + loop_length });
  clip_seq.sort ();

  MidiContent content{
    .events_ = {},
    .intro_length_ = intro_length,
    .loop_length_ = loop_length,
    .clip_start_ = clip_start.asQuantity (),
    .loop_start_ = loop_start.asQuantity (),
    .clip_length_ = units::ticks (midi_clip.length ()->ticks ()),
  };
  content.events_.reserve (static_cast<size_t> (clip_seq.getNumEvents ()));
  for (const auto &event : clip_seq)
    {
      const auto * raw = event->message.getRawData ();
      const auto   raw_size =
        static_cast<size_t> (event->message.getRawDataSize ());
      content.events_.push_back (
        dsp::midi_event::make_raw (
          std::span<const midi_byte_t>{ raw, raw_size },
          units::ticks (event->message.getTimeStamp ())));
    }

  queue_cache (
    PlaybackCache{
      .content_ = std::move (content),
      .quantize_opt_ = quantize_option,
      .end_position_ = units::ticks (midi_clip.length ()->ticks ()),
      .anchor_position_ = anchor_position });
}

void
ClipPlaybackDataProvider::generate_audio_events (
  const arrangement::AudioClip         &audio_clip,
  structure::tracks::ClipQuantizeOption quantize_option,
  std::optional<units::sample_t>        anchor_position)
{
  // Serialize clip
  juce::AudioSampleBuffer audio_buffer;
  arrangement::ClipRenderer::serialize_to_buffer (audio_clip, audio_buffer);

  queue_cache (
    PlaybackCache{
      .content_ = std::move (audio_buffer),
      .quantize_opt_ = quantize_option,
      .end_position_ = units::ticks (audio_clip.length ()->ticks ()),
      .anchor_position_ = anchor_position });
}

void
ClipPlaybackDataProvider::queue_stop_playback (
  structure::tracks::ClipQuantizeOption quantize_option)
{
  // A quantized stop is a queued switch to silence
  if (quantize_option != structure::tracks::ClipQuantizeOption::Immediate)
    {
      queue_cache (
        PlaybackCache{
          .content_ = std::monostate{},
          .quantize_opt_ = quantize_option,
          .end_position_ = units::ticks (0) });
      return;
    }

  // With nothing queued, the played slot cannot change while it is cleared
  const auto state = retract_pending_launch ();
  CacheSlot::ScopedAccess<farbot::ThreadType::nonRealtime> slot{
    slots_.at (current_slot (state))
  };
  *slot = std::nullopt;
}

template <typename ProcessFunc>
void
ClipPlaybackDataProvider::process_chunks_with_looping (
  units::sample_t internal_buffer_start_offset,
  units::sample_t samples_to_process,
  units::sample_t clip_loop_end_samples,
  units::sample_t output_buffer_timestamp_offset,
  ProcessFunc     process_chunk)
{
  // Process events with looping
  auto current_internal_buffer_offset = internal_buffer_start_offset;
  auto remaining_samples_to_process = samples_to_process;

  // Track the total samples processed for output buffer timestamp calculation
  auto total_samples_processed = units::samples (0);

  while (remaining_samples_to_process > units::samples (0))
    {
      // Calculate how much we can process before hitting the loop point or end
      // of buffer
      auto samples_until_loop =
        clip_loop_end_samples - current_internal_buffer_offset;
      auto samples_to_process_in_chunk = remaining_samples_to_process;

      if (
        samples_until_loop > units::samples (0)
        && samples_until_loop < remaining_samples_to_process)
        {
          samples_to_process_in_chunk = samples_until_loop;
        }

      // Process the chunk using the provided function
      process_chunk (
        current_internal_buffer_offset, samples_to_process_in_chunk,
        total_samples_processed, output_buffer_timestamp_offset);

      // Update position and remaining samples
      current_internal_buffer_offset += samples_to_process_in_chunk;
      total_samples_processed += samples_to_process_in_chunk;
      remaining_samples_to_process -= samples_to_process_in_chunk;

      // Check if we need to loop
      if (remaining_samples_to_process > units::samples (0))
        {
          // Reset to beginning of loop
          current_internal_buffer_offset = units::samples (0);
          if constexpr (CLIP_LAUNCHER_EVENT_PROVIDER_DEBUG)
            z_debug ("Looping: resetting current_internal_buffer_offset to 0");
        }
    }

  internal_clip_buffer_position_ = current_internal_buffer_offset;
}

void
ClipPlaybackDataProvider::update_playback_position (
  const dsp::graph::ProcessBlockInfo &time_nfo,
  units::sample_t                     block_offset,
  units::sample_t                     clip_loop_end_samples)
{
  // A clip that was already playing at the block start follows the
  // transport position
  const bool transport_moved =
    block_offset == units::samples (0)
    && time_nfo.transport_position_ != last_seen_timeline_position_;
  if (!transport_moved && !resync_position_)
    return;
  resync_position_ = false;

  const auto current_timeline_position =
    time_nfo.transport_position_ + block_offset;

  // Position within the clip, wrapped into [0, loop end) also when the
  // transport moved before the launch position
  const auto time_since_clip_launch =
    current_timeline_position - clip_launch_timeline_position_;
  auto new_internal_buffer_position =
    time_since_clip_launch % clip_loop_end_samples;
  if (new_internal_buffer_position < units::samples (0))
    new_internal_buffer_position += clip_loop_end_samples;

  if constexpr (CLIP_LAUNCHER_EVENT_PROVIDER_DEBUG)
    z_debug (
      "Timeline position changed to {}: new internal buffer position: {}",
      current_timeline_position.in (units::samples),
      new_internal_buffer_position.in (units::samples));

  internal_clip_buffer_position_.store (new_internal_buffer_position);
}

std::optional<units::sample_t>
ClipPlaybackDataProvider::quantization_offset_in_block (
  const dsp::graph::ProcessBlockInfo   &time_nfo,
  structure::tracks::ClipQuantizeOption quantize_opt) const
{
  if (quantize_opt == structure::tracks::ClipQuantizeOption::Immediate)
    return units::samples (0);

  const auto current_timeline_position = time_nfo.transport_position_;
  const auto start_tick = au::round_as<int64_t> (
    units::ticks,
    tempo_map_.samples_to_tick (current_timeline_position).asQuantity ());
  const auto musical_pos_at_start =
    tempo_map_.tick_to_musical_position (start_tick);
  const auto musical_pos_at_end = tempo_map_.tick_to_musical_position (
    au::round_as<int64_t> (
      units::ticks,
      tempo_map_.samples_to_tick (time_nfo.end_position ()).asQuantity ()));

  const auto at_beat_start =
    musical_pos_at_start.sixteenth == 1 && musical_pos_at_start.tick == 0;

  std::optional<dsp::TempoMap::MusicalPosition> musical_pos_to_start_at;
  std::optional<dsp::TimelineTickI>             tick_to_start_at;
  switch (quantize_opt)
    {
    case structure::tracks::ClipQuantizeOption::NextBar:
      if (musical_pos_at_end.bar > musical_pos_at_start.bar)
        {
          // The block crosses a bar boundary
          musical_pos_to_start_at = dsp::TempoMap::MusicalPosition{
            .bar = musical_pos_at_end.bar, .beat = 1, .sixteenth = 1, .tick = 0
          };
        }
      else if (musical_pos_at_start.beat == 1 && at_beat_start)
        {
          // The block starts exactly on a bar
          musical_pos_to_start_at = musical_pos_at_start;
        }
      break;

    case structure::tracks::ClipQuantizeOption::NextBeat:
      if (
        musical_pos_at_end.bar > musical_pos_at_start.bar
        || musical_pos_at_end.beat > musical_pos_at_start.beat)
        {
          // The block crosses a beat boundary
          musical_pos_to_start_at = dsp::TempoMap::MusicalPosition{
            .bar = musical_pos_at_end.bar,
            .beat = musical_pos_at_end.beat,
            .sixteenth = 1,
            .tick = 0
          };
        }
      else if (at_beat_start)
        {
          // The block starts exactly on a beat
          musical_pos_to_start_at = musical_pos_at_start;
        }
      break;

    case structure::tracks::ClipQuantizeOption::NextQuarterNote:
    case structure::tracks::ClipQuantizeOption::NextEighthNote:
    case structure::tracks::ClipQuantizeOption::NextSixteenthNote:
    case structure::tracks::ClipQuantizeOption::NextThirtySecondNote:
      {
        // Next note-value grid point counted from the start of the bar, at
        // the latest the next bar start (for meters that do not divide
        // evenly)
        const auto grid = units::ticks (note_grid_ticks (quantize_opt));
        const auto bar_start = tempo_map_.musical_position_to_tick (
          { .bar = musical_pos_at_start.bar,
            .beat = 1,
            .sixteenth = 1,
            .tick = 0 });
        const auto next_bar_start = tempo_map_.musical_position_to_tick (
          { .bar = musical_pos_at_start.bar + 1,
            .beat = 1,
            .sixteenth = 1,
            .tick = 0 });
        const auto ticks_into_bar = start_tick - bar_start.asQuantity ();
        const auto grid_steps =
          (ticks_into_bar.in (units::ticks) + grid.in (units::ticks) - 1)
          / grid.in (units::ticks);
        tick_to_start_at = std::min (
          dsp::TimelineTickI{ bar_start.asQuantity () + grid * grid_steps },
          next_bar_start);
      }
      break;

    case structure::tracks::ClipQuantizeOption::Immediate:
      return units::samples (0);
    }

  if (musical_pos_to_start_at.has_value ())
    tick_to_start_at =
      tempo_map_.musical_position_to_tick (*musical_pos_to_start_at);
  if (!tick_to_start_at.has_value ())
    return std::nullopt;

  // The quantization point can round to slightly before the block start when
  // the block starts exactly on it
  const auto quantized_timeline_position = max (
    tempo_map_.tick_to_samples_rounded (dsp::TimelineTick{ *tick_to_start_at }),
    current_timeline_position);
  const auto offset = quantized_timeline_position - current_timeline_position;

  // A point at the block end belongs to the next block
  if (offset >= time_nfo.nframes_)
    return std::nullopt;

  return offset;
}

template <typename ContentT, typename RenderSegmentFunc, typename EndFunc>
void
ClipPlaybackDataProvider::render_slot (
  uint32_t                            slot,
  const dsp::graph::ProcessBlockInfo &time_nfo,
  units::sample_t                     block_offset,
  units::sample_t                     length,
  RenderSegmentFunc                  &render_segment,
  EndFunc                            &on_playback_end) noexcept
{
  if (length == units::samples (0))
    return;

  CacheSlot::ScopedAccess<farbot::ThreadType::realtime> cache{
    slots_.at (slot)
  };
  const auto * content =
    cache->has_value () ? std::get_if<ContentT> (&(*cache)->content_) : nullptr;
  if (content == nullptr)
    {
      if (playing_.load ())
        {
          playing_.store (false);
          on_playback_end (time_nfo.buffer_offset_ + block_offset);
        }
      return;
    }

  if (!playing_.load ())
    return;

  render_segment (*content, **cache, block_offset, length);
}

template <typename ContentT, typename RenderSegmentFunc, typename EndFunc>
void
ClipPlaybackDataProvider::process_block (
  const dsp::graph::ProcessBlockInfo &time_nfo,
  RenderSegmentFunc                   render_segment,
  EndFunc                             on_playback_end) noexcept
{
  const auto state = slot_state_.load (std::memory_order_acquire);
  const auto pending = pending_slot (state);

  // Whether the queued slot holds silence (a queued stop)
  bool pending_is_stop = false;

  // Position the queued clip's start is aligned to, if any
  std::optional<units::sample_t> pending_anchor;

  // Where within the block the queued clip or stop takes over, if it does
  const auto switch_offset = [&] () -> std::optional<units::sample_t> {
    if (pending == kNoSlot)
      return std::nullopt;
    CacheSlot::ScopedAccess<farbot::ThreadType::realtime> pending_cache{
      slots_.at (pending)
    };
    if (!pending_cache->has_value ())
      return std::nullopt;
    const auto &content = (*pending_cache)->content_;
    pending_is_stop = std::holds_alternative<std::monostate> (content);
    pending_anchor = (*pending_cache)->anchor_position_;
    if (!pending_is_stop && !std::holds_alternative<ContentT> (content))
      return std::nullopt;
    return quantization_offset_in_block (
      time_nfo, (*pending_cache)->quantize_opt_);
  }();

  const auto block_length = time_nfo.nframes_;
  const auto played_length = switch_offset.value_or (block_length);
  render_slot<ContentT> (
    current_slot (state), time_nfo, units::samples (0), played_length,
    render_segment, on_playback_end);

  if (switch_offset.has_value ())
    {
      auto expected_state = state;
      if (
        slot_state_.compare_exchange_strong (
          expected_state, make_slot_state (pending, kNoSlot, generation (state)),
          std::memory_order_acq_rel, std::memory_order_acquire))
        {
          if (playing_.load ())
            on_playback_end (time_nfo.buffer_offset_ + *switch_offset);

          // A queued stop leaves the rest of the block silent
          playing_.store (!pending_is_stop);
          if (!pending_is_stop)
            {
              clip_launch_timeline_position_ = pending_anchor.value_or (
                time_nfo.transport_position_ + *switch_offset);
              clip_launch_timeline_tick_ =
                tempo_map_.samples_to_tick (clip_launch_timeline_position_)
                  .asQuantity ();
              internal_clip_buffer_position_.store (units::samples (0));
              midi_playback_tick_ = units::ticks (0.0);
              playback_fraction_.store (0.0);
              resync_position_ = pending_anchor.has_value ();

              render_slot<ContentT> (
                pending, time_nfo, *switch_offset,
                block_length - *switch_offset, render_segment, on_playback_end);
            }
        }
      else
        {
          // The UI thread took the queued clip back: keep playing what is
          // played now
          render_slot<ContentT> (
            current_slot (expected_state), time_nfo, *switch_offset,
            block_length - *switch_offset, render_segment, on_playback_end);
        }
    }

  last_seen_timeline_position_ = time_nfo.transport_position_;
}

void
ClipPlaybackDataProvider::render_midi_segment (
  const MidiContent                  &content,
  const dsp::graph::ProcessBlockInfo &time_nfo,
  units::sample_t                     block_offset,
  units::sample_t                     length,
  dsp::MidiEventBuffer               &output_buffer) noexcept
{
  const auto intro = content.intro_length_;
  const auto loop = content.loop_length_;
  const auto content_end = intro + loop;

  const auto segment_start = time_nfo.transport_position_ + block_offset;
  const auto segment_start_tick =
    tempo_map_.samples_to_tick (segment_start).asQuantity ();
  const auto segment_ticks =
    tempo_map_.samples_to_tick (segment_start + length).asQuantity ()
    - segment_start_tick;

  // A clip that was already playing at the block start follows the
  // transport position; an aligned launch starts at its distance from the
  // anchor
  if (
    (block_offset == units::samples (0)
     && time_nfo.transport_position_ != last_seen_timeline_position_)
    || resync_position_)
    {
      midi_playback_tick_ = segment_start_tick - clip_launch_timeline_tick_;
      resync_position_ = false;
    }

  // Position in the contents for a playback position: the intro and the
  // first loop play once, then the loop repeats
  const auto content_position =
    [&] (units::precise_tick_t playback_tick) -> units::precise_tick_t {
    if (playback_tick >= units::ticks (0.0) && playback_tick < content_end)
      return playback_tick;
    auto into_loop = std::fmod (
      (playback_tick - intro).in (units::ticks), loop.in (units::ticks));
    if (into_loop < 0.0)
      into_loop += loop.in (units::ticks);
    return intro + units::ticks (into_loop);
  };

  const auto playback_start = midi_playback_tick_;
  const auto playback_end = playback_start + segment_ticks;
  auto       playback_tick = playback_start;
  auto       position = content_position (playback_tick);
  while (playback_tick < playback_end)
    {
      // Contents play contiguously up to their end, then wrap to the loop
      const auto run_end =
        min (playback_end, playback_tick + (content_end - position));
      const auto run_reaches_content_end =
        run_end == playback_tick + (content_end - position);
      const auto run_content_end = position + (run_end - playback_tick);

      // In repeated loop passes the contents restart at the loop start, where
      // note-offs belong to the end of the intro, not to the loop
      const auto skip_intro_note_offs =
        playback_tick >= content_end && position == intro;

      for (
        auto it = std::ranges::lower_bound (
          content.events_, position, {}, &dsp::TickBasedMidiEvent::time_);
        it != content.events_.end ()
        && (it->time_ < run_content_end
            // Events at the content end (e.g. note-offs) play before the wrap
            || (run_reaches_content_end && it->time_ <= run_content_end));
        ++it)
        {
          if (
            skip_intro_note_offs && it->time_ == intro
            && utils::midi::midi_is_note_off (it->data ()))
            continue;

          const auto event_playback_tick =
            playback_tick + (it->time_ - position);
          const auto event_sample =
            tempo_map_.tick_to_samples_rounded (
              dsp::TimelineTick{
                segment_start_tick + (event_playback_tick - playback_start) })
            - segment_start;
          const auto clamped_sample = clamp (
            event_sample, units::samples (0), length - units::samples (1));
          output_buffer.push_back (
            au::floor_as<uint32_t> (
              units::samples,
              clamped_sample + time_nfo.buffer_offset_ + block_offset),
            it->data ());
        }

      playback_tick = run_end;
      position = run_reaches_content_end ? intro : run_content_end;
    }

  midi_playback_tick_ = playback_end;
  // Report the position as the clip shows it: the intro maps to the clip
  // start onwards, the loop to the loop start onwards
  const auto end_position = content_position (playback_end);
  const auto clip_tick =
    end_position < intro
      ? content.clip_start_ + end_position
      : content.loop_start_ + (end_position - intro);
  playback_fraction_.store (
    clip_tick.in (units::ticks) / content.clip_length_.in (units::ticks));
}

void
ClipPlaybackDataProvider::process_midi_events (
  const dsp::graph::ProcessBlockInfo &time_nfo,
  dsp::MidiEventBuffer               &output_buffer) noexcept
{
  process_block<MidiContent> (
    time_nfo,
    [&] (
      const MidiContent &content, const PlaybackCache &,
      units::sample_t block_offset, units::sample_t length) {
      render_midi_segment (
        content, time_nfo, block_offset, length, output_buffer);
    },
    [&] (units::sample_t output_timestamp) {
      // Silence notes of the clip that stops or is replaced
      for (const auto channel : std::views::iota (0, 16))
        {
          const auto ev = dsp::midi_event::make_all_notes_off (
            static_cast<midi_byte_t> (channel), output_timestamp);
          output_buffer.push_back (ev.time_, ev.data ());
        }
    });
}

void
ClipPlaybackDataProvider::process_audio_events (
  const dsp::graph::ProcessBlockInfo &time_nfo,
  std::span<float>                    left_buffer,
  std::span<float>                    right_buffer) noexcept
{
  const auto render_chunk =
    [&] (
      const AudioContent &audio_buffer,
      units::sample_t     current_internal_buffer_offset,
      units::sample_t     samples_to_process_in_chunk,
      units::sample_t     total_samples_processed,
      units::sample_t     output_buffer_timestamp_offset) {
      // Calculate the number of samples to copy
      const auto samples_to_copy = std::min (
        samples_to_process_in_chunk.as<int64_t> (units::samples),
        units::samples (audio_buffer.getNumSamples ())
          - current_internal_buffer_offset.as<int64_t> (units::samples));

      if (samples_to_copy <= units::samples (0))
        return;

      const auto output_start_idx_samples =
        (total_samples_processed + output_buffer_timestamp_offset)
          .in<size_t> (units::samples);
      if (output_start_idx_samples >= left_buffer.size ())
        return;

      const auto actual_samples_to_copy = std::min (
        samples_to_copy,
        static_cast<units::sample_t> (
          units::samples (left_buffer.size () - output_start_idx_samples)));

      // Copy left channel
      utils::float_ranges::add2 (
        left_buffer.subspan (
          output_start_idx_samples, actual_samples_to_copy.in (units::samples)),
        { audio_buffer.getReadPointer (
            0, current_internal_buffer_offset.in<int> (units::samples)),
          actual_samples_to_copy.in<size_t> (units::samples) });

      // Copy right channel
      if (
        audio_buffer.getNumChannels () > 1
        && right_buffer.size () > output_start_idx_samples)
        {
          utils::float_ranges::add2 (
            right_buffer.subspan (
              output_start_idx_samples,
              actual_samples_to_copy.in (units::samples)),
            { audio_buffer.getReadPointer (
                1, current_internal_buffer_offset.in<int> (units::samples)),
              actual_samples_to_copy.in<size_t> (units::samples) });
        }
    };

  process_block<AudioContent> (
    time_nfo,
    [&] (
      const AudioContent &audio_buffer, const PlaybackCache &cache,
      units::sample_t block_offset, units::sample_t length) {
      const auto clip_loop_end_samples = tempo_map_.tick_to_samples_rounded (
        dsp::TimelineTick{ cache.end_position_ });
      if (clip_loop_end_samples <= units::samples (0))
        return;

      update_playback_position (time_nfo, block_offset, clip_loop_end_samples);

      process_chunks_with_looping (
        internal_clip_buffer_position_.load (), length, clip_loop_end_samples,
        time_nfo.buffer_offset_ + block_offset,
        [&] (
          units::sample_t current_internal_buffer_offset,
          units::sample_t samples_to_process_in_chunk,
          units::sample_t total_samples_processed,
          units::sample_t output_buffer_timestamp_offset) {
          render_chunk (
            audio_buffer, current_internal_buffer_offset,
            samples_to_process_in_chunk, total_samples_processed,
            output_buffer_timestamp_offset);
        });

      playback_fraction_.store (
        internal_clip_buffer_position_.load ().in<double> (units::samples)
        / clip_loop_end_samples.in<double> (units::samples));
    },
    [] (units::sample_t) {
      // Audio needs no action when a clip stops or is replaced
    });
}
}
