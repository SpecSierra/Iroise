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
rm -rf "$NEW/rust"
mkdir -p "$NEW/rust"

# The Rust crates' counts: one file per library (%m), counted into as the processes run (%c, continuous mode).
LLVM_PROFILE_FILE="$NEW/rust/%c%m.profraw" PACIFIC_ENGINE="$UP/Build/pgo-generate/bin/Ladybird" \
    python3 "$ROOT/scripts/pgo-train.py" "$@"

# llvm-profdata of the LLVM inside the pinned rustc (rust-toolchain.toml), which wrote the counts.
(cd "$UP" && rustup component add llvm-tools >/dev/null)
host="$(cd "$UP" && rustc -vV | sed -n 's/^host: //p')"
profdata="$(cd "$UP" && rustc --print sysroot)/lib/rustlib/$host/bin/llvm-profdata"
raw="$(find "$NEW/rust" -name '*.profraw' | wc -l)"
echo "Rust profile: $raw files"
[ "$raw" -ge 1 ] || { echo "The Rust crates wrote no profile" >&2; exit 1; }
"$profdata" merge -o "$NEW/rust.profdata" "$NEW"/rust/*.profraw
rm -rf "$NEW/rust"
"$profdata" show "$NEW/rust.profdata" | tail -4

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
