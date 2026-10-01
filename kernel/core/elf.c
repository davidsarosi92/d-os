/* =============================================================================
 * elf.c — static ELF loader (M25 stage 2).  See elf.h for the contract.
 *
 * The loader reads both ELF classes.  To avoid duplicating the segment-map
 * loop once per class, each header is decoded into a width-normalised local
 * struct (all fields uintptr_t / uint32_t), then a single loop maps the
 * PT_LOAD segments.  Physical frames come from the PMM (identity-mapped, so
 * we populate them through a direct kernel pointer) and are installed into
 * the target space with vmm_space_map.
 * ============================================================================= */

#include "elf.h"
#include "vmm.h"
#include "vma.h"
#include "pmm.h"
#include "kmap.h"
#include "printf.h"
#include "hal_api.h"   /* hal_elf_can_exec — "is this image for this machine?" */
#include <stdint.h>
#include <stddef.h>

/* ---- ELF on-disk constants ------------------------------------------------ */
#define EI_NIDENT     16
#define EI_CLASS       4
#define EI_DATA        5
#define ELFCLASS32     1
#define ELFCLASS64     2
#define ELFDATA2LSB    1
#define ET_EXEC        2
#define ET_DYN         3
#define PT_LOAD        1
#define PT_INTERP      3
#define PT_PHDR        6
#define PF_X           0x1
#define PF_W           0x2
#define PF_R           0x4

#define PAGE_SIZE      4096u
#define PAGE_MASK      (~(uintptr_t)(PAGE_SIZE - 1))

/* ---- on-disk header layouts (packed) -------------------------------------- */
struct elf32_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} __attribute__((packed));

struct elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type, p_flags;                  /* note: flags precede offset */
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed));

/* ---- width-normalised views ----------------------------------------------- */
struct ehdr_norm { uintptr_t entry, phoff; uint16_t type, phnum, phentsize; };
struct phdr_norm { uint32_t type, flags; uintptr_t offset, vaddr, filesz, memsz; };

/* ---- loader --------------------------------------------------------------- */

