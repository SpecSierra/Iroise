/*
 * Iroise: a loader whose old item stays until it has animated out, as PopUpMenu used Silica's private
 * AnimatedLoader (which the Jolla Store does not allow importing).
 *
 * When active or source changes, the current item becomes replacedItem, a new item is made from source (a
 * Component or a URL) if active, initializeItem() and then animate() are emitted. A handler of animate() that
 * starts an animation keeps `animating` true while it runs; replacedItem is destroyed once nothing animates.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import QtQuick 2.2

Item {
    id: loader

    property bool active: true
    property var source
    property bool animating

    readonly property alias item: loader._item
    readonly property alias replacedItem: loader._replacedItem

    property Item _item
    property Item _replacedItem

    signal initializeItem()
    signal animate()

    function _reload() {
        if (_replacedItem)
            _replacedItem.destroy()
        _replacedItem = _item
        _item = null

        if (active && source) {
            var component = source.createObject !== undefined ? source : Qt.createComponent(source)
            if (component.status === Component.Ready) {
                _item = component.createObject(loader)
                if (_item)
                    initializeItem()
            } else {
                console.warn("AnimatedLoader: cannot load " + source + ": " + component.errorString())
            }
        }

        animate()
        _dropReplacedItem()
    }

    function _dropReplacedItem() {
        if (!animating && _replacedItem) {
            _replacedItem.destroy()
            _replacedItem = null
        }
    }

    onActiveChanged: _reload()
    onSourceChanged: _reload()
    onAnimatingChanged: _dropReplacedItem()
    Component.onCompleted: if (active && source && !_item) _reload()
}
