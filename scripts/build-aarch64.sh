#!/bin/sh
# Build d-os for aarch64.  Thin wrapper over scripts/build.sh (ARCH=aarch64) so the
# three architectures each have a dedicated, discoverable build entry point
# while the actual build logic (Docker image + Makefile target) stays in one
# place.  Any arguments are ignored (build.sh takes none); use the ARCH-generic
# scripts/build.sh directly for custom targets.
exec env ARCH=aarch64 "$(dirname "$0")/build.sh" "$@"
