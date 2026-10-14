# Pacific — a Ladybird-based browser for Sailfish OS

Pacific runs the [Ladybird](https://ladybird.org) engine on Sailfish OS, GPU
accelerated, inside a Silica interface. It is the successor to
Atlantic (WPE WebKit); nothing here is shipped yet.

Upstream's only Linux front end needs Qt 6.10+, which Sailfish OS does not have,
so Pacific carries its own Qt-free front end for the engine and a small set of
patches. Developed and tested on the Jolla Phone 2 (MT6858, Mali-G610).

## Layout

| Path | Contents |
|---|---|
| `upstream/` | Ladybird checkout (not tracked; commit pinned in `UPSTREAM_COMMIT`) |
| `ui/SFOS/` | our Qt-free front end, linked into `upstream/UI/SFOS` by the build script |
| `patches/` | patches applied to `upstream/` |
| `ui/protocol/` | the socket protocol between the engine and the browser interface |
| `browser/` | the browser interface (Qt 5.6, Silica): Atlantic's, with its WebKit layer replaced by `browser/apps/ladybird/` |
| `browser/apps/launcher/` | the app's one executable, `harbour-pacific` |
| `scripts/build.sh` | native aarch64 build of the engine on the build host |
| `scripts/build-browser.sh` | builds `browser/` against the SFOS SDK target |
| `scripts/bundle.sh`, `scripts/deploy.sh` | the app laid out as installed (`dist/harbour-pacific`), copied to `/tmp/ladybird` on the phone |
| `scripts/phone/` | start/stop scripts for the browser on the phone |
| `scripts/build-sqlcipher.sh` | the SQLCipher the password vault opens, without ICU |
| `scripts/package-rpm.sh` | the `harbour-pacific` RPM, as the Jolla Store takes it |
| `browser/apps/browser/qml/icons/icon-launcher-pacific.svg` | the launcher icon; the PNGs are rendered from it |
| `scripts/embed-harness.py` | talks the embedding protocol to an engine on the build host: load a page, tap, print what comes back |

## Build

```sh
git clone https://github.com/LadybirdBrowser/ladybird.git upstream
git -C upstream checkout "$(cat UPSTREAM_COMMIT)"
python3 -m venv .venv && .venv/bin/pip install 'cmake>=3.30'
scripts/build.sh            # the engine: upstream/Build/release (lib/libpacificengine.so, bin/Ladybird)
scripts/build-browser.sh    # the interface and the app's executable: build/browser
scripts/build-sqlcipher.sh  # build/sqlcipher/libsqlcipher.so.0
scripts/bundle.sh           # dist/harbour-pacific, laid out as installed
scripts/deploy.sh           # to /tmp/ladybird on the phone; then sh /tmp/ladybird/start-browser.sh [url]
```

Host needs: `g++-14 nasm autoconf autoconf-archive automake libtool glslang-tools
libpulse-dev gperf bison flex`, plus rustup (the toolchain in
`upstream/rust-toolchain.toml` is fetched automatically).

The build is native, not cross: the SFOS 5.x glibc (2.41) is newer than the
Ubuntu 24.04 host's (2.39), so host-built binaries load on the device as long as
the vcpkg-built libraries and gcc 14's `libstdc++` travel with them.

## One executable

The Jolla Store allows an app a single executable, so everything runs as `bin/harbour-pacific`, a small C
program (`browser/apps/launcher`). Started as `harbour-pacific --engine-role <name> ...` it loads
`libpacificengine.so` and becomes that engine process (`Ladybird`, the engine the interface talks to, or one of its
helpers: `WebContent`, `RequestServer`, `Compositor`, ...); otherwise it loads `libpacificbrowser.so.1`, the interface.
So an engine process loads nothing of Qt, and the engine can bring gcc 14's C++ runtime while the interface uses
the phone's.

`libpacificengine.so` (`ui/SFOS/CMakeLists.txt`) holds every process's `main.cpp`, each with its `ladybird_main`
compiled under its own name, and `EngineMain.cpp` picks the one asked for. It tells the engine (patch 0011) to start
its helpers through the same executable (`LADYBIRD_HELPER_LAUNCHER`) and where its resources are
(`LADYBIRD_APPLICATION_DIRECTORY`), both worked out from where the library is. Everything else is found beside the
libraries too:

```
bin/harbour-pacific
share/harbour-pacific/lib/            the interface's libraries (the executable's rpath)
share/harbour-pacific/lib/engine/     the engine and the libraries it needs, apart so Qt never loads them
share/harbour-pacific/share/Lagom/    the engine's resources
share/harbour-pacific/qml/            the interface, and the search engines it ships
share/harbour-pacific/translations/
```

On the host, `upstream/Build/release/bin/Ladybird` does the launcher's job for the engine alone (the embedding
harness uses it).

