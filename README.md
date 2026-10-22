# Iroise

A web browser for Sailfish OS, built on the [Ladybird](https://ladybird.org) engine.

Iroise puts Ladybird, a new browser engine written from scratch, behind a native Silica interface: the tabs,
toolbar, menus and settings you would expect on a Sailfish phone, with pages painted on the GPU. It is the
successor to Atlantic, our WebKit-based browser, which is now kept in maintenance only.

## Status

Iroise is in development and has not been released. It is meant for the Jolla Store, so it follows the store's
rules on what an app may use. Nothing is published yet, and the app does not use the Ladybird name anywhere you
would see it: the Ladybird project has not yet said how its name may be used by others, and we are waiting for that.

It is developed on the Jolla Phone 2 and also runs on the Xperia 10 II, where pages are still painted in software.

## Building

Iroise builds natively on an aarch64 Linux host with the Sailfish OS SDK's sysroot. In short:

```sh
git clone https://github.com/LadybirdBrowser/ladybird.git upstream
git -C upstream checkout "$(cat UPSTREAM_COMMIT)"
python3 -m venv .venv && .venv/bin/pip install 'cmake>=3.30'
scripts/build.sh && scripts/build-browser.sh && scripts/build-sqlcipher.sh && scripts/bundle.sh
scripts/package-rpm.sh "$(cat VERSION)" 1
```

[docs/BUILDING.md](docs/BUILDING.md) has the details: what the host needs, running a build on the phone without
installing it, the package, and the CI.

## How it works

Ladybird's own Linux front end needs a newer Qt than Sailfish OS has, so Iroise brings a small front end of its
own for the engine, and the interface talks to the engine over a socket. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
explains the pieces and why they are the way they are.

## Licences

Iroise is free software, under three licences:

- **BSD-2-Clause** ([LICENSE](LICENSE)), like Ladybird itself: our front end for the engine (`ui/`), the launcher,
  the scripts and our patches to Ladybird (`patches/`).
- **MPL-2.0** ([LICENSES/MPL-2.0.txt](LICENSES/MPL-2.0.txt)): the browser interface in `browser/`, which comes
  from Jolla's [sailfish-browser](https://github.com/sailfishos/sailfish-browser) by way of Atlantic.
- **LGPL-2.1-or-later** ([LICENSES/LGPL-2.1-or-later.txt](LICENSES/LGPL-2.1-or-later.txt)): the few files under
  `browser/` whose header says so, carried over from Atlantic.

A file's header names its licence; a file without one takes the licence of the part it is in. Ladybird, fetched
at build time, keeps its own licence.
