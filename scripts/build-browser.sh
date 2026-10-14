#!/bin/bash
# Build the browser interface (browser/, Qt 5.6) against the SFOS SDK target, the way Atlantic's UI is built:
# the target's own qmake with the host compiler pointed at the sysroot.
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
make -j"$JOBS" sub-lib-all
make -j"$JOBS" sub-browser-all

# Lay the result out the way the phone scripts expect it under /tmp/ladybird/browser.
DIST="$ROOT/dist/browser"
rm -rf "$DIST"
mkdir -p "$DIST/bin" "$DIST/lib" "$DIST/share/shared" "$DIST/translations"
cp browser/pacific-browser "$DIST/bin/"
cp -a lib/libpacificbrowser.so.1* "$DIST/lib/"
cp -a "$ROOT"/browser/apps/browser/qml/. "$DIST/share/"
cp "$ROOT"/browser/apps/shared/*.qml "$DIST/share/shared/"
cp -a "$ROOT/browser/data/searchEngines" "$DIST/share/" 2>/dev/null || true
cp browser/*.qm "$DIST/translations/" 2>/dev/null || true
du -sh "$DIST"
