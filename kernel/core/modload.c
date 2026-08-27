/* =============================================================================
 * modload.c — loading a relocatable ELF object into the running kernel (§M67).
 *
 * `insmod /modules/ac97.ko` turns a file into a `struct driver` that §M66's
 * registry cannot tell apart from a built-in one.  That is the whole feature,
 * and every hard part of it is in the four steps below.
 *
 * -----------------------------------------------------------------------------
 * WHY A RELOCATABLE OBJECT (ET_REL) AND NOT A SHARED OBJECT (ET_DYN)
 *
 * A .so would let us reuse elf.c, which already loads PT_LOAD segments and is
 * used for every ring-3 program.  It is the wrong shape here.  A shared object
 * resolves its imports through a GOT/PLT that a dynamic linker fills in, which
 * means running a linker (or writing one) inside the kernel; and its segments
 * assume a load bias, i.e. one contiguous mapping at an address of the object's
 * choosing.  A relocatable object asks for neither: the sections are
 * independent, and every reference to the outside world is an explicit
 * relocation against an undefined symbol.  Resolving those against ksym.c IS
 * the link, and it is a few hundred lines rather than a subsystem.
 *
 * -----------------------------------------------------------------------------
 * THE FOUR STEPS, AND THE ORDER IS DELIBERATE
 *
 *   1. VALIDATE, before allocating anything.  The header, the arch, and then
 *      the module's own ABI fingerprint, which is readable straight out of the
 *      file because §M67 deliberately put only scalars in front of the pointers
 *      (see module_abi.h).  A module we are going to refuse costs one read.
 *   2. PLACE.  One allocation for all SHF_ALLOC sections, laid out with each
 *      section's own alignment honoured; .bss zeroed rather than copied.
 *   3. RELOCATE.  Walk .rel/.rela, resolve each symbol (module-local sections,
 *      module-local symbols, or an exported kernel symbol), apply the arch's
 *      relocation types.  An UNKNOWN relocation type is refused — never
 *      skipped.  A skipped relocation is a pointer that stays zero or a call
 *      that lands at a random offset, and it fails at first use, arbitrarily
 *      far from here.
 *   4. ATTACH.  Read the (now relocated) pointers out of the module's own
 *      descriptor and hand its driver to §M66's `driver_attach`.
 *
 * -----------------------------------------------------------------------------
 * WHAT THIS DOES NOT DO, AND IT IS NOT A DETAIL
 *
 * Module code runs in RING 0, in the ONE address space, with no isolation of
 * any kind.  There is no W^X here either — the kernel heap is executable on all
 * three arches today (i386 has no NX without PAE; x86_64 defines PTE_NX and
 * does not use it; aarch64's kernel identity map does not set PXN), which is
 * why the loader can simply place code in kmalloc'd memory.  That is a fact
 * about the current tree, not a design goal: if kernel pages ever become
 * non-executable, this is the file that breaks, and it should break rather than
 * quietly map something writable-and-executable behind everyone's back.
 *
 * So the trust model is: a module is as trusted as the kernel.  §M67's plan
 * says this out loud and says what fixes it — §M33's execution domains — and
 * until that exists the honest scope for this loader is modules built from THIS
 * tree, shipped with the system.  A version check is not a security boundary
 * and is not offered as one; it stops a STALE module, not a hostile one.
 * ============================================================================= */

#include "modload.h"
#include "module_abi.h"
#include "ksym.h"
#include "driver.h"
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include "vfs.h"
#include "config.h"
#include "settings.h"
#include <stddef.h>

/* File-local string/memory helpers, following this tree's convention: there is
 * no shared <string.h> in the kernel and every file that needs these carries
 * its own (exfat.c's `memcpy_`, pkg.c's `strlen_local`).  Copying that here
 * rather than introducing a header is deliberate — a new global `string.h`
 * would be a tree-wide change smuggled in under a driver-loader milestone. */
static void m_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}
static void m_memset(void* dst, int v, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)v;
}
static int m_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static int m_strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}
/* Truncating copy that ALWAYS terminates.  The module descriptor's name field
 * is fixed-size and comes from a file, so it may legitimately be full with no
 * NUL — copying it out without forcing one turns a module name into a walk off
 * the end of the struct. */
