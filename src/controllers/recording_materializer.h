// SPDX-FileCopyrightText: © 2026 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>

#include "controllers/recording_coordinator.h"
#include "controllers/recording_mode.h"
#include "structure/arrangement/arranger_object_all.h"
#include "undo/undo_stack.h"
#include "utils/audio.h"
#include "utils/qt.h"

#include <QObject>

namespace zrythm::controllers
{

/**
 * @brief Converts raw recording packets into arrangement clips.
 *
 * Subscribes to RecordingCoordinator::audioDataReady and midiDataReady to
 * create or expand clips for each track. For audio, the first packet for a
 * given continuous range creates a new AudioClip via the ClipCreator
 * callback; subsequent packets append frames to the clip's clip. For MIDI,
 * note-on/off pairs become MidiNote objects and CC/pitchbend/etc. become
 * MidiControlEvent objects inside a MidiClip.
 *
 * All clip creations within a single recording take (transport rolling to
 * transport stopped) are wrapped in a single undo macro so the entire take
 * can be undone in one step. The macro is finalized when
 * RecordingCoordinator::recordingSessionEnded fires.
 *
 * Operates entirely on the non-RT (main) thread. The UndoStack is held via
 * QPointer to guard against unexpected destruction ordering.
 */
class RecordingMaterializer : public QObject
{
  Q_OBJECT

public:
  using RecordingMode = recording::RecordingMode;

  /** Returned by clip creator callbacks on success. */
  struct CreatedClip
  {
    structure::arrangement::ArrangerObjectUuidReference clip;
    size_t                                              actual_lane_index{};
  };
  using ClipCreationResult = std::optional<CreatedClip>;

  /** Called to query the current recording mode (Takes, TakesMuted, etc.). */
  using RecordingModeProvider = std::function<RecordingMode ()>;

  /**
   * @brief Injected callbacks for creating arranger objects.
   *
   * Positions for MIDI callbacks (midi_note, midi_control_event) are
   * clip-relative sample positions.
   */
  struct ArrangerObjectCreators
  {
    /** Creates an AudioClip with initial audio frames. */
    std::function<ClipCreationResult (
      structure::tracks::TrackUuid     track_id,
      units::sample_t                  start_position,
      const utils::audio::AudioBuffer &initial_frames,
      size_t                           lane_index)>
      audio_clip;

    /** Creates an empty MidiClip. */
    std::function<ClipCreationResult (
      structure::tracks::TrackUuid track_id,
      units::sample_t              start_position,
      size_t                       lane_index)>
      midi_clip;

    /**
     * @brief Creates a ChordClip on a chord track (no lanes).
     *
     * Used as a fallback when @ref midi_clip returns nullopt (chord tracks
     * have no lanes). May be unset for non-chord-track contexts.
     */
    std::function<ClipCreationResult (
      structure::tracks::TrackUuid track_id,
      units::sample_t              start_position)>
      chord_clip;

    /** Creates a MidiNote inside a clip. */
    std::function<void (
      structure::arrangement::MidiClip &clip,
      units::sample_t                   start_position,
      units::sample_t                   end_position,
      int                               pitch,
      int                               velocity,
      int                               channel)>
      midi_note;

    /** Creates a MidiControlEvent inside a clip. */
    std::function<void (
      structure::arrangement::MidiClip                   &clip,
      units::sample_t                                     position,
      structure::arrangement::MidiControlEvent::EventType type,
      int                                                 channel,
      int                                                 controller,
      int                                                 value)>
      midi_control_event;

    /**
     * @brief Creates a ChordObject inside a ChordClip from a MIDI note number.
     *
     * The @p start_position is clip-relative (samples from the clip start).
     * May be unset for non-chord-track contexts.
     */
    std::function<void (
      structure::arrangement::ChordClip &clip,
      units::sample_t                    start_position,
      int                                note_number)>
      chord_object;

    /**
     * @brief Creates an AudioClip for a clip slot recording, not attached to
     * a lane, positioned at @p start_position.
     *
     * May be unset when clip slot recording is not supported.
     */
    std::function<std::optional<
      structure::arrangement::ArrangerObjectUuidReference> (
      structure::tracks::TrackUuid     track_id,
      units::sample_t                  start_position,
      const utils::audio::AudioBuffer &initial_frames)>
      slot_audio_clip;

