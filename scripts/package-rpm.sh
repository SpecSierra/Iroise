#!/bin/bash
# Build the pacific-browser RPM from what scripts/build.sh, bundle.sh and build-browser.sh left in dist/.
# One package: the engine bundle, the browser interface, the launcher entry and its Sailjail profile.
# Usage: scripts/package-rpm.sh [version] [release]     (needs fpm; python3 with cairosvg for the icons)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-0.1.0}"
RELEASE="${2:-1}"
S="$ROOT/build/rpm-stage"
OUT="$ROOT/dist/rpm"

[ -x "$ROOT/dist/ladybird/bin/Ladybird" ] || { echo "run scripts/build.sh and scripts/bundle.sh first" >&2; exit 1; }
[ -x "$ROOT/dist/browser/bin/pacific-browser" ] || { echo "run scripts/build-browser.sh first" >&2; exit 1; }

rm -rf "$S"
mkdir -p "$S/usr/bin" "$S/usr/lib64" "$S/usr/libexec/pacific" "$S/usr/share/pacific-browser/data" \
    "$S/usr/share/translations" "$S/usr/share/applications" "$S/usr/share/dbus-1/services" \
    "$S/etc/sailjail/permissions" "$OUT"

# The engine: a relocatable tree whose binaries find their libraries by $ORIGIN. The interface starts
# /usr/libexec/pacific/bin/Ladybird unless PACIFIC_ENGINE says otherwise.
cp -a "$ROOT/dist/ladybird/bin" "$ROOT/dist/ladybird/lib" "$ROOT/dist/ladybird/libexec" "$ROOT/dist/ladybird/share" \
    "$S/usr/libexec/pacific/"

