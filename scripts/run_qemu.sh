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

# -----------------------------------------------------------------------------
# AArch64 (M21) is booted very differently from the x86 ports: no GRUB / no
# ISO, no VGA / no framebuffer — QEMU's `virt` board loads the raw kernel ELF
# via `-kernel` and the console is the PL011 UART on `-nographic`.  Handle it
# up front and exit; the x86 path below is unchanged.
# -----------------------------------------------------------------------------
if [ "$ARCH" = "aarch64" ]; then
    QEMU=qemu-system-aarch64
    KERNEL=build/aarch64/kernel.bin
    if [ ! -f "$KERNEL" ]; then
        echo "Kernel not found at $KERNEL — run ARCH=aarch64 scripts/build.sh first." >&2
        exit 1
    fi
    # -M virt,gic-version=2: the generic AArch64 board.  Pin GICv2 explicitly
    #   so the interrupt-controller MMIO layout the kernel hard-codes (GICD @
    #   0x08000000, GICC @ 0x08010000) always matches — newer QEMU may default
    #   the board to GICv3, whose programming model is different.
    # -cpu cortex-a72: a widely-available AArch64 core with a stable feature
    #   set (matches the milestone's DoD target).
    # -device virtio-gpu-device: the M21 Phase-I framebuffer.  QEMU `virt` has no
    #   VGA/Bochs-VBE and no linear-VRAM BAR, so the display is a virtio-gpu on a
    #   virtio-mmio slot; the kernel drives it with a 2D scanout and renders the
    #   boot log / shell into a RAM framebuffer.  QEMU opens a graphical window
    #   for it (the default host display).
    # -serial mon:stdio: route the PL011 UART + the QEMU monitor to the terminal
    #   (Ctrl-A C toggles between them, Ctrl-A X quits) now that we no longer use
    #   -nographic (which would suppress the graphical window).
    # -smp 2: the M21 Phase-E SMP bring-up starts the secondary core via PSCI.
    #   Keep this in sync with AARCH64_MAX_CPUS in kernel/hal/aarch64/smp.c.
    # -global virtio-mmio.force-legacy=false: the M21 virtio-mmio drivers (blk +
    #   gpu + input) speak the MODERN (version 2) transport; QEMU `virt` defaults
    #   its virtio-mmio slots to legacy (version 1), so force modern.
    # -device virtio-keyboard-device / virtio-mouse-device: the M21 Phase-J/K
    #   input path (virtio_input.c) — keyboard → VC/shell, relative mouse → the
    #   GUI compositor.  QEMU `virt` has no PS/2.
    # -rtc base=localtime: PL031 RTC values match the host clock (taskbar clock).
    QEMU_MACHINE="-M virt,gic-version=2 -cpu cortex-a72 -smp 2 -m 256M \
        -serial mon:stdio -rtc base=localtime \
        -device virtio-gpu-device -device virtio-keyboard-device \
        -device virtio-mouse-device \
        -global virtio-mmio.force-legacy=false"

    # Attach a virtio-blk disk if build/aarch64/disk.img exists (create one with
    #   `dd if=/dev/zero of=build/aarch64/disk.img bs=1M count=4`).
    DISK="build/aarch64/disk.img"
    DISK_ARGS=""
    if [ -f "$DISK" ]; then
        DISK_ARGS="-drive file=$DISK,if=none,id=hd0,format=raw \
                   -device virtio-blk-device,drive=hd0"
    fi

    # M21 Phase H: QEMU's direct-ELF `-kernel` entry passes no DTB pointer (x0=0)
    #   and places no DTB in RAM, so load one at a fixed address (0x48000000) for
    #   the kernel's device-tree parser to discover RAM size + CPU count.  The
    #   kernel falls back to built-in defaults if it is absent.  Generate the DTB
    #   for THIS machine config once with:
    #     qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -smp 2 \
    #        -m 256M -machine dumpdtb=build/aarch64/virt.dtb
    DTB="build/aarch64/virt.dtb"
    DTB_ARGS=""
    if [ -f "$DTB" ]; then
        DTB_ARGS="-device loader,file=$DTB,addr=0x48000000,force-raw=on"
    fi

    if command -v "$QEMU" >/dev/null 2>&1; then
        exec "$QEMU" $QEMU_MACHINE -kernel "$KERNEL" $DISK_ARGS $DTB_ARGS
    fi
    echo "$QEMU not found on host; running headless inside Docker." >&2
    exec docker run --rm -it -v "$PWD":/src d-os-build-aarch64 \
        "$QEMU" $QEMU_MACHINE -kernel "$KERNEL" $DISK_ARGS $DTB_ARGS
fi

ISO=build/$ARCH/d-os.iso
case "$ARCH" in
    i386)   QEMU=qemu-system-i386 ;;
    x86_64) QEMU=qemu-system-x86_64 ;;
    *) echo "Unsupported ARCH '$ARCH' — supported: i386, x86_64, aarch64" >&2; exit 1 ;;
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
