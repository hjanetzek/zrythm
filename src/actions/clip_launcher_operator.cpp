// SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#include "actions/clip_launcher_operator.h"
#include "commands/change_qobject_property_command.h"
#include "commands/scene_commands.h"
#include "utils/logger.h"

namespace zrythm::actions
{

bool
ClipLauncherOperator::can_operate () const
{
  if (clip_launcher_ == nullptr || undo_stack_ == nullptr)
    {
      z_warning ("Scene operation refused: no clip launcher or undo stack");
      return false;
    }
  return true;
}

std::optional<int>
ClipLauncherOperator::checked_scene_index (
  const structure::scenes::Scene * scene)
{
  if (!can_operate ())
    return std::nullopt;

  const auto &scenes = clip_launcher_->scenes ()->scenes ();
  const auto  it = std::ranges::find_if (scenes, [scene] (const auto &s) {
    return s.get () == scene;
  });
  if (scene == nullptr || it == scenes.end ())
    {
      z_warning ("Scene operation refused: scene is not in the clip launcher");
      return std::nullopt;
    }
  return static_cast<int> (std::ranges::distance (scenes.begin (), it));
}

structure::scenes::Scene *
ClipLauncherOperator::addScene ()
{
  if (!can_operate ())
    return nullptr;
  return insertScene (clip_launcher_->scenes ()->rowCount ());
}

structure::scenes::Scene *
ClipLauncherOperator::insertScene (int index)
{
  if (!can_operate ())
    return nullptr;
  auto * scene_list = clip_launcher_->scenes ();
  if (index < 0 || index > scene_list->rowCount ())
    {
      z_warning ("Scene insertion refused: index {} out of range", index);
      return nullptr;
    }

  auto   scene = clip_launcher_->create_scene ();
  auto * scene_ptr = scene.get ();
  undo_stack_->push (
    std::make_unique<commands::InsertSceneCommand> (
      *scene_list, std::move (scene), index));
  return scene_ptr;
}

void
ClipLauncherOperator::removeScene (structure::scenes::Scene * scene)
{
  const auto index = checked_scene_index (scene);
  if (!index.has_value ())
    return;

  // Clips of a removed scene no longer play
  if (clip_playback_service_ != nullptr)
    {
      clip_playback_service_->stopScene (
        scene, structure::tracks::ClipQuantizeOption::Immediate);
    }

  undo_stack_->push (
    std::make_unique<commands::RemoveSceneCommand> (
      *clip_launcher_->scenes (), *index));
}

void
ClipLauncherOperator::moveScene (structure::scenes::Scene * scene, int toIndex)
{
  const auto index = checked_scene_index (scene);
  if (!index.has_value ())
    return;
  if (toIndex < 0 || toIndex >= clip_launcher_->scenes ()->rowCount ())
    {
      z_warning ("Scene move refused: index {} out of range", toIndex);
      return;
    }
  if (toIndex == *index)
    return;

  undo_stack_->push (
    std::make_unique<commands::MoveSceneCommand> (
      *clip_launcher_->scenes (), *index, toIndex));
}

void
ClipLauncherOperator::renameScene (
  structure::scenes::Scene * scene,
  const QString             &name)
{
  if (!checked_scene_index (scene).has_value () || scene->name () == name)
    return;

  undo_stack_->push (
    std::make_unique<commands::ChangeQObjectPropertyCommand> (
      *scene, QStringLiteral ("name"), name,
      commands::Command::EngineRequirement::None, QObject::tr ("Rename Scene")));
}

void
ClipLauncherOperator::setSceneColor (
  structure::scenes::Scene * scene,
  const QColor              &color)
{
  if (!checked_scene_index (scene).has_value () || scene->color () == color)
    return;

  undo_stack_->push (
    std::make_unique<commands::ChangeQObjectPropertyCommand> (
      *scene, QStringLiteral ("color"), color,
      commands::Command::EngineRequirement::None,
      QObject::tr ("Change Scene Color")));
}

} // namespace zrythm::actions
