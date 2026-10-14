#!/bin/bash
# Build Ladybird with the Qt-free SFOS front end, natively on the aarch64 host.
# Usage: scripts/build.sh [ninja target]   (default: ladybird)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UP="$ROOT/upstream"
JOBS="${JOBS:-12}"

# Apply our patches once; the front end itself lives in this repo and is linked in.
# A patch that is neither applied nor cleanly applicable means upstream/ was edited by hand: regenerate that patch
# from the tree (git -C upstream diff -- <its files>) rather than letting patch half-apply it.
for p in "$ROOT"/patches/[0-9]*.patch; do
    if patch -d "$UP" -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then
        continue
    elif patch -d "$UP" -p1 --dry-run -s -f < "$p" >/dev/null 2>&1; then
        patch -d "$UP" -p1 -s -f < "$p"
    else
        echo "$(basename "$p") does not match upstream/" >&2
        exit 1
    fi
done
ln -sfn "$ROOT/ui/SFOS" "$UP/UI/SFOS"
ln -sfn "$ROOT/ui/protocol" "$UP/UI/protocol"

# CMake >= 3.30 comes from the venv; gcc 14 from the Ubuntu archive.
export PATH="$ROOT/.venv/bin:$PATH"
export LADYBIRD_UI_SFOS=1

# Meta/ladybird.py refuses to configure as root, and this host only has root, so
# run the same three steps it would: vcpkg bootstrap, cmake preset, ninja.
export LADYBIRD_SOURCE_DIR="$UP" LADYBIRD_MAIN_SOURCE_DIR="$UP"
export VCPKG_ROOT="$UP/Build/vcpkg-release"
export PATH="$PATH:$VCPKG_ROOT"
export VCPKG_FORCE_SYSTEM_BINARIES=1 VCPKG_MAX_CONCURRENCY="$JOBS"
BUILD="$UP/Build/release"

cd "$UP"
python3 Meta/ladybird.py vcpkg --preset Release -j "$JOBS"
# Skia's GL backend would link the desktop GL; ours gets its entry points from GLESContext (patch 0009). vcpkg picks
# up an overlay of its own skia port with that one change.
OVERLAY="$UP/Meta/CMake/vcpkg/overlay-ports/skia"
rm -rf "$OVERLAY"
cp -r "$VCPKG_ROOT/ports/skia" "$OVERLAY"
patch -d "$OVERLAY" -p1 -s < "$ROOT/patches/vcpkg-skia-gl-without-system-libraries.patch"
if [ ! -f "$BUILD/build.ninja" ]; then
    cmake --preset Release -S "$UP" -B "$BUILD" \
        -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
        -DPKG_CONFIG_EXECUTABLE="$(command -v pkg-config)" \
        -DBUILD_TESTING=OFF
fi
exec ninja -C "$BUILD" -j "$JOBS" "${1:-ladybird}"
