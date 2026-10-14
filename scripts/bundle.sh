#!/bin/bash
# Lay the app out as it is installed, in dist/harbour-pacific (and dist/harbour-pacific.tar.gz):
#   bin/harbour-pacific                  the one executable (browser/apps/launcher)
#   share/harbour-pacific/lib            the interface's libraries, the engine (libpacificengine.so) and the libraries
#                                        of the engine's build it needs; they find each other by $ORIGIN
#   share/harbour-pacific/share/Lagom    the engine's resources
#   share/harbour-pacific/qml            the interface, with the search engines it ships
#   share/harbour-pacific/translations
# Run scripts/build.sh and scripts/build-browser.sh first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/upstream/Build/release"
UI="$ROOT/build/browser"
OUT="$ROOT/dist/harbour-pacific"
SHARE="$OUT/share/harbour-pacific"

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$SHARE/lib" "$SHARE/share" "$SHARE/qml/shared" "$SHARE/translations"

# The engine, what it loads from our own build, and gcc 14's C++ runtime, newer than the phone's (libcpptrace
# needs CXXABI_1.3.15). Only engine processes load these: the app's executable loads either the engine or the
# interface (browser/apps/launcher), and the interface uses the phone's own runtime with Qt.
cp -L "$BUILD/lib/libpacificengine.so" "$SHARE/lib/"
ldd "$BUILD/lib/libpacificengine.so" | awk '/=>/ {print $3}' | grep -E "^$BUILD/|/libstdc\+\+\.so|/libgcc_s\.so" | sort -u |
    while read -r lib; do cp -L "$lib" "$SHARE/lib/"; done
cp -a "$BUILD/share/Lagom" "$SHARE/share/"
strip --strip-unneeded "$SHARE"/lib/*.so* 2>/dev/null || true
for f in "$SHARE"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done

# The interface.
cp "$UI/launcher/harbour-pacific" "$OUT/bin/"
cp "$UI/browser/libpacificui.so" "$SHARE/lib/"
cp -a "$UI"/lib/libpacificbrowser.so.1* "$SHARE/lib/"
cp -a "$ROOT"/browser/apps/browser/qml/. "$SHARE/qml/"
cp "$ROOT"/browser/apps/shared/*.qml "$SHARE/qml/shared/"
cp -a "$ROOT/browser/data/searchEngines" "$SHARE/qml/"
cp "$UI"/browser/*.qm "$SHARE/translations/"

tar -C "$ROOT/dist" -czf "$ROOT/dist/harbour-pacific.tar.gz" harbour-pacific
du -sh "$OUT" "$ROOT/dist/harbour-pacific.tar.gz"