static void m_strlcpy(char* dst, const char* src, size_t cap) {
    if (!cap) return;
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ----------------------------------------------------------------------
 * ELF types.  Only the NATIVE class is accepted, so these are typedef'd once
 * to the right width instead of the loader carrying both layouts the way
 * elf.c does.  elf.c has to: it loads ring-3 programs and a 64-bit kernel can
 * legitimately run a 32-bit one.  A module is kernel code — a module of the
 * wrong class could never run here, so accepting it would only move the
 * failure later.
 * ---------------------------------------------------------------------- */
#if defined(__i386__)
#  define EM_EXPECTED  3        /* EM_386        */
#  define ELFCLASS_EXPECTED 1   /* ELFCLASS32    */
typedef uint32_t elf_addr_t;
typedef uint32_t elf_off_t;
typedef uint32_t elf_xword_t;
#elif defined(__x86_64__)
#  define EM_EXPECTED  62       /* EM_X86_64     */
#  define ELFCLASS_EXPECTED 2   /* ELFCLASS64    */
typedef uint64_t elf_addr_t;
typedef uint64_t elf_off_t;
typedef uint64_t elf_xword_t;
#elif defined(__aarch64__)
#  define EM_EXPECTED  183      /* EM_AARCH64    */
#  define ELFCLASS_EXPECTED 2
typedef uint64_t elf_addr_t;
typedef uint64_t elf_off_t;
typedef uint64_t elf_xword_t;
#else
#  error "modload: no ELF machine type for this arch"
#endif

#if ELFCLASS_EXPECTED == 1
struct elf_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct elf_shdr {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
};
struct elf_sym {
    uint32_t st_name, st_value, st_size;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
};
struct elf_rel  { uint32_t r_offset, r_info; };
struct elf_rela { uint32_t r_offset, r_info; int32_t r_addend; };
#define ELF_R_SYM(i)   ((i) >> 8)
#define ELF_R_TYPE(i)  ((i) & 0xFF)
#else
struct elf_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct elf_shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
};
struct elf_sym {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};
struct elf_rel  { uint64_t r_offset, r_info; };
struct elf_rela { uint64_t r_offset, r_info; int64_t r_addend; };
#define ELF_R_SYM(i)   ((uint32_t)((i) >> 32))
#define ELF_R_TYPE(i)  ((uint32_t)((i) & 0xFFFFFFFFu))
#endif

#define ET_REL          1
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_NOBITS      8
#define SHT_REL         9
#define SHF_ALLOC       0x2
#define SHN_UNDEF       0
#define SHN_ABS         0xFFF1
#define SHN_COMMON      0xFFF2

/* ----------------------------------------------------------------------
 * Loaded-module bookkeeping.
 *
 * A slot table, like §M66's driver registry and for the same reason: unloading
 * has to find everything the load produced, and "everything" includes the one
 * allocation the sections live in and the veneer pool.  A module that could not
 * be found again could not be unloaded, which would make §M67 point 4
 * impossible rather than merely unwritten.
 * ---------------------------------------------------------------------- */
#define MOD_MAX 16

struct loaded_module {
    int            used;
    char           name[24];
    void*          image;          /* the single allocation for all sections  */
    size_t         image_len;
    struct driver* drv;            /* what we attached, NULL if not a driver  */
    void         (*mod_exit)(void);
    uint32_t       relocs;         /* diagnostics: how much work the load was */
    uint32_t       syms_resolved;
    char           path[96];
};

static struct loaded_module g_mods[MOD_MAX];

static struct loaded_module* mod_slot_alloc(void) {
    for (int i = 0; i < MOD_MAX; i++) if (!g_mods[i].used) return &g_mods[i];
    return NULL;
}

static struct loaded_module* mod_find(const char* name) {
    for (int i = 0; i < MOD_MAX; i++)
        if (g_mods[i].used && m_strcmp(g_mods[i].name, name) == 0) return &g_mods[i];
    return NULL;
}

/* ----------------------------------------------------------------------
 * Step 1 — validation.
 * ---------------------------------------------------------------------- */

/* Compare the module's fingerprint against ours, and on a mismatch say WHICH
 * struct moved.  That is the difference between a message the user can act on
 * and one that only says "no".  The names come from the same X-macro list as
 * the sizes, so the report cannot name the wrong struct. */
static int abi_check(const struct module_abi* m, const char* what) {
    static const uint16_t mine[]  = MODULE_ABI_FINGERPRINT;
    static const char* const nm[] = MODULE_ABI_NAMES;

    if (m->magic != MODULE_ABI_MAGIC) {
        kprintf("insmod: %s: no module descriptor (bad magic)\n", what);
        return -1;
    }
    if (m->word_bytes != (uint16_t)sizeof(void*)) {
        kprintf("insmod: %s: built for %d-bit, this kernel is %d-bit\n",
                what, m->word_bytes * 8, (int)(sizeof(void*) * 8));
        return -1;
    }
    if (m->abi != DOS_MODULE_ABI) {
        kprintf("insmod: %s: module ABI %d, kernel wants %d — rebuild it\n",
                what, (int)m->abi, DOS_MODULE_ABI);
        return -1;
    }
    if (m->nstructs != (uint16_t)MODULE_ABI_NSTRUCTS) {
        kprintf("insmod: %s: fingerprint has %d entries, kernel has %d\n",
                what, (int)m->nstructs, MODULE_ABI_NSTRUCTS);
        return -1;
    }
    for (int i = 0; i < MODULE_ABI_NSTRUCTS; i++) {
        if (m->sizes[i] != mine[i]) {
            kprintf("insmod: %s: struct %s is %d bytes here, %d in the module"
                    " — rebuild it against this kernel\n",
                    what, nm[i], (int)mine[i], (int)m->sizes[i]);
            return -1;
        }
    }
    /* A version difference with a matching fingerprint is INFORMATION, not a
     * refusal: the sizes agreeing means every struct the module can see is
     * laid out identically, and refusing anyway would mean no module survives
     * a patch release.  It is printed because "which build was this compiled
     * against" is the first question when something later goes wrong. */
    if (m_strncmp(m->built_for, DOS_VERSION, sizeof(m->built_for)) != 0)
        kprintf("insmod: %s: built for %s, running %s (layouts match)\n",
                what, m->built_for, DOS_VERSION);
    return 0;
}

