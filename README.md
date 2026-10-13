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
| `ui/protocol/` | the socket protocol between the engine and the Silica app |
| `silica/` | the Silica app (Qt 5.6) that embeds the engine: `LadybirdView` + a toolbar |
| `scripts/build.sh` | native aarch64 build of the engine on the build host |
| `scripts/build-silica.sh` | builds `silica/` against the SFOS SDK target |
| `scripts/bundle.sh`, `scripts/deploy.sh` | relocatable bundle, copied to `/tmp/ladybird` on the phone |
| `scripts/phone/` | start/stop scripts that run on the phone |

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
package, which is in the phone's repos but not installed by default
(`devel-su zypper install libhybris-libvulkan`).

- The wrapper exports most entry points as IFUNCs that only resolve after
  `vkCreateInstance`, so LibGfx must be linked `-z lazy` and must not store
  `&vkGetDeviceProcAddr` (patch 0004). A `-z now` binary that calls them segfaults
  before `main`.
- Helper processes get a filtered environment; `HYBRIS_*` has to reach the
  compositor (patch 0005).
- The compositor's seccomp filter has to allow the Mali kernel driver's ioctls
  (type 0x80, patch 0007); with that, GPU runs work with the sandbox on.
- The driver imports dma-bufs but cannot export its own memory: `vkGetMemoryFdKHR`
  returns success with fd -1. Shared images are therefore allocated from
  `/dev/dma_heap/system` and imported on both sides (patch 0006). The driver takes
  BGRA with the LINEAR modifier as a render target this way.
- The front end presents those images with `VulkanPresenter`: import once per
  buffer (recognised by inode), `vkCmdBlitImage` to a FIFO swapchain on the
  `wl_surface` through the wrapper's Wayland WSI. The swapchain only offers RGBA
  orders, which the blit converts. `LADYBIRD_SFOS_CPU_PRESENT=1` forces the old
  wl_shm copy path for comparison.
- With the package installed no `HYBRIS_*` settings are needed.

## Silica interface

The browser interface is a normal Silica app. It cannot link the engine (Qt 5.6,
C++11 against C++23) and a separate page surface cannot sit under a Silica window
on lipstick (see Atlantic's `direct-composite-overlay` investigation), so:

- The app starts the engine with `LADYBIRD_SFOS_EMBED_SOCKET` set; the engine then
  opens no window and connects back over a `SOCK_SEQPACKET` socket.
- Each frame arrives as a dma-buf file descriptor. `LadybirdView` maps it and
  uploads it to a texture in the Qt scene graph. The phone's EGL has no
  `EGL_EXT_image_dma_buf_import`, so this is a CPU upload per frame for now; the
  zero-copy route would be Android native buffers
  (`EGL_ANDROID_image_native_buffer` + `VK_ANDROID_external_memory_android_hardware_buffer`).
- Touches, the size, and load/back/forward/reload go the other way.

## Milestones

1. Build with a headless-only front end and render a page to a PNG on the host.
2. Run the same bundle on the phone from `/tmp` and render a page there.
3. Put pixels on screen: a Wayland window fed by the engine, with touch input.
