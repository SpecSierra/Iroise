/*
 * Iroise: a grid with cells sized like the launcher's, for the favourites. Silica's own (IconGridViewBase) is
 * private, which the Jolla Store does not allow importing.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import QtQuick 2.4
import Sailfish.Silica 1.0

SilicaGridView {
    property int pageHeight: height
    property int horizontalMargin: Theme.paddingLarge + Theme.paddingSmall
    property real minimumCellWidth: Theme.iconSizeLauncher + Theme.paddingLarge + Theme.paddingMedium
    // Room for the icon, its title and the paddings around them.
    property real minimumCellHeight: minimumCellWidth + labelMetrics.height + Theme.paddingSmall
    property int rows: Math.max(1, Math.floor(pageHeight / minimumCellHeight))
    property int columns: Math.max(1, Math.floor(parent.width / minimumCellWidth))

    readonly property int initialCellWidth: (parent.width - 2 * horizontalMargin) / columns

    // The spare width goes between the icons, not around the grid.
    cellWidth: columns > 1 ? Math.floor(initialCellWidth + (initialCellWidth - Theme.iconSizeLauncher) / (columns - 1))
                           : initialCellWidth
    cellHeight: Math.round(pageHeight / rows)
    width: cellWidth * columns
    anchors.horizontalCenter: parent.horizontalCenter

    FontMetrics {
        id: labelMetrics
        font.pixelSize: Theme.fontSizeTiny
    }
}
