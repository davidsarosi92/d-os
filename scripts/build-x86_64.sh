#!/bin/sh
# Build d-os for x86_64.  Thin wrapper over scripts/build.sh (ARCH=x86_64) so the
# three architectures each have a dedicated, discoverable build entry point
# while the actual build logic (Docker image + Makefile target) stays in one
# place.  Any arguments are ignored (build.sh takes none); use the ARCH-generic
# scripts/build.sh directly for custom targets.
exec env ARCH=x86_64 "$(dirname "$0")/build.sh" "$@"
