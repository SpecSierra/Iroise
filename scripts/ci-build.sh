#!/bin/bash
# The CI build (.github/workflows/build.yml): the engine, the interface, SQLCipher, the bundle and the RPM, as on the
# build host, into dist/rpm. The Ladybird checkout and what outlasts the run: scripts/ci-checkout.sh.
# Usage: scripts/ci-build.sh <rpm release>        (CI_CACHE, default /opt/pacific-ci/cache)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE="${1:?usage: ci-build.sh <rpm release>}"
VERSION="$(cat "$ROOT/VERSION")"
source "$ROOT/scripts/ci-checkout.sh"

# With a profile from scripts/pgo-profile.sh (.github/workflows/pgo.yml), the engine is optimised with it. One
# made at an older pin still helps; PACIFIC_PGO=off builds without.
if [ "${PACIFIC_PGO:-}" != off ] && [ -f "$CACHE/pgo/profile/stamp" ]; then
    export PACIFIC_PGO=use PACIFIC_PGO_DIR="$CACHE/pgo/profile"
    echo "Profile-guided optimisation with profile $(cat "$PACIFIC_PGO_DIR/stamp")"
else
    unset PACIFIC_PGO
fi

"$ROOT/scripts/build.sh"
echo "$STAMP" > "$CACHE/upstream.stamp"
"$ROOT/scripts/build-browser.sh"
"$ROOT/scripts/build-sqlcipher.sh"
"$ROOT/scripts/bundle.sh"
"$ROOT/scripts/package-rpm.sh" "$VERSION" "$RELEASE"
ccache -s | sed -n '1,4p'
