#!/bin/sh
# Run the i386 d-os build in QEMU.  Thin wrapper over scripts/run_qemu.sh
# (ARCH=i386) — see that script for the per-arch QEMU machine flags (i386/
# x86_64 boot the GRUB ISO with a std-VGA framebuffer; aarch64 boots the raw
# ELF on `-M virt` with a virtio-gpu framebuffer + optional virtio-blk disk).
exec env ARCH=i386 "$(dirname "$0")/run_qemu.sh" "$@"
