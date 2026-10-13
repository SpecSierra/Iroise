#!/bin/bash
# Build the Silica app (Qt 5.6) against the SFOS SDK target, the same way Atlantic's UI is built:
# the target's own qmake with the host compiler pointed at the sysroot.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="${SYSROOT:-/opt/sfos-sysroot}"
OUT="$ROOT/build/silica"

export PATH="$SYSROOT/usr/lib64/qt5/bin:$PATH"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_PATH="$SYSROOT/usr/lib64/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"

mkdir -p "$OUT"
cd "$OUT"
qmake -spec "$SYSROOT/usr/share/qt5/mkspecs/linux-g++" "$ROOT/silica/ladybird-silica.pro" \
    "CONFIG+=release" \
    "QMAKE_CXX=g++ --sysroot=$SYSROOT" \
    "QMAKE_CC=gcc --sysroot=$SYSROOT" \
    "QMAKE_LINK=g++ --sysroot=$SYSROOT" \
    "QMAKE_LFLAGS+=-L$SYSROOT/usr/lib64 -Wl,-rpath-link,$SYSROOT/usr/lib64 -Wl,-rpath-link,$SYSROOT/lib64"
make -j8
mkdir -p "$ROOT/dist/silica"
cp ladybird-silica "$ROOT/silica/main.qml" "$ROOT/dist/silica/"
ls -la "$ROOT/dist/silica"