## Package

```sh
scripts/build.sh && scripts/build-browser.sh && scripts/build-sqlcipher.sh && scripts/bundle.sh
scripts/package-rpm.sh 0.2.0 1        # dist/rpm/harbour-pacific-0.2.0-1.aarch64.rpm (needs fpm, cairosvg)
```

The Jolla Store's package: `/usr/bin/harbour-pacific`, everything else under `/usr/share/harbour-pacific`, the
launcher entry and its icons, and nothing else (no profile, no D-Bus service files). It passes
`sdk-harbour-rpmvalidator`, with one warning: the entry is `no-invoker`, because started through the booster
`/proc/self/exe` would be the booster's, and the engine's processes are started from it. Sailjail confines the app
by the entry's `[X-Sailjail]` alone, and the app owns `org.pacific.harbour-pacific` on the session bus: a second
start (a link from another app) hands its arguments to the running one and leaves.

The sandbox cannot read the ambience's wallpaper, so in the package the window asks lipstick to draw it behind
(Silica's `_backgroundVisible`), and the surfaces over the web page are panels in the ambience's colour
(`shared/Wallpaper.qml`).

Not to be published (nor "Ladybird" named in anything users see) until the Ladybird project has guidelines for its
name and for packaging.

## GPU painting (GLES through libhybris)

The Jolla Store allows `libEGL` and `libGLESv2` but not Vulkan, so the engine paints with Skia's GL backend on the
system's GLES (patch 0009, `USE_GLES_HYBRIS`; Vulkan is not built).

- `LibGfx/GLESContext` loads `libEGL.so.1` and `libGLESv2.so.2` with `dlopen(RTLD_LOCAL | RTLD_DEEPBIND)` and hands
  Skia a GL interface assembled from them. Nothing links a GL library by name: the Compositor also carries ANGLE
  for WebGL, which exports the same symbol names. Skia is built with its `gl` feature but no native interface
  (`patches/vcpkg-skia-gl-without-system-libraries.patch`, applied to an overlay of vcpkg's port by `build.sh`).
- The Compositor has no window: a 1x1 pbuffer keeps the context current, with `EGL_PLATFORM=null`.
- Each backing store is a gralloc buffer allocated through libhybris' `EGL_HYBRIS_native_buffer2`, bound as an
  `EGLImage` texture behind a framebuffer that Skia renders into. Its pixel dma-buf goes to the interface, which
  wraps it the same way to show it. The software read/write usage bits keep the Mali gralloc from choosing AFBC.
- The Compositor's seccomp filter allows the Mali kernel driver's ioctls (type 0x80) and MediaTek's
  `gralloc_extra` (type 'g'), which gralloc uses for every buffer (patch 0007).

## Browser interface

The interface is Atlantic's Silica browser (MPL-2.0, itself derived from
sailfish-browser), copied into `browser/` with its identities renamed so it cannot
collide with an Atlantic install (data `org.pacific/harbour-pacific`, dconf
`/apps/harbour-pacific`). Pacific never touches another browser's files.

It cannot link the engine (Qt 5.6 and C++11 against C++23), and a separate page
surface cannot sit under a Silica window on lipstick (see Atlantic's
`direct-composite-overlay` investigation), so:

- `LadybirdEngine` starts one engine process with `LADYBIRD_SFOS_EMBED_SOCKET` set;
  the engine then opens no window and connects back over a `SOCK_SEQPACKET` socket.
  One engine serves every tab; each tab is a numbered view (`ui/protocol/EmbedProtocol.h`).
- Each frame arrives as a dma-buf file descriptor and is shown from that buffer, with
  no copy (`DirectFrameTextures`). The phone's EGL has no
  `EGL_EXT_image_dma_buf_import`; it only takes Android buffer handles. So for each
  new buffer we have gralloc allocate one of the same size through libhybris'
  `EGL_HYBRIS_native_buffer2`, copy its handle with the engine's dma-buf in place of
  gralloc's own pixel memory, and bind the resulting `EGLImage` to the texture the
  scene graph draws. This holds because gralloc's uncompressed BGRA layout (rows
  padded to 16 pixels, size rounded to a page) is the one the engine allocates with.
  The gralloc buffer must be asked for with software read/write usage, otherwise the
  Mali gralloc lays it out as AFBC and the page reads as garbage. Where any of this
  does not hold, or with `PACIFIC_UPLOAD_FRAMES=1`, the frame is mapped and uploaded
  with `glTexSubImage2D` as before. Scripted scroll of Hacker News on the Jolla
  Phone 2: the interface process used 377 CPU ticks against 667 with the upload, at
  the same frame rate (the engine sets it).
