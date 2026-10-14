import QtQuick 2.2
import Sailfish.Silica 1.0

// The band of the screen the camera notch is in, black so the notch disappears into it. For the browser page, which
// draws to the screen's edges (CutoutMode.FullScreen); Silica keeps other pages out of the band itself, and the tab
// overview only keeps its header below it. A child of the page, so the page stack's own decorations stay above it. The notch is at the top of the physical
// portrait screen, on a different edge of the rotated page per orientation.
Rectangle {
    property Item page: parent
    readonly property int orientation: page ? page.orientation : Orientation.Portrait
    readonly property bool landscape: orientation === Orientation.Landscape || orientation === Orientation.LandscapeInverted
    readonly property real band: Screen.hasCutouts ? Screen.topCutout.height : 0

    z: 1000
    visible: band > 0
    color: "black"
    x: orientation === Orientation.LandscapeInverted ? page.width - band : 0
    y: orientation === Orientation.PortraitInverted ? page.height - band : 0
    width: landscape ? band : page.width
    height: landscape ? page.height : band
}
