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
    # cocoa,zoom-to-fit=off (macOS): present the guest 1:1, NO host-side
    #   rescale.  zoom-to-fit=on bilinearly resamples the 1280x800 guest
    #   onto the (non-integer-scaled) Retina window; every small screen
    #   update then re-presents the whole scaled frame and the
    #   interpolation nudges static edges by +-1 px — a continuous
    #   "shimmer" that tracks mouse motion (looks like the compositor is
    #   tearing, but it is pure host scaling).  1:1 is crisp; the window
    #   is physically 1920x1200 device pixels.  The guest resolution is
    #   set in the multiboot header (kernel/hal/x86*/boot.s); raise it
    #   there for a bigger 1:1 window — do NOT re-enable non-integer
    #   zoom-to-fit.
    # -vga none -device VGA,vgamem_mb=32: the M22.6 page flip needs TWO
    #   full frames in VRAM.  At 1920x1200x32 that is 2*9.2 = ~18.4 MiB,
    #   over the std-VGA default of 16 MiB — without the bump the device
    #   clamps the virtual height, fb_flip_init bails, and we fall back to
    #   the (tearing) single-buffer blit.  (`-global VGA.vgamem_mb=` does
    #   NOT match the auto-created device — must replace it explicitly.)
    # -m 256M: two 1920x1200 heap surfaces (backbuffer + wallpaper) are
    #   ~9.2 MiB each, rounded to 16 MiB by the buddy allocator — well
    #   past QEMU's 128 MiB i386 default once the rest of the kernel is in.
    EXTRA="-m 256M -vga none -device VGA,vgamem_mb=32"
    if [ "$(uname -s)" = "Darwin" ]; then
        EXTRA="$EXTRA -display cocoa,zoom-to-fit=off"
    fi
    exec "$QEMU" -rtc base=localtime $EXTRA -cdrom "$ISO"
fi

echo "$QEMU not found on host; running headless inside Docker." >&2
exec docker run --rm -it -v "$PWD":/src d-os-build \
    "$QEMU" -nographic -cdrom "$ISO"
