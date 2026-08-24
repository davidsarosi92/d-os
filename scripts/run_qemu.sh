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
# STORAGE MODE — the same three switches on every arch and every wrapper.
#
# d-os has two genuinely different personalities and BOTH have to stay one
# command away, because each is the honest one for a different question:
#
#   (default)  keep   — attach the existing disk, creating it on first use.
#                       This is the machine that remembers: settings survive a
#                       reboot, the package store can live on disk.
#   --empty           — attach a disk, but throw the old one away and hand the
#                       kernel a freshly formatted, EMPTY volume.  This is what
#                       reproduces a first boot: no config, no store, nothing
#                       carried over from a run whose state nobody remembers.
#   --no-disk         — no storage at all.  The mode where "will NOT survive a
#                       reboot" is the truth, and where every provisioning path
#                       has to work from nothing.
#
# They are FLAGS rather than an environment variable because a mode you have to
# remember the spelling of is a mode that gets tested once.  (`DOS_DISK=none`
# still works — it predates these and scripts use it.)
#
# Parsed here, before the arch branch, so all three arches and all three
# `run-<arch>.sh` wrappers get identical behaviour: the wrappers pass "$@"
# straight through, and an option that means one thing on i386 and another on
# ARM would be worse than no option.
# -----------------------------------------------------------------------------
DISK_MODE=keep
QEMU_EXTRA_ARGS=""

usage() {
    cat >&2 <<'EOF'
usage: run_qemu.sh [--empty | --no-disk] [-- <extra qemu args>]

  (no flag)   attach the persistent disk (created + formatted on first use)
  --empty     wipe that disk and attach a FRESHLY FORMATTED, empty one
  --no-disk   run with no storage at all (RAM only; nothing persists)

  ARCH=i386|x86_64|aarch64   selects the build to boot (default: i386)
  SMP=<n>                    number of vCPUs on the x86 arches (default: 4)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --empty)            DISK_MODE=empty ;;
        --no-disk|--nodisk) DISK_MODE=none ;;
        -h|--help)          usage; exit 0 ;;
        --)                 shift; QEMU_EXTRA_ARGS="$*"; break ;;
        *)
            # Refuse rather than pass through: a mistyped flag that silently
            # reaches QEMU either fails with QEMU's error (about our script) or,
            # worse, changes the machine in a way nobody asked for.
            echo "run: unknown option '$1'" >&2
            usage
            exit 2 ;;
    esac
    shift
done

# `DOS_DISK=none` is the older spelling of --no-disk; keep it working.
# Written as an `if` and not `[ … ] && …`: under `set -e` a trailing test that
# is simply FALSE is a failing command, and the script would exit here on every
# run that did not set the variable.
if [ "${DOS_DISK:-}" = "none" ]; then DISK_MODE=none; fi

# Create + format a 64 MiB exFAT volume at $1, replacing whatever is there.
# mkfs.exfat lives in the build container (not on a Mac host), and formatting is
# arch-independent — so the x86 build image does this for every arch.
dos_format_disk() {
    _p="$1"
    mkdir -p "$(dirname "$_p")"
    rm -f "$_p"
    if docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build \
           bash -c "dd if=/dev/zero of=/src/$_p bs=1M count=64 status=none &&
                    mkfs.exfat -n DOS /src/$_p >/dev/null" 2>/dev/null; then
        return 0
    fi
    echo "run: could not format $_p (is the d-os-build image present?)" >&2
    rm -f "$_p"
    return 1
}

# Resolve $1 (the arch's disk path) against DISK_MODE; prints the path to
# attach, or nothing.  Never fails the run: a machine that will not boot
# because its optional disk could not be formatted is a worse outcome than one
# that boots without persistence and says so.
# -----------------------------------------------------------------------------
# §M23 — THE SOUND CARD, on the path a PERSON runs.
#
# This script attached no audio device at all, so `lsaudio` on an everyday boot
# printed nothing and `play`/`tone` had nothing to play into — while every audio
# TEST passed its own `-device`.  That is the fifth time this exact shape has
# appeared here (§M48's missing NIC, §M49's missing -smp, §4.66's missing disk,
# §4.67.1's watchdog/VGA): the measured machine and the used machine were
# different machines.
#
# The BACKEND is host-specific and cannot be guessed portably, so it is chosen
# from `uname` with an escape hatch that matches the one storage already has
# (`DOS_DISK=none`): DOS_AUDIO=none boots with no sound card on purpose, and
# DOS_AUDIO=<backend> forces one.
# -----------------------------------------------------------------------------
dos_audio_backend() {
    if [ -n "${DOS_AUDIO:-}" ]; then echo "$DOS_AUDIO"; return; fi
    case "$(uname -s)" in
        Darwin) echo coreaudio ;;
        *)      echo pa ;;
    esac
}

