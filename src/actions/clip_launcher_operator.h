// SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#pragma once

#include "structure/scenes/clip_launcher.h"
#include "structure/scenes/clip_playback_service.h"
#include "undo/undo_stack.h"

#include <QPointer>
#include <QtQmlIntegration/qqmlintegration.h>

namespace zrythm::actions
{

/**
 * @brief Undoable scene operations on a clip launcher.
 *
 * Operations are refused with a warning if the clip launcher or the undo
 * stack is not set.
 */
class ClipLauncherOperator : public QObject
{
  Q_OBJECT
  Q_PROPERTY (
    zrythm::structure::scenes::ClipLauncher * clipLauncher READ clipLauncher
      WRITE setClipLauncher NOTIFY clipLauncherChanged)
  Q_PROPERTY (
    zrythm::structure::scenes::ClipPlaybackService * clipPlaybackService READ
      clipPlaybackService WRITE setClipPlaybackService NOTIFY
        clipPlaybackServiceChanged)
  Q_PROPERTY (
    zrythm::undo::UndoStack * undoStack READ undoStack WRITE setUndoStack NOTIFY
      undoStackChanged)
  QML_ELEMENT

public:
  explicit ClipLauncherOperator (QObject * parent = nullptr) : QObject (parent)
  {
  }

  structure::scenes::ClipLauncher * clipLauncher () const
  {
    return clip_launcher_;
  }
  void setClipLauncher (structure::scenes::ClipLauncher * clipLauncher)
  {
    if (clip_launcher_ != clipLauncher)
      {
        clip_launcher_ = clipLauncher;
        Q_EMIT clipLauncherChanged ();
      }
  }
  Q_SIGNAL void clipLauncherChanged ();

  /**
   * @brief Service used to stop the clips of removed scenes (optional).
   */
  structure::scenes::ClipPlaybackService * clipPlaybackService () const
  {
    return clip_playback_service_;
  }
  void setClipPlaybackService (structure::scenes::ClipPlaybackService * service)
  {
    if (clip_playback_service_ != service)
      {
        clip_playback_service_ = service;
        Q_EMIT clipPlaybackServiceChanged ();
      }
  }
  Q_SIGNAL void clipPlaybackServiceChanged ();

  undo::UndoStack * undoStack () const { return undo_stack_; }
  void              setUndoStack (undo::UndoStack * undoStack)
  {
    if (undo_stack_ != undoStack)
      {
        undo_stack_ = undoStack;
        Q_EMIT undoStackChanged ();
      }
  }
  Q_SIGNAL void undoStackChanged ();

  /**
   * @brief Adds an empty scene after the last one.
   *
   * @return The new scene, or nullptr if refused.
   */
  Q_INVOKABLE structure::scenes::Scene * addScene ();

  /**
   * @brief Inserts an empty scene at @p index (0 to the scene count).
   *
   * @return The new scene, or nullptr if refused.
   */
  Q_INVOKABLE structure::scenes::Scene * insertScene (int index);

  /**
   * @brief Removes @p scene, stopping its clips immediately.
   */
  Q_INVOKABLE void removeScene (structure::scenes::Scene * scene);

  /**
   * @brief Moves @p scene to @p toIndex (0 to the scene count - 1).
   */
  Q_INVOKABLE void moveScene (structure::scenes::Scene * scene, int toIndex);

  Q_INVOKABLE void
  renameScene (structure::scenes::Scene * scene, const QString &name);

  /**
   * @brief Sets the color of @p scene; an invalid color unsets it.
   */
  Q_INVOKABLE void
  setSceneColor (structure::scenes::Scene * scene, const QColor &color);

  /**
   * @brief Removes the clip from @p clipSlot of @p track (undoable),
   * stopping its playback immediately.
   */
  Q_INVOKABLE void deleteClip (
    structure::tracks::Track *    track,
    structure::scenes::ClipSlot * clipSlot);

  /**
   * @brief Sets the clip launcher's default launch quantization (undoable).
   */
  Q_INVOKABLE void
  setLaunchQuantization (structure::tracks::ClipQuantizeOption quantization);

  /**
   * @brief Removes the color of @p scene.
   */
  Q_INVOKABLE void unsetSceneColor (structure::scenes::Scene * scene)
  {
    setSceneColor (scene, QColor ());
  }

private:
  /**
   * @brief Index of @p scene in the scene list, or nullopt (with a warning)
   * if the operation cannot be performed.
   */
  std::optional<int>
  checked_scene_index (const structure::scenes::Scene * scene);

  /**
   * @brief Whether the clip launcher and the undo stack are set; logs a
   * warning otherwise.
   */
  bool can_operate () const;

  QPointer<structure::scenes::ClipLauncher>        clip_launcher_;
  QPointer<structure::scenes::ClipPlaybackService> clip_playback_service_;
  QPointer<undo::UndoStack>                        undo_stack_;
};

} // namespace zrythm::actions
