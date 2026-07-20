FROM --platform=linux/amd64 ubuntu:22.04

# Non-interactive apt: python3-jsonschema pulls in tzdata, whose postinst
# otherwise prompts for a timezone and hangs `docker build` (no tty).
ENV DEBIAN_FRONTEND=noninteractive

RUN apt update && apt install -y \
    build-essential \
    gcc-multilib \
    nasm \
    grub-pc-bin \
    grub-common \
    xorriso \
    mtools \
    exfatprogs \
    qemu-system-x86

# §M38 — tools to build a musl C++ cross-toolchain from source (musl-cross-make):
# git/wget to fetch, bison/flex/texinfo to build gcc.  (musl-cross-make builds
# its own gmp/mpfr/mpc, so no -dev packages needed.)
# §M39 — Mbed TLS's build generates PSA driver wrappers with Python
# (jsonschema + jinja2); python3/perl are already present via build-essential.
# §M42 — NetSurf's libhubbub/libcss/libdom generate perfect-hash tables with
# gperf at build time.
RUN apt update && apt install -y \
    git \
    wget \
    bison \
    flex \
    texinfo \
    gperf \
    python3-jsonschema \
    python3-jinja2

WORKDIR /src