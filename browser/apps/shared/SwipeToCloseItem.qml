/*
 * Pacific: a list or grid item that can be swiped away, as the tab cards used Silica's private SwipeItem (which the
 * Jolla Store does not allow importing). Its contentItem follows a sideways drag; let go past a third of the item's
 * width it slides out and swipedAway() is emitted, otherwise it springs back.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import QtQuick 2.2
import Sailfish.Silica 1.0

BackgroundItem {
    id: root

    signal swipedAway
    property real swipeDistance: width

    highlighted: down && !drag.active

    onPressed: drag.target = contentItem
    drag {
        axis: Drag.XAxis
        minimumX: -swipeDistance
        maximumX: swipeDistance
        onActiveChanged: {
            if (drag.active)
                return
            slide.away = Math.abs(contentItem.x) > width / 3
            slide.to = !slide.away ? 0 : contentItem.x < 0 ? -swipeDistance : swipeDistance
            slide.restart()
        }
    }

    NumberAnimation {
        id: slide

        property bool away

        target: root.contentItem
        property: "x"
        duration: 200
        easing.type: Easing.InOutQuad
        onStopped: if (away) root.swipedAway()
    }
}
