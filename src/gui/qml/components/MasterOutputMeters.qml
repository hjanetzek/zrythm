// SPDX-FileCopyrightText: © 2024-2026 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Zrythm
import ZrythmStyle

// Spectrum and waveform of the master output, and the DSP load and xrun count
// of the engine
RowLayout {
  id: root

  required property Project project

  SpectrumAnalyzer {
    Layout.preferredWidth: 60
    fftSize: 512
    portObservationManager: root.project.portObservationManager
    sampleRate: root.project.engine.sampleRate
    stereoPort: root.project.tracklist.singletonTracks.masterTrack.channel.audioOutPort

    ToolTip {
      text: qsTr("Master Output Spectrum")
    }
  }

  WaveformViewer {
    Layout.preferredWidth: 60
    portObservationManager: root.project.portObservationManager
    stereoPort: root.project.tracklist.singletonTracks.masterTrack.channel.audioOutPort

    ToolTip {
      text: qsTr("Master Output Visualizer")
    }
  }

  DspLoadIndicator {
    id: dspLoadIndicator
  }

  Timer {
    interval: 600
    repeat: true
    running: root.visible

    onTriggered: {
      dspLoadIndicator.xRuns = root.project.engine.xRunCount();
      dspLoadIndicator.loadValue = root.project.engine.loadPercentage();
    }
  }
}
