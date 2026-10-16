#!/bin/bash
# Copy the app (scripts/bundle.sh) and the phone-side scripts to /tmp/ladybird on the dev phone, laid out as it is
# installed: /tmp/ladybird/bin/harbour-pacific, /tmp/ladybird/share/harbour-pacific/...
# The phone is reached over SSH as defaultuser: PACIFIC_PHONE (default 127.0.0.1), PACIFIC_PHONE_PORT (default 2222,
# a forwarded port), and the phone's developer-mode password in PACIFIC_PHONE_PASSWORD (or use an SSH key and leave
# it unset).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="defaultuser@${PACIFIC_PHONE:-127.0.0.1}"
PORT="${PACIFIC_PHONE_PORT:-2222}"
OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8)
run() {
    if [ -n "${PACIFIC_PHONE_PASSWORD:-}" ]; then
        SSHPASS="$PACIFIC_PHONE_PASSWORD" sshpass -e "$@"
    else
        "$@"
    fi
}
run scp -P "$PORT" "${OPTS[@]}" "$ROOT/dist/harbour-pacific.tar.gz" "$HOST:/tmp/"
run ssh -p "$PORT" "${OPTS[@]}" "$HOST" \
    'sh /tmp/ladybird/stop.sh 2>/dev/null; rm -rf /tmp/ladybird && mkdir /tmp/ladybird &&
     tar xzf /tmp/harbour-pacific.tar.gz -C /tmp/ladybird --strip-components=1 && rm /tmp/harbour-pacific.tar.gz'
run scp -P "$PORT" "${OPTS[@]}" "$ROOT"/scripts/phone/*.sh "$HOST:/tmp/ladybird/"
