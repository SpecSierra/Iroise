#!/bin/bash
# Build Ladybird with the Qt-free SFOS front end, natively on the aarch64 host.
# Usage: scripts/build.sh [ninja target]   (default: ladybird)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UP="$ROOT/upstream"
JOBS="${JOBS:-12}"

# Apply our patches once; the front end itself lives in this repo and is linked in.
for p in "$ROOT"/patches/*.patch; do
    if ! patch -d "$UP" -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then
        patch -d "$UP" -p1 < "$p"
    fi
done
ln -sfn "$ROOT/ui/SFOS" "$UP/UI/SFOS"

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
if [ ! -f "$BUILD/build.ninja" ]; then
    cmake --preset Release -S "$UP" -B "$BUILD" \
        -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
        -DPKG_CONFIG_EXECUTABLE="$(command -v pkg-config)" \
        -DBUILD_TESTING=OFF
fi
exec ninja -C "$BUILD" -j "$JOBS" "${1:-ladybird}"
