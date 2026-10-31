// SPDX-FileCopyrightText: © 2025-2026 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQml.Models
import Zrythm

import Qt.labs.synchronizer

// Note: ColumnLayout wasn't working correctly so we're using Item for now
Item {
  id: root

  required property ClipEditor clipEditor
  required property ClipLauncher clipLauncher
  required property ClipPlaybackService clipPlaybackService
  required property ClipSlotRecorder clipSlotRecorder
  readonly property EditorSettings editorSettings: timeline
  required property FileImporter fileImporter

  // Synchronize horizontal scrolling
  property real horizontalScrollPosition: 0
  required property ArrangerObjectCreator objectCreator
  readonly property url playIconSource: ResourceManager.getIconUrl("gnome-icon-library", "media-playback-start-symbolic.svg")
  property int rulerHeight: 24
  property int sceneWidth: 80
  readonly property url stopIconSource: ResourceManager.getIconUrl("gnome-icon-library", "media-playback-stop-symbolic.svg")
  readonly property url switchToTimelineIconSource: ResourceManager.getIconUrl("gnome-icon-library", "ruler-angled-symbolic.svg")
  required property var timeline
  required property TrackCollection trackCollection
  required property Tracklist tracklist
  required property UndoStack undoStack

  ClipLauncherOperator {
    id: sceneOperator

    clipLauncher: root.clipLauncher
    clipPlaybackService: root.clipPlaybackService
    undoStack: root.undoStack
  }

  // Scene the scene menu, rename dialog and color picker act on
  property Scene editedScene: null

  Menu {
    id: sceneMenu

    MenuItem {
      text: qsTr("Rename Scene…")

      onTriggered: {
        sceneNameField.text = root.editedScene.name;
        sceneRenameDialog.open();
      }
    }

    MenuItem {
      text: qsTr("Scene Color…")

      onTriggered: sceneColorPicker.open()
    }

    MenuSeparator {
    }

    MenuItem {
      text: qsTr("Insert Scene Before")

      onTriggered: sceneOperator.insertScene(root.sceneIndex(root.editedScene))
    }

    MenuItem {
      enabled: root.sceneIndex(root.editedScene) > 0
      text: qsTr("Move Scene Left")

      onTriggered: sceneOperator.moveScene(root.editedScene, root.sceneIndex(root.editedScene) - 1)
    }

    MenuItem {
      enabled: root.sceneIndex(root.editedScene) < root.clipLauncher.scenes.rowCount() - 1
      text: qsTr("Move Scene Right")

      onTriggered: sceneOperator.moveScene(root.editedScene, root.sceneIndex(root.editedScene) + 1)
    }

    MenuSeparator {
    }

    MenuItem {
      text: qsTr("Stop Scene")

      onTriggered: root.clipPlaybackService.stopScene(root.editedScene, root.clipLauncher.launchQuantization)
    }

    MenuItem {
      text: qsTr("Delete Scene")

      onTriggered: sceneOperator.removeScene(root.editedScene)
    }
  }

  Dialog {
    id: sceneRenameDialog

    anchors.centerIn: Overlay.overlay
    modal: true
    parent: Overlay.overlay
    standardButtons: Dialog.Ok | Dialog.Cancel
    title: qsTr("Rename Scene")

    onAccepted: sceneOperator.renameScene(root.editedScene, sceneNameField.text)
    onOpened: sceneNameField.selectAll()

    TextField {
      id: sceneNameField

      focus: true
      implicitWidth: 240

      onAccepted: sceneRenameDialog.accept()
    }
  }

  ColorPickerPopover {
    id: sceneColorPicker

    currentColor: root.editedScene !== null && root.editedScene.color.valid ? root.editedScene.color : "transparent"
    defaultColor: root.palette.button
    parent: Overlay.overlay
    useColor: root.editedScene !== null && root.editedScene.color.valid
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2

    onColorSelected: picked => sceneOperator.setSceneColor(root.editedScene, picked)
    onUnsetRequested: () => sceneOperator.unsetSceneColor(root.editedScene)
  }

  // Index of a scene in the clip launcher, or -1
  function sceneIndex(scene: Scene): int {
    return root.clipLauncher.scenes.indexOfScene(scene);
  }

  // Adds a scene and lets @p fill put something into it, as one undo step
  function addSceneWith(macroText: string, fill: var) {
    root.undoStack.beginMacro(macroText);
    const scene = sceneOperator.addScene();
    if (scene !== null)
      fill(scene);
    root.undoStack.endMacro();
  }

  SortFilterProxyModel {
    id: pinnedTrackModel

    sourceModel: root.trackCollection

    filters: [
      FunctionFilter {
        function filter(track: Track): bool {
          return root.tracklist.shouldBeVisible(track) && root.tracklist.isTrackPinned(track);
        }
      }
    ]
  }

  SortFilterProxyModel {
    id: unpinnedTrackModel

    sourceModel: root.trackCollection

    filters: [
      FunctionFilter {
        function filter(track: Track): bool {
          return root.tracklist.shouldBeVisible(track) && !root.tracklist.isTrackPinned(track);
        }
      }
    ]
  }

  Connections {
    function onPinnedTracksCutoffChanged() {
      pinnedTrackModel.invalidate();
      unpinnedTrackModel.invalidate();
    }

    target: root.tracklist
  }

  RowLayout {
    id: header

    anchors.left: parent.left
    anchors.right: parent.right
    anchors.top: parent.top
    height: root.rulerHeight

    ToolButton {
      icon.source: root.stopIconSource

      onClicked: root.clipPlaybackService.stopAllClips(root.clipLauncher.launchQuantization)

      ToolTip {
        text: qsTr("Stop Scenes")
      }
    }

    ComboBox {
      id: launchQuantizationComboBox

      Layout.preferredHeight: root.rulerHeight
      currentIndex: launchQuantizationComboBox.options.findIndex(option => option.value === root.clipLauncher.launchQuantization)
      model: launchQuantizationComboBox.options
      textRole: "text"
      valueRole: "value"

      readonly property var options: [
        {
          text: qsTr("None"),
          value: ClipSlot.ClipQuantizeOption.Immediate
        },
        {
          text: qsTr("1 Bar"),
          value: ClipSlot.ClipQuantizeOption.NextBar
        },
        {
          text: qsTr("1 Beat"),
          value: ClipSlot.ClipQuantizeOption.NextBeat
        },
        {
          text: qsTr("1/4"),
          value: ClipSlot.ClipQuantizeOption.NextQuarterNote
        },
        {
          text: qsTr("1/8"),
          value: ClipSlot.ClipQuantizeOption.NextEighthNote
        },
        {
          text: qsTr("1/16"),
          value: ClipSlot.ClipQuantizeOption.NextSixteenthNote
        },
        {
          text: qsTr("1/32"),
          value: ClipSlot.ClipQuantizeOption.NextThirtySecondNote
        }
      ]

      onActivated: sceneOperator.setLaunchQuantization(launchQuantizationComboBox.currentValue)

      ToolTip {
        text: qsTr("Launch Quantization")
      }
    }

    ComboBox {
      id: recordingLengthComboBox

      Layout.preferredHeight: root.rulerHeight
      currentIndex: recordingLengthComboBox.options.findIndex(option => option.value === root.clipSlotRecorder.fixedLengthBars)
      model: recordingLengthComboBox.options
      textRole: "text"
      valueRole: "value"

      readonly property var options: [
        {
          text: qsTr("Free"),
          value: 0
        },
        {
          text: qsTr("1 Bar"),
          value: 1
        },
        {
          text: qsTr("2 Bars"),
          value: 2
        },
        {
          text: qsTr("4 Bars"),
          value: 4
        },
        {
          text: qsTr("8 Bars"),
          value: 8
        }
      ]

      onActivated: root.clipSlotRecorder.fixedLengthBars = recordingLengthComboBox.currentValue

      ToolTip {
        text: qsTr("Recording Length")
      }
    }

    ScrollView {
      id: sceneHeadersScrollView

      Layout.fillWidth: true

      // Only allow horizontal scrolling
      ScrollBar.horizontal.policy: ScrollBar.AsNeeded
      ScrollBar.vertical.policy: ScrollBar.AlwaysOff

      RowLayout {
        id: sceneHeaderColumns

        Repeater {
          model: root.clipLauncher.scenes

          delegate: Item {
            id: sceneHeaderRow

            required property int index
            required property Scene scene

            Layout.fillWidth: true
            Layout.preferredHeight: root.rulerHeight
            Layout.preferredWidth: root.sceneWidth

            // Scene color strip
            Rectangle {
              anchors.bottom: parent.bottom
              anchors.left: parent.left
              anchors.right: parent.right
              color: sceneHeaderRow.scene.color
              height: 3
              visible: sceneHeaderRow.scene.color.valid
            }

            RowLayout {
              anchors.fill: parent

              ToolButton {
                id: playSceneButton

                icon.source: root.playIconSource

                onClicked: root.clipPlaybackService.launchScene(sceneHeaderRow.scene, root.clipLauncher.launchQuantization)

                ToolTip {
                  text: qsTr("Play Scene")
                }
              }

              Label {
                id: sceneHeaderLabel

                Layout.fillWidth: true
                elide: Text.ElideRight
                text: sceneHeaderRow.scene.name !== "" ? sceneHeaderRow.scene.name : qsTr("Scene %1").arg(sceneHeaderRow.index + 1)
              }
            }

            TapHandler {
              acceptedButtons: Qt.RightButton

              onTapped: {
                root.editedScene = sceneHeaderRow.scene;
                sceneMenu.popup();
              }
            }

            TapHandler {
              onLongPressed: {
                root.editedScene = sceneHeaderRow.scene;
                sceneMenu.popup();
              }
            }
          }
        }

        ToolButton {
          Layout.preferredHeight: root.rulerHeight
          Layout.preferredWidth: root.sceneWidth
          text: "+"

          onClicked: sceneOperator.addScene()

          ToolTip {
            text: qsTr("Add Scene")
          }
        }
      }

      Synchronizer {
        sourceObject: root
        sourceProperty: "horizontalScrollPosition"
        targetObject: sceneHeadersScrollView.contentItem
        targetProperty: "contentX"
      }
    }

    ToolButton {
      icon.source: root.switchToTimelineIconSource

      onClicked: root.clipPlaybackService.setAllTracksToTimelineMode()

      ToolTip {
        text: qsTr("Switch Playback to Timeline")
      }
    }
  }

  RowsForTrackModel {
    id: pinnedTracksSection

    anchors.left: parent.left
    anchors.right: parent.right
    anchors.top: header.bottom
    anchors.topMargin: 2
    // height: childrenRect.height
    model: pinnedTrackModel
  }

  ScrollView {
    id: unpinnedTracksScrollView

    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical.policy: ScrollBar.AlwaysOff
    anchors.bottom: parent.bottom
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.top: pinnedTracksSection.bottom
    anchors.topMargin: 2

    Synchronizer {
      sourceObject: root.editorSettings
      sourceProperty: "y"
      targetObject: unpinnedTracksScrollView.contentItem
      targetProperty: "contentY"
    }

    RowsForTrackModel {
      id: unpinnedTracksSection

      model: unpinnedTrackModel
      width: unpinnedTracksScrollView.width
    }
  }

  component RowsForTrackModel: RowLayout {
    id: rowsForTrackModel

    required property SortFilterProxyModel model

    TrackStopButtonListView {
      Layout.preferredHeight: contentHeight
      Layout.preferredWidth: contentWidth
      model: rowsForTrackModel.model
    }

    ScrollView {
      id: sceneColumnsHorizontalScrollView

      Layout.fillWidth: true
      Layout.preferredHeight: contentHeight // disable vertical scroll

      ScrollBar.horizontal.policy: ScrollBar.AsNeeded
      ScrollBar.vertical.policy: ScrollBar.AlwaysOff

      RowLayout {
        id: sceneColumnsForTrackModel

        Repeater {
          model: root.clipLauncher.scenes

          delegate: TrackSceneClipSlotListView {
            required property int index

            Layout.preferredHeight: contentHeight
            Layout.preferredWidth: root.sceneWidth
            model: rowsForTrackModel.model
          }
        }

        ListView {
          id: phantomSceneClipSlots

          Layout.preferredHeight: contentHeight
          Layout.preferredWidth: root.sceneWidth
          boundsBehavior: Flickable.StopAtBounds
          model: rowsForTrackModel.model

          delegate: Rectangle {
            id: phantomClipSlot

            required property Track track

            color: phantomDropArea.containsDrag ? ZrythmTheme.adjustColorForHoverOrVisualFocusOrDown(palette.button, true, false, false) : palette.button
            height: track.fullVisibleHeight
            radius: ZrythmTheme.textFieldRadius
            width: root.sceneWidth

            DropArea {
              id: phantomDropArea

              // Marks this as a drop target for clips dragged out of an
              // arranger
              readonly property bool acceptsArrangerClips: true

              // The clip dragged from an arranger, or null for other drags (files)
              function draggedClip(source: QtObject): ArrangerObject {
                const clipDragItem = source as ArrangerClipDragItem;
                return clipDragItem !== null ? clipDragItem.arrangerClip : null;
              }

              anchors.fill: parent

              onDropped: drop => {
                const clip = phantomDropArea.draggedClip(drop.source);
                if (clip !== null) {
                  // A new scene receives the clip
                  root.addSceneWith(qsTr("Add Scene With Clip"), scene => {
                    root.objectCreator.copyClipToClipSlot(clip, phantomClipSlot.track, scene.clipSlots.clipSlotForTrack(phantomClipSlot.track));
                  });
                  drop.accept(Qt.CopyAction);
                  return;
                }
                let uniqueFilePaths = DragUtils.getUniqueFilePaths(drop);
                if (uniqueFilePaths.length > 0) {
                  root.addSceneWith(qsTr("Add Scene With Clip"), scene => {
                    root.fileImporter.importFileToClipSlot(uniqueFilePaths[0], phantomClipSlot.track, scene, scene.clipSlots.clipSlotForTrack(phantomClipSlot.track));
                  });
                }
              }
              onEntered: drag => {
                // Clips that do not fit this track are not accepted
                const clip = phantomDropArea.draggedClip(drag.source);
                if (clip !== null && !root.objectCreator.canCopyClipToClipSlot(clip, phantomClipSlot.track))
                  drag.accepted = false;
              }
            }

            TapHandler {
              onDoubleTapped: {
                root.addSceneWith(qsTr("Add Scene With Clip"), scene => {
                  root.objectCreator.addEmptyMidiClipToClip(phantomClipSlot.track, scene.clipSlots.clipSlotForTrack(phantomClipSlot.track));
                });
              }
            }
          }
        }
      }

      Synchronizer {
        sourceObject: root
        sourceProperty: "horizontalScrollPosition"
        targetObject: sceneColumnsHorizontalScrollView.contentItem
        targetProperty: "contentX"
      }
    }

    TrackSwitchPlaybackButtonListView {
      Layout.preferredHeight: contentHeight
      Layout.preferredWidth: contentWidth
      Layout.rightMargin: 4
      model: rowsForTrackModel.model
    }
  }
  component TrackSceneClipSlotListView: ListView {
    id: clipSlotsListView

    required property Scene scene

    boundsBehavior: Flickable.StopAtBounds

    delegate: ClipSlotView {
      id: trackClipSlotForScene

      clipEditor: root.clipEditor
      clipLauncherOperator: sceneOperator
      clipSlotRecorder: root.clipSlotRecorder
      clipPlaybackService: root.clipPlaybackService
      clipSlot: clipSlotsListView.scene.clipSlots.clipSlotForTrack(track)
      height: track.fullVisibleHeight
      launchQuantization: root.clipLauncher.launchQuantization
      objectCreator: root.objectCreator
      scene: clipSlotsListView.scene
      width: root.sceneWidth

      onFilesDropped: files => {
        if (files.length > 0) {
          root.fileImporter.importFileToClipSlot(files[0], track, scene, clipSlot);
        }
      }
    }
  }
  component TrackStopButtonListView: ListView {
    boundsBehavior: Flickable.StopAtBounds
    implicitWidth: 20

    delegate: ToolButton {
      id: trackStopButton

      required property Track track

      icon.source: root.stopIconSource
      implicitHeight: trackStopButton.track.fullVisibleHeight

      onClicked: root.clipPlaybackService.stopTrackClips(trackStopButton.track, root.clipLauncher.launchQuantization)

      ToolTip {
        text: qsTr("Stop %1").arg(trackStopButton.track.name)
      }
    }
  }
  component TrackSwitchPlaybackButtonListView: ListView {
    boundsBehavior: Flickable.StopAtBounds
    implicitWidth: 20

    delegate: ToolButton {
      id: trackSwitchPlaybackButton

      required property Track track

      enabled: track.clipLauncherMode
      icon.source: root.switchToTimelineIconSource
      implicitHeight: track.fullVisibleHeight

      onClicked: root.clipPlaybackService.setTrackToTimelineMode(track)

      ToolTip {
        text: qsTr("Switch Playback to Timeline")
      }
    }
  }
}
