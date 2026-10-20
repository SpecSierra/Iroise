#!/bin/bash
# Run the engine with our front end (ui/SFOS) as the interface runs it, over the embedding protocol, and check what
# it does with a finger and what it tells the interface (tests/embed). CI runs it after every build, on the bundle
# scripts/bundle.sh laid out; each test's engine log lands in dist/front-end-test-logs.
# Usage: scripts/test-front-end.sh [unittest arguments...]   (e.g. test_engine.Scrolling for some of them)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PACIFIC_ENGINE="${PACIFIC_ENGINE:-$ROOT/dist/harbour-pacific/bin/harbour-pacific}"
export PACIFIC_TEST_LOGS="$ROOT/dist/front-end-test-logs"
rm -rf "$PACIFIC_TEST_LOGS"
cd "$ROOT/tests/embed"
exec python3 -m unittest -v "${@:-test_engine}"
