// SPDX-FileCopyrightText: © 2025 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#pragma once

#include "undo/undo_stack.h"

#include <QPointer>
#include <QtQmlIntegration/qqmlintegration.h>

namespace zrythm::actions
{
class QObjectPropertyOperator : public QObject
{
  Q_OBJECT
  Q_PROPERTY (
    QObject * currentObject READ currentObject WRITE setCurrentObject NOTIFY
      currentObjectChanged)
  Q_PROPERTY (
    zrythm::undo::UndoStack * undoStack READ undoStack WRITE setUndoStack NOTIFY
      undoStackChanged)
  QML_ELEMENT

public:
  explicit QObjectPropertyOperator (QObject * parent = nullptr)
      : QObject (parent)
  {
  }

  Q_SIGNAL void currentObjectChanged ();
  Q_SIGNAL void undoStackChanged ();

  /**
   * @brief The object whose properties are changed.
   *
   * May be null (e.g. while a QML binding's source is destroyed), and
   * becomes null when the object is destroyed.
   */
  QObject * currentObject () const { return current_object_; }
  void      setCurrentObject (QObject * object)
  {
    if (current_object_ != object)
      {
        current_object_ = object;
        Q_EMIT currentObjectChanged ();
      }
  }

  /**
   * @brief The undo stack the changes are pushed to.
   *
   * May be null; becomes null when the undo stack is destroyed.
   */
  undo::UndoStack * undoStack () const { return undo_stack_; }
  void              setUndoStack (undo::UndoStack * undoStack)
  {
    if (undo_stack_ != undoStack)
      {
        undo_stack_ = undoStack;
        Q_EMIT undoStackChanged ();
      }
  }

  /**
   * @brief Sets a property of the current object (undoable).
   *
   * Refused with a warning if there is no current object or undo stack.
   */
  Q_INVOKABLE void setValue (QString propertyName, QVariant value);

  /**
   * @brief Set a value that affects the tempo map.
   *
   * The audio engine will be stopped while changes to this property are made.
   * Refused with a warning if there is no current object or undo stack.
   */
  Q_INVOKABLE void
  setValueAffectingTempoMap (QString propertyName, QVariant value);

private:
  /**
   * @brief Whether both the current object and the undo stack are set;
   * logs a warning otherwise.
   */
  bool can_change_value () const;

  QPointer<QObject>         current_object_;
  QPointer<undo::UndoStack> undo_stack_;
};
}
