// SPDX-FileCopyrightText: © 2024-2026 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

import QtQuick
import QtQuick.Controls
import Zrythm
import ZrythmStyle
import Qt.labs.synchronizer

ZrythmToolBar {
  id: root

  required property AppSettings appSettings
  required property ControlRoom controlRoom
  readonly property Project project: session.project
  required property NotificationCenter notificationCenter
  required property ProjectSession session
  // Whether to leave out the transport readouts and the master output meters
  property bool compact: false

  centerItems: [
    TransportControls {
      id: transportControls

      appSettings: root.appSettings
      metronome: root.controlRoom.metronome
      transport: root.project.transport
      transportController: root.session.transportController
    },
    TransportReadouts {
      tempoMap: root.project.tempoMap
      transport: root.project.transport
      undoStack: root.session.undoStack
      visible: !root.compact
    }
  ]
  leftItems: [
    UndoSplitButton {
      id: undoBtn

      isUndo: true
      undoStack: root.session.undoStack
    },
    UndoSplitButton {
      id: redoBtn

      isUndo: false
      undoStack: root.session.undoStack
    },
    ToolSeparator {
    },
    ToolBox {
      id: toolBox

      tool: root.session.uiState.tool
    },
    SnapGridButton {
      snapGrid: root.session.uiState.snapGridTimeline
    }
  ]
  rightItems: [
    MasterOutputMeters {
      project: root.project
      visible: !root.compact
    },
    ToolSeparator {
      visible: !root.compact
    },
    ToolButton {
      id: toggleLeftDock

      checkable: true
      icon.source: ResourceManager.getIconUrl("gnome-icon-library", "dock-left-symbolic.svg")

      Synchronizer on checked {
        sourceObject: root.appSettings
        sourceProperty: "leftPanelVisible"
      }

      ToolTip {
        text: qsTr("Toggle Left Panel")
      }
    },
    ToolButton {
      id: toggleBottomDock

      checkable: true
      icon.source: ResourceManager.getIconUrl("gnome-icon-library", "dock-bottom-symbolic.svg")

      Synchronizer on checked {
        sourceObject: root.appSettings
        sourceProperty: "bottomPanelVisible"
      }

      ToolTip {
        text: qsTr("Toggle Bottom Panel")
      }
    },
    ToolButton {
      id: toggleRightDock

      checkable: true
      icon.source: ResourceManager.getIconUrl("gnome-icon-library", "dock-right-symbolic.svg")

      Synchronizer on checked {
        sourceObject: root.appSettings
        sourceProperty: "rightPanelVisible"
      }

      ToolTip {
        text: qsTr("Toggle Right Panel")
      }
    },
    NotificationCenterButton {
      notificationCenter: root.notificationCenter
    }
  ]
}
