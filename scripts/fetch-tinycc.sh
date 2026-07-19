#!/bin/sh
# =============================================================================
# fetch-tinycc.sh — pin + clone TinyCC (§M43 on-device compiler).
#
# TinyCC (tcc) is a tiny, single-pass C compiler that emits runnable ELFs with
# its own built-in linker — the pragmatic vehicle for "compile C ON d-os"
# (gcc/clang are far too big to run on d-os).  We cross-build it with our musl
# C++ toolchain (make musl-cross-i686) so the `tcc` binary is itself an
# i686-musl ELF that runs on d-os under §M37 dynamic linking; its target paths
# (crt/lib/include/interp) point at d-os locations so the code IT compiles also
# runs on d-os.  Vendored pristine (gitignored), like musl / Mbed TLS.
#
# PROVEN (2026-07-19): the tcc binary cross-builds (376 KB, i386) and, run under
# qemu-i386 in the build container, compiles a .c to a valid i386 .o.  Build
# recipe (see `make tcc`):
#     export PATH=third_party/musl-cross-i686/bin:$PATH
#     cp third_party/musl-cross-i686/i686-linux-musl/lib/libc.so \
#        /lib/ld-musl-i386.so.1        # so qemu-i386 can run musl build helpers
#     CC=i686-linux-musl-gcc ./configure --cpu=i386 \
#         --elfinterp=/lib/ld-musl-i386.so.1 --crtprefix=/lib --libpaths=/lib \
#         --sysincludepaths="{B}/include:/usr/include" --prefix=/usr
#     make tcc            # the compiler binary (libtcc1.a is a follow-up)
# TODO to finish the port: build libtcc1.a (needs musl headers on a clean
# include path, NOT the host /usr/include which conflicts); provision on d-os
# tcc + {B}/include (tcc's own stddef.h/…) + musl headers at /usr/include +
# crt1/crti/crtn + libc.so + libtcc1.a; a `tcc` shell command; then the Editor
# "Compile & Run" button.
# =============================================================================
set -eu
cd "$(dirname "$0")/.."

DIR=third_party/tinycc
URL=https://repo.or.cz/tinycc.git

if [ ! -d "$DIR/.git" ]; then
    echo "Cloning TinyCC into $DIR ..."
    git clone --depth 1 "$URL" "$DIR"
fi
echo "TinyCC ready at $DIR."
echo "Next (in the container): make tcc"
