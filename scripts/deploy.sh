#!/bin/bash
# Copy the bundle and the phone-side scripts to /tmp/ladybird on the dev phone.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8)
sshpass -e scp -P 2222 "${OPTS[@]}" "$ROOT/dist/ladybird.tar.gz" defaultuser@localhost:/tmp/
sshpass -e ssh -p 2222 "${OPTS[@]}" defaultuser@localhost \
    'sh /tmp/ladybird/stop.sh 2>/dev/null; cd /tmp && rm -rf ladybird && tar xzf ladybird.tar.gz && rm ladybird.tar.gz'
sshpass -e scp -P 2222 "${OPTS[@]}" "$ROOT"/scripts/phone/*.sh defaultuser@localhost:/tmp/ladybird/
if [ -d "$ROOT/dist/silica" ]; then
    sshpass -e scp -r -P 2222 "${OPTS[@]}" "$ROOT/dist/silica" defaultuser@localhost:/tmp/ladybird/
fi
