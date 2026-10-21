#!/bin/bash
# Sourced by the CI scripts (ci-build.sh, pgo-profile.sh): the Ladybird checkout at the pin in CI_CACHE, linked as
# upstream/, and the Python venv with CMake. What is slow to make again lives in CI_CACHE and outlasts the run: the
# checkout with its build directories and vcpkg, the venv, and the ccache.
# Sets CACHE, UP and STAMP; write STAMP to $CACHE/upstream.stamp once a build of the checkout has succeeded.

CACHE="${CI_CACHE:-/opt/pacific-ci/cache}"
UP="$CACHE/upstream"
PIN="$(cat "$ROOT/UPSTREAM_COMMIT")"
mkdir -p "$CACHE"

export CCACHE_DIR="$CACHE/ccache" CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-30G}"

if [ ! -d "$CACHE/venv" ]; then
    python3 -m venv "$CACHE/venv"
    "$CACHE/venv/bin/pip" install -q 'cmake>=3.30'
fi
ln -sfn "$CACHE/venv" "$ROOT/.venv"

# The checkout is put back to the pin, as Ladybird has it, whenever the pin or our patches change; build.sh then
# applies the patches. Its build directories stay (Ladybird ignores Build/), so only what changed is built again.
if [ ! -d "$UP/.git" ]; then
    git clone -q https://github.com/LadybirdBrowser/ladybird.git "$UP"
fi
# NB: build.sh too, which decides how the patches go in.
STAMP="$(cat "$ROOT/UPSTREAM_COMMIT" "$ROOT"/patches/*.patch "$ROOT/scripts/build.sh" | sha256sum | cut -d' ' -f1)"
if [ "$(cat "$CACHE/upstream.stamp" 2>/dev/null)" != "$STAMP" ]; then
    git -C "$UP" cat-file -e "$PIN^{commit}" 2>/dev/null || git -C "$UP" fetch -q origin
    git -C "$UP" checkout -q -f --detach "$PIN"
    git -C "$UP" clean -q -fd
    rm -f "$CACHE/upstream.stamp"
fi
ln -sfn "$UP" "$ROOT/upstream"
