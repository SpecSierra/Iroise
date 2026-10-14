#!/bin/bash
# Copy the app (scripts/bundle.sh) and the phone-side scripts to /tmp/ladybird on the dev phone, laid out as it is
# installed: /tmp/ladybird/bin/harbour-pacific, /tmp/ladybird/share/harbour-pacific/...
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8)
sshpass -e scp -P 2222 "${OPTS[@]}" "$ROOT/dist/harbour-pacific.tar.gz" defaultuser@localhost:/tmp/
sshpass -e ssh -p 2222 "${OPTS[@]}" defaultuser@localhost \
    'sh /tmp/ladybird/stop.sh 2>/dev/null; rm -rf /tmp/ladybird && mkdir /tmp/ladybird &&
     tar xzf /tmp/harbour-pacific.tar.gz -C /tmp/ladybird --strip-components=1 && rm /tmp/harbour-pacific.tar.gz'
sshpass -e scp -P 2222 "${OPTS[@]}" "$ROOT"/scripts/phone/*.sh defaultuser@localhost:/tmp/ladybird/
