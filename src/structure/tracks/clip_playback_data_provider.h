// SPDX-FileCopyrightText: © 2025 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <variant>

#include "dsp/graph_node.h"
#include "dsp/midi_event_buffer.h"
#include "structure/tracks/track_fwd.h"

#include <farbot/RealtimeObject.hpp>
#include <juce_audio_basics/juce_audio_basics.h>

namespace zrythm::structure::tracks
{

/**
 * @brief Event provider for clip launcher-based MIDI and audio events.
 */
class ClipPlaybackDataProvider final
{
  /**
   * @brief MIDI clip contents, timed in ticks from the clip start.
   *
   * Playback runs through the intro (clip start to the first loop end) once,
   * then repeats the loop (the last @ref loop_length_ ticks) indefinitely.
   */
  struct MidiContent
  {
    /** Events sorted by time, from 0 to intro + loop length. */
    std::vector<dsp::TickBasedMidiEvent> events_;

    /** Length from the clip start to the first loop end. */
    units::precise_tick_t intro_length_;

    /** Length of the repeated loop. */
    units::precise_tick_t loop_length_;

    /** Clip start and loop start markers, and the clip length (for
     * reporting the position within the clip). */
    units::precise_tick_t clip_start_;
    units::precise_tick_t loop_start_;
    units::precise_tick_t clip_length_;
  };
  using AudioContent = juce::AudioSampleBuffer;

  /**
   * @brief Rendered clip contents prepared for realtime playback.
   */
  struct PlaybackCache
  {
    /** Clip contents, or std::monostate for silence (a queued stop). */
    std::variant<std::monostate, MidiContent, AudioContent> content_;
    structure::tracks::ClipQuantizeOption                   quantize_opt_;

    /**
     * @brief End position to loop at (audio clips).
     */
    units::precise_tick_t end_position_;
  };

  using CacheSlot = farbot::RealtimeObject<
    std::optional<PlaybackCache>,
    farbot::RealtimeObjectOptions::nonRealtimeMutatable>;

public:
  ClipPlaybackDataProvider (const dsp::TempoMap &tempo_map);

  /**
   * @brief Queues a MIDI clip to start playing at its quantization point.
   *
   * A clip that is already playing continues until that point. Replaces a
   * clip that was queued earlier and has not started yet.
   *
   * The clip plays from its clip start to its loop end, then repeats from
   * its loop start to its loop end; its length is not used. Event times
   * follow the tempo map from the launch position on.
   *
   * Must be called from the UI thread.
   */
  void generate_midi_events (
    const arrangement::MidiClip          &midi_clip,
    structure::tracks::ClipQuantizeOption quantize_option);

  /**
   * @brief Queues an audio clip to start playing at its quantization point.
   *
   * @see generate_midi_events()
   */
  void generate_audio_events (
    const arrangement::AudioClip         &audio_clip,
    structure::tracks::ClipQuantizeOption quantize_option);

  /**
   * @brief Stops playback at the quantization point and discards a queued
   * clip.
   *
   * With Immediate quantization, playback stops on the next processing
   * cycle. Otherwise the playing clip continues until the quantization point;
   * a later launch replaces the queued stop. Must be called from the UI
   * thread.
   */
  void
  queue_stop_playback (structure::tracks::ClipQuantizeOption quantize_option);

  /**
   * @brief Discards a queued clip that has not started yet.
   *
   * A clip that is already playing continues. Must be called from the UI
   * thread.
   */
  void cancel_pending_launch ();

  /**
   * @brief Whether a clip or a stop is queued and has not taken effect yet.
   */
  bool launch_pending () const
  {
    return pending_slot (slot_state_.load ()) != kNoSlot;
  }

  // TrackEventProvider interface
  void process_midi_events (
    const dsp::graph::ProcessBlockInfo &time_nfo,
    dsp::MidiEventBuffer &output_buffer) noexcept [[clang::nonblocking]];

  /**
   * @brief Process audio events for clip launcher playback.
   */
  void process_audio_events (
    const dsp::graph::ProcessBlockInfo &time_nfo,
    std::span<float>                    left_buffer,
    std::span<float> right_buffer) noexcept [[clang::nonblocking]];

  /**
   * @brief Whether currently playing any part of the clip.
   */
  auto playing () const { return playing_.load (); }

  /**
   * @brief Position within the playing clip as a fraction of its length.
   */
  double current_playback_fraction () const
  {
    return playback_fraction_.load ();
  }

private:
  /**
   * @brief Packed slot state.
   *
   * Bits 0-1 hold the index of the slot being played, bits 2-3 the index of
   * the slot queued to play next (or kNoSlot), and the remaining bits a
   * counter that changes every time a clip is queued.
   */
  using SlotState = uint32_t;
  static constexpr uint32_t kNoSlot = 3;

  static constexpr uint32_t current_slot (SlotState state)
  {
    return state & 0b11U;
  }
  static constexpr uint32_t pending_slot (SlotState state)
  {
    return (state >> 2) & 0b11U;
  }
  static constexpr uint32_t generation (SlotState state) { return state >> 4; }
  static constexpr SlotState
  make_slot_state (uint32_t current, uint32_t pending, uint32_t gen)
  {
    return current | (pending << 2) | (gen << 4);
  }

