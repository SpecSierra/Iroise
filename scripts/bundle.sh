#!/bin/bash
# Collect a relocatable bundle in dist/ladybird: binaries, resources, and every
# library that came out of our own build tree, plus gcc 14's libstdc++/libgcc_s
# (the device's libstdc++ is older). Everything else resolves on the device.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/upstream/Build/release"
OUT="$ROOT/dist/ladybird"

rm -rf "$OUT"
mkdir -p "$OUT/lib"
cp -a "$BUILD/bin" "$BUILD/libexec" "$BUILD/share" "$OUT/"

for f in "$OUT"/bin/* "$OUT"/libexec/*; do ldd "$f" 2>/dev/null; done |
    awk '/=>/ {print $3}' | sort -u |
    grep -E "^$BUILD/|/libstdc\+\+\.so|/libgcc_s\.so" |
    while read -r lib; do cp -L "$lib" "$OUT/lib/"; done

strip --strip-unneeded "$OUT"/lib/*.so* "$OUT"/libexec/* "$OUT"/bin/* 2>/dev/null || true

# The build bakes in an absolute build-host RUNPATH, and helper processes are
# spawned without our LD_LIBRARY_PATH, so make every ELF find lib/ by itself.
for f in "$OUT"/bin/* "$OUT"/libexec/*; do patchelf --set-rpath '$ORIGIN/../lib' "$f"; done
for f in "$OUT"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$f"; done

cat > "$OUT/run.sh" <<'RUN'
#!/bin/sh
HERE="$(cd "$(dirname "$0")" && pwd)"
exec "$HERE/bin/Ladybird" "$@"
RUN
chmod +x "$OUT/run.sh"

tar -C "$ROOT/dist" -czf "$ROOT/dist/ladybird.tar.gz" ladybird
du -sh "$OUT" "$ROOT/dist/ladybird.tar.gz"
