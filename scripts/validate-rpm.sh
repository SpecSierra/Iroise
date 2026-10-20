#!/bin/bash
# Check a package against the Jolla Store's rules with the store's own validator (sailfishos/sdk-harbour-rpmvalidator),
# at the commit pinned below. Fails on any error; warnings are only shown. CI runs it on every package.
# Usage: scripts/validate-rpm.sh <package.rpm>        (VALIDATOR_DIR, default $CI_CACHE/harbour-rpmvalidator or
#                                                       ~/.cache/harbour-rpmvalidator)
# NB: The rules change with the validator, so a newer one is taken on purpose: move the pin, run it, fix what it finds.
set -euo pipefail

VALIDATOR_COMMIT=7dd7dd5d62aa3e814c92ade089fdca4c3872b8ac

RPM="$(readlink -f "${1:?usage: validate-rpm.sh <package.rpm>}")"
DIR="${VALIDATOR_DIR:-${CI_CACHE:+$CI_CACHE/harbour-rpmvalidator}}"
DIR="${DIR:-$HOME/.cache/harbour-rpmvalidator}"

if [ ! -d "$DIR/.git" ]; then
    git clone -q https://github.com/sailfishos/sdk-harbour-rpmvalidator.git "$DIR"
fi
git -C "$DIR" cat-file -e "$VALIDATOR_COMMIT^{commit}" 2>/dev/null || git -C "$DIR" fetch -q origin
git -C "$DIR" checkout -q -f --detach "$VALIDATOR_COMMIT"

# The validator unpacks the package where it runs.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
if bash "$DIR/rpmvalidation.sh" -g "$DIR" "$RPM" > report.txt 2>&1; then
    status=0
else
    status=1
fi
# What failed or warns, without the colours; the full report too when it failed.
sed 's/\x1b\[[0-9;]*m//g' report.txt > plain.txt
grep -E "^(ERROR|WARNING)|^Validation " plain.txt || true
if [ $status -ne 0 ]; then
    echo "--- full report ---"
    cat plain.txt
fi
exit $status
