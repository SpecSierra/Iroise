#!/bin/bash
# Lay the app out as it is installed, in dist/harbour-pacific (and dist/harbour-pacific.tar.gz):
#   bin/harbour-pacific                  the one executable (browser/apps/launcher)
#   share/harbour-pacific/lib            the interface's libraries, found by the executable's rpath
#   share/harbour-pacific/lib/engine     the engine (libpacificengine.so) and the libraries of its build it needs,
#                                        which find each other by $ORIGIN. Kept apart, so the interface's Qt never
#                                        picks up the engine's own libcrypto, freetype and the like.
#   share/harbour-pacific/share/Lagom    the engine's resources
#   share/harbour-pacific/qml            the interface, with the search engines it ships
#   share/harbour-pacific/translations
# Run scripts/build.sh, scripts/build-browser.sh and scripts/build-sqlcipher.sh first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/upstream/Build/release"
UI="$ROOT/build/browser"
OUT="$ROOT/dist/harbour-pacific"
SHARE="$OUT/share/harbour-pacific"
ENGINE="$SHARE/lib/engine"

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$ENGINE" "$SHARE/share" "$SHARE/qml/shared" "$SHARE/translations"

# The engine, what it loads from our own build, and gcc 14's C++ runtime, newer than the phone's (libcpptrace
# needs CXXABI_1.3.15). Only engine processes load these: the app's executable loads either the engine or the
# interface (browser/apps/launcher), and the interface uses the phone's own runtime with Qt.
cp -L "$BUILD/lib/libpacificengine.so" "$ENGINE/"
ldd "$BUILD/lib/libpacificengine.so" | awk '/=>/ {print $3}' | grep -E "^$BUILD/|/libstdc\+\+\.so|/libgcc_s\.so" | sort -u |
    while read -r lib; do cp -L "$lib" "$ENGINE/"; done
cp -a "$BUILD/share/Lagom" "$SHARE/share/"
strip --strip-unneeded "$ENGINE"/*.so* 2>/dev/null || true
for f in "$ENGINE"/*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done

# The interface.
cp "$UI/launcher/harbour-pacific" "$OUT/bin/"
strip --strip-unneeded "$OUT/bin/harbour-pacific"
# The Jolla Store wants this rpath (DT_RPATH) on the executable of an app with private libraries.
patchelf --force-rpath --set-rpath '$ORIGIN/../share/harbour-pacific/lib' "$OUT/bin/harbour-pacific"
cp -a "$UI"/lib/libpacificbrowser.so.1* "$SHARE/lib/"
strip --strip-unneeded "$SHARE"/lib/libpacificbrowser.so.1.*.*
cp "$ROOT/build/sqlcipher/libsqlcipher.so.0" "$SHARE/lib/"
cp -a "$ROOT"/browser/apps/browser/qml/. "$SHARE/qml/"
cp "$ROOT"/browser/apps/shared/*.qml "$SHARE/qml/shared/"
cp -a "$ROOT/browser/data/searchEngines" "$SHARE/qml/"
cp "$UI"/lib/*.qm "$SHARE/translations/"

tar -C "$ROOT/dist" -czf "$ROOT/dist/harbour-pacific.tar.gz" harbour-pacific
du -sh "$OUT" "$ROOT/dist/harbour-pacific.tar.gz"
