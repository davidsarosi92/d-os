#!/bin/sh
# =============================================================================
# fetch-libpng.sh — pin + clone libpng (§M38 support libs, toward NetSurf).
#
# libpng is the reference PNG image codec (libpng license); it DEPENDS ON zlib
# (deflate/inflate).  Like zlib/musl/mbedTLS it is a PRISTINE vendored dep
# (gitignored, not forked): cross-built against musl and installed into the
# content-addressed store as a versioned package (soname libpng16.so.16) whose
# dependency CLOSURE pins zlib — the first support lib with a real store dep.
#
# Run ON THE HOST once; then build in the container with `make libpng`.
# =============================================================================
set -eu
cd "$(dirname "$0")/.."

DIR=third_party/libpng
URL=https://github.com/pnggroup/libpng.git
REF=v1.6.43                            # current stable release tag

if [ ! -d "$DIR/.git" ]; then
    echo "Cloning libpng into $DIR ..."
    git clone --depth 1 --branch "$REF" "$URL" "$DIR"
fi
git -C "$DIR" fetch --depth 1 origin "$REF" 2>/dev/null || true
git -C "$DIR" checkout "$REF" 2>/dev/null || echo "note: using current checkout"

echo "libpng ready at $DIR ($REF).  Needs zlib fetched too (scripts/fetch-zlib.sh)."
echo "Next (in the container): make libpng ARCH=x86_64"
