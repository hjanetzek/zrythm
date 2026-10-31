// SPDX-FileCopyrightText: © 2025 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#include "structure/arrangement/clip_renderer.h"
#include "structure/tracks/clip_playback_data_provider.h"
#include "utils/float_ranges.h"

namespace zrythm::structure::tracks
{

// Define to enable debug logging for clip launcher event provider
static constexpr bool CLIP_LAUNCHER_EVENT_PROVIDER_DEBUG = false;

ClipPlaybackDataProvider::ClipPlaybackDataProvider (
  const dsp::TempoMap &tempo_map)
    : tempo_map_ (tempo_map), last_seen_timeline_position_ (units::samples (0)),
      clip_launch_timeline_position_ (units::samples (0))
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
  structure::tracks::ClipQuantizeOption quantize_option)
{
  juce::MidiMessageSequence clip_seq;

  // Serialize clip (timings in ticks)
  arrangement::ClipRenderer::serialize_to_sequence (midi_clip, clip_seq);

  // Convert JUCE sequence to native events with sample timestamps
  std::vector<dsp::SampleBasedMidiEvent> native_events;
  native_events.reserve (clip_seq.getNumEvents ());
  for (const auto &event : clip_seq)
    {
      const auto sample_time = tempo_map_.tick_to_samples_rounded (
        dsp::TimelineTick{ units::ticks (event->message.getTimeStamp ()) });
      const auto * raw = event->message.getRawData ();
      const auto   raw_size =
        static_cast<size_t> (event->message.getRawDataSize ());
      native_events.push_back (
        dsp::midi_event::make_raw (
          std::span<const midi_byte_t>{ raw, raw_size }, sample_time));
    }

  queue_cache (
    PlaybackCache{
      .content_ = std::move (native_events),
      .quantize_opt_ = quantize_option,
      .end_position_ = units::ticks (midi_clip.length ()->ticks ()) });
}

void
ClipPlaybackDataProvider::generate_audio_events (
  const arrangement::AudioClip         &audio_clip,
  structure::tracks::ClipQuantizeOption quantize_option)
{
  // Serialize clip
  juce::AudioSampleBuffer audio_buffer;
  arrangement::ClipRenderer::serialize_to_buffer (audio_clip, audio_buffer);

  queue_cache (
    PlaybackCache{
      .content_ = std::move (audio_buffer),
      .quantize_opt_ = quantize_option,
      .end_position_ = units::ticks (audio_clip.length ()->ticks ()) });
}

void
ClipPlaybackDataProvider::queue_stop_playback (
  [[maybe_unused]] structure::tracks::ClipQuantizeOption quantize_option)
{
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
  units::sample_t                     clip_loop_end_samples)
{
  const auto current_timeline_position = time_nfo.transport_position_;
  if (current_timeline_position == last_seen_timeline_position_)
    return;

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
  const auto musical_pos_at_start = tempo_map_.tick_to_musical_position (
    au::round_as<int64_t> (
      units::ticks,
      tempo_map_.samples_to_tick (current_timeline_position).asQuantity ()));
  const auto musical_pos_at_end = tempo_map_.tick_to_musical_position (
    au::round_as<int64_t> (
      units::ticks,
      tempo_map_.samples_to_tick (time_nfo.end_position ()).asQuantity ()));

  const auto at_beat_start =
    musical_pos_at_start.sixteenth == 1 && musical_pos_at_start.tick == 0;

  std::optional<dsp::TempoMap::MusicalPosition> musical_pos_to_start_at;
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

    default:
      // Finer quantization options are not supported yet and start
      // immediately
      return units::samples (0);
    }

  if (!musical_pos_to_start_at.has_value ())
    return std::nullopt;

  // The quantization point can round to slightly before the block start when
  // the block starts exactly on it
  const auto quantized_timeline_position = max (
    tempo_map_.tick_to_samples_rounded (
      tempo_map_.musical_position_to_tick (*musical_pos_to_start_at)),
    current_timeline_position);
  const auto offset = quantized_timeline_position - current_timeline_position;

  // A point at the block end belongs to the next block
  if (offset >= time_nfo.nframes_)
    return std::nullopt;

  return offset;
}

