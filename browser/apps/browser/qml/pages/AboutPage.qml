/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.6
import Sailfish.Silica 1.0
import "../shared" as Shared

// What Iroise is, and what it is not: it is built on the Ladybird engine but is
// an independent port, and says so as Ladybird's trademark policy asks.
Page {
    id: page

    Shared.Background {
        anchors.fill: parent
        z: -1
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingLarge
            bottomPadding: Theme.paddingLarge

            PageHeader {
                //% "About Iroise"
                title: qsTrId("iroise-he-about")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.highlightColor
                font.pixelSize: Theme.fontSizeSmall
                //% "A web browser for Sailfish OS, based on the Ladybird engine."
                text: qsTrId("iroise-la-about_based_on")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeSmall
                //% "Iroise is an independent port. It is not an official Ladybird release, and it is not endorsed by the Ladybird Browser Initiative."
                text: qsTrId("iroise-la-about_not_official")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                //% "Ladybird is a trademark of its owners. Iroise uses the name only to say what it is built on, under Ladybird's trademark policy at ladybird.org/trademark."
                text: qsTrId("iroise-la-about_trademark")
            }
        }
    }
}
