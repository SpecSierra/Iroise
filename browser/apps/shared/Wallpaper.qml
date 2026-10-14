/*
 * Pacific: the ambience's wallpaper, for the frosted surfaces to blur. In the Jolla Store's sandbox it cannot be
 * read (only the Ambience permission opens it, and a store app may not ask for that), and this stays transparent:
 * the compositor draws the same wallpaper behind the window there (Silica's ApplicationWindow asks it to), which the
 * surfaces' tint then lies over.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import QtQuick 2.2
import Sailfish.Silica 1.0

Image {
    source: Theme.backgroundImage
    fillMode: Image.PreserveAspectCrop
    cache: true
}