/* ----------------------------------------------------------------------
 * Step 3 — relocation, per arch.
 *
 * `where` is the address being patched, `sym` the resolved symbol value, `add`
 * the addend.  Returns 0 on success; anything else means the type is one this
 * loader does not implement, and the load is ABANDONED.
 *
 * REFUSING AN UNKNOWN TYPE IS THE WHOLE DISCIPLINE HERE.  The tempting
 * alternative — warn and continue — produces a module that loads cleanly and
 * contains one wrong pointer.  Nothing about the failure then points back at
 * the load.
 * ---------------------------------------------------------------------- */
#if defined(__i386__)
#define R_386_NONE      0
#define R_386_32        1
#define R_386_PC32      2
#define R_386_PLT32     4

static int apply_reloc(uint32_t type, uintptr_t where, uintptr_t sym,
                       intptr_t add) {
    switch (type) {
    case R_386_NONE:  return 0;
    case R_386_32:    *(uint32_t*)where = (uint32_t)(sym + (uintptr_t)add);
                      return 0;
    /* PLT32 is treated as PC32: there is no PLT in a kernel module, and the
     * assembler emits it for ordinary calls in newer toolchains.  The
     * arithmetic is identical once the target is known. */
    case R_386_PC32:
    case R_386_PLT32: *(uint32_t*)where =
                          (uint32_t)(sym + (uintptr_t)add - where);
                      return 0;
    default: return -1;
    }
}
#elif defined(__x86_64__)
#define R_X86_64_NONE   0
#define R_X86_64_64     1
#define R_X86_64_PC32   2
#define R_X86_64_PLT32  4
#define R_X86_64_32     10
#define R_X86_64_32S    11

static int apply_reloc(uint32_t type, uintptr_t where, uintptr_t sym,
                       intptr_t add) {
    uint64_t v = (uint64_t)(sym + (uintptr_t)add);
    switch (type) {
    case R_X86_64_NONE: return 0;
    case R_X86_64_64:   *(uint64_t*)where = v; return 0;
    case R_X86_64_PC32:
    case R_X86_64_PLT32: {
        /* A 32-bit displacement across a 64-bit address space.  The kernel is
         * built -mcmodel=large precisely so its own calls do not depend on
         * this, but the assembler still emits PC32 for local references, and
         * a module's sections all live in one allocation — so the range is
         * fine in practice and CHECKED anyway.  Silently truncating a
         * displacement produces a call into nothing. */
        int64_t d = (int64_t)v - (int64_t)where;
        if (d < -0x80000000LL || d > 0x7FFFFFFFLL) return -2;
        *(uint32_t*)where = (uint32_t)(int32_t)d;
        return 0;
    }
    case R_X86_64_32:
        if (v > 0xFFFFFFFFULL) return -2;
        *(uint32_t*)where = (uint32_t)v; return 0;
    case R_X86_64_32S: {
        int64_t s = (int64_t)v;
        if (s < -0x80000000LL || s > 0x7FFFFFFFLL) return -2;
        *(uint32_t*)where = (uint32_t)(int32_t)s; return 0;
    }
    default: return -1;
    }
}
#elif defined(__aarch64__)
#define R_AARCH64_NONE              0
#define R_AARCH64_ABS64             257
#define R_AARCH64_ABS32             258
#define R_AARCH64_PREL32            261
#define R_AARCH64_ADR_PREL_PG_HI21  275
#define R_AARCH64_ADD_ABS_LO12_NC   277
#define R_AARCH64_JUMP26            282
#define R_AARCH64_CALL26            283
#define R_AARCH64_LDST8_ABS_LO12_NC  278
#define R_AARCH64_LDST16_ABS_LO12_NC 284
#define R_AARCH64_LDST32_ABS_LO12_NC 285
#define R_AARCH64_LDST64_ABS_LO12_NC 286
#define R_AARCH64_LDST128_ABS_LO12_NC 299

/* Patch a bit-field into an existing instruction word.  aarch64 relocations
 * are almost all "insert these N bits at this offset", so doing it once here
 * keeps every case below to one line of arithmetic and one call. */
static void ins_bits(uintptr_t where, uint32_t val, int lsb, int nbits) {
    uint32_t insn = *(uint32_t*)where;
    uint32_t mask = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1u);
    insn &= ~(mask << lsb);
    insn |= (val & mask) << lsb;
    *(uint32_t*)where = insn;
}