template <typename ContentT, typename RenderChunkFunc, typename EndFunc>
void
ClipPlaybackDataProvider::render_slot (
  uint32_t                            slot,
  const dsp::graph::ProcessBlockInfo &time_nfo,
  units::sample_t                     block_offset,
  units::sample_t                     length,
  RenderChunkFunc                    &render_chunk,
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

  const auto clip_loop_end_samples = tempo_map_.tick_to_samples_rounded (
    dsp::TimelineTick{ (*cache)->end_position_ });
  if (clip_loop_end_samples <= units::samples (0))
    return;

  // A clip that was already playing at the block start follows the
  // transport position
  if (block_offset == units::samples (0))
    update_playback_position (time_nfo, clip_loop_end_samples);

  process_chunks_with_looping (
    internal_clip_buffer_position_.load (), length, clip_loop_end_samples,
    time_nfo.buffer_offset_ + block_offset,
    [&] (
      units::sample_t current_internal_buffer_offset,
      units::sample_t samples_to_process_in_chunk,
      units::sample_t total_samples_processed,
      units::sample_t output_buffer_timestamp_offset) {
      render_chunk (
        *content, current_internal_buffer_offset, samples_to_process_in_chunk,
        total_samples_processed, output_buffer_timestamp_offset);
    });
}

template <typename ContentT, typename RenderChunkFunc, typename EndFunc>
void
ClipPlaybackDataProvider::process_block (
  const dsp::graph::ProcessBlockInfo &time_nfo,
  RenderChunkFunc                     render_chunk,
  EndFunc                             on_playback_end) noexcept
{
  const auto state = slot_state_.load (std::memory_order_acquire);
  const auto pending = pending_slot (state);

  // Where within the block the queued clip takes over, if it does
  const auto switch_offset = [&] () -> std::optional<units::sample_t> {
    if (pending == kNoSlot)
      return std::nullopt;
    CacheSlot::ScopedAccess<farbot::ThreadType::realtime> pending_cache{
      slots_.at (pending)
    };
    if (
      !pending_cache->has_value ()
      || !std::holds_alternative<ContentT> ((*pending_cache)->content_))
      return std::nullopt;
    return quantization_offset_in_block (
      time_nfo, (*pending_cache)->quantize_opt_);
  }();

  const auto block_length = time_nfo.nframes_;
  const auto played_length = switch_offset.value_or (block_length);
  render_slot<ContentT> (
    current_slot (state), time_nfo, units::samples (0), played_length,
    render_chunk, on_playback_end);

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

          playing_.store (true);
          clip_launch_timeline_position_ =
            time_nfo.transport_position_ + *switch_offset;
          internal_clip_buffer_position_.store (units::samples (0));

          render_slot<ContentT> (
            pending, time_nfo, *switch_offset, block_length - *switch_offset,
            render_chunk, on_playback_end);
        }
      else
        {
          // The UI thread took the queued clip back: keep playing what is
          // played now
          render_slot<ContentT> (
            current_slot (expected_state), time_nfo, *switch_offset,
            block_length - *switch_offset, render_chunk, on_playback_end);
        }
    }

  last_seen_timeline_position_ = time_nfo.transport_position_;
}

void
ClipPlaybackDataProvider::process_midi_events (
  const dsp::graph::ProcessBlockInfo &time_nfo,
  dsp::MidiEventBuffer               &output_buffer) noexcept
{
  process_block<MidiContent> (
    time_nfo,
    [&] (
      const MidiContent &midi_events,
      units::sample_t    current_internal_buffer_offset,
      units::sample_t    samples_to_process_in_chunk,
      units::sample_t    total_samples_processed,
      units::sample_t    output_buffer_timestamp_offset) {
      const auto chunk_end =
        current_internal_buffer_offset + samples_to_process_in_chunk;
      for (
        auto it = std::ranges::lower_bound (
          midi_events, current_internal_buffer_offset, {},
          &dsp::SampleBasedMidiEvent::time_);
        it != midi_events.end () && it->time_ < chunk_end; ++it)
        {
          // Event timestamp relative to the start of this chunk, plus the
          // samples already rendered and the segment's output offset
          const auto local_timestamp =
            total_samples_processed
            + (it->time_ - current_internal_buffer_offset)
            + output_buffer_timestamp_offset;
          output_buffer.push_back (
            au::floor_as<uint32_t> (units::samples, local_timestamp),
            it->data ());
        }
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
  process_block<AudioContent> (
    time_nfo,
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
    },
    [] (units::sample_t) {
      // Audio needs no action when a clip stops or is replaced
    });
}
}
