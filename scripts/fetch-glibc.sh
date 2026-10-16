#!/bin/sh
# =============================================================================
# fetch-glibc.sh — §M93: the GLIBC package that sits beside musl.
#
# Most Linux software released as a tarball (a Temurin JDK, many CLI tools) is
# linked against glibc and asks for its loader by an absolute path
# (/lib64/ld-linux-x86-64.so.2, /lib/ld-linux-aarch64.so.1).  d-os's own
# userland is musl; the Linux personality serves both, and the libc is per
# program.  This takes the binaries of Debian 12's libc6 (glibc 2.36) — plus
# libgcc_s and libstdc++, which glibc-linked releases very often need —
# unmodified, from the official Debian images, and packs them in the flat
# rootfs format (scripts/pack-rootfs.py) for the kernel to provision at boot:
#   third_party/glibc-<arch>.bin
# Run ON THE HOST once (needs Docker); `make` embeds the blob when present.
# =============================================================================
set -eu
cd "$(dirname "$0")/.."
for arch in x86_64 aarch64; do
    case $arch in
        x86_64)  plat=linux/amd64; ma=x86_64-linux-gnu;  ld=ld-linux-x86-64.so.2; ldpath=/lib64/$ld ;;
        aarch64) plat=linux/arm64; ma=aarch64-linux-gnu; ld=ld-linux-aarch64.so.1; ldpath=/lib/$ld ;;
    esac
    out=third_party/glibc-$arch
    rm -rf "$out"; mkdir -p "$out/lib"
    docker run --rm --platform=$plat -v "$PWD/$out":/o debian:bookworm-slim sh -c "
        set -e
        cd /usr/lib/$ma
        for f in $ld libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libresolv.so.2 \
                 libutil.so.1 libanl.so.1 libnss_files.so.2 libnss_dns.so.2 libgcc_s.so.1 \
                 libstdc++.so.6; do
            cp -L \$f /o/lib/\$f
        done
        dpkg-query -W -f='\${Version}' libc6 > /o/VERSION
    "
    ver=$(cat "$out/VERSION")
    echo "glibc $ver for $arch:"; ls -la "$out/lib"
    specs=""
    for f in "$out"/lib/*; do
        b=$(basename "$f")
        specs="$specs $f:/lib/$ma/$b"
    done
    # the loader also at the path every glibc program names
    specs="$specs $out/lib/$ld:$ldpath"
    printf 'glibc %s (Debian libc6), unmodified\n' "$ver" > "$out/README"
    specs="$specs $out/README:/lib/$ma/.glibc-package"
    python3 scripts/pack-rootfs.py third_party/glibc-$arch.bin $specs
    ls -la third_party/glibc-$arch.bin
done