static int apply_reloc(uint32_t type, uintptr_t where, uintptr_t sym,
                       intptr_t add) {
    uint64_t v = (uint64_t)(sym + (uintptr_t)add);
    switch (type) {
    case R_AARCH64_NONE:  return 0;
    case R_AARCH64_ABS64: *(uint64_t*)where = v; return 0;
    case R_AARCH64_ABS32:
        if (v > 0xFFFFFFFFULL) return -2;
        *(uint32_t*)where = (uint32_t)v; return 0;
    case R_AARCH64_PREL32: {
        int64_t d = (int64_t)v - (int64_t)where;
        if (d < -0x80000000LL || d > 0x7FFFFFFFLL) return -2;
        *(uint32_t*)where = (uint32_t)(int32_t)d; return 0;
    }
    /* B / BL: a 26-bit word displacement, i.e. +-128 MiB.  THIS IS THE ONE
     * THAT CAN LEGITIMATELY GO OUT OF RANGE on this arch — the kernel is
     * linked low in RAM and a module lands wherever the heap is, which on a
     * big `-m` can be further than that.  Out of range is REFUSED with its own
     * code so the message can say what is wrong; the fix, if it is ever needed,
     * is a veneer pool next to the module (see modload.h). */
    case R_AARCH64_JUMP26:
    case R_AARCH64_CALL26: {
        int64_t d = (int64_t)v - (int64_t)where;
        if (d < -(1LL << 27) || d >= (1LL << 27) || (d & 3)) return -2;
        ins_bits(where, (uint32_t)((uint64_t)d >> 2), 0, 26);
        return 0;
    }
    /* ADRP: the page offset between the instruction's page and the symbol's,
     * split across two immediate fields (immlo at bit 29, immhi at bit 5). */
    case R_AARCH64_ADR_PREL_PG_HI21: {
        int64_t d = (int64_t)(v & ~0xFFFULL) - (int64_t)(where & ~0xFFFULL);
        d >>= 12;
        if (d < -(1LL << 20) || d >= (1LL << 20)) return -2;
        ins_bits(where, (uint32_t)((uint64_t)d & 0x3), 29, 2);
        ins_bits(where, (uint32_t)(((uint64_t)d >> 2) & 0x7FFFF), 5, 19);
        return 0;
    }
    /* The LO12 family: the low 12 bits of the address, SHIFTED DOWN BY THE
     * ACCESS SIZE for the load/store forms because their immediate is scaled.
     * Getting the shift wrong does not fail to build and does not fault — it
     * reads the right page at the wrong offset. */
    case R_AARCH64_ADD_ABS_LO12_NC:
        ins_bits(where, (uint32_t)(v & 0xFFF), 10, 12); return 0;
    case R_AARCH64_LDST8_ABS_LO12_NC:
        ins_bits(where, (uint32_t)(v & 0xFFF), 10, 12); return 0;
    case R_AARCH64_LDST16_ABS_LO12_NC:
        ins_bits(where, (uint32_t)((v & 0xFFF) >> 1), 10, 12); return 0;
    case R_AARCH64_LDST32_ABS_LO12_NC:
        ins_bits(where, (uint32_t)((v & 0xFFF) >> 2), 10, 12); return 0;
    case R_AARCH64_LDST64_ABS_LO12_NC:
        ins_bits(where, (uint32_t)((v & 0xFFF) >> 3), 10, 12); return 0;
    case R_AARCH64_LDST128_ABS_LO12_NC:
        ins_bits(where, (uint32_t)((v & 0xFFF) >> 4), 10, 12); return 0;
    default: return -1;
    }
}
#endif

/* ----------------------------------------------------------------------
 * The loader proper.
 * ---------------------------------------------------------------------- */

struct sec_place {
    uintptr_t addr;     /* where this section ended up, 0 if not allocated */
};

static int in_bounds(size_t off, size_t len, size_t total) {
    return off <= total && len <= total - off;
}

/* Resolve one symbol-table entry to an absolute address.
 *
 * Three cases, and the third is where the kernel symbol table earns its
 * keep: a symbol defined in one of the module's own sections (add the
 * section's placement), an absolute symbol (take it as it is), or an
 * UNDEFINED one — which must be an export or the module cannot run. */
static int resolve_sym(const struct elf_sym* s, const char* strtab,
                       size_t strtab_len, const struct sec_place* place,
                       uint16_t shnum, uintptr_t* out, const char** name_out) {
    const char* nm = (s->st_name < strtab_len) ? strtab + s->st_name : "";
    *name_out = nm;

    if (s->st_shndx == SHN_ABS) { *out = (uintptr_t)s->st_value; return 0; }

    if (s->st_shndx == SHN_UNDEF) {
        if (!nm[0]) { *out = 0; return 0; }      /* the null symbol */
        void* a = ksym_lookup(nm);
        if (!a) return -1;                        /* caller reports the name */
        *out = (uintptr_t)a;
        return 1;                                 /* 1 = came from the kernel */
    }
    if (s->st_shndx == SHN_COMMON) return -2;     /* -fcommon; see below */
    if (s->st_shndx >= shnum || !place[s->st_shndx].addr) return -3;

    *out = place[s->st_shndx].addr + (uintptr_t)s->st_value;
    return 0;
}

