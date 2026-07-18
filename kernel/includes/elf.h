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
 * a hardened one.
 *
 * §M37 (dynamic linking): the loader now also handles ET_DYN (PIE) objects —
 * it applies a caller-supplied load bias to every p_vaddr and reports back
 * the info the kernel needs to run a dynamically-linked program: the interp
 * path from PT_INTERP, the in-memory address of the program-header table
 * (for the AT_PHDR auxv the dynamic linker reads), and phnum/phentsize.  The
 * kernel does NOT apply relocations or resolve symbols — that is the job of
 * the interpreter (musl's ld.so) running in ring 3; the loader just maps the
 * main object + the interpreter and sets up the auxv.  See proc.c.
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

/* Rich load result (§M37).  All addresses are USER virtual addresses in the
 * target space; `load_bias` is what was added to each p_vaddr (0 for ET_EXEC,
 * the caller-supplied bias for ET_DYN). */
struct elf_load_info {
    uintptr_t entry;          /* e_entry + load_bias — where to start THIS object */
    uintptr_t load_bias;      /* bias applied to p_vaddr (0 for ET_EXEC)           */
    uintptr_t phdr_uva;       /* user VA of the program-header table (for AT_PHDR) */
    uint16_t  phnum;          /* e_phnum  (for AT_PHNUM)                           */
    uint16_t  phentsize;      /* e_phentsize (for AT_PHENT)                        */
    int       has_interp;     /* PT_INTERP present?                               */
    char      interp[96];     /* interpreter path (NUL-terminated) if has_interp   */
};

/* Load `image` (`len` bytes) into `space`, applying `load_bias` to every
 * p_vaddr for ET_DYN images (ignored for ET_EXEC — always loaded at its fixed
 * p_vaddr).  Fills *out and returns ELF_OK, or a negative ELF_E* code (the
 * caller should destroy `space` to reclaim any frames already mapped). */
int elf_load_ex(struct vmm_space* space, const void* image, size_t len,
                uintptr_t load_bias, struct elf_load_info* out);

/* Back-compat thin wrapper: load a static/native image at its own vaddrs and
 * hand back just the entry point (the pre-§M37 contract). */
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
