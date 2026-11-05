// SPDX-FileCopyrightText: © 2025 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Zrythm
import ZrythmStyle

ColumnLayout {
  id: root

  required property FileSystemModel fileSystemModel

  signal fileActivated(string filePath)

  // Shared drag source for dragging files out of the browser: owned by the
  // page so it survives the destruction of the file delegate being dragged
  AutoDragItem {
    id: fileDragItem

    property string filePath: ""

    Drag.keys: ["file", "text/plain"]
    Drag.supportedActions: Qt.CopyAction
    Drag.mimeData: fileDragItem.filePath !== "" ? {
      "text/plain": fileDragItem.filePath,
      "application/x-file-path": fileDragItem.filePath
    } : ({})
  }

  Item {
    id: filters

  }

  Item {
    id: searchBar

  }

  TreeView {
    id: fileListView

    property var selectedFile

    Layout.fillHeight: true
    Layout.fillWidth: true
    activeFocusOnTab: true
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    focus: true
    model: root.fileSystemModel
    rootIndex: root.fileSystemModel.rootIndex

    ScrollBar.vertical: ScrollBar {
    }
    delegate: TreeViewDelegate {
      id: itemDelegate

      required property int column
      required property var fileInfo
      required property string fileName
      required property string filePath
      required property int index

      // Starts or ends dragging this row's file out of the tree
      function setFileDragActive(active: bool) {
        if (active) {
          fileListView.selectedFile = itemDelegate.filePath;
          fileDragItem.filePath = itemDelegate.filePath;
          fileDragItem.Drag.active = true;
        } else {
          fileDragItem.Drag.active = false;
        }
      }

      highlighted: row == treeView.currentRow
      text: fileName
      width: fileListView.width

      TapHandler {
        onDoubleTapped: {
          if (!root.fileSystemModel.isDir(itemDelegate.fileInfo))
            root.fileActivated(itemDelegate.filePath);
        }
        onTapped: fileListView.selectedFile = itemDelegate.filePath
      }

      DragHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
        target: null

        onActiveChanged: itemDelegate.setFileDragActive(active)
      }

      // On touchscreens only a horizontal swipe drags the file out; vertical
      // swipes scroll the tree
      DragHandler {
        acceptedDevices: PointerDevice.TouchScreen
        target: null
        yAxis.enabled: false

        onActiveChanged: itemDelegate.setFileDragActive(active)
      }
    }
    selectionModel: ItemSelectionModel {
      model: fileListView.model
    }
  }

  Label {
    id: pluginInfoLabel

    Layout.fillWidth: true
    text: root.fileSystemModel.getFileInfoAsString(fileListView.selectedFile)
  }
}
