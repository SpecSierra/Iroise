#!/bin/sh
# The RUSTC_WORKSPACE_WRAPPER of profile-guided builds (scripts/build.sh): rustc, with PACIFIC_PGO_RUSTFLAGS for the
# engine's own crates. Their build scripts and procedural macros run on the build host and are left alone.
rustc="$1"
shift
case " $* " in
    *" --crate-name build_script_"* | *" --crate-type proc-macro "*) exec "$rustc" "$@" ;;
esac
# shellcheck disable=SC2086
exec "$rustc" "$@" $PACIFIC_PGO_RUSTFLAGS