    /**
     * @brief Creates an empty MidiClip for a clip slot recording, not
     * attached to a lane, positioned at @p start_position.
     *
     * May be unset when clip slot recording is not supported.
     */
    std::function<std::optional<
      structure::arrangement::ArrangerObjectUuidReference> (
      structure::tracks::TrackUuid track_id,
      units::sample_t              start_position)>
      slot_midi_clip;
  };

  /** Attaches the clip created for a clip slot recording to its slot. */
  using AttachClipFunc =
    std::function<void (structure::arrangement::ArrangerObjectUuidReference)>;

  explicit RecordingMaterializer (
    RecordingCoordinator  &recording_coordinator,
    undo::UndoStack       &undo_stack,
    ArrangerObjectCreators creators,
    RecordingModeProvider  recording_mode_provider,
    QObject *              parent = nullptr);

  ~RecordingMaterializer () override;

  Q_DISABLE_COPY_MOVE (RecordingMaterializer)

  /**
   * @brief Records the track's input from @p start_position into a new clip
   * that @p attach_clip puts into a clip slot.
   *
   * Recording is independent of the transport's record button and replaces
   * timeline recording for the track while it runs. It ends at the position
   * given to end_clip_slot_recording(), at a gap in the recorded data (e.g.
   * a transport loop), or when the recording session ends; then
   * clipSlotRecordingFinished() is emitted. The finished clip starts at
   * position 0 and loops over its recorded length.
   *
   * @return False (with a warning) if the slot clip creators are not set or
   * the track already records into a clip slot.
   */
  bool begin_clip_slot_recording (
    structure::tracks::TrackUuid track_id,
    units::sample_t              start_position,
    AttachClipFunc               attach_clip);

  /**
   * @brief Ends the track's clip slot recording at @p end_position.
   *
   * Data recorded past @p end_position is cut off. Does nothing if the
   * track does not record into a clip slot.
   */
  void end_clip_slot_recording (
    structure::tracks::TrackUuid track_id,
    units::sample_t              end_position);

  /**
   * @brief Whether the track currently records into a clip slot.
   */
  bool is_recording_into_clip_slot (structure::tracks::TrackUuid track_id) const
  {
    return slot_states_.contains (track_id);
  }

  /**
   * @brief Emitted when a clip slot recording ends.
   *
   * @param clip The recorded clip, or nullptr if nothing was recorded.
   */
  Q_SIGNAL void clipSlotRecordingFinished (
    structure::tracks::TrackUuid   track_id,
    structure::arrangement::Clip * clip);

private:
  /**
   * @brief Per-track state tracking the current recording context.
   *
   * Shared by both audio and MIDI paths. A track can only be in one session
   * (audio or MIDI) at a time, so the state is keyed by TrackUuid.
   */
  struct TrackRecordingState
  {
    /** A note-on waiting for its matching note-off. */
    struct PendingNote
    {
      units::sample_t start_position;
      uint8_t         velocity{};
      uint8_t         channel{};
    };

    /** Currently active clip (AudioClip or MidiClip). */
    std::optional<structure::arrangement::ArrangerObjectUuidReference>
      current_clip;
    /** End sample of the last contiguous packet (used for gap detection). */
    std::optional<units::sample_t> last_end_position;
    /** Lane index for the next clip creation (increments on discontinuity). */
    size_t current_lane_index = 0;
    /**
     * Notes with note-on received but no note-off yet.
     * Key: (channel << 8) | pitch. Value: FIFO queue of pending notes
     * (supports repeated note-ons on the same pitch before note-off).
     */
    std::unordered_map<uint16_t, std::deque<PendingNote>> unended_notes;

    /** Recording window and slot of a clip slot recording. */
    struct SlotTarget
    {
      units::sample_t                start_position;
      std::optional<units::sample_t> end_position;
      AttachClipFunc                 attach_clip;
    };

    /** Set when this state records into a clip slot. */
    std::optional<SlotTarget> slot_target;
  };

  /** Handles RecordingCoordinator::audioDataReady signal. */
  void on_audio_data_ready (
    structure::tracks::TrackUuid             track_id,
    const std::vector<RecordingAudioPacket> &packets);

