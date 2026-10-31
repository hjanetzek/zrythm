// SPDX-FileCopyrightText: © 2025-2026 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#pragma once

#include "structure/arrangement/arranger_object_all.h"
#include "structure/scenes/clip_slot.h"
#include "structure/scenes/scene.h"
#include "structure/tracks/clip_playback_data_provider.h"
#include "structure/tracks/track_collection.h"

#include <QtQmlIntegration/qqmlintegration.h>

namespace zrythm::structure::scenes
{

/**
 * @brief Service for managing clip playback operations.
 *
 * This service handles launching and stopping clips with proper quantization,
 * managing track processor states, and coordinating between the UI and audio
 * engine.
 */
class ClipPlaybackService : public QObject
{
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE ("")

public:
  explicit ClipPlaybackService (
    utils::IObjectRegistry        &registry,
    const tracks::TrackCollection &track_collection,
    QObject *                      parent = nullptr);

  // ========================================================================
  // Clip Operations
  // ========================================================================

  /**
   * @brief Launch a clip with quantization.
   *
   * @param track The track to play the clip on
   * @param clipSlot The clip slot containing the clip to launch
   * @param quantize Quantization option for timing
   */
  Q_INVOKABLE void launchClip (
    tracks::Track *                       track,
    ClipSlot *                            clipSlot,
    structure::tracks::ClipQuantizeOption quantize =
      structure::tracks::ClipQuantizeOption::NextBar);

  /**
   * @brief Launches a clip right away, playing as if it had started at
   * @p anchor_position (which may lie in the past).
   *
   * Used to continue seamlessly into a clip whose start lies before the
   * moment it can be launched, e.g. a clip that was just recorded.
   */
  void launch_clip_aligned (
    tracks::Track * track,
    ClipSlot *      clipSlot,
    units::sample_t anchor_position);

  /**
   * @brief Stop a clip with quantization.
   *
   * @param track The track containing the playing clip
   * @param clipSlot The clip slot to stop
   * @param quantize Quantization option for timing
   */
  Q_INVOKABLE void stopClip (
    tracks::Track *                       track,
    ClipSlot *                            clipSlot,
    structure::tracks::ClipQuantizeOption quantize =
      structure::tracks::ClipQuantizeOption::NextBar);

  /**
   * @brief Launch all clips in a scene.
   *
   * @param scene The scene to launch
   * @param quantize Quantization option for timing
   */
  Q_INVOKABLE void launchScene (
    Scene *                               scene,
    structure::tracks::ClipQuantizeOption quantize =
      structure::tracks::ClipQuantizeOption::NextBar);

  /**
   * @brief Stop all clips in a scene.
   *
   * @param scene The scene to stop
   * @param quantize Quantization option for timing
   */
  Q_INVOKABLE void stopScene (
    Scene *                               scene,
    structure::tracks::ClipQuantizeOption quantize =
      structure::tracks::ClipQuantizeOption::NextBar);

  /**
   * @brief Stop all playing clips across all tracks.
   *
   * @param quantize Quantization option for timing
   */
  Q_INVOKABLE void stopAllClips (
    structure::tracks::ClipQuantizeOption quantize =
      structure::tracks::ClipQuantizeOption::NextBar);

  /**
   * @brief Stop the clip playing or queued to play on a track.
   *
   * Does nothing if no clip is playing on the track.
   *
   * @param track The track to stop
   * @param quantize Quantization option for timing
   */
  Q_INVOKABLE void stopTrackClips (
    tracks::Track *                       track,
    structure::tracks::ClipQuantizeOption quantize =
      structure::tracks::ClipQuantizeOption::NextBar);

  // ========================================================================
  // State Queries
  // ========================================================================

  /**
   * @brief Check if a clip is currently playing.
   *
   * @param clipSlot The clip slot to check
   * @return true if the clip is playing
   */
  Q_INVOKABLE bool isClipPlaying (ClipSlot * clipSlot) const;

  /**
   * @brief Get the current playback position of a clip, in.
   *
   * @param clipSlot The clip slot to get the position for
   * @return Position in the clip as a percentage (0.0 to 1.0), or -1 if not
   * playing
   */
  Q_INVOKABLE double getClipPlaybackPosition (ClipSlot * clipSlot) const;

  /**
   * @brief Get all currently playing clips.
   *
   * @return List of playing clip slots
   */
  Q_INVOKABLE QList<ClipSlot *> getPlayingClips () const;

  // ========================================================================
  // Track Playback Mode Management
  // ========================================================================

  /**
   * @brief Set a track to use clip launcher mode.
   *
   * @param track The track to configure
   */
  void setTrackToClipLauncherMode (tracks::Track * track);

