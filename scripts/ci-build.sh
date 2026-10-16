#!/bin/bash
# The CI build (.github/workflows/build.yml): the engine, the interface, SQLCipher, the bundle and the RPM, as on the
# build host, into dist/rpm. What is slow to make again lives in CI_CACHE and outlasts the run: the Ladybird
# checkout with its build directory and vcpkg, the Python venv with CMake, and the ccache.
# Usage: scripts/ci-build.sh <rpm release>        (CI_CACHE, default /opt/pacific-ci/cache)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE="${1:?usage: ci-build.sh <rpm release>}"
CACHE="${CI_CACHE:-/opt/pacific-ci/cache}"
UP="$CACHE/upstream"
PIN="$(cat "$ROOT/UPSTREAM_COMMIT")"
VERSION="$(cat "$ROOT/VERSION")"
mkdir -p "$CACHE"

export CCACHE_DIR="$CACHE/ccache" CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-30G}"

if [ ! -d "$CACHE/venv" ]; then
    python3 -m venv "$CACHE/venv"
    "$CACHE/venv/bin/pip" install -q 'cmake>=3.30'
fi
ln -sfn "$CACHE/venv" "$ROOT/.venv"

# The checkout is put back to the pin, as Ladybird has it, whenever the pin or our patches change; build.sh then
# applies the patches. Its build directory stays (Ladybird ignores Build/), so only what changed is built again.
if [ ! -d "$UP/.git" ]; then
    git clone -q https://github.com/LadybirdBrowser/ladybird.git "$UP"
fi
stamp="$(cat "$ROOT/UPSTREAM_COMMIT" "$ROOT"/patches/*.patch | sha256sum | cut -d' ' -f1)"
if [ "$(cat "$CACHE/upstream.stamp" 2>/dev/null)" != "$stamp" ]; then
    git -C "$UP" cat-file -e "$PIN^{commit}" 2>/dev/null || git -C "$UP" fetch -q origin
    git -C "$UP" checkout -q -f --detach "$PIN"
    git -C "$UP" clean -q -fd
    rm -f "$CACHE/upstream.stamp"
fi
ln -sfn "$UP" "$ROOT/upstream"

"$ROOT/scripts/build.sh"
echo "$stamp" > "$CACHE/upstream.stamp"
"$ROOT/scripts/build-browser.sh"
"$ROOT/scripts/build-sqlcipher.sh"
"$ROOT/scripts/bundle.sh"
"$ROOT/scripts/package-rpm.sh" "$VERSION" "$RELEASE"
ccache -s | sed -n '1,4p'
