#!/bin/sh
# Build d-os for i386.  Thin wrapper over scripts/build.sh (ARCH=i386) so the
# three architectures each have a dedicated, discoverable build entry point
# while the actual build logic (Docker image + Makefile target) stays in one
# place.  Any arguments are ignored (build.sh takes none); use the ARCH-generic
# scripts/build.sh directly for custom targets.
exec env ARCH=i386 "$(dirname "$0")/build.sh" "$@"
