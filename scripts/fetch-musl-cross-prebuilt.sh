#!/bin/sh
# =============================================================================
# fetch-musl-cross-prebuilt.sh — download a PREBUILT musl cross-toolchain
# (musl.cc) instead of building one from source (§M38/§M43, x86_64).
#
# Building gcc from source (scripts/fetch-musl-cross.sh + `make musl-cross-*`)
# takes ~10h under amd64 emulation on Apple Silicon.  For x86_64 we use the
# prebuilt musl.cc toolchain instead: a few minutes to download, no build.  It
# ships gcc 11.2.1 + g++ + a musl sysroot (libstdc++.so.6 + libc.so + crt), and
# runs in the linux/amd64 build container (emulated, but only for the actual
# — fast — compilation, not a gcc build).  It carries its own musl (ABI-
# compatible with our vendored 1.2.x).  Gitignored, like the from-source builds.
#
# Run ON THE HOST once; the toolchain lands at third_party/musl-cross-x86_64/.
# =============================================================================
set -eu
cd "$(dirname "$0")/.."

TARGET=${1:-x86_64-linux-musl}
URL="https://musl.cc/${TARGET}-cross.tgz"
DEST="third_party/musl-cross-${TARGET%%-*}"        # x86_64-linux-musl → musl-cross-x86_64
TMP="/tmp/${TARGET}-cross.tgz"

if [ ! -x "$DEST/bin/${TARGET}-gcc" ]; then
    echo "Downloading prebuilt $TARGET toolchain from $URL ..."
    curl -fL --connect-timeout 20 -o "$TMP" "$URL"
    rm -rf third_party/"${TARGET}-cross" "$DEST"
    ( cd third_party && tar xzf "$TMP" && mv "${TARGET}-cross" "musl-cross-${TARGET%%-*}" )
    rm -f "$TMP"
fi
echo "Prebuilt toolchain ready: $DEST/bin/${TARGET}-{gcc,g++}"
