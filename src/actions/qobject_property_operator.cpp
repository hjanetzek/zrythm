// SPDX-FileCopyrightText: © 2025-2026 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#include "actions/qobject_property_operator.h"
#include "commands/change_qobject_property_command.h"
#include "utils/logger.h"

namespace zrythm::actions
{
bool
QObjectPropertyOperator::can_change_value () const
{
  if (current_object_ == nullptr || undo_stack_ == nullptr)
    {
      z_warning ("Property change refused: no current object or undo stack");
      return false;
    }
  return true;
}

void
QObjectPropertyOperator::setValue (QString propertyName, QVariant value)
{
  if (!can_change_value ())
    return;

  auto cmd = std::make_unique<commands::ChangeQObjectPropertyCommand> (
    *current_object_, propertyName, value,
    commands::Command::EngineRequirement::None);
  undo_stack_->push (std::move (cmd));
}

void
QObjectPropertyOperator::setValueAffectingTempoMap (
  QString  propertyName,
  QVariant value)
{
  if (!can_change_value ())
    return;

  auto cmd = std::make_unique<commands::ChangeQObjectPropertyCommand> (
    *current_object_, propertyName, value,
    commands::Command::EngineRequirement::PauseProcessing);
  undo_stack_->push (std::move (cmd));
}
}
