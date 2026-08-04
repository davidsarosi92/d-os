#!/bin/sh
# =============================================================================
# build-mesa.sh — cross-build libdrm + Mesa (gallium swrast) for a d-os arch.
#
# §M40 shipped Mesa for x86_64 by hand; this captures the recipe so the second
# arch is a parameter rather than a re-derivation.  Everything runs inside the
# build container (paths below are the container's /src view).
#
#   ARCH=i386 ./scripts/build-mesa.sh
#
# Output lands in third_party/mesa-$ARCH/, which is exactly what the Makefile's
# EGL blob block probes for.
# =============================================================================
set -eu
cd "$(dirname "$0")/.."
ARCH=${ARCH:-x86_64}

case "$ARCH" in
    i386)   CROSS=third_party/mesa-cross-i386.txt ;;
    x86_64) CROSS=third_party/mesa-cross.txt ;;
    *) echo "build-mesa.sh: unsupported ARCH '$ARCH'" >&2; exit 1 ;;
esac

docker build --platform=linux/amd64 -f Dockerfile -t d-os-build . >/dev/null

docker run --rm --platform=linux/amd64 -v "$PWD":/src -e DOS_ARCH="$ARCH" \
    d-os-build sh -c "
set -eu
cd /src

# ---- libdrm ---------------------------------------------------------------
# Mesa's build wants libdrm's headers even for a purely software driver.
if [ ! -d third_party/libdrm-$ARCH ]; then
    rm -rf /tmp/libdrm-build
    cp -r third_party/libdrm /tmp/libdrm-src
    cd /tmp/libdrm-src
    meson setup /tmp/libdrm-build --cross-file /src/$CROSS \
        --prefix=/src/third_party/libdrm-$ARCH --buildtype=release \
        -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
        -Dvmwgfx=disabled -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled \
        -Dcairo-tests=disabled -Dvalgrind=disabled -Dman-pages=disabled
    ninja -C /tmp/libdrm-build install
    cd /src
fi

# ---- Mesa -----------------------------------------------------------------
# Built from a /tmp copy: Mesa writes generated headers into its own tree and
# the vendored source must stay pristine.
rm -rf /tmp/mesa-src /tmp/mesa-build
cp -r third_party/mesa /tmp/mesa-src
cd /tmp/mesa-src
meson setup /tmp/mesa-build --cross-file /src/$CROSS \
    --prefix=/src/third_party/mesa-$ARCH --buildtype=release \
    -Dgallium-drivers=swrast -Dvulkan-drivers= -Dplatforms=wayland \
    -Dglx=disabled -Degl=enabled -Dgbm=disabled -Dgles1=disabled -Dgles2=enabled \
    -Dshared-glapi=enabled -Dllvm=disabled -Dlmsensors=disabled \
    -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
    -Dbuild-tests=false -Dgallium-vdpau=disabled -Dgallium-va=disabled \
    -Dgallium-xa=disabled -Dosmesa=false
ninja -C /tmp/mesa-build install
"
echo "build-mesa.sh: third_party/mesa-$ARCH ready"
