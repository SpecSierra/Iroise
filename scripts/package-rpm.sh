#!/bin/bash
# Build the harbour-pacific RPM, as the Jolla Store takes it, from what scripts/bundle.sh laid out in
# dist/harbour-pacific: the one executable in /usr/bin, everything else in /usr/share/harbour-pacific, and the
# launcher entry with its icons. Sailjail confines it by the entry's [X-Sailjail] alone.
# NB: No "Ladybird" in anything users see, "Powered by" included, until the Ladybird project publishes guidelines
#     for its name and for packaging: it is their trademark.
# Usage: scripts/package-rpm.sh [version] [release]     (needs fpm; python3 with cairosvg for the icons)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-0.2.0}"
RELEASE="${2:-1}"
APP="$ROOT/dist/harbour-pacific"
S="$ROOT/build/rpm-stage"
OUT="$ROOT/dist/rpm"

[ -x "$APP/bin/harbour-pacific" ] || { echo "run scripts/bundle.sh first" >&2; exit 1; }

rm -rf "$S"
mkdir -p "$S/usr/share/applications" "$OUT"
cp -a "$APP/bin" "$APP/share" "$S/usr/"

python3 - "$ROOT/browser/apps/browser/qml/icons/icon-launcher-pacific.svg" "$S" <<'PY'
import os, sys, cairosvg
svg, stage = sys.argv[1], sys.argv[2]
for size in (86, 108, 128, 172):
    directory = f"{stage}/usr/share/icons/hicolor/{size}x{size}/apps"
    os.makedirs(directory, exist_ok=True)
    cairosvg.svg2png(url=svg, write_to=f"{directory}/harbour-pacific.png", output_width=size, output_height=size)
PY

# no-invoker: started through the booster, /proc/self/exe would be the booster's, and the engine's helper
# processes are started from it (see browser/apps/launcher).
cat > "$S/usr/share/applications/harbour-pacific.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Pacific
Icon=harbour-pacific
Exec=harbour-pacific %U
Comment=Web browser
MimeType=text/html;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;
X-Nemo-Application-Type=no-invoker

[X-Sailjail]
Permissions=Internet;Audio;Microphone;Location;Downloads
OrganizationName=org.pacific
ApplicationName=harbour-pacific
DESKTOP

# NB: No automatic dependencies: the engine brings its own libraries, and the scan would turn each of them into
#     a requirement that nothing on the phone provides.
fpm -s dir -t rpm -n harbour-pacific -v "$VERSION" --iteration "$RELEASE" --architecture aarch64 \
    --rpm-summary "Web browser" \
    --description "A web browser for Sailfish OS." \
    --license "BSD-2-Clause and MPL-2.0" --url "https://github.com/SpecSierra/Pacific" \
    --vendor "" --no-auto-depends --rpm-rpmbuild-define "_build_id_links none" \
    --depends sailfishsilica-qt5 --depends qt5-qtpositioning --depends qt5-qtgraphicaleffects --depends libkeepalive \
    --depends nemo-qml-plugin-configuration-qt5 --depends nemo-qml-plugin-dbus-qt5 \
    --depends nemo-qml-plugin-notifications-qt5 \
    --depends sailfish-components-webview-qt5-popups --depends sailfish-components-pickers-qt5 \
    --force -p "$OUT/harbour-pacific-$VERSION-$RELEASE.aarch64.rpm" -C "$S" .
ls -la "$OUT"
