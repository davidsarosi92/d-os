#!/usr/bin/env python3
# =============================================================================
# pack-rootfs.py — pack a set of host files into a tiny flat archive blob that
# the kernel unpacks into the VFS at boot (§M43 — provisioning the on-device
# compiler's headers/crt into d-os).
#
# Format (little-endian), repeated until a 0 pathlen terminator:
#     u32 pathlen ; path bytes (no NUL) ; u32 datalen ; data bytes
#     ... ; u32 0   (terminator)
#
# Usage: pack-rootfs.py OUT.bin  SRCDIR:DESTPREFIX  [SRCDIR:DESTPREFIX ...]
#   Each SRCDIR is walked recursively; every file is stored at
#   DESTPREFIX + its path relative to SRCDIR (so headers land under /usr/include
#   etc. on d-os).  A plain FILE:DESTPATH pair stores a single file verbatim.
# =============================================================================
import os, sys, struct

def emit(out, dest, data):
    p = dest.encode()
    out.write(struct.pack('<I', len(p))); out.write(p)
    out.write(struct.pack('<I', len(data))); out.write(data)

def main():
    out_path = sys.argv[1]
    specs = sys.argv[2:]
    with open(out_path, 'wb') as out:
        for spec in specs:
            src, dst = spec.split(':', 1)
            if os.path.isdir(src):
                for root, _, files in os.walk(src):
                    for fn in files:
                        full = os.path.join(root, fn)
                        rel = os.path.relpath(full, src)
                        dest = dst.rstrip('/') + '/' + rel.replace(os.sep, '/')
                        with open(full, 'rb') as f:
                            emit(out, dest, f.read())
            else:
                with open(src, 'rb') as f:
                    emit(out, dst, f.read())
        out.write(struct.pack('<I', 0))          # terminator
    print(f"packed {out_path}")

if __name__ == '__main__':
    main()
