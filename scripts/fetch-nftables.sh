#!/bin/sh
# =============================================================================
# fetch-nftables.sh — §M90 3b: the netfilter userspace (`nft`).
#
# Docker's nftables firewall backend drives the kernel through the `nft`
# program, and `nft list ruleset` is how a person (and the tests) read back
# what was installed.  Three pristine upstream RELEASE tarballs — release
# tarballs because they carry the generated configure scripts and the bison
# parser, so the build container needs no autotools:
#   libmnl     the netlink message helpers
#   libnftnl   the nf_tables object library
#   nftables   the `nft` command
# Built statically against musl by `make nftables ARCH=<arch>` (the main
# build image has what configure needs); nothing is forked.
# Run ON THE HOST once.
# =============================================================================
set -eu
cd "$(dirname "$0")/.."
DIR=third_party/nftables-src
mkdir -p "$DIR"
get() {
    if [ ! -f "$DIR/$2" ]; then
        echo "Fetching $2 ..."
        curl -fL --retry 3 -A "Mozilla/5.0" -o "$DIR/$2.part" "$1/$2" && mv "$DIR/$2.part" "$DIR/$2"
    fi
}
get https://www.netfilter.org/pub/libmnl    libmnl-1.0.5.tar.bz2
get https://www.netfilter.org/pub/libnftnl  libnftnl-1.2.8.tar.xz
get https://www.netfilter.org/pub/nftables  nftables-1.1.1.tar.xz
ls -l "$DIR"
echo "Next (in the container): make nftables ARCH=aarch64"
