#!/bin/sh
# Build d-os inside the Dockerized toolchain defined in ./Dockerfile.
# Host only needs Docker; the image brings gcc, nasm, grub-mkrescue, xorriso.
#
# Default ARCH is i386 (the reference port).  Override to build x86_64:
#     ARCH=x86_64 ./scripts/build.sh

set -eu

cd "$(dirname "$0")/.."

ARCH=${ARCH:-i386}

IMAGE=d-os-build
docker build --platform=linux/amd64 -t "$IMAGE" .
docker run --rm --platform=linux/amd64 -v "$PWD":/src "$IMAGE" make ARCH="$ARCH" iso
