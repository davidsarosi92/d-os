#!/bin/sh
# Build d-os inside the Dockerized toolchain defined in ./Dockerfile.
# Host only needs Docker; the image brings gcc, nasm, grub-mkrescue, xorriso.

set -eu

cd "$(dirname "$0")/.."

IMAGE=d-os-build
docker build --platform=linux/amd64 -t "$IMAGE" .
docker run --rm --platform=linux/amd64 -v "$PWD":/src "$IMAGE" make iso
