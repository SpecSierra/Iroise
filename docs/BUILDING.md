# Building Pacific

## What the host needs

The build runs natively on an aarch64 Linux machine (we use Ubuntu 24.04). It is not a cross build: Sailfish OS 5's
glibc is newer than Ubuntu 24.04's, so binaries built on the host load on the phone, provided the libraries vcpkg
builds and gcc 14's C++ runtime travel with them, which the bundle takes care of.

You need:

- gcc 14 and the usual build tools: `g++-14 nasm autoconf autoconf-archive automake libtool glslang-tools
  libpulse-dev gperf bison flex`;
- rustup (Ladybird fetches the Rust toolchain it pins by itself);
- CMake 3.30 or newer, which the build takes from a Python venv in `.venv`;
- the Sailfish OS SDK's target sysroot in `/opt/sfos-sysroot` (or point `SYSROOT` at it), for the interface;
- `fpm` and Python's `cairosvg` to make the package.

## The steps

```sh
git clone https://github.com/LadybirdBrowser/ladybird.git upstream
git -C upstream checkout "$(cat UPSTREAM_COMMIT)"
python3 -m venv .venv && .venv/bin/pip install 'cmake>=3.30'

scripts/build.sh            # the engine, in upstream/Build/release
scripts/build-browser.sh    # the interface and the app's executable, in build/browser
scripts/build-sqlcipher.sh  # the SQLCipher the password vault uses
scripts/bundle.sh           # everything laid out as installed, in dist/harbour-pacific
```

`upstream/` is a plain Ladybird checkout at the commit in `UPSTREAM_COMMIT`. `scripts/build.sh` applies our patches
from `patches/` to it once and links our front end (`ui/`) into it. The first engine build takes a long while,
mostly vcpkg building Ladybird's dependencies; later ones only rebuild what changed.

## Trying a build on the phone

You do not have to install a package to try a build. `scripts/deploy.sh` copies the bundle to `/tmp/ladybird` on
the phone over SSH; then, on the phone, `sh /tmp/ladybird/start-browser.sh [url] [minutes]` starts it, with its
profile under `/tmp` too, so your own browser and its data are left alone. It stops by itself after the given
minutes (15 by default), or with `sh /tmp/ladybird/stop.sh`.

`deploy.sh` reaches the phone as `defaultuser` at `PACIFIC_PHONE` (default `127.0.0.1`) on `PACIFIC_PHONE_PORT`
(default `2222`, for a forwarded port), with the developer-mode password in `PACIFIC_PHONE_PASSWORD`, or with an
SSH key if you leave that unset.

Close the installed Pacific before starting a test copy: the app allows only one of itself, so a second start just
hands its address to the running one, which opens it as a new tab.

A few tools help with measuring:

- `scripts/phone/startbench.sh` times launch to the first page shown, over several runs;
- `scripts/render-timing.py` summarises the interface's frame times from a log taken with `QSG_RENDER_TIMING=1`;
- `scripts/embed-harness.py` drives an engine on the build host directly, without the interface: load a page,
  tap, and see what comes back.

## The package

```sh
scripts/package-rpm.sh "$(cat VERSION)" 1    # dist/rpm/harbour-pacific-<version>-1.aarch64.rpm
```

The package is laid out the way the Jolla Store wants it: the one executable in `/usr/bin`, everything else under
`/usr/share/harbour-pacific`, plus the launcher entry and its icons, and nothing else. It depends only on packages
the store allows (the pickers come with `Sailfish.Pickers`, an import the store allows, so the package names none),
and its release is a plain number, as the store wants. It passes the store's validator
([sdk-harbour-rpmvalidator](https://github.com/sailfishos/sdk-harbour-rpmvalidator),
`scripts/validate-rpm.sh <package>`, which CI runs on every package) with one warning, given twice: the launcher entry starts the app
without the booster (see [ARCHITECTURE.md](ARCHITECTURE.md#one-executable)). Sailjail confines the app with the
permissions the entry asks for: internet, audio, microphone, location, the Downloads, Documents, Pictures, Videos
and Music folders, removable media, and the media index the file picker reads.

## CI

Every push to `main` builds the engine, the interface and the package on our self-hosted aarch64 runner
(`.github/workflows/build.yml`, which runs `scripts/ci-build.sh`). The runner keeps the Ladybird checkout, its build
directory and a compiler cache between runs, so a run only rebuilds what the engine pin, the patches or our own
code changed. The package is kept with the run for a week, for testing; it is not published.

Each run then checks the package against the store's rules (`scripts/validate-rpm.sh`) and runs the engine's tests
(`scripts/test-engine.sh`): Ladybird's own tests of input, pointer, wheel and DOM events, which our patches change,
and our tests of the touch patches, with Ladybird's headless test runner on the runner itself. Then it runs the
engine with our front end as the interface runs it, over the embedding protocol (`scripts/test-front-end.sh`,
`tests/embed`): loads and history, taps, drags and flicks, touch-action, dialogs, downloads, fullscreen, text
input and a crashed page, on small pages of its own. Their results are kept with the run too. The same scripts run
them on the build host, or a few of them: `scripts/test-engine.sh -f 'Text/input/UIEvents/touch-*'`,
`scripts/test-front-end.sh test_engine.Scrolling`.

Nothing runs on pull requests. A self-hosted runner would otherwise run whatever code a fork sends it.

## Updating Ladybird

To move to a newer Ladybird, our patches are rebased rather than reapplied by hand: in `upstream/`, the pinned
commit plus one commit per patch (in their file order), rebased onto the new commit with git, which resolves what
it can by three-way merge. Then each patch file is written again from its rebased commit (`git diff <commit>^
<commit>`), the new commit goes into `UPSTREAM_COMMIT`, and the engine is built and tried on the phone before it is
committed. A patch that upstream has made unnecessary is dropped rather than carried along.
