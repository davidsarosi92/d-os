#!/bin/sh
# =============================================================================
# fetch-mbedtls.sh — pin + clone Mbed TLS (§M39 stage 2).
#
# Mbed TLS is a small, self-contained C crypto + TLS library (Apache-2.0) — the
# PLAN's chosen first crypto port.  Kept as a PRISTINE vendored dependency
# (gitignored, not forked), like musl: d-os provides the C library + syscalls it
# needs (getrandom for entropy, the M24 socket API for the TLS BIO); we do NOT
# patch its crypto.  Built for i686-musl by `make mbedtls`.
#
# Run ON THE HOST once; then build in the container.
# =============================================================================
set -eu
cd "$(dirname "$0")/.."

DIR=third_party/mbedtls
URL=https://github.com/Mbed-TLS/mbedtls.git
REF=v3.6.2                            # current LTS tag

if [ ! -d "$DIR/.git" ]; then
    echo "Cloning Mbed TLS into $DIR ..."
    git clone --depth 1 --branch "$REF" "$URL" "$DIR"
fi
git -C "$DIR" fetch --depth 1 origin "$REF" 2>/dev/null || true
git -C "$DIR" checkout "$REF" 2>/dev/null || echo "note: using current checkout"

echo "Mbed TLS ready at $DIR ($REF)."
echo "Next (in the container): make mbedtls"
