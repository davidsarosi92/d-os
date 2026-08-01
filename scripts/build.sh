#!/bin/sh
# Build d-os inside the Dockerized toolchain defined in ./Dockerfile.
# Host only needs Docker; the image brings gcc, nasm, grub-mkrescue, xorriso.
#
# Default ARCH is i386 (the reference port).  Override to build x86_64:
#     ARCH=x86_64 ./scripts/build.sh

set -eu

cd "$(dirname "$0")/.."

ARCH=${ARCH:-i386}

# Cross-arch artifact hygiene: every user-space artifact lives at an ARCH-AGNOSTIC
# path (user/*.{muslelf,dynelf,cxxelf,so,so.N}) — its objcopy blob symbol name
# derives from the path — so an i386 build and an x86_64 build overwrite each
# other, and using a stale wrong-arch one fails ("file in wrong format" at link,
# or an ELF-load failure at boot).  Multi-arch rule: 32-bit runs ONLY 32-bit,
# 64-bit runs 64- (and later 32-)bit, so a program's 32- vs 64-bit build MUST be
# kept distinct.  Until the store is arch-tagged, we wipe the shared artifacts on
# every ARCH change so they rebuild for the target.  (Per-arch build/<arch>/
# objects never conflict and are kept.)
#
# NOTE: the §M42 NetSurf binary + its lib stack + freetype are gated by
# PARSE-TIME `ifneq ($(wildcard user/…),)` guards (the browser + the slow
# freetype compile are only embedded when their prebuilt artifact is present).
# So after wiping them we must REBUILD them for the new arch in a SEPARATE make
# BEFORE the `iso` make parses — otherwise the guards see them absent and the
# feature is silently dropped from the image (browser missing / no fonts).
#
# PER-ARCH ARTIFACT CACHE.  Wiping on every ARCH switch meant flipping i386 ↔
# x86_64 (or any `make clean`, which also wipes user/) re-compiled the whole
# NetSurf + freetype stack — ~25 minutes of rebuild for artifacts that were
# perfectly good and merely belonged to the OTHER arch.  So instead of deleting
# them we PARK them: `build/.userartifacts/<arch>/` holds each arch's set, we
# stash the outgoing arch's files there on a switch and restore the incoming
# arch's.  The `user/` paths themselves stay arch-agnostic (their objcopy blob
# symbol names derive from the path, so renaming them would ripple through the
# kernel sources) — the cache is what makes them per-arch.  The cache lives
# under build/ but NOT under build/<arch>, so `make clean` (which removes only
# build/<arch>) leaves it intact; `make clean-all` drops everything, as intended.
# Set DOS_ARTIFACT_CACHE=0 to opt out and rebuild from scratch.
CACHE=build/.userartifacts
ART_GLOBS='user/*.muslelf user/*.dynelf user/*.cxxelf user/*.so user/*.so.* user/*.bin user/ldmusl.so'

