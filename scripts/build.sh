#!/bin/sh
# Build d-os inside the Dockerized toolchain defined in ./Dockerfile.
# Host only needs Docker; the image brings gcc, nasm, grub-mkrescue, xorriso.
#
# Default ARCH is i386 (the reference port).  Override to build x86_64:
#     ARCH=x86_64 ./scripts/build.sh

set -eu

cd "$(dirname "$0")/.."

ARCH=${ARCH:-i386}

# Cross-arch artifact hygiene: the user-space programs built for BOTH x86 arches
# (user/*.{muslelf,dynelf,cxxelf} + the C++/dynamic .so's) are arch-AGNOSTIC
# paths — their objcopy blob symbol names derive from the path — so building
# x86_64 overwrites them and a later i386 build would otherwise embed the
# wrong-arch binary (e.g. an x86_64 cpptest in an i386 kernel → ELF load fail).
# When the ARCH changes from the last build, wipe just those shared artifacts
# (NOT the per-arch build/<arch> objects, which never conflict), so they
# rebuild for the right target.  The x86_64-ONLY support libs (libz/libpng16/
# libfreetype/libharfbuzz) are deliberately EXCLUDED: only x86_64 builds them
# (so no cross-arch staleness), and freetype/harfbuzz are minutes-long compiles
# guarded on the prebuilt .so — wiping them would silently drop the feature.
STAMP=build/.last_arch
if [ -f "$STAMP" ] && [ "$(cat "$STAMP" 2>/dev/null)" != "$ARCH" ]; then
    echo "build: ARCH changed ($(cat "$STAMP") → $ARCH) — clearing shared user/ artifacts"
    rm -f user/*.muslelf user/*.dynelf user/*.cxxelf \
          user/libgreet.so user/libcpplib.so user/libstdcxx.so \
          user/libgccs.so user/ldmusl.so user/rootfs.bin 2>/dev/null || true
fi
mkdir -p build
printf '%s\n' "$ARCH" > "$STAMP"

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