static void copy_bytes(uint8_t* dst, const uint8_t* src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

/* §M90 (2026-10-01) — WHERE THE IMAGE'S BYTES COME FROM.  The loader used to
 * need the whole file in one kernel buffer, and so did every caller: execve
 * refused anything over 16 MiB and a shell launch asked kmalloc for the file's
 * full size — a 44 MB `docker` or a 100 MB `dockerd` could not start at all.
 * Now a load reads the HEADERS from a buffer and everything past it from the
 * file itself, page by page (the middle pages come from the page cache
 * anyway, §M74).  `buf` covers [0, buflen); `len` is the file's length. */
struct elf_img { const uint8_t* buf; size_t buflen; size_t len; struct file* f; };

#include "vfs.h"
static int img_copy(const struct elf_img* im, uint8_t* dst, size_t off, size_t n) {
    if (off + n <= im->buflen) { copy_bytes(dst, im->buf + off, n); return 0; }
    if (!im->f) return -1;
    im->f->pos = off;
    return vfs_read(im->f, dst, n) == (ssize_t)n ? 0 : -1;
}

/* Map one PT_LOAD segment into `space`: page-by-page allocate a frame, zero
 * it, copy the segment's file bytes that fall in this page, and map it.
 * Assumes p_vaddr and p_offset share page alignment (the standard ELF
 * constraint; true for our static images).  BSS (memsz > filesz) is covered
 * by the zeroing. */
/* §M86 — A PAGE SHARED BY TWO SEGMENTS.
 *
 * The loader used to allocate a fresh, ZEROED frame for every page of every
 * segment and map it — so where one segment ends and the next begins on the
 * SAME page, the second mapping replaced the first and that page lost the
 * first segment's bytes.  GNU ld normally starts each segment on a new page,
 * which is why nothing broke; a tightly linked binary (`-N`, `--nmagic`, a
 * hand-written linker script) would have lost the tail of its code.  And with
 * no-execute enforced it would be worse than lost bytes: the shared page takes
 * the SECOND segment's permissions, so the end of the code segment becomes a
 * non-executable data page.
 *
 * Overlap can only happen at a segment boundary, so the loader remembers the
 * pages it has already placed, and a later segment landing on one of them
 * copies into THAT frame (no zeroing) and maps it with the UNION of the two
 * segments' permissions — the only mapping that keeps both correct. */
struct placed_page { uintptr_t va; pmm_phys_t frame; uint32_t flags; };
#define PLACED_MAX 16          /* 2 per segment; on the kernel stack */
struct placed_set { struct placed_page p[PLACED_MAX]; int n; };

static struct placed_page* placed_find(struct placed_set* ps, uintptr_t va) {
    for (int i = 0; ps && i < ps->n; i++) if (ps->p[i].va == va) return &ps->p[i];
    return NULL;
}

/* §M74 rung 2 — the FILE the image was read from, when there is one (an
 * interpreter, a program exec'd from the VFS).  A page lying wholly inside a
 * segment's file data at a page-aligned file offset then comes from the page
 * cache, mapped copy-on-write, instead of being copied: every program started
 * through the same ld.so / libc.so shares one copy of its text.  A PARAMETER,
 * not a load-scoped global: two CPUs load programs at once, and a global would
 * map one file's pages into the other's program. */
#include "pcache.h"

static int map_segment(struct vmm_space* space, const struct elf_img* im,
                       const struct phdr_norm* p, uintptr_t bias,
                       struct placed_set* placed, struct file* src) {
    if (p->offset + p->filesz < p->offset) return ELF_ESEGBOUND;   /* overflow */
    if (p->offset + p->filesz > im->len)   return ELF_ESEGBOUND;

    uint32_t flags = VMM_USER;
    if (p->flags & PF_W) flags |= VMM_WRITABLE;
    if (p->flags & PF_X) flags |= VMM_EXEC;

    uintptr_t va_base = (p->vaddr + bias) & PAGE_MASK;
    uintptr_t page_off = (p->vaddr + bias) - va_base;   /* usually 0 */
    uintptr_t span = page_off + p->memsz;

    for (uintptr_t off = 0; off < span; off += PAGE_SIZE) {
        /* §M86 — a USER page, so it may come from highmem, and the kernel
         * writes it through a short kmap rather than the direct map. */
        uintptr_t va = va_base + off;
        /* §M74 — from the page cache, when it is safe to share: a MIDDLE page
         * of the segment (the first and last may be merged with a neighbour
         * segment below, which writes into them), wholly file data, at a
         * page-aligned file offset. */
        if (src && off != 0 && off + PAGE_SIZE < span && off >= page_off &&
            (off - page_off) + PAGE_SIZE <= p->filesz &&
            !((p->offset + (off - page_off)) & (PAGE_SIZE - 1)) &&
            !placed_find(placed, va) && pcache_enabled()) {
            pmm_phys_t fr;
            if (pcache_map_page(src, (p->offset + (off - page_off)) / PAGE_SIZE, &fr) == 0) {
                if (vmm_space_map(space, va, fr,
                                  (flags & ~(uint32_t)VMM_WRITABLE) | VMM_COW) == 0)
                    continue;
                if (vmm_frame_unshare(fr)) pmm_free_frame(fr);
            }
            /* no cache page: fall through to the private copy */
        }
        struct placed_page* prev = placed_find(placed, va);
        pmm_phys_t frame = prev ? prev->frame : pmm_alloc_frame_user();
        if (!frame) return ELF_ENOMEM;
        uint8_t* dst = (uint8_t*)kmap_frame(frame);
        if (!dst) { if (!prev) pmm_free_frame(frame); return ELF_ENOMEM; }
        /* A page the previous segment already filled keeps its bytes; only
         * the part this segment owns is written below (its BSS tail is
         * already zero from the first placement). */
        if (!prev) for (int i = 0; i < (int)PAGE_SIZE; i++) dst[i] = 0;

        /* Copy the slice of file data that lands in this page.  `fpos` is
         * the byte offset within the segment's file image for the first
         * data byte of this page (accounting for the leading page_off). */
        if (off + PAGE_SIZE > page_off && off < page_off + p->filesz) {
            uintptr_t page_start = off;                       /* within span */
            uintptr_t dst_lo = page_start < page_off ? page_off - page_start : 0;
            uintptr_t seg_lo = page_start > page_off ? page_start - page_off : 0;
            uintptr_t avail  = p->filesz - seg_lo;
            uintptr_t room   = PAGE_SIZE - dst_lo;
            uintptr_t n      = avail < room ? avail : room;
            if (img_copy(im, dst + dst_lo, p->offset + seg_lo, (size_t)n) != 0) {
                kunmap_frame(dst);
                if (!prev) pmm_free_frame(frame);
                return ELF_ESEGBOUND;
            }
        }
        kunmap_frame(dst);

        uint32_t pf = prev ? (prev->flags | flags) : flags;   /* union */
        if (vmm_space_map(space, va, frame, pf) != 0) {
            if (!prev) pmm_free_frame(frame);
            return ELF_ENOMEM;
        }
        if (prev) {
            prev->flags = pf;
        } else if (placed && placed->n < PLACED_MAX &&
                   (off == 0 || off + PAGE_SIZE >= span)) {
            /* Only a segment's FIRST and LAST page can be shared. */
            placed->p[placed->n].va    = va;
            placed->p[placed->n].frame = frame;
            placed->p[placed->n].flags = pf;
            placed->n++;
        }
    }
    return ELF_OK;
}

static int elf_load_impl(struct vmm_space* space, const struct elf_img* im,
                         uintptr_t load_bias, struct elf_load_info* out, struct file* src) {
    const uint8_t* image = im->buf;
    size_t len = im->buflen;              /* the HEADERS must lie in the buffer */
    struct placed_set placed;                   /* §M86 — shared boundary pages */
    placed.n = 0;
    if (len < EI_NIDENT) return ELF_EBADMAG;
    if (image[0] != 0x7F || image[1] != 'E' || image[2] != 'L' || image[3] != 'F')
        return ELF_EBADMAG;

    struct ehdr_norm eh;
    unsigned cls = image[EI_CLASS];
    unsigned mach = 0;
    if (cls == ELFCLASS32) {
        if (len < sizeof(struct elf32_ehdr)) return ELF_EBADHDR;
        const struct elf32_ehdr* e = (const struct elf32_ehdr*)image;
        eh.type = e->e_type; eh.entry = e->e_entry; eh.phoff = e->e_phoff;
        eh.phnum = e->e_phnum; eh.phentsize = e->e_phentsize;
        mach = e->e_machine;
    } else if (cls == ELFCLASS64) {
        if (len < sizeof(struct elf64_ehdr)) return ELF_EBADHDR;
        const struct elf64_ehdr* e = (const struct elf64_ehdr*)image;
        eh.type = e->e_type; eh.entry = (uintptr_t)e->e_entry;
        eh.phoff = (uintptr_t)e->e_phoff;
        eh.phnum = e->e_phnum; eh.phentsize = e->e_phentsize;
        mach = e->e_machine;
    } else {
        return ELF_EBADCLASS;
    }

    /* Is this image built for the machine we are running on?
     *
     * Nothing later in this function would fail on its own if it is not: the
     * segments map fine and we hand the entry point to the CPU, which then
     * executes foreign instruction encodings and dies somewhere unrelated —
     * a 32-bit image on a 64-bit kernel is the classic case.  Worse on the
     * other side: a 64-bit image loaded by the 32-bit kernel has its p_vaddr
     * fields truncated to 32 bits by the normalisation above, so the loader
     * silently maps segments at the WRONG addresses.  Checking here turns a
     * whole class of confusing multi-arch failures into one clear error.
     * The arch answers for itself (hal_elf_can_exec) — this file must not
     * grow a list of machine types. */
    if (!hal_elf_can_exec(cls, mach)) return ELF_EBADARCH;

    /* Bias applies only to position-independent (ET_DYN) images; a fixed
     * ET_EXEC always loads at its own p_vaddr. */
    uintptr_t bias = (eh.type == ET_DYN) ? load_bias : 0;

    /* Program-header table must lie fully within the image. */
    if (eh.phoff + (uintptr_t)eh.phnum * eh.phentsize > len ||
        eh.phoff + (uintptr_t)eh.phnum * eh.phentsize < eh.phoff)
        return ELF_EBADHDR;

    if (out) {
        out->entry = 0; out->load_bias = bias; out->phdr_uva = 0;
        out->phnum = eh.phnum; out->phentsize = eh.phentsize;
        out->has_interp = 0; out->interp[0] = '\0';
    }

    int loaded = 0;
    uintptr_t phdr_from_pt = 0;                   /* PT_PHDR vaddr, if present */
    uintptr_t phdr_from_load = 0;                 /* derived from covering LOAD */
    for (uint16_t i = 0; i < eh.phnum; i++) {
        uintptr_t off = eh.phoff + (uintptr_t)i * eh.phentsize;
        struct phdr_norm p;
        if (cls == ELFCLASS32) {
            const struct elf32_phdr* ph = (const struct elf32_phdr*)(image + off);
            p.type = ph->p_type; p.flags = ph->p_flags;
            p.offset = ph->p_offset; p.vaddr = ph->p_vaddr;
            p.filesz = ph->p_filesz; p.memsz = ph->p_memsz;
        } else {
            const struct elf64_phdr* ph = (const struct elf64_phdr*)(image + off);
            p.type = ph->p_type; p.flags = ph->p_flags;
            p.offset = (uintptr_t)ph->p_offset; p.vaddr = (uintptr_t)ph->p_vaddr;
            p.filesz = (uintptr_t)ph->p_filesz; p.memsz = (uintptr_t)ph->p_memsz;
        }

        if (p.type == PT_PHDR) {
            phdr_from_pt = p.vaddr + bias;        /* linker-provided phdr VA   */
            continue;
        }
        if (p.type == PT_INTERP && out) {
            /* Copy the interpreter path (bounded, NUL-terminated). */
            if (p.offset + p.filesz > im->len) return ELF_ESEGBOUND;
            uintptr_t n = p.filesz;
            if (n >= sizeof out->interp) n = sizeof out->interp - 1;
            if (img_copy(im, (uint8_t*)out->interp, p.offset, n) != 0) return ELF_ESEGBOUND;
            out->interp[n] = '\0';
            out->has_interp = 1;
            continue;
        }
        if (p.type != PT_LOAD || p.memsz == 0) continue;

        /* If this LOAD segment's file range covers the program-header table
         * (file offset e_phoff), the phdr's mapped VA is derivable from it.
         * This is how AT_PHDR is found for images without a PT_PHDR (our
         * static ET_EXEC binaries); PIE/musl carry an authoritative PT_PHDR. */
        if (!phdr_from_load && p.offset <= eh.phoff &&
            eh.phoff < p.offset + p.filesz)
            phdr_from_load = p.vaddr + bias + (eh.phoff - p.offset);

        /* §M90 (2026-10-01) — A SEGMENT MAY ONLY LAND WHERE A PROGRAM MAY
         * LIVE.  A fixed-address image is placed at its own p_vaddr, and on
         * x86_64 the kernel itself runs at low virtual addresses: a static Go
         * binary linked at 0x200000 (`docker`) was mapped THROUGH the page
         * tables the kernel shares with every process, writing a user page
         * into the kernel's own text mapping — any ELF could overwrite the
         * kernel.  Refused, with the address, before anything is mapped. */
        {
            uintptr_t lo = (p.vaddr + bias) & PAGE_MASK;
            uintptr_t hi = p.vaddr + bias + p.memsz;
            if (hi < lo || hi > vmm_user_limit() ||
                vmm_space_range_state(space, lo, (hi + PAGE_SIZE - 1) & PAGE_MASK) & VMA_RS_KERNEL) {
                kprintf("elf: segment %lx..%lx lies in the kernel's address range on "
                        "this machine - not loaded\n", (unsigned long)lo, (unsigned long)hi);
                return ELF_EKERNELVA;
            }
        }
        int rc = map_segment(space, im, &p, bias, &placed, src);
        if (rc != ELF_OK) return rc;
        loaded++;
    }
    if (!loaded) return ELF_ENOLOAD;

    if (out) {
        out->entry = eh.entry + bias;
        /* AT_PHDR: prefer the authoritative PT_PHDR VA, else the VA derived
         * from whichever PT_LOAD maps the header table.  Both are real user
         * virtual addresses (unlike the raw file offset e_phoff). */
        out->phdr_uva = phdr_from_pt ? phdr_from_pt : phdr_from_load;
    }
    return ELF_OK;
}

/* §M74 — elf_load_ex with the file the image was read from (see g_elf_src).
 * The image must be the file's whole contents, byte for byte. */
int elf_load_ex_file(struct vmm_space* space, const void* image, size_t len,
                     uintptr_t load_bias, struct elf_load_info* out, struct file* src) {
    struct elf_img im = { (const uint8_t*)image, len, len, src };
    return elf_load_impl(space, &im, load_bias, out, src);
}
int elf_load_ex(struct vmm_space* space, const void* image, size_t len,
                uintptr_t load_bias, struct elf_load_info* out) {
    struct elf_img im = { (const uint8_t*)image, len, len, NULL };
    return elf_load_impl(space, &im, load_bias, out, NULL);
}

/* §M90 — load straight from an open file: only the headers are buffered. */
#include "kmalloc.h"
int elf_head(struct file* f, uint8_t* buf, size_t cap, size_t* got) {
    *got = 0;
    if (!f || !f->inode) return -1;
    size_t flen = (size_t)f->inode->size;
    size_t n = flen < cap ? flen : cap;
    f->pos = 0;
    if (vfs_read(f, buf, n) != (ssize_t)n) return -1;
    *got = n;
    return 0;
}
int elf_load_file(struct vmm_space* space, struct file* f, uintptr_t load_bias,
                  struct elf_load_info* out) {
    if (!f || !f->inode || f->inode->size == 0) return ELF_EBADMAG;
    size_t cap = ELF_HEAD_BYTES;
    uint8_t* head = (uint8_t*)kmalloc(cap);
    if (!head) return ELF_ENOMEM;
    size_t got = 0;
    if (elf_head(f, head, cap, &got) != 0) { kfree(head); return ELF_EBADHDR; }
    struct elf_img im = { head, got, (size_t)f->inode->size, f };
    int rc = elf_load_impl(space, &im, load_bias, out, f);
    kfree(head);
    return rc;
}

int elf_load(struct vmm_space* space, const void* image, size_t len,
             uintptr_t* entry) {
    struct elf_load_info info;
    int rc = elf_load_ex(space, image, len, 0, &info);
    if (rc == ELF_OK && entry) *entry = info.entry;
    return rc;
}

/* ---- self-test image builder (native class) ------------------------------- */

static void put16(uint8_t* b, uint16_t v) { b[0]=v; b[1]=v>>8; }
static void put32(uint8_t* b, uint32_t v) { for (int i=0;i<4;i++) b[i]=(uint8_t)(v>>(8*i)); }
static void put64(uint8_t* b, uint64_t v) { for (int i=0;i<8;i++) b[i]=(uint8_t)(v>>(8*i)); }

size_t elf_build_selftest(void* buf, size_t cap, uintptr_t vaddr,
                          const void* payload, size_t payload_len) {
    const uintptr_t seg_off = PAGE_SIZE;          /* segment file offset = page 2 */
    size_t total = seg_off + payload_len;
    if (cap < total) return 0;

    uint8_t* b = (uint8_t*)buf;
    for (size_t i = 0; i < total; i++) b[i] = 0;

    /* e_ident */
    b[0]=0x7F; b[1]='E'; b[2]='L'; b[3]='F';
    b[EI_DATA]=ELFDATA2LSB; b[6]=1 /*EV_CURRENT*/;

    int is64 = (sizeof(uintptr_t) == 8);
    b[EI_CLASS] = is64 ? ELFCLASS64 : ELFCLASS32;

    if (!is64) {
        /* ELF32: Ehdr(52) + Phdr(32) at e_phoff=52. */
        put16(b+16, ET_EXEC);           /* e_type */
        put16(b+18, 3 /*EM_386*/);      /* e_machine (informational) */
        put32(b+20, 1);                 /* e_version */
        put32(b+24, (uint32_t)vaddr);   /* e_entry */
        put32(b+28, 52);                /* e_phoff */
        put32(b+32, 0);                 /* e_shoff */
        put32(b+36, 0);                 /* e_flags */
        put16(b+40, 52);                /* e_ehsize */
        put16(b+42, 32);                /* e_phentsize */
        put16(b+44, 1);                 /* e_phnum */
        uint8_t* p = b + 52;
        put32(p+0,  PT_LOAD);
        put32(p+4,  (uint32_t)seg_off);      /* p_offset */
        put32(p+8,  (uint32_t)vaddr);        /* p_vaddr */
        put32(p+12, (uint32_t)vaddr);        /* p_paddr */
        put32(p+16, (uint32_t)payload_len);  /* p_filesz */
        put32(p+20, (uint32_t)payload_len);  /* p_memsz */
        put32(p+24, PF_R | PF_X);            /* p_flags */
        put32(p+28, PAGE_SIZE);              /* p_align */
    } else {
        /* ELF64: Ehdr(64) + Phdr(56) at e_phoff=64. */
        put16(b+16, ET_EXEC);
        put16(b+18, 62 /*EM_X86_64 (informational)*/);
        put32(b+20, 1);
        put64(b+24, (uint64_t)vaddr);   /* e_entry */
        put64(b+32, 64);                /* e_phoff */
        put64(b+40, 0);                 /* e_shoff */
        put32(b+48, 0);                 /* e_flags */
        put16(b+52, 64);                /* e_ehsize */
        put16(b+54, 56);                /* e_phentsize */
        put16(b+56, 1);                 /* e_phnum */
        uint8_t* p = b + 64;
        put32(p+0,  PT_LOAD);                /* p_type */
        put32(p+4,  PF_R | PF_X);            /* p_flags */
        put64(p+8,  (uint64_t)seg_off);      /* p_offset */
        put64(p+16, (uint64_t)vaddr);        /* p_vaddr */
        put64(p+24, (uint64_t)vaddr);        /* p_paddr */
        put64(p+32, (uint64_t)payload_len);  /* p_filesz */
        put64(p+40, (uint64_t)payload_len);  /* p_memsz */
        put64(p+48, PAGE_SIZE);              /* p_align */
    }

    copy_bytes(b + seg_off, (const uint8_t*)payload, payload_len);
    return total;
}