int modload_load(const char* path) {
    /* ---- read the file whole.  A module is tens of kilobytes; streaming it
     * would buy nothing and cost the ability to look at any section in any
     * order, which relocation needs. ------------------------------------- */
    struct file* f = vfs_open(path, 0);
    if (!f) { kprintf("insmod: cannot open %s\n", path); return -1; }

    size_t cap = 1u << 20, len = 0;
    uint8_t* img = (uint8_t*)kmalloc(cap);
    if (!img) { vfs_close(f); kprintf("insmod: out of memory\n"); return -1; }
    for (;;) {
        ssize_t n = vfs_read(f, img + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
        if (len == cap) { kprintf("insmod: %s is too large (>1 MiB)\n", path);
                          kfree(img); vfs_close(f); return -1; }
    }
    vfs_close(f);

    int rc = -1;
    struct sec_place* place = NULL;
    void* image = NULL;

    if (len < sizeof(struct elf_ehdr)) { kprintf("insmod: %s: truncated\n", path); goto out; }
    const struct elf_ehdr* eh = (const struct elf_ehdr*)img;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        kprintf("insmod: %s: not an ELF file\n", path); goto out;
    }
    if (eh->e_ident[4] != ELFCLASS_EXPECTED) {
        kprintf("insmod: %s: wrong ELF class (%d-bit object)\n",
                path, eh->e_ident[4] == 1 ? 32 : 64); goto out;
    }
    if (eh->e_machine != EM_EXPECTED) {
        kprintf("insmod: %s: built for machine %d, this kernel is %d\n",
                path, eh->e_machine, EM_EXPECTED); goto out;
    }
    if (eh->e_type != ET_REL) {
        kprintf("insmod: %s: not a relocatable object (e_type %d) — a module"
                " is built with -c, not linked\n", path, eh->e_type); goto out;
    }
    if (!in_bounds((size_t)eh->e_shoff,
                   (size_t)eh->e_shnum * eh->e_shentsize, len) ||
        eh->e_shentsize != sizeof(struct elf_shdr)) {
        kprintf("insmod: %s: bad section table\n", path); goto out;
    }

    const struct elf_shdr* sh = (const struct elf_shdr*)(img + eh->e_shoff);
    uint16_t shnum = eh->e_shnum;

    /* Section-header string table, for naming sections in diagnostics. */
    const char* shstr = "";
    size_t shstr_len = 0;
    if (eh->e_shstrndx < shnum &&
        in_bounds((size_t)sh[eh->e_shstrndx].sh_offset,
                  (size_t)sh[eh->e_shstrndx].sh_size, len)) {
        shstr = (const char*)(img + sh[eh->e_shstrndx].sh_offset);
        shstr_len = (size_t)sh[eh->e_shstrndx].sh_size;
    }
    #define SECNAME(i) (((size_t)sh[i].sh_name < shstr_len) \
                        ? shstr + sh[i].sh_name : "?")

    /* ---- Step 1b: the ABI check, straight out of the file.
     *
     * This runs BEFORE anything is allocated, which is the reason
     * module_abi.h put the scalar fields ahead of the pointers.  The pointers
     * in the descriptor are still section-relative garbage at this point and
     * are not touched. ------------------------------------------------------ */
    const struct module_abi* mabi_file = NULL;
    for (uint16_t i = 0; i < shnum; i++) {
        if (m_strcmp(SECNAME(i), ".dosmod") != 0) continue;
        if (!in_bounds((size_t)sh[i].sh_offset, sizeof(struct module_abi), len))
            break;
        mabi_file = (const struct module_abi*)(img + sh[i].sh_offset);
        break;
    }
    if (!mabi_file) {
        kprintf("insmod: %s: no .dosmod section — is this a d-os module?\n", path);
        goto out;
    }
    if (abi_check(mabi_file, path) != 0) goto out;

    if (mod_find(mabi_file->modname)) {
        kprintf("insmod: '%s' is already loaded\n", mabi_file->modname);
        goto out;
    }

    /* ---- Step 2: place the allocated sections.
     *
     * One allocation for the lot.  Two passes: measure with each section's own
     * alignment honoured, then copy.  Sections are NOT reordered — keeping
     * file order means the layout is reproducible and a dump of the image can
     * be read against `readelf -S` on the host. ------------------------------ */
    place = (struct sec_place*)kcalloc(shnum, sizeof *place);
    if (!place) { kprintf("insmod: out of memory\n"); goto out; }

    /* BOTH PASSES COMPUTE OFFSETS FROM ZERO, and the base is added afterwards.
     *
     * THE FIRST VERSION DID NOT, AND IT WAS A REAL BUG WITH A LONG FUSE.  The
     * measuring pass aligned offsets relative to 0 while the placing pass
     * aligned ADDRESSES relative to a 16-byte-aligned base.  Those two agree
     * only when the base is at least as aligned as every section wants — true
     * on i386, where nothing in the module asked for more than 16, and FALSE on
     * x86_64, where GCC gives `.data` and `.bss` an alignment of 32.  The
     * placing pass then advanced further than the measurement had predicted and
     * the last section ran off the end of the allocation.
     *
     * The symptom was not a crash.  The module loaded, the driver probed, the
     * codec answered, the interrupt installed — every bring-up value printed
     * identically to the working arch — and only the AUDIO came out wrong
     * (peak 32620 instead of 8000, 1030 Hz instead of 443, left and right no
     * longer equal), because the module's `.bss` overlapped whatever the heap
     * handed out next and its state was being rewritten underneath it.
     *
     * Computing the layout ONCE, in offsets, makes the two passes the same
     * arithmetic rather than two arithmetics that have to be kept in step. */
    size_t total = 0, maxalign = 16;
    for (uint16_t i = 0; i < shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        size_t align = (size_t)sh[i].sh_addralign;
        if (align < 1) align = 1;
        if (align > maxalign) maxalign = align;
        total = (total + align - 1) & ~(align - 1);
        place[i].addr = total;              /* an OFFSET for now; base added below */
        total += (size_t)sh[i].sh_size;
    }
    if (total == 0) { kprintf("insmod: %s: nothing to load\n", path); goto out; }

    /* Over-align the image to the STRICTEST section alignment, not a constant.
     * kmalloc makes no promise past its own granularity, so the slack has to
     * cover the worst request the module actually made. */
    image = kmalloc(total + maxalign);
    if (!image) { kprintf("insmod: out of memory (%d bytes)\n", (int)total); goto out; }
    uintptr_t base = ((uintptr_t)image + maxalign - 1) & ~(uintptr_t)(maxalign - 1);

    kprintf("MODDBG total=%d maxalign=%d image=%p base=%p\n",
            (int)total, (int)maxalign, image, (void*)base);
    for (uint16_t i = 0; i < shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        uintptr_t cur = base + place[i].addr;
        kprintf("MODDBG  %s off=%d size=%d align=%d at=%p\n", SECNAME(i),
                (int)place[i].addr, (int)sh[i].sh_size,
                (int)sh[i].sh_addralign, (void*)cur);
        place[i].addr = cur;

        if (sh[i].sh_type == SHT_NOBITS) {
            /* .bss — zeroed, never copied.  A NOBITS section occupies no file
             * bytes, so copying from sh_offset would read whatever follows it
             * in the file and call it initialised data. */
            m_memset((void*)cur, 0, (size_t)sh[i].sh_size);
        } else {
            if (!in_bounds((size_t)sh[i].sh_offset, (size_t)sh[i].sh_size, len)) {
                kprintf("insmod: %s: section %s runs past the file\n",
                        path, SECNAME(i));
                goto out;
            }
            m_memcpy((void*)cur, img + sh[i].sh_offset, (size_t)sh[i].sh_size);
        }
    }

    /* ---- Step 3: relocate. ------------------------------------------------ */
    uint32_t nrel = 0, nksym = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        int is_rela = (sh[i].sh_type == SHT_RELA);
        if (!is_rela && sh[i].sh_type != SHT_REL) continue;

        uint16_t target = (uint16_t)sh[i].sh_info;
        if (target >= shnum || !place[target].addr) continue;   /* e.g. .rel.debug */

        uint16_t symsec = (uint16_t)sh[i].sh_link;
        if (symsec >= shnum || sh[symsec].sh_type != SHT_SYMTAB) {
            kprintf("insmod: %s: %s has no symbol table\n", path, SECNAME(i));
            goto out;
        }
        uint16_t strsec = (uint16_t)sh[symsec].sh_link;
        if (strsec >= shnum) { kprintf("insmod: %s: bad strtab link\n", path); goto out; }

        const struct elf_sym* syms =
            (const struct elf_sym*)(img + sh[symsec].sh_offset);
        size_t nsyms = (size_t)sh[symsec].sh_size / sizeof(struct elf_sym);
        const char* strtab = (const char*)(img + sh[strsec].sh_offset);
        size_t strtab_len = (size_t)sh[strsec].sh_size;

        size_t entsz = is_rela ? sizeof(struct elf_rela) : sizeof(struct elf_rel);
        size_t n = (size_t)sh[i].sh_size / entsz;

        for (size_t k = 0; k < n; k++) {
            const uint8_t* e = img + sh[i].sh_offset + k * entsz;
            elf_addr_t r_offset;
            elf_xword_t r_info;
            intptr_t add;
            if (is_rela) {
                const struct elf_rela* r = (const struct elf_rela*)e;
                r_offset = r->r_offset; r_info = r->r_info; add = (intptr_t)r->r_addend;
            } else {
                const struct elf_rel* r = (const struct elf_rel*)e;
                r_offset = r->r_offset; r_info = r->r_info; add = 0;
            }

            if ((size_t)r_offset >= (size_t)sh[target].sh_size) {
                kprintf("insmod: %s: relocation past the end of %s\n",
                        path, SECNAME(target));
                goto out;
            }

            uint32_t symidx = ELF_R_SYM(r_info);
            uint32_t rtype  = ELF_R_TYPE(r_info);
            if (symidx >= nsyms) {
                kprintf("insmod: %s: relocation names symbol %d of %d\n",
                        path, (int)symidx, (int)nsyms);
                goto out;
            }

            uintptr_t symval = 0;
            const char* symname = "";
            int sres = resolve_sym(&syms[symidx], strtab, strtab_len,
                                   place, shnum, &symval, &symname);
            if (sres == -1) {
                /* THE MESSAGE THAT MATTERS MOST IN THIS FILE.  A module that
                 * calls something the kernel does not export cannot be made to
                 * work by trying again, and the user's next move is either to
                 * stop using that symbol or to export it — so name it. */
                kprintf("insmod: %s: unresolved symbol '%s'"
                        " (not in the kernel's export table; try `ksyms`)\n",
                        path, symname);
                goto out;
            }
            if (sres == -2) {
                kprintf("insmod: %s: symbol '%s' is a COMMON block —"
                        " build the module with -fno-common\n", path, symname);
                goto out;
            }
            if (sres == -3) {
                kprintf("insmod: %s: symbol '%s' lives in a section that was"
                        " not loaded\n", path, symname);
                goto out;
            }
            if (sres == 1) nksym++;

            /* Note the addend source: for REL (i386) the addend is stored IN
             * the patch site, so it has to be read from there and added.  For
             * RELA it is in the entry and the site is ignored.  Mixing these
             * up produces a module that works until the first non-zero
             * addend — i.e. until the first array or struct field access. */
            uintptr_t where = place[target].addr + (uintptr_t)r_offset;
            if (!is_rela) {
#if ELFCLASS_EXPECTED == 1
                add = (intptr_t)(*(int32_t*)where);
#else
                add = (intptr_t)(*(int64_t*)where);
#endif
            }

            int ar = apply_reloc(rtype, where, symval, add);
            if (ar == -1) {
                kprintf("insmod: %s: relocation type %d is not implemented"
                        " (symbol '%s')\n", path, (int)rtype, symname);
                goto out;
            }
            if (ar == -2) {
                kprintf("insmod: %s: relocation to '%s' is out of range"
                        " — the module landed too far from the kernel\n",
                        path, symname);
                goto out;
            }
            nrel++;
        }
    }

    /* ---- Step 4: attach.
     *
     * NOW the descriptor's pointers mean something.  Find it in the loaded
     * image rather than re-reading the file copy — that one was never
     * relocated. ----------------------------------------------------------- */
    const struct module_abi* mabi = NULL;
    for (uint16_t i = 0; i < shnum; i++)
        if (m_strcmp(SECNAME(i), ".dosmod") == 0 && place[i].addr)
            { mabi = (const struct module_abi*)place[i].addr; break; }
    if (!mabi) { kprintf("insmod: %s: .dosmod was not loaded\n", path); goto out; }

    /* A MODULE MUST BE ABLE TO STOP.  §M66 already refuses to stop a driver
     * with no shutdown hook, because it has no way to withdraw its
     * registrations; for a module the consequence is sharper — its code and its
     * descriptor live in memory `rmmod` frees, so a driver that can never be
     * stopped is a module that can never be removed, i.e. a leak by
     * construction rather than by accident.
     *
     * Refused at LOAD time, which is the only point where saying no costs
     * nothing.  The loopback driver had `.shutdown = NULL` and this is what
     * made it grow one. */
    if (mabi->driver && (!mabi->driver->ops || !mabi->driver->ops->shutdown)) {
        kprintf("insmod: %s: driver '%s' has no shutdown hook — it could never"
                " be unloaded\n", path, mabi->driver->name);
        goto out;
    }

    struct loaded_module* slot = mod_slot_alloc();
    if (!slot) { kprintf("insmod: too many modules loaded (%d)\n", MOD_MAX); goto out; }

    if (mabi->mod_init) {
        int ir = mabi->mod_init();
        if (ir != 0) {
            kprintf("insmod: %s: module init failed (%d)\n", path, ir);
            goto out;
        }
    }

    if (mabi->driver) {
        int dr = driver_attach(mabi->driver);
        if (dr != 0) {
            if (mabi->mod_exit) mabi->mod_exit();
            kprintf("insmod: %s: driver_attach failed (%d)\n", path, dr);
            goto out;
        }
    }

    slot->used      = 1;
    slot->image     = image;
    slot->image_len = total + 16;
    slot->drv       = mabi->driver;
    slot->mod_exit  = mabi->mod_exit;
    slot->relocs    = nrel;
    slot->syms_resolved = nksym;
    m_strlcpy(slot->name, mabi->modname, sizeof slot->name);
    m_strlcpy(slot->path, path, sizeof slot->path);

    kprintf("insmod: loaded '%s' (%d bytes, %d relocations, %d kernel symbols)\n",
            slot->name, (int)total, (int)nrel, (int)nksym);
    klog(KLOG_INFO, "mod", "loaded %s from %s", slot->name, path);

    /* A loaded driver is not a started one.  §M66's `drv start` is what brings
     * it up, and keeping the two separate means loading a module for a device
     * that is not present is not an error — the next rescan picks it up when
     * the hardware appears, which is the whole point of that milestone. */
    if (mabi->driver)
        kprintf("insmod: driver '%s' attached — `drv start %s` to bring it up\n",
                mabi->driver->name, mabi->driver->name);

    image = NULL;          /* ownership moved to the slot */
    rc = 0;

