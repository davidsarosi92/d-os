#!/bin/sh
# =============================================================================
# fetch-zlib.sh — pin + clone zlib (§M38 support libs, toward NetSurf).
#
# zlib is the foundational compression library (permissive zlib license) that
# freetype/libpng/… + NetSurf's own gzip transport depend on.  Kept as a
# PRISTINE vendored dependency (gitignored, not forked), like musl/mbedTLS: we
# cross-build it against musl and install it into the content-addressed store as
# a versioned, swappable package (soname libz.so.1) — NOT into a global /usr.
#
# Run ON THE HOST once; then build in the container with `make zlib`.
# =============================================================================
set -eu
cd "$(dirname "$0")/.."

DIR=third_party/zlib
URL=https://github.com/madler/zlib.git
REF=v1.3.1                             # current stable release tag

if [ ! -d "$DIR/.git" ]; then
    echo "Cloning zlib into $DIR ..."
    git clone --depth 1 --branch "$REF" "$URL" "$DIR"
fi
git -C "$DIR" fetch --depth 1 origin "$REF" 2>/dev/null || true
git -C "$DIR" checkout "$REF" 2>/dev/null || echo "note: using current checkout"

echo "zlib ready at $DIR ($REF)."
echo "Next (in the container): make zlib ARCH=x86_64"
