#!/bin/bash
# DuckDuckGo's autoconsent (MPL-2.0), which answers sites' cookie-consent dialogs with "reject": the standalone
# build (rules built in), pinned by its npm tarball's SHA-256, into build/autoconsent/autoconsent.js. The engine
# runs it in every page while cookie-banner blocking is on (ui/SFOS/main.cpp, patch 0018). Atlantic's version.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=16.23.0
SHA256=b61907d0cae6f1f26679533c9951fe56573ca8038e12addfee5cb3f58de5fccc
OUT="$ROOT/build/autoconsent"
TARBALL="$OUT/autoconsent-$VERSION.tgz"

mkdir -p "$OUT"
if [ ! -s "$TARBALL" ]; then
    curl -fsSL "https://registry.npmjs.org/@duckduckgo/autoconsent/-/autoconsent-$VERSION.tgz" -o "$TARBALL.part"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$SHA256  $TARBALL" | sha256sum -c --quiet
tar -xzf "$TARBALL" -O package/dist/autoconsent.standalone.js > "$OUT/autoconsent.js.full"

# The standalone build starts itself with heuristic detection and verbose logs on. Detection only records which
# cookie-banner phrases a page contains, for reports nobody reads, and costs a forced layout plus ~140 regular
# expressions over the page's text up to 21 times per frame; the logs are thousands of console messages a page.
# Both off, as in Atlantic; the rules that answer dialogs are unchanged.
python3 - "$OUT/autoconsent.js.full" "$OUT/autoconsent.js" <<'PY'
import re, sys
source = open(sys.argv[1], encoding="utf-8").read()
start = source.find("const config = {\n      isMainWorld: true")
end = source.find("};", start)
if start < 0 or end < 0:
    sys.exit("autoconsent: the standalone config block was not found; check the new version by hand")
block = source[start:end]
for key in ("enableHeuristicDetection", "lifecycle", "rulesteps", "waits"):
    block, count = re.subn(r"\b%s: true\b" % key, "%s: false" % key, block)
    if count != 1:
        sys.exit("autoconsent: %s not found once in the config block" % key)
open(sys.argv[2], "w", encoding="utf-8").write(source[:start] + block + source[end:])
PY
rm "$OUT/autoconsent.js.full"