out:
    if (image) kfree(image);
    if (place) kfree(place);
    kfree(img);
    return rc;
    #undef SECNAME
}

/* ----------------------------------------------------------------------
 * Unload (§M67 point 4).
 *
 * The order is the load's in reverse, and every step of it is §M66's work
 * being cashed in: the driver is stopped THROUGH the registry (so the class it
 * registered into gets its chance to refuse while somebody is inside a call),
 * detached from the slot table, and only then is the memory it was executing
 * from released.
 *
 * FREEING FIRST WOULD BE THE CLASSIC TEARDOWN CRASH — the same shape as
 * §4.74's GUI teardown, where a surface freed while the compositor still held
 * it was a multi-megabyte use-after-free.  Here it is worse: the memory being
 * freed is the code that is still on somebody's call stack.
 * ---------------------------------------------------------------------- */
int modload_unload(const char* name) {
    struct loaded_module* m = mod_find(name);
    if (!m) { kprintf("rmmod: '%s' is not loaded\n", name); return -1; }

    if (m->drv) {
        /* Stop it if it is running.  A refusal here is the class saying
         * somebody is still using the device, and it is FATAL to the unload —
         * proceeding would free the driver's code out from under that user. */
        if (driver_state(m->drv) & DRV_S_INITED) {
            if (driver_stop(m->drv->name) != 0) {
                kprintf("rmmod: '%s' would not stop — module still loaded\n", name);
                return -1;
            }
        }
        if (driver_detach(m->drv) != 0) {
            kprintf("rmmod: '%s' could not be detached from the registry\n", name);
            return -1;
        }
    }

    if (m->mod_exit) m->mod_exit();

    kfree(m->image);
    kprintf("rmmod: unloaded '%s'\n", m->name);
    klog(KLOG_INFO, "mod", "unloaded %s", m->name);
    m_memset(m, 0, sizeof *m);
    return 0;
}