# The interface. NB: Sailjail only starts ELF binaries, so there is no wrapper script; whatever the browser needs
# from its environment comes from the profile below.
install -m 755 "$ROOT/dist/browser/bin/pacific-browser" "$S/usr/bin/pacific-browser"
cp -a "$ROOT"/dist/browser/lib/libpacificbrowser.so.1* "$S/usr/lib64/"
cp -a "$ROOT"/dist/browser/share/. "$S/usr/share/pacific-browser/"
cp -a "$ROOT"/dist/browser/translations/*.qm "$S/usr/share/translations/"

# Icons, drawn from the one SVG: the launcher's sizes, and the one the interface shows on its own pages.
python3 - "$ROOT/browser/data/icon-launcher-pacific.svg" "$S" <<'PY'
import os, sys, cairosvg
svg, stage = sys.argv[1], sys.argv[2]
for size in (86, 108, 128, 172, 256):
    directory = f"{stage}/usr/share/icons/hicolor/{size}x{size}/apps"
    os.makedirs(directory, exist_ok=True)
    cairosvg.svg2png(url=svg, write_to=f"{directory}/icon-launcher-pacific.png", output_width=size, output_height=size)
cairosvg.svg2png(url=svg, write_to=f"{stage}/usr/share/pacific-browser/data/icon-launcher-browser.png", output_width=544, output_height=544)
PY

# The launcher entry starts the browser inside Sailjail, and names itself as the profile to apply.
#  - No X-Maemo-Service: with it lipstick would start the browser through D-Bus activation, whose Exec is the
#    service file's. Those go through Sailjail too (below), so either way nothing runs unconfined.
#  - WebView is the permission the stock browser has for the system's transfer list (downloads) among other
#    things; the rest are what their names say.
#  - OrganizationName and ApplicationName are what Sailjail derives the allowed data directories and bus names
#    from; they are the ones the browser sets on its QGuiApplication.
cat > "$S/usr/share/applications/pacific-browser.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Pacific
Icon=icon-launcher-pacific
Exec=/usr/bin/sailjail -p pacific-browser.desktop /usr/bin/pacific-browser %U
Comment=Pacific Browser (Ladybird engine)
MimeType=text/html;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;

[X-Sailjail]
Permissions=Internet;Audio;Microphone;WebView;UserDirs;MediaIndexing;RemovableMedia;Downloads
OrganizationName=org.pacific
ApplicationName=pacificbrowser
DESKTOP

for name in org.pacific.browser org.pacific.browser.ui; do
    cat > "$S/usr/share/dbus-1/services/$name.service" <<DBUS
[D-BUS Service]
Name=$name
Exec=/usr/bin/sailjail -p pacific-browser.desktop /usr/bin/pacific-browser
DBUS
done

# Sailjail includes /etc/sailjail/permissions/<desktop file's name>.profile as a firejail profile.
cat > "$S/etc/sailjail/permissions/pacific-browser.profile" <<'PROFILE'
# -*- mode: sh -*-
# Pacific Browser: per-application Sailjail (firejail) profile, included because of the desktop file's name.

# The GPU is an Android driver behind libhybris. Base's disable-common.inc would hide these.
# FIXME: Needed for Vulkan; check whether GLES works without them (a Jolla Store app cannot ship this profile).
noblacklist /usr/libexec/droid-hybris
noblacklist /usr/libexec/pacific
noblacklist /odm
noblacklist /vendor/firmware_mnt

# The interface's QML and data. Base only lets through what is named under /usr/share.
whitelist /usr/share/pacific-browser

# The ambience wallpaper behind the start page and the toolbar, which the interface opens as a file.
whitelist /usr/share/ambience
whitelist ${HOME}/.cache/ambienced
mkdir     ${HOME}/.local/share/ambienced/wallpapers
whitelist ${HOME}/.local/share/ambienced/wallpapers

# The browser's own data. The engine keeps its profile and cache in an "engine" directory inside the first two
# (see LadybirdEngine::start), so there is nothing of its own to let through.
mkdir     ${HOME}/.local/share/org.pacific/pacificbrowser
whitelist ${HOME}/.local/share/org.pacific/pacificbrowser
mkdir     ${HOME}/.cache/org.pacific/pacificbrowser
whitelist ${HOME}/.cache/org.pacific/pacificbrowser
mkdir     ${HOME}/.config/org.pacific/pacificbrowser
whitelist ${HOME}/.config/org.pacific/pacificbrowser

# Where downloads are saved.
mkdir     ${HOME}/Downloads
whitelist ${HOME}/Downloads

# The bus names the browser owns: one running copy, and "open this link" from other applications.
dbus-user.own org.pacific.browser
dbus-user.own org.pacific.browser.ui
# "Downloads" in the menu opens Settings > Transfers.

env QT_QPA_PLATFORM=wayland
PROFILE

post="$ROOT/build/rpm-post.sh"
printf '#!/bin/sh\n/sbin/ldconfig || :\nupdate-desktop-database -q /usr/share/applications 2>/dev/null || :\n' > "$post"

# NB: No automatic dependencies: the engine brings its own libraries, and the scan would turn each of them into
#     a requirement that nothing on the phone provides.
fpm -s dir -t rpm -n pacific-browser -v "$VERSION" --iteration "$RELEASE" --architecture aarch64 \
    --rpm-summary "Pacific Browser (Ladybird engine)" \
    --description "A web browser for Sailfish OS on the Ladybird engine, with the Silica interface of Atlantic." \
    --license "BSD-2-Clause and MPL-2.0" --url "https://github.com/SpecSierra/Pacific" \
    --after-install "$post" --after-remove "$post" \
    --rpm-auto-add-directories --no-auto-depends --rpm-rpmbuild-define "_build_id_links none" \
    --depends sailjail --depends firejail --depends xdg-dbus-proxy \
    --depends sailfishsilica-qt5 --depends qt5-qtdeclarative \
    --force -p "$OUT/pacific-browser-$VERSION-$RELEASE.aarch64.rpm" -C "$S" .
ls -la "$OUT"
