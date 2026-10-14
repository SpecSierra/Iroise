#!/bin/bash
# Build the browser interface (browser/, Qt 5.6) and the app's executable against the SFOS SDK target: the target's
# own qmake with the host compiler pointed at the sysroot. scripts/bundle.sh lays the result out.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="${SYSROOT:-/opt/sfos-sysroot}"
OUT="$ROOT/build/browser"
JOBS="${JOBS:-12}"

export PATH="$SYSROOT/usr/lib64/qt5/bin:$PATH"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib64/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"

mkdir -p "$OUT"
cd "$OUT"
qmake -spec "$SYSROOT/usr/share/qt5/mkspecs/linux-g++" "$ROOT/browser/apps/apps.pro" \
    "CONFIG+=release" \
    "QMAKE_CXX=ccache g++ --sysroot=$SYSROOT" \
    "QMAKE_CC=ccache gcc --sysroot=$SYSROOT" \
    "QMAKE_LINK=g++ --sysroot=$SYSROOT" \
    "QMAKE_LFLAGS+=-L$SYSROOT/usr/lib64 -Wl,-rpath-link,$SYSROOT/usr/lib64 -Wl,-rpath-link,$SYSROOT/lib64" \
    "SFOS_SYSROOT=$SYSROOT"
make -j"$JOBS" sub-lib-all sub-launcher-all