- Touches, the size, visibility, navigation, and on-screen keyboard text go the other way.
- `EngineView` is one engine view as a Qt Quick item (frames, input, size, visibility, navigation); `WebPage`
  builds a tab's page on it (what the QML reads and calls); `WebContainer` keeps the tabs' pages. Not wired yet:
  extensions (waiting for upstream support).
- The camera notch: the page is kept out of the band of the screen the notch is in
  (the `crop_notch` setting, on by default), and that band is painted in the page's
  `theme-color`, or failing that in the colour the page paints behind its content,
  both of which the engine reports. So the page reads as reaching the screen's edge.
- "Block ads on this site" off: the hosts on the interface's allowlist become
  exception rules (`@@*$domain=host` and `@@||host^$elemhide`) given to the engine as
  its custom filters, on top of the lists.
- Downloads: the engine saves every download itself, into the Downloads folder (`XDG_DOWNLOAD_DIR`), and reports
  its progress. `DownloadModel` lists this run's downloads for the interface's Downloads page (from Settings and
  the toolbar menu), with a notification when one starts and when it ends. The Sailfish transfer list is not used:
  the Jolla Store does not allow its library. There is no "Save as": the store's Downloads permission covers that
  folder only. The engine is held to one connection per download: its downloads over several connections at once
  sometimes never finished their last part. PDFs open in the engine's own viewer. Patch 0008 fixes a link to a file
  on another site never being saved: the response goes to the process that would host the document, and its claim
  to the download was refused.
- Scripts: `LB_EMBED_RUN_SCRIPT` evaluates an expression in the page through the
  engine's console input, the one path that hands a value back. Only strings come
  back intact, so `EngineView::evaluate` wraps the expression in `JSON.stringify`
  together with a number that says which question the answer belongs to.
- Page messages: the engine reports neither scrolling nor selections, so a small script of ours (the page agent,
  `kPageAgentScript`) runs in every page and posts them, at most once a frame, through `console.log` with a marker.
  The engine forwards those as `LB_EMBED_PAGE_MESSAGE`. Any script in the page could post the same, so they are
  hints for the interface (where to put the toolbar, the selection handles), never grounds for trust.
- Text selection: a finger held for half a second is a long press (detected in the interface, which then cancels
  the touch so the engine makes no tap of it). A script looks at what is under it: a link or an image opens the
  action panel; text gets its word selected, and the agent reports the selection and where its two ends are. The
  page answers in CSS pixels and the handles are placed in device pixels, so the answer is scaled by the device
  pixel ratio, and by the visual viewport when the page is pinched.
- Find in page, `<select>` menus, `alert`/`confirm`/`prompt`, the dark colour
  scheme and pinch zoom use the engine's own API, each through its own message.
  A page waits for its dialog to be answered; before this, one `alert()` blocked
  the page for good.
- The file chooser and the colour picker are the engine's requests too, shown with
  the interface's own pickers. Date and time inputs have no engine request: a tap
  that lands on one (checked by a script) opens the interface's picker, which writes
  the value back and fires `input` and `change`.