void modload_list(void) {
    int n = 0;
    for (int i = 0; i < MOD_MAX; i++) {
        if (!g_mods[i].used) continue;
        if (!n) kprintf("loaded modules:\n");
        kprintf("  %s  driver=%s  %d relocs, %d imports  <- %s\n",
                g_mods[i].name,
                g_mods[i].drv ? g_mods[i].drv->name : "(none)",
                (int)g_mods[i].relocs, (int)g_mods[i].syms_resolved,
                g_mods[i].path);
        n++;
    }
    if (!n) kprintf("no modules loaded\n");
}

int modload_is_loaded(const char* name) { return mod_find(name) != NULL; }

/* =============================================================================
 * Autoload (§M67).
 *
 * WHY THIS EXISTS RATHER THAN LEAVING EVERY MODULE TO A TYPED COMMAND.  The
 * loopback network device ships as a module on all three arches, and it is
 * something the system is EXPECTED to have — `ping 127.0.0.1` and §M24's TCP
 * tests use it, and CLAUDE.md documents it as part of that milestone.  Turning
 * it into something a person must remember to load would be a regression
 * dressed up as a feature.
 *
 * So the default is that /modules is loaded at boot, which also means the
 * loader is exercised on EVERY boot on EVERY arch rather than only when a test
 * types `insmod` — the difference between a code path that is tested and one
 * that merely exists.  `modules.autoload = 0` turns it off, and that is how the
 * tests demonstrate a machine running with the module absent.
 *
 * IDEMPOTENT ON PURPOSE.  aarch64 has two boot paths (framebuffer and serial)
 * that each call pkg_init, and §4.63 already paid for a feature wired into one
 * of them: a flag here is cheaper than remembering which sites exist.
 * ============================================================================= */
