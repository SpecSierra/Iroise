# ladybird-sfos — Ladybird on Sailfish OS

Exploration: get the [Ladybird](https://ladybird.org) engine compiled and running
on Sailfish OS, without Qt 6 (upstream's only Linux front end needs Qt 6.10+).

Not related to the Atlantic repos; nothing here is shipped.

## Layout

| Path | Contents |
|---|---|
| `upstream/` | Ladybird checkout (not tracked; commit pinned in `UPSTREAM_COMMIT`) |
| `ui/SFOS/` | our Qt-free front end, linked into `upstream/UI/SFOS` by the build script |
| `patches/` | patches applied to `upstream/` |
| `scripts/build.sh` | native aarch64 build on the build host |

## Build

```sh
git clone https://github.com/LadybirdBrowser/ladybird.git upstream
git -C upstream checkout "$(cat UPSTREAM_COMMIT)"
python3 -m venv .venv && .venv/bin/pip install 'cmake>=3.30'
scripts/build.sh            # builds target `ladybird` into upstream/Build/release
```

Host needs: `g++-14 nasm autoconf autoconf-archive automake libtool glslang-tools
libpulse-dev gperf bison flex`, plus rustup (the toolchain in
`upstream/rust-toolchain.toml` is fetched automatically).

The build is native, not cross: the SFOS 5.x glibc (2.41) is newer than the
Ubuntu 24.04 host's (2.39), so host-built binaries load on the device as long as
the vcpkg-built libraries and gcc 14's `libstdc++` travel with them.

## Milestones

1. Build with a headless-only front end and render a page to a PNG on the host.
2. Run the same bundle on the phone from `/tmp` and render a page there.
3. Put pixels on screen: a Wayland window fed by the engine, with touch input.