- The toolbar hides when the page scrolls down and returns when it scrolls up or reaches the top, going by the
  agent's scroll messages.
- Clearing the cache and clearing cookies and site data in Settings go to the
  engine's `clear_browsing_data`.
- Permissions: the engine asks the front end about the microphone, and it grants geolocation to any page and then
  wants a position. The front end puts both to the user (`LB_EMBED_PERMISSION`, the interface's permission banner)
  before the view's own handling runs; an answer holds for that site and kind until the browser closes. Positions
  come from the interface's Qt Positioning source (`LadybirdGeolocation`) through a `Core::GeolocationProvider` of
  the front end's own.
- The engine presents itself as a touch phone (patch 0012): Chrome on Android's user agent and `navigator.platform`,
  `pointer: coarse`, `hover: none` and five touch points, so sites send their mobile pages. Desktop mode sends
  Chrome on Linux's user agent.
- Per-site user agents, the per-site JavaScript switch and desktop mode: the engine gets the site map once
  (`LB_EMBED_SET_SITE_OPTIONS`; user agents are the engine's own presets) and applies it whenever it learns where a
  tab is going: a load the interface asks for, the start of any load, and every page that comes to host the tab.
  A link to another site has already asked for its document by the time the engine hears of it, so that one
  request goes out with the previous site's options. JavaScript off also stops the interface's page agent there.
- New windows: a link with a target or `window.open` makes the engine open a view of its own
  (`LB_EMBED_NEW_VIEW`, numbered from `LB_EMBED_FIRST_ENGINE_VIEW`), which the interface adopts as a new tab;
  `window.close` closes it again (`LB_EMBED_CLOSE_VIEW`). Private tabs are private views in the engine (their own
  cookies and storage, nothing kept), and the windows they open are too.
- Video: the engine's FFmpeg has no H.264 decoder (a patent-encumbered codec, left out upstream), so pages get
  `""` from `canPlayType` for it; VP9, AV1 and the other free codecs play.
- Saved logins: the page agent posts the password field and the text field before it when a form is sent or a
  button pressed (`login` message). The interface offers to save it once the page goes on (or after four seconds)
  into the SQLCipher vault, opening the vault first if need be, and skips a login it already has. A saved login
  is filled into a login form the user has just touched (a page focusing a field itself gets nothing), on the
  https origin the tab is at, which the fill script checks again in the page. Ladybird has no isolated script
  world, so this runs in the page's own; the site always comes from the tab's URL, never from the page.
- Certificates: a certificate the engine cannot verify ends the load on its error page, which the interface
  recognises and answers with its banner. Accepting trusts that host for the rest of the run: the hosts go to every
  RequestServer (`LB_EMBED_SET_CERTIFICATE_EXCEPTIONS`, patch 0010), which then skips verification for exactly
  them. The security panel's details (issuer, dates, cipher, errors) come from a TLS connection of the interface's
  own (`QSslSocket`), once per host and run, since the engine does not hand out the certificates it checked.
- Favicons: the engine decodes the page's icon and sends it as a PNG data URL (`LB_EMBED_FAVICON`; an icon too
  large for one message is left out). The interface keeps it per site for the tab overview, favourites and
  history, except for private tabs.
- Ad blocking is the engine's own: Ladybird ships Brave's `adblock` crate with
  request blocking, cosmetic filtering and built-in list subscriptions, all off by
  default. The interface's "Adblock" switch turns on EasyList, EasyPrivacy, the
  anti-adblock-warning list and the list for the system language; its cookie-banner
  switch turns on the EasyList cookie list. The engine downloads the lists into its
  profile and refreshes them daily. An embedded engine therefore keeps a real
  profile instead of a temporary one.

On the phone: `sh /tmp/ladybird/start-browser.sh [url] [minutes]` after `scripts/deploy.sh`.

## Milestones

1. Build with a headless-only front end and render a page to a PNG on the host.
2. Run the same bundle on the phone from `/tmp` and render a page there.
3. Put pixels on screen: a Wayland window fed by the engine, with touch input (since replaced by the embedded
   engine; the standalone window was removed).
