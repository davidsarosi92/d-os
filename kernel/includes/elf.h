/* =============================================================================
 * elf.h — static ELF program loader (M25 stage 2).
 *
 * Loads a statically-linked ELF executable (already resident in a kernel
 * buffer — read from the VFS by the caller) into a per-process address
 * space: it walks the program headers and, for every PT_LOAD segment,
 * allocates physical frames, copies the file image in, zero-fills the BSS
 * tail, and maps the pages into the target `vmm_space` as user pages with
 * the segment's R/W/X permissions.  On success it hands back the entry
 * point virtual address; a later step drops to ring 3 / EL0 there.
 *
 * Portability: the loader understands BOTH ELF classes at runtime
 * (ELFCLASS32 for i386, ELFCLASS64 for x86_64 / aarch64), selected from
 * e_ident[EI_CLASS] — so this is arch-neutral core code, no #ifdef.  It
 * validates just enough to be safe against a malformed image (magic,
 * class, in-bounds header/segment offsets); it is a teaching loader, not
 * a hardened one (no dynamic linking, no interp, no relocations — static
 * executables only, as M25 scopes).
 * ============================================================================= */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

struct vmm_space;

/* Result codes (0 = success, negative = failure). */
#define ELF_OK          0
#define ELF_EBADMAG    -1       /* not an ELF (bad magic)                    */
#define ELF_EBADCLASS  -2       /* EI_CLASS neither 32- nor 64-bit           */
#define ELF_EBADHDR    -3       /* header / phdr table out of bounds         */
#define ELF_ENOLOAD    -4       /* no PT_LOAD segment                        */
#define ELF_ESEGBOUND  -5       /* a segment's file range exceeds the image  */
#define ELF_ENOMEM     -6       /* PMM/map allocation failed                 */

/* Load `image` (`len` bytes) into `space`.  On success writes the entry
 * point VA to *entry and returns ELF_OK; on failure returns a negative
 * ELF_E* code (the caller should destroy `space` to reclaim any frames
 * already mapped). */
int elf_load(struct vmm_space* space, const void* image, size_t len,
             uintptr_t* entry);

/* Test helper: synthesise a minimal valid static ELF of the NATIVE class
 * (ELF32 on i386, ELF64 on 64-bit) into `buf` — one PT_LOAD segment at
 * virtual address `vaddr` (page-aligned) carrying `payload` (copied at the
 * segment start), R+X, with e_entry = vaddr.  Returns the image length, or
 * 0 if it doesn't fit in `cap`.  Lets the ELF loader be exercised without a
 * userland toolchain in the tree yet. */
size_t elf_build_selftest(void* buf, size_t cap, uintptr_t vaddr,
                          const void* payload, size_t payload_len);

#endif
