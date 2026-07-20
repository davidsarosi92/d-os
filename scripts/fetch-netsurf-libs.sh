#!/bin/sh
# =============================================================================
# fetch-netsurf-libs.sh — pin + clone NetSurf's own component libraries (§M42
# validation target).  These are the browser's parsing/DOM/decoding libs, each
# a small C library from git.netsurf-browser.org: libwapcaplet (string intern),
# libparserutils, libhubbub (HTML), libcss (CSS), libdom (DOM), libnsgif/
# libnsbmp (image decoders).  PRISTINE vendored deps (gitignored); cross-built
# vs musl into versioned store packages, like zlib/freetype/harfbuzz.
# =============================================================================
set -eu
cd "$(dirname "$0")/.."
BASE=https://git.netsurf-browser.org
LIBS="${*:-libwapcaplet}"
for lib in $LIBS; do
    DIR=third_party/$lib
    if [ ! -d "$DIR/.git" ]; then
        echo "Cloning $lib ..."
        git clone --depth 1 "$BASE/$lib.git" "$DIR" || \
        git clone --depth 1 "git://git.netsurf-browser.org/$lib.git" "$DIR"
    fi
    echo "$lib ready at $DIR"
done
