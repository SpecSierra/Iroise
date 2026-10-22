#!/bin/bash
# Sourced by the CI build (ci-build.sh) and the engine tests (test-engine.sh), which both run build.sh and so must
# build the same: with the profile from scripts/pgo-profile.sh when CI has one. One made at an older pin still
# helps; PACIFIC_PGO=off builds without.
pgo_cache="${CI_CACHE:-/opt/pacific-ci/cache}"
if [ "${PACIFIC_PGO:-}" != off ] && [ -f "$pgo_cache/pgo/profile/stamp" ]; then
    export PACIFIC_PGO=use PACIFIC_PGO_DIR="$pgo_cache/pgo/profile"
    echo "Profile-guided optimisation with profile $(cat "$PACIFIC_PGO_DIR/stamp")"
else
    unset PACIFIC_PGO
fi
