#!/bin/sh
# fetch-harfbuzz.sh — pin + clone HarfBuzz (§M38 support libs, toward NetSurf).
# HarfBuzz is the text-shaping engine (MIT) NetSurf uses with FreeType.  PRISTINE
# vendored dep (gitignored); cross-built vs musl → a versioned store package
# (soname libharfbuzz.so.0), closure pins freetype (+ zlib + libstdc++).
set -eu
cd "$(dirname "$0")/.."
DIR=third_party/harfbuzz
URL=https://github.com/harfbuzz/harfbuzz.git
REF=8.5.0
if [ ! -d "$DIR/.git" ]; then
    echo "Cloning HarfBuzz into $DIR ..."
    git clone --depth 1 --branch "$REF" "$URL" "$DIR"
fi
echo "HarfBuzz ready at $DIR ($REF)."