  /**
   * @brief Set a track to use timeline mode.
   *
   * Pending clip operations on the track are cancelled and a playing clip
   * is stopped immediately.
   *
   * @param track The track to configure
   */
  Q_INVOKABLE void setTrackToTimelineMode (tracks::Track * track);

  /**
   * @brief Set all tracks to use timeline mode.
   *
   * @see setTrackToTimelineMode()
   */
  Q_INVOKABLE void setAllTracksToTimelineMode ();

  /**
   * @brief Cancel any pending clip operations for a track.
   *
   * This is called when switching modes to ensure clips don't get stuck in
   * queued state.
   *
   * @param track The track to cancel operations for
   */
  void cancelPendingClipOperations (tracks::Track * track);

Q_SIGNALS:
  /**
   * @brief Emitted when a clip is launched.
   */
  void clipLaunched (tracks::Track * track, ClipSlot * clipSlot);

  /**
   * @brief Emitted when a clip is stopped.
   */
  void clipStopped (tracks::Track * track, ClipSlot * clipSlot);

  /**
   * @brief Emitted when a scene is launched.
   */
  void sceneLaunched (Scene * scene);

  /**
   * @brief Emitted when a scene is stopped.
   */
  void sceneStopped (Scene * scene);

private:
  // ========================================================================
  // Internal Implementation
  // ========================================================================

  /**
   * @brief Schedule a clip launch at the appropriate quantized position.
   */
  void scheduleClipLaunch (
    tracks::Track *                       track,
    ClipSlot *                            clipSlot,
    structure::tracks::ClipQuantizeOption quantize,
    std::optional<units::sample_t>        anchor_position = std::nullopt);

  /**
   * @brief Schedule a clip stop at the appropriate quantized position.
   */
  void scheduleClipStop (
    tracks::Track *                       track,
    ClipSlot *                            clipSlot,
    structure::tracks::ClipQuantizeOption quantize);

  /**
   * @brief Generate events for a clip (clip).
   */
  template <arrangement::ClipObject ClipT>
  void generateClipEvents (
    tracks::Track *                       track,
    const ClipT                          &clip,
    structure::tracks::ClipQuantizeOption quantize,
    std::optional<units::sample_t>        anchor_position)
  {
    if (track == nullptr)
      return;

    auto * processor = track->get_track_processor ();
    if (processor == nullptr)
      return;

    // Generate events using the clip launcher event provider
    if constexpr (std::is_same_v<ClipT, arrangement::MidiClip>)
      {
        processor->clip_playback_data_provider ().generate_midi_events (
          clip, quantize, anchor_position);
      }
    else if constexpr (std::is_same_v<ClipT, arrangement::AudioClip>)
      {
        processor->clip_playback_data_provider ().generate_audio_events (
          clip, quantize, anchor_position);
      }
  }

  /**
   * @brief Update clip slot state and emit appropriate signals.
   */
  void updateClipSlotState (ClipSlot * clipSlot, ClipSlot::ClipState newState);

  /**
   * @brief Starts polling the track processors for playback changes.
   */
  void startPlaybackStateMonitor ();

  /**
   * @brief Returns the track with @p track_id, or nullptr if it is no longer
   * in the track collection.
   */
  tracks::Track * trackIfPresent (const tracks::Track::Uuid &track_id) const;

  /**
   * @brief Starts tracking @p clip_slot so that it is forgotten when
   * destroyed.
   */
  void watchClipSlot (ClipSlot * clip_slot);

  /**
   * @brief Stops or cancels playback of a destroyed clip slot and forgets
   * it.
   *
   * @p object is only compared against tracked slots, never accessed.
   */
  void onClipSlotDestroyed (QObject * object);

  /**
   * @brief Updates clip slot states from the playback state of each track's
   * processor.
   */
  void checkPlaybackState ();

  /**
   * @brief Clip slots of a track that are playing or waiting for a
   * playback change.
   */
  struct TrackClipState
  {
    /** Slot whose clip is playing. */
    ClipSlot * playing_slot{};

    /** Slot whose clip is queued to start. */
    ClipSlot * queued_slot{};

    /** Whether the playing clip was asked to stop. */
    bool stop_requested{};

    /**
     * Whether the requested stop waits for a quantization point (and can
     * still be replaced by a launch).
     */
    bool stop_quantized{};
  };

private:
  utils::IObjectRegistry        &registry_;
  const tracks::TrackCollection &track_collection_;

  /**
   * @brief Clip slot states by track UUID.
   */
  std::unordered_map<tracks::Track::Uuid, TrackClipState> track_states_;

  /**
   * @brief Polls the track processors while any track waits for a playback
   * change.
   */
  utils::QObjectUniquePtr<QTimer> playback_state_monitor_;
};

}