  /**
   * @brief Takes back a queued clip so that the slot state cannot change
   * until the UI thread stores a new one.
   *
   * @return The slot state after retracting.
   */
  SlotState retract_pending_launch ();

  /**
   * @brief Writes @p cache into the slot that is not being played and
   * queues it.
   */
  void queue_cache (PlaybackCache &&cache);

  /**
   * @brief Distance from the block start to the quantization point, if the
   * point lies within the block.
   *
   * Immediate quantization yields 0.
   */
  std::optional<units::sample_t> quantization_offset_in_block (
    const dsp::graph::ProcessBlockInfo   &time_nfo,
    structure::tracks::ClipQuantizeOption quantize_opt) const;

  /**
   * @brief Recalculates the position within the clip after the transport
   * position changed.
   */
  void update_playback_position (
    const dsp::graph::ProcessBlockInfo &time_nfo,
    units::sample_t                     clip_loop_end_samples);

  /**
   * @brief Renders one block: the playing clip up to the point where a
   * queued clip takes over, then the queued clip.
   *
   * @param render_segment Called with the clip contents, the playback
   * cache, the offset of the segment within the block and its length in
   * samples.
   * @param on_playback_end Called with the output buffer offset at which a
   * playing clip stops or is replaced.
   */
  template <typename ContentT, typename RenderSegmentFunc, typename EndFunc>
  void process_block (
    const dsp::graph::ProcessBlockInfo &time_nfo,
    RenderSegmentFunc                   render_segment,
    EndFunc on_playback_end) noexcept [[clang::nonblocking]];

  /**
   * @brief Renders @p length samples of the clip in @p slot, starting
   * @p block_offset samples into the block.
   */
  template <typename ContentT, typename RenderSegmentFunc, typename EndFunc>
  void render_slot (
    uint32_t                            slot,
    const dsp::graph::ProcessBlockInfo &time_nfo,
    units::sample_t                     block_offset,
    units::sample_t                     length,
    RenderSegmentFunc                  &render_segment,
    EndFunc &on_playback_end) noexcept [[clang::nonblocking]];

  /**
   * @brief Renders a segment of a MIDI clip, following the tempo map.
   */
  void render_midi_segment (
    const MidiContent                  &content,
    const dsp::graph::ProcessBlockInfo &time_nfo,
    units::sample_t                     block_offset,
    units::sample_t                     length,
    dsp::MidiEventBuffer &output_buffer) noexcept [[clang::nonblocking]];

  /**
   * @brief Template method for processing chunks with looping.
   */
  template <typename ProcessFunc>
  void process_chunks_with_looping (
    units::sample_t internal_buffer_start_offset,
    units::sample_t samples_to_process,
    units::sample_t clip_loop_end_samples,
    units::sample_t output_buffer_timestamp_offset,
    ProcessFunc     process_chunk);

private:
  /**
   * @brief Two cache slots: one being played and one for the next clip.
   *
   * The UI thread writes only to a slot that is neither played nor queued.
   */
  std::array<CacheSlot, 2> slots_;

  /**
   * @brief Which slot is played and which is queued (see SlotState).
   *
   * The audio thread changes it only to switch to the queued slot; the UI
   * thread changes it to queue or take back a slot.
   */
  std::atomic<SlotState> slot_state_{ make_slot_state (0, kNoSlot, 0) };

  const dsp::TempoMap &tempo_map_;

  /**
   * @brief Whether currently playing.
   *
   * If false, we are currently queued or stopped.
   *
   * This is atomic because it may be accessed from the UI thread to update
   * the clip launcher visuals (e.g., to show play/pause state).
   */
  std::atomic<bool> playing_{ false };

  /**
   * @brief Current internal buffer position (offset within the clip)
   *
   * This is atomic because it may be accessed from the UI thread to update
   * the clip launcher visuals (e.g., to show playback progress within the clip).
   */
  std::atomic<units::sample_t> internal_clip_buffer_position_;

  /**
   * @brief Last timeline position we've seen (audio thread only).
   */
  units::sample_t last_seen_timeline_position_;

  /**
   * @brief Timeline position where the clip was launched (audio thread
   * only).
   */
  units::sample_t clip_launch_timeline_position_;

  /**
   * @brief Timeline tick where the clip was launched (audio thread only).
   */
  units::precise_tick_t clip_launch_timeline_tick_;

  /**
   * @brief Ticks played since the launch (MIDI clips, audio thread only).
   */
  units::precise_tick_t midi_playback_tick_;

  /**
   * @brief Position within the played contents, from 0 to 1 (see
   * current_playback_fraction()).
   */
  std::atomic<double> playback_fraction_{ 0.0 };

  static_assert (decltype (internal_clip_buffer_position_)::is_always_lock_free);
  static_assert (decltype (slot_state_)::is_always_lock_free);
};

}
