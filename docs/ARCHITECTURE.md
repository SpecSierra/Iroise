# How Pacific works

## The big picture

Pacific is two programs that talk to each other.

The **engine** is Ladybird, with a small front end of our own (`ui/SFOS`) in place of upstream's Qt 6 one, which
Sailfish OS cannot run. It runs the way Ladybird always does: one process that manages the tabs, and helper
processes for page content, networking, image decoding, audio, video and compositing. One engine serves every tab.

The **interface** is the Silica browser you see (`browser/`): Atlantic's interface, itself derived from Jolla's
sailfish-browser, with its WebKit layer replaced by one that talks to Ladybird (`browser/apps/ladybird`).

The two cannot simply be linked together. The interface is Qt 5.6 and C++11; the engine is C++23 with its own
runtime. And a page drawn by a separate window cannot be placed under a Silica window on Sailfish's compositor. So
the interface starts the engine as a child process, the engine connects back over a Unix socket and opens no
window of its own, and they exchange messages (`ui/protocol/EmbedProtocol.h`). Each tab is a numbered view in the
engine. Finished frames come one way, as GPU buffers the interface shows directly; touches, typing, navigation and
everything else go the other.

## One executable

The Jolla Store allows an app a single executable, so everything runs as `harbour-pacific`, a few lines of C
(`browser/apps/launcher`). Started with `--engine-role <name>` it loads the engine library and becomes that engine
process; otherwise it loads the interface library. That way an engine process never loads Qt, and the engine can
bring gcc 14's C++ runtime while the interface uses the phone's.

The engine library holds every engine process's `main()`, each compiled under its own name, and picks the one it
was asked for. It tells Ladybird to start its helpers through the same executable and where its resources are,
both worked out from where the library itself is installed:

```
bin/harbour-pacific
share/harbour-pacific/lib/            the interface's libraries
share/harbour-pacific/lib/engine/     the engine and the libraries only it needs, apart so Qt never sees them
share/harbour-pacific/share/Lagom/    the engine's resources (fonts, icons, themes)
share/harbour-pacific/qml/            the interface, and the search engines it ships
```

The launcher entry starts the app without Sailfish's booster: under the booster the process would not be our own
executable, and the engine's helpers are started from it.

## Painting on the GPU

The Jolla Store allows an app OpenGL ES but not Vulkan, so the engine paints with Skia's OpenGL backend on the
phone's GLES. The phone's graphics drivers are Android's, reached through libhybris, and that shapes everything
here.

The engine loads the GL libraries itself, at run time and kept to itself, rather than linking them: the
compositor process also carries ANGLE for WebGL, which exports the very same function names. Each of the engine's
backing stores is an Android graphics buffer, which Skia renders into and whose memory is then handed to the
interface. The interface shows that same memory as a texture, so a frame is never copied. There is one catch: the
phone's EGL only accepts Android buffer handles, not plain shared memory, so for each new buffer the interface has
the driver allocate a buffer of the same size and shape, swaps in the engine's memory, and shows that. It works
because both sides ask for the same simple, uncompressed layout. Where it cannot work, the interface falls back
to copying the frame into a texture (`PACIFIC_UPLOAD_FRAMES=1` forces that).

On the Jolla Phone 2, scrolling a page this way costs the interface about half the CPU the copying did. Pages are
paced at the screen's own refresh rate, which the interface tells the engine (90 Hz there; Ladybird assumes 60
otherwise).

## The interface

The interface was renamed throughout so it can live beside an Atlantic install without touching its files: it
keeps its data under `org.pacific/harbour-pacific` and its settings under `/apps/harbour-pacific`.

Three classes carry the engine into it. `EngineView` is one engine view as a Qt Quick item: it shows frames and
sends input, size and navigation. `WebPage` builds a tab's page on top of it, which is what the QML uses.
`WebContainer` keeps the tabs' pages, and discards background ones when memory runs short.

## Talking to pages

The engine does not tell a front end everything a browser interface needs: where a page is scrolled to, what is
selected, whether a video is playing. So a small script of ours runs in every page and reports those as messages,
at most once a frame. Ladybird has no separate world for scripts like ours, so it runs in the page's own, and any
script on the page, including a frame from another site, could send the same messages. The interface treats them
as hints (where to put the toolbar, where to draw the selection handles) and never as proof. Things only the user
can cause, such as a login form being sent or a selection being started, count only within a few seconds of the
user's own touch or key press.

The interface can also ask the page a question: it runs an expression through the engine's console and gets the
answer back as JSON, numbered so that each answer finds its question.

## Features, one by one

**Touch and selection.** One finger scrolls or taps, two fingers zoom. A finger held for half a second is a long
press: on a link or an image it opens the action panel, on text it selects the word and shows handles you can
drag.

**Dialogs and pickers.** `alert`, `confirm` and `prompt`, `<select>` menus, the file chooser and the colour picker
are the engine's own requests, shown with Silica's dialogs and pickers. Date and time fields have no engine
request, so a tap on one opens the Silica picker and the chosen value is written back into the page.

**Downloads.** The engine saves every download itself into the Downloads folder and reports its progress; the
interface lists the run's downloads and sends a notification when one starts and ends. There is no "Save as":
the store's permission covers the Downloads folder only. PDFs open in the engine's own viewer.

**Permissions.** A page asking for the microphone or the location is asked about with a banner, and the answer
holds for that site until the browser closes. Location requests from frames of other sites are refused. Positions
come from Sailfish's location service.