dos_prepare_disk() {
    _p="$1"
    case "$DISK_MODE" in
        none) return 0 ;;
        empty)
            echo "run: --empty — formatting a fresh 64 MiB exFAT disk at $_p" >&2
            dos_format_disk "$_p" || return 0 ;;
        keep)
            if [ ! -f "$_p" ]; then
                echo "run: creating a 64 MiB exFAT disk at $_p (settings + files live here)" >&2
                dos_format_disk "$_p" || return 0
            fi ;;
    esac
    [ -f "$_p" ] && echo "$_p"
    return 0
}

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
    # USB (M21 Phase M) is exercised separately to avoid double-typing with the
    # virtio keyboard; test it with:
    #   ... -device qemu-xhci -device usb-kbd    (drop virtio-keyboard-device)
    # The xHCI comes up over the PCIe ECAM bus (pci.c) and the HID keyboard
    # drives the shell.
    # -netdev user + virtio-net-device: the §M24 NIC.  Present here as well as
    #   in scripts/dos-shell-test.py deliberately — §M48 found a whole class of
    #   bug in the gap between the path the tests take and the path a person
    #   takes, and an absent NIC is exactly that gap.
    QEMU_MACHINE="-M virt,gic-version=2 -cpu cortex-a72 -smp 2 -m 256M \
        -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
        -serial mon:stdio -rtc base=localtime \
        -device virtio-gpu-device -device virtio-keyboard-device \
        -device virtio-mouse-device \
        -global virtio-mmio.force-legacy=false"

    # §M23 — the sound card, for the same reason as the NIC and the display
    # above.  This arch has no AC97 (that is a PCI card and `virt` has no PCI
    # slot for it), so the device is virtio-sound on a virtio-MMIO slot — the
    # same way this machine gets every other device.
    AUDIO_BACKEND=$(dos_audio_backend)
    if [ "$AUDIO_BACKEND" != "none" ]; then
        QEMU_MACHINE="$QEMU_MACHINE -audiodev $AUDIO_BACKEND,id=snd0 \
            -device virtio-sound-device,audiodev=snd0"
    fi

    # Storage, through the SAME three switches the x86 arches use (see the top
    # of the file).  Until this was shared, ARM attached a disk only if one
    # happened to exist and nothing ever created it — so the arch where the
    # settings store is hardest to reach was also the one where the everyday
    # run never had it.
    DISK=$(dos_prepare_disk "build/aarch64/disk.img")
    DISK_ARGS=""
    if [ -n "$DISK" ]; then
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
        exec "$QEMU" $QEMU_MACHINE -kernel "$KERNEL" $DISK_ARGS $DTB_ARGS \
             $QEMU_EXTRA_ARGS
    fi
    echo "$QEMU not found on host; running headless inside Docker." >&2
    exec docker run --rm -it -v "$PWD":/src d-os-build-aarch64 \
        "$QEMU" $QEMU_MACHINE -kernel "$KERNEL" $DISK_ARGS $DTB_ARGS \
        $QEMU_EXTRA_ARGS
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
    # -m 1024M: the i386 identity map now runs to 1 GiB (it stopped at 256 MiB
    #   until §M48, which is what made Mesa run out of memory on a machine that
    #   had plenty), so this is the point where extra RAM starts being usable.
    #   x86_64 has no such ceiling at all.
    EXTRA="-m 1024M -vga none -device VGA,vgamem_mb=32"
    # MORE THAN ONE CPU.  Until §M49 this script passed no -smp at all, so the
    # default run was uniprocessor and the per-CPU runqueue + load balancer
    # (§M18.6.1) NEVER EXECUTED on the path a person actually uses — every test
    # that ever exercised SMP passed its own -smp, which meant the measured
    # path and the used path were two different paths.  (Exactly the shape of
    # the §M48 missing-NIC bug: the browser could not load a page because the
    # everyday script had no network card, while every network test supplied
    # one.)  Override with SMP=1 to reproduce a UP-only issue.
    EXTRA="$EXTRA -smp ${SMP:-4}"
    # A NETWORK CARD.  Its absence is why the browser could not open a single
    # site however well the fetcher worked: there was nothing to fetch over.
    # `user` mode needs no host privileges and gives the guest 10.0.2.15 with a
    # NAT gateway and DNS at 10.0.2.3 — which is exactly what /etc/resolv.conf
    # is provisioned for.
    EXTRA="$EXTRA -netdev user,id=net0 -device virtio-net-pci,netdev=net0"
    # §M23 — AC97, the codec both x86 arches drive.
    AUDIO_BACKEND=$(dos_audio_backend)
    if [ "$AUDIO_BACKEND" != "none" ]; then
        EXTRA="$EXTRA -audiodev $AUDIO_BACKEND,id=snd0 -device AC97,audiodev=snd0"
    fi
    if [ "$(uname -s)" = "Darwin" ]; then
        EXTRA="$EXTRA -display cocoa,zoom-to-fit=off"
    fi
    # Capture COM1 to a file so kernel diagnostics survive a freeze/panic (the
    # SPINLOCK-STUCK deadlock report, exception dumps, klog).  After a hang, read
    # the tail of this file to see where the kernel got stuck.
    SERLOG="${DOS_SERIAL:-/tmp/dos-serial.log}"
    # Expose the QEMU monitor on a unix socket so a FROZEN guest can be probed
    # from outside: `scripts/dos-dump.sh` connects and dumps `info registers`
    # (EIP + EFLAGS.IF) for every vCPU — the definitive way to locate a silent
    # HALTED freeze (0% CPU, IRQs off), which the in-guest diagnostics can't
    # report because nothing runs.
    MONSOCK="${DOS_MONITOR:-/tmp/dos-monitor.sock}"
    rm -f "$MONSOCK"
    echo "run: serial log -> $SERLOG ; monitor -> $MONSOCK (probe with scripts/dos-dump.sh)" >&2
    # §M31 L3 — hardware watchdog: the ib700 ISA device counts independently of
    # the CPU; if the kernel stops petting it (a HARD lockup: spinning / hlt with
    # IRQs off — the one hang the task-based watchdog can't catch), it fires an
    # NMI (~4 s).  The NMI handler (idt.c) LOGS the stuck EIP to the serial file,
    # then recovers: a ring-3 lockup is force-killed in place; a ring-0 (kernel)
    # lockup reboots.  So a wedged package no longer freezes the box AND we get
    # the exact hang location for a targeted fix.  (Delivery works now that LVT
    # LINT1 is set to NMI mode — lapic.c.)  Map a logged eip with scripts/dos-sym.sh.
    # A WRITABLE DISK.  §M63 stage 0 made settings persist onto the first
    # writable volume — but this script attached no disk, so on the path a
    # PERSON uses there was no such volume and every saved setting still died
    # at reboot, correctly reporting "will NOT survive a reboot" to a user who
    # had no way to fix it.  That is the §M48 missing-NIC / §M49 missing-smp
    # shape for the third time: the tests supplied their own disk, the everyday
    # run had none, so the measured path and the used path were different
    # paths.  Created on first use (exFAT is formatted inside the build
    # container, which is where mkfs.exfat lives).
    # `--no-disk` runs WITHOUT storage on purpose — the mode where nothing
    # persists.  It has to stay easy to reach, because that is the mode whose
    # honesty matters ("will NOT survive a reboot"), and a path nobody can run
    # is a path nobody tests.  `--empty` is the other one worth reaching: a
    # first boot, with nothing carried over from a run nobody remembers.
    DISK=$(dos_prepare_disk "${DOS_DISK:-build/$ARCH/disk.img}")
    DISK_ARGS=""
    # -boot d: a FORMATTED image carries a boot signature and SeaBIOS would
    # boot the disk instead of the CD, hanging with no output at all.
    if [ -n "$DISK" ]; then
        DISK_ARGS="-drive if=virtio,file=$DISK,format=raw -boot d"
    fi

    exec "$QEMU" -rtc base=localtime $EXTRA -serial "file:$SERLOG" \
         -monitor "unix:$MONSOCK,server,nowait" $DISK_ARGS \
         -device ib700 -action watchdog=inject-nmi -cdrom "$ISO" \
         $QEMU_EXTRA_ARGS
fi

echo "$QEMU not found on host; running headless inside Docker." >&2
exec docker run --rm -it -v "$PWD":/src d-os-build \
    "$QEMU" -nographic -cdrom "$ISO" $QEMU_EXTRA_ARGS
