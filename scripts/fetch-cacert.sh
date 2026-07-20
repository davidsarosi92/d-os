#!/usr/bin/env sh
# =============================================================================
# fetch-cacert.sh — vendor a public CA trust bundle for §M39 (real HTTPS).
#
# d-os provisions this bundle into the VFS at /etc/ssl/cert.pem at boot; musl
# TLS clients (httpstest, a future wget) load it as the mbedTLS trust store and
# verify server certificates against it — REAL certificate verification, not
# VERIFY_NONE theatre.
#
# Canonical source: the Mozilla CA set as repackaged by the curl project
# (https://curl.se/ca/cacert.pem), the de-facto portable PEM bundle.  Pinned by
# vendoring the file into the repo (third_party/cacert.pem); this script only
# refreshes it.  Falls back to the host's system bundle when offline (both are
# the same Mozilla set), so the build stays reproducible without the network.
# =============================================================================
set -e
DEST="third_party/cacert.pem"
URL="https://curl.se/ca/cacert.pem"

if command -v curl >/dev/null 2>&1 && curl -fsSL "$URL" -o "$DEST.tmp" 2>/dev/null; then
    mv "$DEST.tmp" "$DEST"
    echo "fetch-cacert: downloaded $URL -> $DEST"
elif [ -f /etc/ssl/cert.pem ]; then
    cp /etc/ssl/cert.pem "$DEST"
    echo "fetch-cacert: offline — copied system bundle /etc/ssl/cert.pem -> $DEST"
else
    echo "fetch-cacert: could not fetch $URL and no system bundle found" >&2
    exit 1
fi
echo "fetch-cacert: $(grep -c 'BEGIN CERT' "$DEST") certificates, $(wc -c <"$DEST") bytes"
