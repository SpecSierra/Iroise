#!/bin/bash
# Makes the profile for profile-guided optimisation (.github/workflows/pgo.yml): builds the instrumented engine
# (scripts/build.sh, PACIFIC_PGO=generate), trains it (scripts/pgo-train.py), and keeps the profile in
# $CACHE/pgo/profile, where scripts/ci-build.sh finds it. Run again after a Ladybird bump; until then the old profile
# still helps. Also packs it into dist/pgo-profile.tar.gz.
# Usage: scripts/pgo-profile.sh [seconds for Speedometer]        (CI_CACHE, default /opt/pacific-ci/cache)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/ci-checkout.sh"

NEW="$CACHE/pgo/new"
rm -rf "$NEW"
mkdir -p "$NEW"
# NB: Not exec'd as in CI's build: the instrumented build's own tools write profiles while it builds; they are
#     cleared below.
PACIFIC_PGO=generate PACIFIC_PGO_DIR="$NEW" "$ROOT/scripts/build.sh"
echo "$STAMP" > "$CACHE/upstream.stamp"
find "$NEW" -name '*.gcda' -delete

PACIFIC_ENGINE="$UP/Build/pgo-generate/bin/Ladybird" python3 "$ROOT/scripts/pgo-train.py" "$@"

count="$(find "$NEW" -name '*.gcda' | wc -l)"
echo "Profile: $count files"
# A training that ran is thousands of files; a handful means the engine never got going.
[ "$count" -ge 500 ] || { echo "Too few profile files: the training did not run" >&2; exit 1; }
echo "$(cut -c1-12 "$ROOT/UPSTREAM_COMMIT")-$(date -u +%Y%m%d%H%M)" > "$NEW/stamp"
rm -rf "$CACHE/pgo/profile"
mv "$NEW" "$CACHE/pgo/profile"
mkdir -p "$ROOT/dist"
tar -C "$CACHE/pgo" -czf "$ROOT/dist/pgo-profile.tar.gz" profile
du -sh "$CACHE/pgo/profile" "$ROOT/dist/pgo-profile.tar.gz"
