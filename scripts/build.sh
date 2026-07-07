#!/bin/sh
# Build d-os inside the Dockerized toolchain defined in ./Dockerfile.
# Host only needs Docker; the image brings gcc, nasm, grub-mkrescue, xorriso.
#
# Default ARCH is i386 (the reference port).  Override to build x86_64:
#     ARCH=x86_64 ./scripts/build.sh

set -eu

cd "$(dirname "$0")/.."

ARCH=${ARCH:-i386}

# The AArch64 cross toolchain conflicts with gcc-multilib (i386 -m32), so it
# lives in a SEPARATE image built from Dockerfile.aarch64.  x86 targets are
# packaged as a bootable GRUB ISO; the AArch64 port is booted as a raw ELF via
# QEMU `-M virt -kernel` (no GRUB), so it only needs the `kernel` target.
case "$ARCH" in
    aarch64)
        IMAGE=d-os-build-aarch64
        DOCKERFILE=Dockerfile.aarch64
        TARGET=kernel
        ;;
    *)
        IMAGE=d-os-build
        DOCKERFILE=Dockerfile
        TARGET=iso
        ;;
esac

docker build --platform=linux/amd64 -f "$DOCKERFILE" -t "$IMAGE" .
docker run --rm --platform=linux/amd64 -v "$PWD":/src "$IMAGE" make ARCH="$ARCH" "$TARGET"
