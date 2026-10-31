// SPDX-FileCopyrightText: © 2025 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import Zrythm

Control {
  id: root

  required property ClipPlaybackService clipPlaybackService
  required property ClipSlot clipSlot
  // Quantization of the slot's launch/stop button (a ClipQuantizeOption)
  required property int launchQuantization
  required property ArrangerObjectCreator objectCreator
  readonly property ArrangerObject clipObject: clipSlot.clipObject
  required property Scene scene
  required property Track track

  signal filesDropped(var filePaths)
  signal slotLaunched

  hoverEnabled: true

  Rectangle {
    anchors.fill: parent
    clip: true
    color: {
      let c = root.clipSlot.clipObject
        ? TrackColors.effectiveColor(root.track, ZrythmTheme.darkMode)
        : palette.button;
      return ZrythmTheme.adjustColorForHoverOrVisualFocusOrDown(c, root.hovered || dropArea.containsDrag, root.activeFocus, tapHandler.pressed);
    }
    radius: ZrythmTheme.textFieldRadius

    Loader {
      active: root.clipSlot.clipObject !== null && root.clipSlot.clipObject.type === ArrangerObject.MidiClip
      anchors.fill: parent
      visible: active

      sourceComponent: MidiClipContent {
        contentHeight: parent.height
        contentWidth: parent.width
        midiClip: root.clipSlot.clipObject as MidiClip
      }
    }

    Loader {
      active: root.clipSlot.clipObject !== null && root.clipSlot.clipObject.type === ArrangerObject.AudioClip
      anchors.fill: parent
      visible: active

      sourceComponent: AudioClipContent {
        contentHeight: parent.height
        contentWidth: parent.width
        audioClip: root.clipSlot.clipObject as AudioClip
      }
    }

    ClipLaunchButton {
      id: playButton

      clipPlaybackService: root.clipPlaybackService
      clipSlot: root.clipSlot
      enabled: root.clipObject !== null
      launchQuantization: root.launchQuantization
      track: root.track

      anchors {
        left: parent.left
        top: parent.top
      }
    }

    // Playback position indicator
    Rectangle {
      id: playbackPositionIndicator

      color: palette.text
      opacity: 0.6
      visible: root.clipSlot.state === ClipSlot.Playing || root.clipSlot.state === ClipSlot.PlayQueued
      width: 2

      anchors {
        bottom: parent.bottom
        top: parent.top
      }

      // Update position based on playback progress
      Timer {
        interval: 16 // ~60fps
        repeat: true
        running: playbackPositionIndicator.visible

        onTriggered: {
          let position = root.clipPlaybackService.getClipPlaybackPosition(root.clipSlot);
          if (position >= 0) {
            playbackPositionIndicator.x = position * root.width;
          }
        }
      }
    }

    TapHandler {
      id: tapHandler

      onDoubleTapped: {
        if (root.clipSlot.clipObject === null) {
          root.objectCreator.addEmptyMidiClipToClip(root.track, root.clipSlot);
        }
      }
    }

    DropArea {
      id: dropArea

      // Marks this as a drop target for clips dragged out of an arranger
      readonly property bool acceptsArrangerClips: true

      // The clip dragged from an arranger, or null for other drags (files)
      function draggedClip(source: QtObject): ArrangerObject {
        const clipDragItem = source as ArrangerClipDragItem;
        return clipDragItem !== null ? clipDragItem.arrangerClip : null;
      }

      anchors.fill: parent

      onDropped: drop => {
        const clip = dropArea.draggedClip(drop.source);
        if (clip !== null) {
          root.objectCreator.copyClipToClipSlot(clip, root.track, root.clipSlot);
          drop.accept(Qt.CopyAction);
          return;
        }
        let uniqueFilePaths = DragUtils.getUniqueFilePaths(drop);
        root.filesDropped(uniqueFilePaths);
      }
      onEntered: drag => {
        // Clips that do not fit this track are not accepted
        const clip = dropArea.draggedClip(drag.source);
        if (clip !== null && !root.objectCreator.canCopyClipToClipSlot(clip, root.track))
          drag.accepted = false;
      }
    }
  }
}