static int g_autoload_done = 0;

void modload_autoload(void) {
    if (g_autoload_done) return;
    g_autoload_done = 1;

    if (config_get_long("modules.autoload", 1) == 0) {
        kprintf("modules: autoload disabled (modules.autoload = 0)\n");
        return;
    }

    struct file* d = vfs_open("/modules", 0);
    if (!d) return;                          /* no modules in this build */

    /* NOTE THE LOOP CONDITION.  This VFS returns >0 per entry and 0 at end —
     * §M64 got this wrong the other way round and produced a feature whose two
     * halves each looked correct in isolation (`shortcut add` succeeded,
     * `shortcut list` showed nothing). */
    struct dirent e;
    int loaded = 0, failed = 0;
    while (vfs_readdir(d, &e) > 0) {
        char path[128];
        int n = 0;
        const char* pfx = "/modules/";
        while (pfx[n] && n < (int)sizeof path - 1) { path[n] = pfx[n]; n++; }
        int k = 0;
        while (e.name[k] && n < (int)sizeof path - 1) path[n++] = e.name[k++];
        path[n] = 0;

        /* Only .ko.  A directory listing is not a promise about contents, and
         * handing an arbitrary file to the relocator would turn a stray note
         * into a parse error nobody asked for. */
        if (k < 3 || e.name[k-3] != '.' || e.name[k-2] != 'k' || e.name[k-1] != 'o')
            continue;

        if (modload_load(path) == 0) loaded++;
        else                         failed++;
    }
    vfs_close(d);

    /* Report BOTH counts, always.  A silent autoload cannot be told apart from
     * one that found nothing, and "why is there no loopback device" should be
     * answerable from the boot log. */
    if (loaded || failed)
        kprintf("modules: %d loaded, %d refused\n", loaded, failed);
}

CONFIG_KEY(modules_autoload) = {
    .key     = "modules.autoload",
    .group   = "System",
    .type    = CFG_BOOL,
    .def     = "1",
    .help    = "load /modules/*.ko at boot",
};

/* =============================================================================
 * The command surface.
 *
 * Here rather than in a shell, so aarch64's own serial REPL gets the same
 * implementation — §M24's rule.  This tree has paid for breaking it twice
 * (§4.63's `setconf`, which existed on x86 only while ARM could create a
 * persistent store it had no command able to write to; and §M24's own network
 * tests, which could only run on the arches whose shell they lived in).
 * ============================================================================= */

static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

void modload_cmd_insmod(const char* args) {
    const char* p = skip_ws(args ? args : "");
    if (!*p) {
        kprintf("usage: insmod <path.ko>\n");
        kprintf("  a module is attached, not started — `drv start <name>`"
                " brings it up\n");
        return;
    }
    /* Trim a trailing newline / spaces: the path comes from a line editor and
     * a stray blank turns a correct command into "cannot open". */
    static char buf[96];
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' &&
           n + 1 < sizeof buf) { buf[n] = p[n]; n++; }
    buf[n] = 0;
    modload_load(buf);
}

void modload_cmd_rmmod(const char* args) {
    const char* p = skip_ws(args ? args : "");
    if (!*p) { kprintf("usage: rmmod <name>\n"); modload_list(); return; }
    static char buf[24];
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\n' &&
           n + 1 < sizeof buf) { buf[n] = p[n]; n++; }
    buf[n] = 0;
    modload_unload(buf);
}
