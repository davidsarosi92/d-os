#!/bin/sh
# fetch-freetype.sh — pin + clone FreeType (§M38 support libs, toward NetSurf).
# FreeType is the font-rasterisation library (FTL/GPL dual) NetSurf uses for
# text.  PRISTINE vendored dep (gitignored); cross-built vs musl → a versioned
# store package (soname libfreetype.so.6), closure pins zlib.
set -eu
cd "$(dirname "$0")/.."
DIR=third_party/freetype
URL=https://github.com/freetype/freetype.git
REF=VER-2-13-2
if [ ! -d "$DIR/.git" ]; then
    echo "Cloning FreeType into $DIR ..."
    git clone --depth 1 --branch "$REF" "$URL" "$DIR"
fi
echo "FreeType ready at $DIR ($REF)."