**Phone or desktop.** The engine presents itself as a touch phone (Chrome on Android's user agent, coarse pointer,
no hover), so sites send their mobile pages; desktop mode switches to a desktop browser's identity. Per-site user
agents and a per-site JavaScript switch are applied as soon as the engine knows where a tab is going.

**Touch.** Ladybird has no touch events, and a page written for phones often needs them: a video player's control
bar cancels `touchend` so that the tap does not also reach the player and hide the bar. A script of ours, run in
every document before the document's own (`ui/SFOS/TouchEvents.h`), gives pages `ontouchstart` and the rest, and
says so once a page listens for them. From then on the front end presses the mouse in a way no real mouse does,
with the Forward button held as well; the script hides that press from the page and fires the touch event in its
place, and tells the front end whether the page cancelled it. Only then does the finger scroll, or end in the mouse
events of a tap, as in other browsers. Once it scrolls, the page gets `touchcancel` and no more touch events for
that finger, as in Chrome on Android: the engine takes input in turn, and touch events, which wait on the page's
script, would hold up the scroll steps behind them. The press being a real one is what lets a page go fullscreen or start a
video from `touchend`. Pages that do not listen for touches get a scroll or a tap straight away, and a page in
fullscreen without touch listeners gets the finger as a mouse with its button held, so a seek bar can be dragged.

**New windows and private tabs.** A link that opens a new window, or `window.open`, becomes a new tab, and
`window.close` closes it. Private tabs get their own cookies and storage, kept in memory and dropped when the last
private tab closes.

**Video.** Ladybird's own FFmpeg leaves out H.264 and AAC, which are patent-encumbered. Sailfish OS ships FFmpeg
5.1, so Pacific uses the phone's for those two, loading it at run time; VP9, AV1 and the other free codecs use the
engine's own. On the Jolla Phone 2, H.264 plays at full speed up to 1080p at 30 frames a second, and at 53 to 57 frames a
second for 1080p60, all decoded in software.

**Blocking ads and cookie banners.** Ladybird has an ad blocker built in (Brave's), and Pacific gives it the
lists Atlantic used. The Adblock switch turns on EasyList and EasyPrivacy, uBlock Origin's filters and annoyances,
Fanboy's annoyance and social lists, eyeo's lists against sites that defeat blockers (in English and the phone's
language), the ads list for the phone's language, and a few rules of our own for what those miss. The
cookie-banner switch turns on EasyList's and uBlock's cookie-notice lists, and DuckDuckGo's autoconsent, which
answers the consent dialogs it knows with "reject" (on a site that only offers "accept or subscribe", it
accepts). Autoconsent runs in every frame of every page before the page's own scripts. Turning blocking off for
one site adds an exception for it.

The lists are downloaded on the phone and refreshed daily, so they stay current without a new package; uBlock's
script rules (`+js`) are not run, but its blocking and hiding rules are. Each page process compiles the lists for
itself, about 55 MB of memory per process with the full set.

**Saved logins.** When a login form is sent, the interface offers to save it into the password vault (SQLCipher,
opened with a master password). A saved login is filled in only when you touch a login field yourself, and only
on the secure site it was saved for, which is always taken from the tab's address, never from the page.

**Certificates.** A site whose certificate cannot be checked ends on the engine's error page, and the interface
offers to continue anyway. Since a page could pretend to be that error page, the interface first checks the
site's certificate with a connection of its own. If you accept, the site is trusted for the rest of the run;
the interface remembers which certificate you accepted and withdraws the trust if the site later shows a different
one. Clearing site data forgets it, and so does closing the last private tab for what was accepted there.

**The camera notch.** The page is kept out of the strip of screen the notch is in, and that strip is painted in
the page's own colour, so the page still seems to reach the edge.

## When something goes wrong

If the engine crashes, every tab shows a crash banner and a new engine is started straight away, with the app's
settings sent to it again; Reload brings the page back. An engine that keeps crashing is not restarted again and
again.

Messages from the engine are never silently lost. If the interface is too busy to take them, they wait and are
delivered in order, except frames, where only the latest matters. A message too large for the socket is refused
with a warning rather than cut in half.

## Our patches to Ladybird

Each patch in `patches/` does one thing:

| Patch | What it does |
|---|---|
| 0001 | lets the build pick our front end instead of upstream's Qt one |
| 0002 | fits the garbage collector's memory reservation into the phone's smaller address space |
| 0007 | lets the compositor's sandbox talk to the phone's GPU and buffer drivers |
| 0008 | lets a download from a link to another site be saved |
| 0009 | paints with OpenGL ES through libhybris instead of Vulkan |
| 0010 | trusts a certificate the user accepted, for that site only |
| 0011 | runs every engine process from the one executable |
| 0012 | presents the engine as a touch phone |
| 0013 | tells sites the user's colour scheme up front, so they can send their dark pages |
| 0014 | makes the audio buffer big enough for Sailfish's deep-buffer output |
| 0015 | builds for plain ARMv8.0, which older phones' processors need |
| 0016 | maps read-only files privately, which older kernels (the Xperia 10 II's) need |
| 0017 | accepts Sailfish OS's FFmpeg 5.1 for H.264 and AAC |
| 0018 | runs the embedder's scripts in every document, frames included, before the document's own (autoconsent) |

`patches/vcpkg-skia-gl-without-system-libraries.patch` builds Skia's OpenGL backend without linking a GL library,
for the reason above.
