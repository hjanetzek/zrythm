// SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

import QtQuick
import Zrythm

// Carries a clip dragged out of an arranger to other views. Drop targets
// receive it as the drag source and read arrangerClip.
Item {
  id: root

  // The dragged clip; the drag is active while it is set
  property ArrangerObject arrangerClip: null

  Drag.active: root.arrangerClip !== null
  Drag.dragType: Drag.Internal
  Drag.keys: ["application/x-zrythm-arranger-clip"]
  height: 1
  width: 1
}