stash_artifacts() {          # $1 = arch whose artifacts are currently in user/
    [ "${DOS_ARTIFACT_CACHE:-1}" = 1 ] || return 0
    mkdir -p "$CACHE/$1"
    for f in $ART_GLOBS; do
        if [ -e "$f" ]; then mv -f "$f" "$CACHE/$1/" 2>/dev/null || true; fi
    done
}
restore_artifacts() {        # $1 = arch to restore INTO user/ (missing files only)
    [ "${DOS_ARTIFACT_CACHE:-1}" = 1 ] || return 0
    [ -d "$CACHE/$1" ] || return 0
    for f in "$CACHE/$1"/*; do
        if [ -e "$f" ] && [ ! -e "user/$(basename "$f")" ]; then
            cp -p "$f" user/ 2>/dev/null || true
        fi
    done
}
refresh_cache() {            # $1 = arch — keep the cache in step after a build
    [ "${DOS_ARTIFACT_CACHE:-1}" = 1 ] || return 0
    mkdir -p "$CACHE/$1"
    for f in $ART_GLOBS; do
        if [ -e "$f" ]; then cp -p "$f" "$CACHE/$1/" 2>/dev/null || true; fi
    done
}

mkdir -p build user
STAMP=build/.last_arch
ARCH_CHANGED=0
PREV_ARCH=""
[ -f "$STAMP" ] && PREV_ARCH=$(cat "$STAMP" 2>/dev/null)
if [ -n "$PREV_ARCH" ] && [ "$PREV_ARCH" != "$ARCH" ]; then
    echo "build: ARCH changed ($PREV_ARCH → $ARCH) — parking $PREV_ARCH user/ artifacts in $CACHE"
    stash_artifacts "$PREV_ARCH"
    ARCH_CHANGED=1
fi
# Restore whatever this arch has cached (also covers a plain `make clean`, which
# wipes user/ but leaves the cache) — never overwrites a file already present.
restore_artifacts "$ARCH"
# A restored set means nothing has to be rebuilt after all.
if [ "$ARCH_CHANGED" = 1 ] && [ -f user/netsurf.dynelf ] && [ -f user/libfreetype.so.6 ]; then
    echo "build: restored $ARCH artifacts from the cache — no NetSurf/freetype rebuild needed"
    ARCH_CHANGED=0
fi
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

# Rebuild the PARSE-TIME-guarded slow artifacts (freetype, NetSurf) BEFORE the
# main `iso` make (see the hygiene note above), whenever they are MISSING or the
# ARCH changed.  The `iso`/kernel guards embed the browser only if its prebuilt
# artifact is present, so a plain build after they were wiped (a prior arch
# switch, a `make clean`, an interrupted build) would SILENTLY ship without the
# browser — the Start-menu entry (a static GUI_APP registration) would still show
# but clicking it does nothing.  Rebuilding-if-missing makes every build
# self-healing.  x86 only — the NetSurf/freetype stack is not built on aarch64.
if [ "$TARGET" = iso ]; then
    if [ -f third_party/freetype/include/ft2build.h ] && \
       { [ "$ARCH_CHANGED" = 1 ] || [ ! -f user/libfreetype.so.6 ]; }; then
        echo "build: (re)building freetype for $ARCH (font support)"
        docker run --rm --platform=linux/amd64 -v "$PWD":/src "$IMAGE" make ARCH="$ARCH" freetype
    fi
    if [ -d third_party/netsurf ] && \
       { [ "$ARCH_CHANGED" = 1 ] || [ ! -f user/netsurf.dynelf ]; }; then
        echo "build: (re)building NetSurf for $ARCH (browser)"
        docker run --rm --platform=linux/amd64 -v "$PWD":/src "$IMAGE" make ARCH="$ARCH" netsurf || \
            echo "build: WARNING — NetSurf rebuild failed; image will build WITHOUT the browser"
    fi
fi

docker run --rm --platform=linux/amd64 -v "$PWD":/src "$IMAGE" make ARCH="$ARCH" "$TARGET"

# Keep this arch's cache in step with what we just built, so the NEXT ARCH
# switch can restore it instead of recompiling the stack.
refresh_cache "$ARCH"

# Sanity: the NetSurf Start-menu entry is a static registration that shows even
# when the browser blob is absent — so a missing binary is otherwise invisible
# until you click it and nothing happens.  Flag it loudly here.
if [ "$TARGET" = iso ] && [ -d third_party/netsurf ] && [ ! -f user/netsurf.dynelf ]; then
    echo ""
    echo "build: !! NetSurf is NOT in this image (user/netsurf.dynelf missing) —"
    echo "build:    the Start-menu 'NetSurf' entry will do nothing.  Fix with:"
    echo "build:      docker run --rm --platform=linux/amd64 -v \"\$PWD\":/src $IMAGE make ARCH=$ARCH netsurf"
    echo "build:    then re-run this script."
fi
