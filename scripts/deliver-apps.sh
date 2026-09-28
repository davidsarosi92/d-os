#!/bin/sh
# =============================================================================
# deliver-apps.sh <disk.img> <arch> — put application images onto a d-os disk
# (§M89).  Called by run_qemu.sh before QEMU starts.
#
# Every OCI archive in build/apps/ (`docker save <image> -o build/apps/<name>.tar`)
# is copied to the disk's /incoming/ — once per archive version (a stamp file
# beside the disk remembers what was delivered).  On boot d-os's `apps` service
# installs anything in /incoming that is not installed yet, as application
# <name>: into /mnt/apps/<name>/<version>, with its programs linked into /bin.
# A NEWER archive under the same name is delivered again and installs beside
# the old version (which stays, for `app use <name> <old version>`).
#
# The disk grows when it is too small (the default is 64 MiB; a JDK needs its
# archive AND its installed tree): its files are copied off, the disk is
# re-created bigger, and they are copied back — settings survive.
#
# macOS only for now (hdiutil mounts the exFAT image); elsewhere it says what
# to do by hand.  A temporary measure: a download inside d-os replaces it.
# =============================================================================
set -eu
DISK="$1"
ARCH="$2"
APPS_DIR="build/apps"

ls "$APPS_DIR"/*.tar >/dev/null 2>&1 || exit 0
case "$ARCH" in
    x86_64|aarch64) ;;
    *) echo "run: build/apps/ images are for 64-bit guests - not delivered to $ARCH" >&2; exit 0 ;;
esac
[ -f "$DISK" ] || exit 0

STAMP="$DISK.apps"
born=$(stat -f %B "$DISK")          # a re-created disk (--empty) gets everything again
todo=""
total=0
for f in "$APPS_DIR"/*.tar; do
    sz=$(stat -f %z "$f"); mt=$(stat -f %m "$f")
    total=$((total + sz))
    key="$(basename "$f") $sz $mt $born"
    grep -qxF "$key" "$STAMP" 2>/dev/null || todo="$todo $f"
done
[ -n "$todo" ] || exit 0

if ! command -v hdiutil >/dev/null 2>&1; then
    echo "run: build/apps has images to deliver, but this host has no hdiutil." >&2
    echo "run:   Copy them by hand onto the disk's /incoming/ directory." >&2
    exit 0
fi

attach() {   # prints the mount point
    hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse "$1" |
        awk -F'\t' '/\/Volumes\// { print $NF }' | tail -1
}

# Big enough?  Everything in build/apps twice (archive + installed tree) plus
# headroom for the system's own files.
want_mb=$((total * 2 / 1048576 + 512))
cur_mb=$(( $(stat -f %z "$DISK") / 1048576 ))
if [ "$cur_mb" -lt "$want_mb" ]; then
    echo "run: growing $DISK from $cur_mb MiB to $want_mb MiB for the applications (files kept)" >&2
    keep=$(mktemp -d)
    mnt=$(attach "$DISK")
    [ -n "$mnt" ] || { echo "run: could not mount $DISK - is QEMU still using it?" >&2; exit 0; }
    cp -RX "$mnt"/. "$keep"/ 2>/dev/null || true
    hdiutil detach "$mnt" >/dev/null
    rm -f "$DISK"
    docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build \
        bash -c "dd if=/dev/zero of=/src/$DISK bs=1M count=0 seek=$want_mb status=none &&
                 mkfs.exfat -n DOS /src/$DISK >/dev/null" || {
        echo "run: could not re-create $DISK (is the d-os-build image present?)" >&2; exit 1; }
    mnt=$(attach "$DISK")
    cp -RX "$keep"/. "$mnt"/ 2>/dev/null || true
    hdiutil detach "$mnt" >/dev/null
    rm -rf "$keep"
fi

mnt=$(attach "$DISK")
[ -n "$mnt" ] || { echo "run: could not mount $DISK - is QEMU still using it?" >&2; exit 0; }
mkdir -p "$mnt/incoming"
for f in $todo; do
    b=$(basename "$f")
    echo "run: delivering $b to the disk (d-os installs it on boot; it takes a few minutes)" >&2
    cp -X "$f" "$mnt/incoming/$b"
    rm -f "$mnt/incoming/$b.installed"          # a new archive installs again
done
find "$mnt" -name '._*' -delete 2>/dev/null || true
rm -rf "$mnt/.fseventsd" "$mnt/.Spotlight-V100" 2>/dev/null || true
hdiutil detach "$mnt" >/dev/null
born=$(stat -f %B "$DISK")
for f in $todo; do
    echo "$(basename "$f") $(stat -f %z "$f") $(stat -f %m "$f") $born" >> "$STAMP"
done
