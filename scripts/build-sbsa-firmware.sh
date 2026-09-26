#!/usr/bin/env bash
# =============================================================================
# build-sbsa-firmware.sh — the firmware QEMU's `sbsa-ref` machine boots (§M85).
#
# sbsa-ref ignores `-kernel`: like a real server it starts in firmware — TF-A
# (EL3) then EDK2 (UEFI) — from two 256 MiB flash images, and describes itself
# with ACPI.  Homebrew's QEMU does not ship those images (it has edk2 for
# `virt` only), so this builds them FROM SOURCE, the way every other third-
# party piece of this tree is produced: a script, pinned tags, no downloaded
# binaries.
#
# Runs in a NATIVE arm64 Linux container (on an Apple-silicon host that is not
# emulated — the amd64 d-os-build image would compile EDK2 under emulation,
# which is the difference between minutes and an hour).
#
# Output: build/firmware/sbsa/SBSA_FLASH0.fd + SBSA_FLASH1.fd (256 MiB each).
# Use:    qemu-system-aarch64 -M sbsa-ref \
#             -drive if=pflash,file=build/firmware/sbsa/SBSA_FLASH0.fd,format=raw \
#             -drive if=pflash,file=build/firmware/sbsa/SBSA_FLASH1.fd,format=raw ...
# =============================================================================
set -eu

OUT="$(pwd)/build/firmware/sbsa"
WORK="$(pwd)/build/firmware/sbsa-src"
mkdir -p "$OUT" "$WORK"

# Pinned revisions: a firmware that changes under the tests is a variable
# nobody asked for.
TFA_TAG=${TFA_TAG:-v2.12.0}
EDK2_TAG=${EDK2_TAG:-edk2-stable202608}
# edk2-platforms / edk2-non-osi have no release tags; pinned by commit, chosen
# to match EDK2_TAG (a newer edk2-platforms needs libraries an older edk2 lacks
# — the first attempt failed exactly so, on MdeModulePkg/GptLib).
PLAT_REV=${PLAT_REV:-9c2cdde2d73d6bf0261e57b79bfb68cd98fccc1f}
NONOSI_REV=${NONOSI_REV:-7ac12d81e02b323bffdf1ef3c188ea33c2185c91}

docker run --rm --platform linux/arm64 -v "$WORK":/w -v "$OUT":/out \
    -e TFA_TAG="$TFA_TAG" -e EDK2_TAG="$EDK2_TAG" -e PLAT_REV="$PLAT_REV" \
    -e NONOSI_REV="$NONOSI_REV" ubuntu:22.04 bash -euc '
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential git python3 python-is-python3 python3-venv uuid-dev \
    acpica-tools device-tree-compiler bison flex libssl-dev bc >/dev/null
cd /w
[ -d trusted-firmware-a ] || git clone -q --depth 1 -b "$TFA_TAG" \
    https://github.com/ARM-software/arm-trusted-firmware trusted-firmware-a
# A checkout at a different revision is replaced, not reused.
[ "$(cat edk2/.dos-rev 2>/dev/null)" = "$EDK2_TAG" ] || rm -rf edk2
[ -d edk2 ] || { git clone -q --depth 1 -b "$EDK2_TAG" --recurse-submodules \
    --shallow-submodules https://github.com/tianocore/edk2 edk2 && echo "$EDK2_TAG" > edk2/.dos-rev; }
fetch_rev() {   # dir url rev
    [ "$(cat $1/.dos-rev 2>/dev/null)" = "$3" ] && return 0
    rm -rf "$1"; git init -q "$1"; git -C "$1" fetch -q --depth 1 "$2" "$3"
    git -C "$1" checkout -q FETCH_HEAD; echo "$3" > "$1/.dos-rev"
}
fetch_rev edk2-platforms https://github.com/tianocore/edk2-platforms "$PLAT_REV"
fetch_rev edk2-non-osi   https://github.com/tianocore/edk2-non-osi   "$NONOSI_REV"

echo "== TF-A ($TFA_TAG)"
make -s -C trusted-firmware-a PLAT=qemu_sbsa all fip >/dev/null
mkdir -p edk2-non-osi/Platform/Qemu/Sbsa
cp trusted-firmware-a/build/qemu_sbsa/release/bl1.bin edk2-non-osi/Platform/Qemu/Sbsa/
cp trusted-firmware-a/build/qemu_sbsa/release/fip.bin edk2-non-osi/Platform/Qemu/Sbsa/

echo "== EDK2 ($EDK2_TAG)"
export WORKSPACE=/w PACKAGES_PATH=/w/edk2:/w/edk2-platforms:/w/edk2-non-osi
# The C tools only: the BaseTools Python test suite fails on a bind-mounted
# host directory (it rmtree()s through a symlink) and tests nothing we use.
make -s -C edk2/BaseTools/Source/C >/dev/null
set +u; . edk2/edksetup.sh >/dev/null; set -u
build -q -b RELEASE -a AARCH64 -t GCC -p Platform/Qemu/SbsaQemu/SbsaQemu.dsc
cp Build/SbsaQemu/RELEASE_GCC/FV/SBSA_FLASH0.fd Build/SbsaQemu/RELEASE_GCC/FV/SBSA_FLASH1.fd /out/
truncate -s 256M /out/SBSA_FLASH0.fd /out/SBSA_FLASH1.fd
echo "== done: /out/SBSA_FLASH0.fd /out/SBSA_FLASH1.fd"
'
ls -la "$OUT"