  /** Adds one audio packet to the state's current clip. */
  void materialize_audio_packet (
    TrackRecordingState         &state,
    structure::tracks::TrackUuid track_id,
    const RecordingAudioPacket  &packet);

  /** Adds one MIDI packet to the state's current clip. */
  void materialize_midi_packet (
    TrackRecordingState         &state,
    structure::tracks::TrackUuid track_id,
    const RecordingMidiPacket   &packet);

  /**
   * @brief Part of [@p packet_start, @p packet_start + @p nframes) inside a
   * clip slot recording window, or nullopt if none.
   */
  static std::optional<std::pair<units::sample_t, units::sample_t>>
  slot_window_overlap (
    const TrackRecordingState::SlotTarget &target,
    units::sample_t                        packet_start,
    units::sample_t                        nframes);

  /**
   * @brief Whether a packet starting at @p packet_start continues the
   * state's recording without a gap.
   */
  static bool continues_recording (
    const TrackRecordingState &state,
    units::sample_t            packet_start)
  {
    return !state.last_end_position.has_value ()
           || packet_start == *state.last_end_position;
  }

  /**
   * @brief Finishes the track's clip slot recording at @p end_position (or
   * where the recorded data ends) and emits clipSlotRecordingFinished().
   */
  void finish_clip_slot_recording (
    structure::tracks::TrackUuid   track_id,
    std::optional<units::sample_t> end_position);

  /** Finishes the clip slot recording if its window ends at or before
   * @p position. */
  void finish_clip_slot_recording_if_past_end (
    structure::tracks::TrackUuid track_id,
    units::sample_t              position);

  /** Creates a new AudioClip or returns the existing one for this track. */
  std::optional<structure::arrangement::ArrangerObjectUuidReference>
  get_or_create_clip (
    TrackRecordingState             &state,
    structure::tracks::TrackUuid     track_id,
    units::sample_t                  start_position,
    const utils::audio::AudioBuffer &initial_frames);

  static dsp::FileAudioSource *
  get_audio_source_for_clip (structure::arrangement::AudioClip &clip);

  /**
   * @brief Writes the current audio clip's pending tail frames as a block.
   *
   * Called when a recorded clip is finished (gap or session end) so its
   * manifest is complete on disk.
   */
  void flush_current_clip_tail (TrackRecordingState &state);

  /** Handles RecordingCoordinator::midiDataReady signal. */
  void on_midi_data_ready (
    structure::tracks::TrackUuid            track_id,
    const std::vector<RecordingMidiPacket> &packets);

  /** Creates a MidiClip if none exists for this track. Returns false on
   * failure. */
  bool ensure_midi_clip (
    TrackRecordingState         &state,
    structure::tracks::TrackUuid track_id,
    units::sample_t              start_position);

  /**
   * @brief Called when a gap is detected between packets.
   *
   * Force-completes any pending MIDI notes, handles lane increment/muting
   * based on recording mode, and resets the current clip.
   */
  void handle_discontinuity (TrackRecordingState &state);

  /**
   * @brief Creates notes for any pending note-ons that never received a
   * note-off.
   *
   * Notes are ended at last_end_position. Called during discontinuity and
   * session finalization.
   */
  void force_complete_pending_notes (TrackRecordingState &state);

  /** Begins the undo macro if not already active. */
  void ensure_recording_macro ();

  /** Ends the undo macro and clears all per-track state. */
  void finalize_recording_macro ();

  RecordingCoordinator     &recording_coordinator_;
  QPointer<undo::UndoStack> undo_stack_;
  ArrangerObjectCreators    creators_;
  RecordingModeProvider     recording_mode_provider_;

  /** Per-track recording state, populated on first packet for each track. */
  std::unordered_map<structure::tracks::TrackUuid, TrackRecordingState>
    track_states_;

  /**
   * Per-track state of clip slot recordings. Kept apart from track_states_,
   * which is cleared when a timeline recording take ends.
   */
  std::unordered_map<structure::tracks::TrackUuid, TrackRecordingState>
    slot_states_;

  /**
   * Pre-allocated scratch buffer reused across on_audio_data_ready()
   * calls to avoid per-packet heap allocations. Resized as needed when
   * the incoming frame count exceeds the current capacity.
   */
  utils::audio::AudioBuffer scratch_buf_{ 2, 0 };

  bool recording_macro_active_ = false;
};

}
