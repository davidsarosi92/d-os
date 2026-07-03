#!/bin/sh
# Boot the built ISO in QEMU. Prefers a host-installed qemu (faster, gives
# you a real window); falls back to the Docker image with -nographic.
#
# Default arch is i386 (the reference port).  Override with ARCH=x86_64
# to boot the long-mode build:
#     ARCH=x86_64 ./scripts/run_qemu.sh

set -eu

cd "$(dirname "$0")/.."

ARCH=${ARCH:-i386}
ISO=build/$ARCH/d-os.iso
case "$ARCH" in
    i386)   QEMU=qemu-system-i386 ;;
    x86_64) QEMU=qemu-system-x86_64 ;;
    *) echo "Unsupported ARCH '$ARCH' — supported: i386, x86_64" >&2; exit 1 ;;
esac

if [ ! -f "$ISO" ]; then
    echo "ISO not found at $ISO — run scripts/build.sh first (with matching ARCH)." >&2
    exit 1
fi

if command -v "$QEMU" >/dev/null 2>&1; then
    # -rtc base=localtime: the GUI taskbar clock reads the CMOS RTC;
    #   without this QEMU feeds it UTC.
    # cocoa,zoom-to-fit (macOS): make the guest display scale with the
    #   window — on a Retina panel the raw 1280x800 window is tiny.
    EXTRA=""
    if [ "$(uname -s)" = "Darwin" ]; then
        EXTRA="-display cocoa,zoom-to-fit=on"
    fi
    exec "$QEMU" -rtc base=localtime $EXTRA -cdrom "$ISO"
fi

echo "$QEMU not found on host; running headless inside Docker." >&2
exec docker run --rm -it -v "$PWD":/src d-os-build \
    "$QEMU" -nographic -cdrom "$ISO"
