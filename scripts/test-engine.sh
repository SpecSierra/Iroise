#!/bin/bash
# Run Ladybird's own tests for what our patches change, and our own tests of them (patches/0025), with Ladybird's
# test runner, headless on the build host. CI runs it after every build; the results land in dist/test-results.
# Usage: scripts/test-engine.sh [test-web arguments...]   (e.g. -f 'Text/input/UIEvents/touch-*' for ours only)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UP="$ROOT/upstream"
BUILD="$UP/Build/release"
JOBS="${JOBS:-12}"
RESULTS="$ROOT/dist/test-results"

"$ROOT/scripts/build.sh" test-web

# What the patches change, by the tests Ladybird keeps for it: input and pointer events (0021-0025), wheel and
# scrolling (0022, 0023), and DOM events (0021, 0022).
filters=(
    -f 'Text/input/UIEvents/*'
    -f 'Text/input/wpt-import/pointerevents/*'
    -f 'Text/input/wpt-import/uievents/*'
    -f 'Text/input/wpt-import/dom/events/*'
    -f 'Text/input/*wheel*'
    -f 'Text/input/*scroll*'
)
[ $# -gt 0 ] && filters=("$@")

rm -rf "$RESULTS"
mkdir -p "$RESULTS"
cd "$UP"
LADYBIRD_SOURCE_DIR="$UP" "$BUILD/bin/test-web" --test-path "$UP/Tests/LibWeb" --results-dir "$RESULTS" \
    --test-concurrency "$JOBS" --per-test-timeout 60 --python-executable "$(command -v python3)" "${filters[@]}"
