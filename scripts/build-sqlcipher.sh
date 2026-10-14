#!/bin/bash
# Build the SQLCipher the password vault opens (browser/apps/browser/settings/credentialstore.cpp), into
# build/sqlcipher/libsqlcipher.so.0. The phone's own needs ICU, which a Jolla Store app may not link; this one
# needs only libcrypto.so.3. scripts/bundle.sh ships it in share/harbour-pacific/lib.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=4.6.1
SRC="$ROOT/build/sqlcipher-$VERSION"
OUT="$ROOT/build/sqlcipher"

if [ ! -d "$SRC" ]; then
    curl -fsSL "https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v$VERSION.tar.gz" | tar -C "$ROOT/build" -xz
fi
cd "$SRC"
[ -f Makefile ] || ./configure --enable-tempstore=yes --disable-tcl --disable-static --disable-static-shell \
    CFLAGS="-O2 -fPIC -DSQLITE_HAS_CODEC -DSQLITE_TEMP_STORE=2 -DSQLCIPHER_CRYPTO_OPENSSL" LDFLAGS="-lcrypto"
make -j"$(nproc)" libsqlcipher.la
mkdir -p "$OUT"
cp -L .libs/libsqlcipher.so.0 "$OUT/"
strip --strip-unneeded "$OUT/libsqlcipher.so.0"
readelf -d "$OUT/libsqlcipher.so.0" | grep NEEDED
