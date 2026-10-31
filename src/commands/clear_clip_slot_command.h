// SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#pragma once

#include "commands/command.h"
#include "structure/scenes/clip_slot.h"

namespace zrythm::commands
{

/**
 * @brief Removes the clip from a clip slot.
 *
 * The command keeps a reference to the removed clip, so undo puts the same
 * clip back.
 */
class ClearClipSlotCommand : public Command
{
public:
  explicit ClearClipSlotCommand (
    structure::scenes::ClipSlotUuidReference slot_ref)
      : Command (QObject::tr ("Delete Clip"), EngineRequirement::None),
        slot_ref_ (std::move (slot_ref))
  {
  }

  void redo () override
  {
    auto * slot = slot_ref_.get ();
    removed_clip_ = slot->clipReference ();
    slot->clearClip ();
  }

  void undo () override
  {
    if (removed_clip_.has_value ())
      {
        slot_ref_.get ()->setClip (
          removed_clip_->get_object_as<structure::arrangement::Clip> ());
      }
  }

private:
  structure::scenes::ClipSlotUuidReference slot_ref_;
  std::optional<structure::arrangement::ArrangerObjectUuidReference>
    removed_clip_;
};

} // namespace zrythm::commands
