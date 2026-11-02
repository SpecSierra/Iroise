# Touch events

Ladybird has had touch events of its own since `a3f24be6` (October 2026): `Touch`, `TouchList` and `TouchEvent`, a
touch's pointer events of type `"touch"`, and the mouse events and `click` of a tap, which a page can cancel. Iroise
uses them, and carries two small patches for what a phone needs beyond them.

## How it works

**A finger is a pointer of type `"touch"`.** The front end (`ui/SFOS/main.cpp`) sends a finger as mouse down, move
and up events marked as a touch (`pointer_data`). The engine fires the pointer and touch events and makes the tap.
None of this goes through the compositor.

**A finger scrolls by wheel steps that the page decides (patch 0021).** Once the finger has moved 24 device pixels,
the front end also sends scroll steps marked `is_touch_scroll`. Such a step:

- fires no `wheel` event, so a drag on a map does not zoom it;
- is refused if the page cancelled the finger's `touchstart` or `touchmove`;
- is refused if `touch-action` on the element the finger came down on, or around it, does not allow panning in the
  direction the finger first moved;
- leaves no mouse position behind, so nothing that scrolls under it is hovered.

The compositor holds a finger's first step back until the page has decided it, since it does not know `touch-action`.
Once the page lets a step through, the rest scroll without waiting. Non-passive `touchstart` and `touchmove`
listeners also hold steps back, as a non-passive `wheel` listener does. Listeners on the window, the document and
the body are passive unless they say otherwise.

**The first step's answer decides.** If the page lets the step through, the front end cancels the finger's pointer
(`pointercancel` and `touchcancel`, as in Chrome), so it makes no tap. If the page refuses it, the finger stays the
page's and scrolls nothing. A finger that lifts while it scrolls, before the page has answered, is cancelled too.

**Pages find the touch screen (patch 0022).** `ontouchstart` and the rest are on every element, the document and the
window. Ladybird leaves them out on purpose, so that desktop sites do not think there is a touch screen; the patch
updates Ladybird's test of that. Patch 0012
reports `maxTouchPoints` 5 and a coarse pointer that cannot hover.

## Not yet

- A finger's scroll starts only once the page's main thread has decided its first step: a busy page starts late.
  The compositor would need `touch-action` areas to start without it.
- The touch's pointer events go straight to WebContent and its scroll steps go through the compositor. A step can
  only follow its `touchstart` because a finger has to move first.
- One finger: a second one cancels the touch and pinches.
- A page in fullscreen gets the finger as a mouse with its button held, which Ladybird's own video controls need.
- A blocking touch listener also holds back a real mouse wheel, which a phone does not have.
- `IROISE_TOUCH_EVENTS=0` sends a tap as a plain mouse click and no touch events, as Iroise did before them.
