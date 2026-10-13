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

## GPU painting (Vulkan through libhybris)

Works on the Jolla Phone 2 (Mali-G610, Vulkan 1.3) with Jolla's `libhybris-libvulkan`
package, which is in the phone's repos but not installed by default. For tests it
was fetched with `pkcon download` and unpacked into the bundle's `lib/` instead
(with `$ORIGIN` rpaths added, since its plugin directory is normally `/usr/lib64/libhybris`).

- The wrapper exports most entry points as IFUNCs that only resolve after
  `vkCreateInstance`, so LibGfx must be linked `-z lazy` and must not store
  `&vkGetDeviceProcAddr` (patch 0004). A `-z now` binary that calls them segfaults
  before `main`.
- Helper processes get a filtered environment; `HYBRIS_*` has to reach the
  compositor (patch 0005).
- The compositor's seccomp filter kills the Mali driver's ioctls, so GPU runs
  currently need `--disable-sandbox`.
- Frames are still read back into a shared bitmap (patch 0003). The driver does
  offer the four extensions upstream's dma-buf sharing path needs
  (`VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`,
  `VK_KHR_external_memory_fd`, `VK_KHR_image_format_list`) plus
  `VK_KHR_wayland_surface` and `VK_KHR_swapchain`, so presenting without the
  readback is the next step.

## Milestones

1. Build with a headless-only front end and render a page to a PNG on the host.
2. Run the same bundle on the phone from `/tmp` and render a page there.
3. Put pixels on screen: a Wayland window fed by the engine, with touch input.
