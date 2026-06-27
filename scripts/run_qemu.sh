#!/bin/sh
# Boot the built ISO in QEMU. Prefers a host-installed qemu (faster, gives
# you a real window); falls back to the Docker image with -nographic.

set -eu

cd "$(dirname "$0")/.."

ISO=build/d-os.iso
if [ ! -f "$ISO" ]; then
    echo "ISO not found at $ISO — run scripts/build.sh first." >&2
    exit 1
fi

if command -v qemu-system-i386 >/dev/null 2>&1; then
    exec qemu-system-i386 -cdrom "$ISO"
fi

echo "qemu-system-i386 not found on host; running headless inside Docker." >&2
exec docker run --rm -it -v "$PWD":/src d-os-build \
    qemu-system-i386 -nographic -cdrom "$ISO"
