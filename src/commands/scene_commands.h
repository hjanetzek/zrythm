// SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#pragma once

#include "commands/command.h"
#include "structure/scenes/scene.h"
#include "utils/qt.h"

namespace zrythm::commands
{

/**
 * @brief Inserts a scene into a scene list.
 *
 * While undone, the command owns the scene (with its clip slots and clips).
 */
class InsertSceneCommand : public Command
{
public:
  InsertSceneCommand (
    structure::scenes::SceneList                     &scene_list,
    utils::QObjectUniquePtr<structure::scenes::Scene> scene,
    int                                               index)
      : Command (QObject::tr ("Add Scene"), EngineRequirement::None),
        scene_list_ (scene_list), scene_ (std::move (scene)), index_ (index)
  {
  }

  void redo () override
  {
    scene_list_.insert_scene (std::move (scene_), index_);
  }
  void undo () override { scene_ = scene_list_.take_scene (index_); }

private:
  structure::scenes::SceneList                     &scene_list_;
  utils::QObjectUniquePtr<structure::scenes::Scene> scene_;
  int                                               index_;
};

/**
 * @brief Removes a scene from a scene list.
 *
 * While done, the command owns the removed scene (with its clip slots and
 * clips), so undo restores it unchanged.
 */
class RemoveSceneCommand : public Command
{
public:
  RemoveSceneCommand (structure::scenes::SceneList &scene_list, int index)
      : Command (QObject::tr ("Delete Scene"), EngineRequirement::None),
        scene_list_ (scene_list), index_ (index)
  {
  }

  void redo () override { scene_ = scene_list_.take_scene (index_); }
  void undo () override
  {
    scene_list_.insert_scene (std::move (scene_), index_);
  }

private:
  structure::scenes::SceneList                     &scene_list_;
  utils::QObjectUniquePtr<structure::scenes::Scene> scene_;
  int                                               index_;
};

/**
 * @brief Moves a scene to another position in a scene list.
 */
class MoveSceneCommand : public Command
{
public:
  MoveSceneCommand (
    structure::scenes::SceneList &scene_list,
    int                           from_index,
    int                           to_index)
      : Command (QObject::tr ("Move Scene"), EngineRequirement::None),
        scene_list_ (scene_list), from_index_ (from_index), to_index_ (to_index)
  {
  }

  void redo () override { scene_list_.moveScene (from_index_, to_index_); }
  void undo () override { scene_list_.moveScene (to_index_, from_index_); }

private:
  structure::scenes::SceneList &scene_list_;
  int                           from_index_;
  int                           to_index_;
};

} // namespace zrythm::commands
