#!/bin/sh
# =============================================================================
# fetch-wayland.sh — pin + clone UPSTREAM libwayland and libffi (§M40).
#
# §M26 built d-os's own Wayland SERVER plus a hand-written mini client library
# (user/libwl).  §M40 is the other half: run the REAL libwayland-client, so an
# unmodified Wayland application — eventually GTK/Qt/SDL — links against the
# same library it links against on Linux.
#
# Two pristine vendored dependencies (gitignored, never forked):
#
#   wayland — the client library itself.  Only a handful of C files matter
#             (wayland-client.c, connection.c, wayland-util.c, wayland-os.c)
#             plus the code wayland-scanner GENERATES from the protocol XML.
#             The scanner runs on the HOST (apt's wayland-scanner, see the
#             Dockerfile); nothing generated is committed.
#
#   libffi  — libwayland dispatches an incoming event by building an argument
#             list at runtime and calling the listener through ffi_call.  There
#             is no way around it short of forking the library, which is exactly
#             what this milestone exists NOT to do.
#
# Run ON THE HOST once; then build in the container with `make wayland`.
# =============================================================================
set -eu
cd "$(dirname "$0")/.."

clone() {   # clone <dir> <url> <ref>
    dir=$1; url=$2; ref=$3
    if [ ! -d "$dir/.git" ]; then
        echo "Cloning $(basename "$dir") into $dir ..."
        git clone --depth 1 --branch "$ref" "$url" "$dir"
    fi
    git -C "$dir" fetch --depth 1 origin "$ref" 2>/dev/null || true
    git -C "$dir" checkout "$ref" 2>/dev/null || echo "note: $dir using current checkout"
}

# 1.22.0 is the last release whose client library still builds with a plain
# C compiler invocation (no meson-only generated headers beyond the scanner's).
clone third_party/wayland https://gitlab.freedesktop.org/wayland/wayland.git 1.22.0
clone third_party/libffi  https://github.com/libffi/libffi.git              v3.4.6

echo "Wayland + libffi ready."
echo "Next (in the container): make wayland            # i386"
echo "                          make ARCH=x86_64 wayland"
