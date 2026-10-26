#!/bin/bash
# Lay the app out as it is installed, in dist/harbour-iroise (and dist/harbour-iroise.tar.gz):
#   bin/harbour-iroise                  the one executable (browser/apps/launcher)
#   share/harbour-iroise/lib            the interface's libraries, found by the executable's rpath
#   share/harbour-iroise/lib/engine     the engine (libiroiseengine.so) and the libraries of its build it needs,
#                                        which find each other by $ORIGIN. Kept apart, so the interface's Qt never
#                                        picks up the engine's own libcrypto, freetype and the like.
#   share/harbour-iroise/share/Lagom    the engine's resources, and ours in iroise/ (autoconsent.js)
#   share/harbour-iroise/qml            the interface, with the search engines it ships
#   share/harbour-iroise/translations
# Run scripts/build.sh, scripts/build-browser.sh and scripts/build-sqlcipher.sh first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/upstream/Build/release"
UI="$ROOT/build/browser"
OUT="$ROOT/dist/harbour-iroise"
SHARE="$OUT/share/harbour-iroise"
ENGINE="$SHARE/lib/engine"

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$ENGINE" "$SHARE/share" "$SHARE/qml/shared" "$SHARE/translations"

# The engine, what it loads from our own build, and gcc 14's C++ runtime, newer than the phone's (libcpptrace
# needs CXXABI_1.3.15). Only engine processes load these: the app's executable loads either the engine or the
# interface (browser/apps/launcher), and the interface uses the phone's own runtime with Qt.
cp -L "$BUILD/lib/libiroiseengine.so" "$ENGINE/"
# NB: Compared by real path: the build directory is reached through symlinks (CI's checkout dir has changed name, and
#     the CMake cache keeps the old one), so the paths ldd prints need not start with $BUILD.
BUILD_REAL="$(realpath "$BUILD")"
ldd "$BUILD/lib/libiroiseengine.so" | awk '/=>/ {print $3}' | sort -u |
    while read -r lib; do
        case "$(realpath "$lib")" in
            "$BUILD_REAL"/* | */libstdc++.so* | */libgcc_s.so*) cp -L "$lib" "$ENGINE/" ;;
        esac
    done
cp -a "$BUILD/share/Lagom" "$SHARE/share/"
# The cookie-consent answerer the engine runs in pages (scripts/fetch-autoconsent.sh).
"$ROOT/scripts/fetch-autoconsent.sh"
mkdir -p "$SHARE/share/Lagom/iroise"
cp "$ROOT/build/autoconsent/autoconsent.js" "$SHARE/share/Lagom/iroise/"
strip --strip-unneeded "$ENGINE"/*.so* 2>/dev/null || true
for f in "$ENGINE"/*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done

# The interface.
cp "$UI/launcher/harbour-iroise" "$OUT/bin/"
strip --strip-unneeded "$OUT/bin/harbour-iroise"
# The Jolla Store wants this rpath (DT_RPATH) on the executable of an app with private libraries.
patchelf --force-rpath --set-rpath '$ORIGIN/../share/harbour-iroise/lib' "$OUT/bin/harbour-iroise"
cp -a "$UI"/lib/libiroisebrowser.so.1* "$SHARE/lib/"
strip --strip-unneeded "$SHARE"/lib/libiroisebrowser.so.1.*.*
cp "$ROOT/build/sqlcipher/libsqlcipher.so.0" "$SHARE/lib/"
cp -a "$ROOT"/browser/apps/browser/qml/. "$SHARE/qml/"
cp "$ROOT"/browser/apps/shared/*.qml "$SHARE/qml/shared/"
cp -a "$ROOT/browser/data/searchEngines" "$SHARE/qml/"
cp "$UI"/lib/*.qm "$SHARE/translations/"

tar -C "$ROOT/dist" -czf "$ROOT/dist/harbour-iroise.tar.gz" harbour-iroise
du -sh "$OUT" "$ROOT/dist/harbour-iroise.tar.gz"
