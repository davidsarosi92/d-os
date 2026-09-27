#!/bin/sh
# fsck-exfat.sh <image> — check a d-os volume with a CURRENT exfatprogs.
#
# Why not the build image's fsck.exfat: it is exfatprogs 1.1.3, which reads
# every secondary entry after the Stream Extension as a File Name entry and so
# reports "failed to get name dentry" for any benign secondary — including the
# Vendor Extension entry d-os stores ownership in (§M32, DOCS §4.109).  The
# exFAT spec (7.8) says an implementation ignores a benign secondary it does not
# know; exfatprogs 1.2.x does.  Built once into build/tools, then reused.
set -eu
IMG="${1:?usage: fsck-exfat.sh <image>}"
VER=1.2.9
mkdir -p build/tools
if [ ! -x build/tools/fsck.exfat-$VER ]; then
    docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build bash -c "
        cd /tmp && wget -q -O e.tgz https://github.com/exfatprogs/exfatprogs/releases/download/$VER/exfatprogs-$VER.tar.gz &&
        tar xzf e.tgz && cd exfatprogs-$VER && ./configure -q >/dev/null 2>&1 &&
        make -s -j4 >/dev/null 2>&1 && cp fsck/fsck.exfat /src/build/tools/fsck.exfat-$VER"
fi
docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build \
    /src/build/tools/fsck.exfat-$VER -n "/src/$IMG"
