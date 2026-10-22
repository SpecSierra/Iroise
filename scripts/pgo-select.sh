#!/bin/bash
# Sourced by the CI build (ci-build.sh) and the engine tests (test-engine.sh), which both run build.sh and so must
# build the same: with the profile from scripts/pgo-profile.sh when CI has one. One made at an older pin still
# helps; IROISE_PGO=off builds without.
pgo_cache="${CI_CACHE:-/opt/pacific-ci/cache}"
if [ "${IROISE_PGO:-}" != off ] && [ -f "$pgo_cache/pgo/profile/stamp" ]; then
    export IROISE_PGO=use IROISE_PGO_DIR="$pgo_cache/pgo/profile"
    echo "Profile-guided optimisation with profile $(cat "$IROISE_PGO_DIR/stamp")"
else
    unset IROISE_PGO
fi
