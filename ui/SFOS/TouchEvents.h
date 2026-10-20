/*
 * Pacific: touch events for pages, which the engine does not have.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

// Pages written for phones listen for touchstart, touchmove and touchend, and many cancel touchend to keep the tap
// from also being a click (a video player's control bar, which would otherwise hide itself). The engine knows only
// a mouse, so this script, run in every document before the document's own (patch 0018), gives pages touch events:
// the front end presses the mouse in a way no real mouse does (the Forward button held as well, see
// PresentedWebView::send_carrier), this script hides that press from the page and fires the touch event in its
// place. What the page made of it goes back to the front end in a console message, which then sends the page the
// mouse events of a tap, or scrolls, unless the page cancelled the touch.
//
// NB: The press being a real one is what lets a page enter fullscreen or start a video from touchend: the engine
//     counts it as the user's activation.
namespace Pacific {

static constexpr auto touch_message_marker = "\001pacific-touch"sv;
static constexpr unsigned touch_carrier_button = 16; // Forward
static constexpr unsigned touch_flag_button = 8;     // Backward: a touchmove that cannot be cancelled, or touchcancel

static constexpr auto touch_events_script = R"js((() => {
    "use strict";
    if (window.__pacificTouchEvents)
        return;
    Object.defineProperty(window, "__pacificTouchEvents", { value: true });

    const marker = "\u0001pacific-touch";
    const carrier = 16;
    const flag = 8;
    const log = console.log.bind(console);
    const add = EventTarget.prototype.addEventListener;
    const types = ["touchstart", "touchmove", "touchend", "touchcancel"];

    // The front end presses the mouse for touch events only once a page listens for them.
    let listening = false;
    const listen = () => {
        if (!listening) {
            listening = true;
            log(marker, "listening");
        }
    };
    const relisten = () => {
        if (listening)
            log(marker, "listening");
    };
    add.call(window, "DOMContentLoaded", relisten);
    add.call(window, "pageshow", relisten);

    Object.defineProperty(EventTarget.prototype, "addEventListener", {
        configurable: true,
        writable: true,
        value: function addEventListener(type, listener, options) {
            if (listener && types.includes(String(type)))
                listen();
            return add.call(this, type, listener, options);
        },
    });

    const handlers = new WeakMap();
    for (const proto of [Window.prototype, Document.prototype, HTMLElement.prototype, SVGElement.prototype]) {
        for (const type of types) {
            const name = "on" + type;
            if (name in proto)
                continue;
            Object.defineProperty(proto, name, {
                configurable: true,
                enumerable: true,
                get() {
                    const own = handlers.get(this);
                    return (own && own[name]) || null;
                },
                set(value) {
                    let own = handlers.get(this);
                    if (!own) {
                        own = {};
                        handlers.set(this, own);
                    }
                    if (!(name in own)) {
                        add.call(this, type, function (event) {
                            const handler = own[name];
                            if (typeof handler === "function" && handler.call(this, event) === false)
                                event.preventDefault();
                        });
                    }
                    own[name] = typeof value === "function" ? value : null;
                    if (own[name])
                        listen();
                },
            });
        }
    }

    class Touch {
        constructor(init) {
            this.identifier = init.identifier;
            this.target = init.target;
            this.screenX = init.screenX || 0;
            this.screenY = init.screenY || 0;
            this.clientX = init.clientX || 0;
            this.clientY = init.clientY || 0;
            this.pageX = init.pageX || 0;
            this.pageY = init.pageY || 0;
            this.radiusX = init.radiusX || 0;
            this.radiusY = init.radiusY || 0;
            this.rotationAngle = init.rotationAngle || 0;
            this.force = init.force || 0;
        }
    }
    class TouchList {
        constructor(touches) {
            const list = Array.from(touches || []);
            Object.defineProperty(this, "_list", { value: list });
            list.forEach((touch, index) => { this[index] = touch; });
        }
        get length() { return this._list.length; }
        item(index) { return this._list[index] || null; }
        [Symbol.iterator]() { return this._list[Symbol.iterator](); }
    }
    class TouchEvent extends UIEvent {
        constructor(type, init = {}) {
            super(type, init);
            const list = touches => touches instanceof TouchList ? touches : new TouchList(touches);
            Object.defineProperties(this, {
                touches: { value: list(init.touches), enumerable: true },
                targetTouches: { value: list(init.targetTouches), enumerable: true },
                changedTouches: { value: list(init.changedTouches), enumerable: true },
                altKey: { value: !!init.altKey, enumerable: true },
                metaKey: { value: !!init.metaKey, enumerable: true },
                ctrlKey: { value: !!init.ctrlKey, enumerable: true },
                shiftKey: { value: !!init.shiftKey, enumerable: true },
            });
        }
    }
    for (const [name, value] of [["Touch", Touch], ["TouchList", TouchList], ["TouchEvent", TouchEvent]]) {
        if (!(name in window))
            Object.defineProperty(window, name, { configurable: true, writable: true, value });
    }

    // The touch under way: its target stays the element it began on, as in a real touch.
    let active = null;
    let nextIdentifier = 0;
    const fire = (type, cancelable, from) => {
        const touch = new Touch({
            identifier: active.identifier, target: active.target,
            screenX: from.screenX, screenY: from.screenY, clientX: from.clientX, clientY: from.clientY,
            pageX: from.pageX, pageY: from.pageY, radiusX: 12, radiusY: 12, force: 1,
        });
        const ended = type === "touchend" || type === "touchcancel";
        const event = new TouchEvent(type, {
            bubbles: true, cancelable, composed: true, view: window,
            touches: ended ? [] : [touch], targetTouches: ended ? [] : [touch], changedTouches: [touch],
        });
        const target = active.target.isConnected ? active.target : document.documentElement;
        return !target.dispatchEvent(event);
    };

    const isCarrier = event => event.isTrusted && (event.buttons & carrier) !== 0;
    const hide = event => {
        event.preventDefault();
        event.stopImmediatePropagation();
    };
    const capture = { capture: true };
    add.call(window, "pointerdown", event => {
        if (!isCarrier(event))
            return;
        hide(event);
        active = { identifier: nextIdentifier++, target: event.composedPath()[0] || event.target, owned: false, moves: 0 };
        active.owned = fire("touchstart", true, event);
        log(marker, "start " + (active.owned ? 1 : 0));
    }, capture);
    add.call(window, "pointermove", event => {
        if (!isCarrier(event))
            return;
        hide(event);
        if (!active)
            return;
        const cancelable = (event.buttons & flag) === 0;
        if (fire("touchmove", cancelable, event) && cancelable)
            active.owned = true;
        if (cancelable)
            log(marker, "move " + (active.owned ? 1 : 0) + " " + ++active.moves);
    }, capture);
    add.call(window, "pointerup", event => {
        if (!isCarrier(event))
            return;
        hide(event);
        let cancelled = false;
        if (active) {
            const cancel = (event.buttons & flag) !== 0;
            cancelled = fire(cancel ? "touchcancel" : "touchend", !cancel, event) || active.owned;
        }
        active = null;
        log(marker, "end " + (cancelled ? 1 : 0));
    }, capture);
    // The rest of the press: the mouse events the pointer events would bring, and the click it ends in.
    for (const type of ["pointerover", "pointerout", "pointerenter", "pointerleave", "pointercancel", "mousedown",
             "mousemove", "mouseup", "mouseover", "mouseout", "mouseenter", "mouseleave", "click", "auxclick",
             "dblclick", "contextmenu"]) {
        add.call(window, type, event => {
            if (isCarrier(event))
                hide(event);
        }, capture);
    }
})();
)js"sv;

}
