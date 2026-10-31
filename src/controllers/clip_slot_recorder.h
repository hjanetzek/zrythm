// SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#pragma once

#include "controllers/recording_materializer.h"
#include "dsp/tempo_map.h"
#include "dsp/transport.h"
#include "structure/scenes/clip_launcher.h"
#include "structure/scenes/clip_playback_service.h"
#include "structure/tracks/track_collection.h"

#include <QPointer>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

namespace zrythm::controllers
{

/**
 * @brief Records track input into clip launcher slots.
 *
 * Recording starts and stops at the clip launcher's launch quantization
 * point. While waiting for the start the slot is RecordQueued, then
 * Recording; when the recording ends it holds the recorded clip. A
 * recording stopped with stopRecording() while the transport rolls
 * continues seamlessly into playback of the recorded clip from the stop
 * point; otherwise the slot is left Stopped.
 */
class ClipSlotRecorder : public QObject
{
  Q_OBJECT
  Q_PROPERTY (
    int fixedLengthBars READ fixedLengthBars WRITE setFixedLengthBars NOTIFY
      fixedLengthBarsChanged)
  QML_ELEMENT
  QML_UNCREATABLE ("")

public:
  /** Puts a recorded clip into a clip slot (undoable). */
  using AttachClipFunc = std::function<void (
    structure::tracks::Track &,
    structure::scenes::ClipSlot &,
    structure::arrangement::ArrangerObjectUuidReference)>;

  ClipSlotRecorder (
    RecordingMaterializer                    &materializer,
    dsp::Transport                           &transport,
    const dsp::TempoMap                      &tempo_map,
    const structure::scenes::ClipLauncher    &clip_launcher,
    const structure::tracks::TrackCollection &track_collection,
    AttachClipFunc                            attach_clip,
    QObject *                                 parent = nullptr);

  /**
   * @brief Service used to stop a track's playing clip when recording into
   * one of its slots starts (optional).
   */
  void
  set_clip_playback_service (structure::scenes::ClipPlaybackService * service)
  {
    clip_playback_service_ = service;
  }

  /**
   * @brief Length of new recordings in bars, or 0 to record until
   * stopRecording() is called.
   *
   * A fixed-length recording ends by itself and continues into playback
   * like a stopped one.
   */
  int           fixedLengthBars () const { return fixed_length_bars_; }
  void          setFixedLengthBars (int bars);
  Q_SIGNAL void fixedLengthBarsChanged ();

  /**
   * @brief Whether recording into @p clipSlot of @p track can start: the
   * track can record, the slot is empty and the track does not record into
   * another slot.
   */
  Q_INVOKABLE bool canRecordIntoClipSlot (
    structure::tracks::Track *    track,
    structure::scenes::ClipSlot * clipSlot) const;

  /**
   * @brief Starts recording into @p clipSlot at the next quantization point,
   * arming the track and starting the transport if needed.
   *
   * Refused with a warning if canRecordIntoClipSlot() is false.
   */
  Q_INVOKABLE void recordIntoClipSlot (
    structure::tracks::Track *    track,
    structure::scenes::ClipSlot * clipSlot);

  /**
   * @brief Ends the track's clip slot recording at the next quantization
   * point.
   */
  Q_INVOKABLE void stopRecording (structure::tracks::Track * track);

  /**
   * @brief First tick at or after @p tick where @p quantization takes
   * effect.
   */
  static dsp::TimelineTickI next_quantization_tick (
    const dsp::TempoMap                  &tempo_map,
    units::precise_tick_t                 tick,
    structure::tracks::ClipQuantizeOption quantization);

  /**
   * @brief Emitted when a recording into @p clipSlot has finished.
   *
   * @param clip The recorded clip, or nullptr if nothing was recorded.
   */
  Q_SIGNAL void clipSlotRecorded (
    structure::scenes::ClipSlot *  clipSlot,
    structure::arrangement::Clip * clip);

private:
  /** Next quantization point after the playhead, in samples. */
  units::sample_t next_quantization_point () const;

  /** Moves queued recordings to Recording once the playhead reaches them. */
  void update_recording_states ();

  void on_clip_slot_recording_finished (
    structure::tracks::TrackUuid   track_id,
    structure::arrangement::Clip * clip);

  struct SlotRecording
  {
    QPointer<structure::scenes::ClipSlot> clip_slot;
    units::sample_t                       start_position;

    /** Where stopRecording() ends the recording, once requested. */
    std::optional<units::sample_t> requested_end_position;
  };

  RecordingMaterializer                           &materializer_;
  dsp::Transport                                  &transport_;
  const dsp::TempoMap                             &tempo_map_;
  const structure::scenes::ClipLauncher           &clip_launcher_;
  const structure::tracks::TrackCollection        &track_collection_;
  AttachClipFunc                                   attach_clip_;
  QPointer<structure::scenes::ClipPlaybackService> clip_playback_service_;

  std::unordered_map<structure::tracks::TrackUuid, SlotRecording> recordings_;

  int fixed_length_bars_{};

  /** Polls the playhead while recordings wait for their start. */
  utils::QObjectUniquePtr<QTimer> state_timer_;
};

} // namespace zrythm::controllers
