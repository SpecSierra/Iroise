# Touch events

Ladybird has no touch events: a page never hears `touchstart`, `touchmove` or `touchend`, and every pointer is a
mouse. Pages written for phones need them. A video player's control bar, for one, cancels `touchend` so that the tap
does not also reach the player behind it and hide the bar. This is how Pacific gives pages touch events until
Ladybird has its own, built so that it is cheap to carry across Ladybird updates and easy to drop when that day comes.

## How it works

**Touches ride on the mouse input path.** `Web::MouseEvent` gains four types, `TouchStart`, `TouchMove`, `TouchEnd`
and `TouchCancel`, and a flag, `is_touch_scroll`, on the scroll steps a finger makes. A touch therefore travels
exactly as a mouse event does: from the front end through the compositor to WebContent, in order with the scroll
steps, and on into a frame that another process hosts. No new IPC message, queue or routing is needed.

**WebContent dispatches real DOM touch events.** `Touch`, `TouchList` and `TouchEvent` exist as interfaces, with
`ontouchstart` and the rest on every element, the document and the window. A touch keeps the target it began on
until it ends, also when the finger leaves it, and goes to the same frame each time. `touchend` gives the user's
activation, so a page may enter fullscreen or start a video from it.

**Scrolling decides as in Chrome.** A `touchstart` or `touchmove` listener that is not passive counts as a blocking
listener, as a blocking `wheel` listener does. The compositor already scrolls without the page wherever no blocking
listener is, and holds a gesture back for the page where one is. A scroll step from a finger fires no `wheel` event.
The page decides by its touch events instead: a cancelled `touchstart` or `touchmove` keeps the finger from
scrolling. Once a step has scrolled, `touchmove` can no longer be cancelled. Listeners on the window, the document
and the body are passive unless they say otherwise, so on most pages nothing ever waits.

**Touches are pointers too.** Each touch fires pointer events of type `"touch"` before its touch events, as in
Chrome: `pointerover`, `pointerenter` and `pointerdown`, then `pointermove`, then `pointerup`, `pointerout` and
`pointerleave`. The pointer stays with the element it went down on (implicit capture), and once the finger scrolls
it gets `pointercancel` and fires no more. The mouse events of a tap fire no pointer events of their own, and a
cancelled `pointerdown` keeps its `mousedown` and `mouseup` from firing (its `click` still does). Where a tap moves
the hover, it fires `mouseover` and the rest, but no `pointerover` or its like of type `"mouse"`. A pointer whose
element leaves the document goes on at the document, so that a page counting its pointers there (a map's) still hears
it go up.

**`touch-action` decides what a finger may pan.** At `touchstart`, the engine works out what `touch-action` allows
on the element the touch began on and every element around it. Where it allows no panning at all (`none`, as on a
map), the result of the `touchstart` tells the front end, which then scrolls nothing for that finger. Where it allows
one direction only (`pan-y`, as on a carousel), the direction the finger first moves in decides: the other direction
is the page's, and its pointer is not cancelled.

**The tap's mouse events come after the page's say.** The front end sends the mouse events of a tap only once
WebContent has finished `TouchEnd`, and only if the page cancelled neither `touchstart` nor `touchend`. It learns that
from the result of the input event (`on_finish_handling_mouse_event`).

## Where the code is

Most of it is in files of our own, which a Ladybird update cannot conflict with:

- `Libraries/LibWeb/UIEvents/Touch`, `TouchList`, `TouchEvent` (`.idl`, `.h`, `.cpp`) and `TouchEventHandlers.idl`.

The rest are hooks of a few lines each in Ladybird's own files:

- `LibWebCommon/Page/InputEvent.{h,cpp}`: the four types and the flag, and their IPC.
- `LibWeb/Page/EventHandler.{h,cpp}`: `handle_touch` and its dispatch (one block at the end of the file), and the
  scroll step's decision in `handle_mousewheel`.
- `LibWeb/Page/Page.{h,cpp}`: the state of the touch under way that a scroll step needs.
- `LibWeb/HTML/EventLoop/EventLoop.cpp`, `Services/WebContent/PageClient.cpp`, `Services/Compositor/ContextState.cpp`,
  `Services/Compositor/CompositorState.cpp`, `LibCompositing/UIOverlays.h`, `LibWebView/ViewImplementation.cpp`: a case
  for the new types where the mouse event types are switched over.
- `LibWeb/DOM/EventTarget.cpp`: touch listeners count as blocking, and `touchend` gives activation.
- Lists: `UIEvents/EventNames.h`, `HTML/GlobalEventHandlers.h`, `HTML/AttributeNames.h`, `idl_files.cmake`,
  `CMakeLists.txt`.

They are four patches: `0021-web-touch-event-interfaces` (the interfaces and the lists), `0022-web-touch-input`
(the input path, the dispatch and the scroll decision) and `0023-web-touch-pointer-events` (the pointer events of a
touch, and a tap's mouse events without their own), and `0024-web-no-mouse-pointer-boundary-events-for-a-tap` (the
same for the hover a tap moves, in `Document.cpp`, kept apart as the one hook there). The front end sends touches in `ui/SFOS/main.cpp`;
`PACIFIC_TOUCH_EVENTS=0` turns them off, which brings back the browser without touch events.

## Not yet

- `touch-action` is decided at `touchstart` and by the first scroll step, not by the compositor, which knows only
  blocking listeners. Where no blocking listener holds the steps back, a `pan-y` area on a page that can scroll
  sideways still scrolls it sideways (its pointer goes on regardless). The compositor would need `touch-action`
  areas next to its blocking-listener areas.
- `setPointerCapture`, which Ladybird does not implement yet (a touch's pointer keeps its own element anyway).
- More than one finger: a second finger cancels the touch and pinches, as before.
- A page in fullscreen still gets the finger as a held mouse button, which Ladybird's own video controls need.
  Touches there wait on a bug of Ladybird's: hit testing does not go inside the fullscreen element.
- A blocking touch listener also holds back a real mouse wheel, which a phone does not have. Ladybird's own
  version would tell the two apart.
