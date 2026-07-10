#!/bin/sh
# Run the aarch64 d-os build in QEMU.  Thin wrapper over scripts/run_qemu.sh
# (ARCH=aarch64) — see that script for the per-arch QEMU machine flags (i386/
# x86_64 boot the GRUB ISO with a std-VGA framebuffer; aarch64 boots the raw
# ELF on `-M virt` with a virtio-gpu framebuffer + optional virtio-blk disk).
exec env ARCH=aarch64 "$(dirname "$0")/run_qemu.sh" "$@"
