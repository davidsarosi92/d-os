#!/bin/sh
# =============================================================================
# fetch-musl.sh — vendor an UNMODIFIED musl at a pinned version (§M36).
#
# Modularity: musl is a clean external dependency — we do NOT fork it or patch
# its syscall layer.  It is fetched (not committed) into third_party/musl at a
# pinned tag, kept pristine; d-os provides the Linux i386 syscall ABI it expects
# (kernel/hal/x86/linux_abi.c).  Re-run to (re)materialise the tree.
#
# Usage:  ./scripts/fetch-musl.sh
# =============================================================================
set -eu

cd "$(dirname "$0")/.."

MUSL_VERSION="v1.2.5"                 # pinned; bump deliberately
DEST="third_party/musl"

if [ -d "$DEST/.git" ]; then
    echo "musl already present at $DEST (checkout $MUSL_VERSION)"
    git -C "$DEST" fetch --tags --depth 1 origin "$MUSL_VERSION" 2>/dev/null || true
    git -C "$DEST" checkout -q "$MUSL_VERSION"
    exit 0
fi

mkdir -p third_party
echo "cloning musl $MUSL_VERSION → $DEST ..."
git clone --depth 1 --branch "$MUSL_VERSION" \
    https://git.musl-libc.org/git/musl "$DEST" 2>/dev/null \
  || git clone --depth 1 --branch "$MUSL_VERSION" \
    https://github.com/bminor/musl "$DEST"

echo "musl $MUSL_VERSION fetched (pristine).  Build it with 'make musl' inside"
echo "the build container (see third_party/MUSL.md)."
