#!/usr/bin/env python3
# =============================================================================
# make-stale-module.py — manufacture a module the kernel MUST refuse (§M67).
#
# A version check nobody has seen fail is a version check nobody has tested,
# and this one guards a failure mode that is invisible when it happens: a module
# built against an older header reads the wrong struct offsets and corrupts
# something unrelated.  So the build produces two deliberately-wrong modules and
# the test suite loads them expecting a specific refusal.
#
# Making them by PATCHING a good module rather than by compiling one against a
# doctored header is deliberate: it means the stale module is byte-identical to
# the working one except for the field under test, so a refusal cannot be a side
# effect of anything else having changed.
#
# Two variants, because §M67's two checks catch different things:
#
#   abi          — bump `module_abi.abi`.  Stands in for a SEMANTIC change: a
#                  function that kept its signature and changed what it means.
#                  The automatic fingerprint is blind to these by construction.
#
#   fingerprint  — grow `sizes[0]`, i.e. claim `struct driver` was a different
#                  size when the module was compiled.  Stands in for a LAYOUT
#                  change, the common case, and the one nobody has to remember
#                  to declare.
#
# Layout of the header it patches (module_abi.h — scalars first ON PURPOSE, so
# they can be read out of an unrelocated file):
#     u32 magic ; u32 abi ; u16 word_bytes ; u16 nstructs ; u16 sizes[nstructs]
#
# Usage: make-stale-module.py <in.ko> <out.ko> <abi|fingerprint>
# =============================================================================
import sys, struct


def section_offset(blob, want):
    """Return (file_offset, size) of a section by name, for ELF32 or ELF64."""
    if blob[:4] != b'\x7fELF':
        raise SystemExit("not an ELF file")
    is64 = blob[4] == 2
    if is64:
        e_shoff, = struct.unpack_from('<Q', blob, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', blob, 0x3A)
        name_o, off_o, size_o = 0x00, 0x18, 0x20
        rd = lambda b, o: struct.unpack_from('<Q', b, o)[0]
    else:
        e_shoff, = struct.unpack_from('<I', blob, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', blob, 0x2E)
        name_o, off_o, size_o = 0x00, 0x10, 0x14
        rd = lambda b, o: struct.unpack_from('<I', b, o)[0]

    shstr = e_shoff + e_shstrndx * e_shentsize
    strtab_off = rd(blob, shstr + off_o)

    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        nm_idx, = struct.unpack_from('<I', blob, sh + name_o)
        end = blob.index(b'\0', strtab_off + nm_idx)
        nm = blob[strtab_off + nm_idx:end].decode()
        if nm == want:
            return rd(blob, sh + off_o), rd(blob, sh + size_o)
    raise SystemExit(f"no {want} section — is this a d-os module?")


def main():
    src, dst, mode = sys.argv[1], sys.argv[2], sys.argv[3]
    blob = bytearray(open(src, 'rb').read())
    off, _ = section_offset(blob, '.dosmod')

    magic, abi = struct.unpack_from('<II', blob, off)
    if magic != 0x444F534D:
        raise SystemExit(f"bad magic {magic:#x} — the descriptor moved?")

    if mode == 'abi':
        struct.pack_into('<I', blob, off + 4, abi + 97)
        what = f"abi {abi} -> {abi + 97}"
    elif mode == 'fingerprint':
        # sizes[0] is `sizeof(struct driver)`; claim it was four bytes bigger.
        sz, = struct.unpack_from('<H', blob, off + 12)
        struct.pack_into('<H', blob, off + 12, sz + 4)
        what = f"sizeof(struct driver) {sz} -> {sz + 4}"
    else:
        raise SystemExit("mode must be 'abi' or 'fingerprint'")

    open(dst, 'wb').write(blob)
    print(f"make-stale-module: {dst}: {what}")


main()
