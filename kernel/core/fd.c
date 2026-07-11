/* =============================================================================
 * fd.c — generic open-file objects + shared-memory objects (M25 stage 4+).
 * See fd.h.  Arch-neutral: frames come from the PMM, everything else is plain
 * bookkeeping.
 * ============================================================================= */

#include "fd.h"
#include "vfs.h"
#include "pmm.h"
#include "kmalloc.h"
#include <stddef.h>
#include <stdint.h>

/* unix socket teardown lives in the (stage-5) socket module; declared weakly
 * here so FD_SOCK unref links before that module exists. */
void usock_close(struct usock* s) __attribute__((weak));
void usock_close(struct usock* s) { (void)s; }

/* network-socket teardown lives in usyscall.c (M24 socket API); weak here so
 * FD_NETSOCK unref links even in builds that don't pull it in. */
void netsock_close(struct netsock* s) __attribute__((weak));
void netsock_close(struct netsock* s) { (void)s; }

/* ---- ofile ---------------------------------------------------------------- */

static struct ofile* ofile_alloc(enum fd_kind k) {
    struct ofile* o = (struct ofile*)kcalloc(1, sizeof *o);
    if (!o) return NULL;
    o->kind = k;
    o->refcount = 1;
    return o;
}

struct ofile* ofile_from_file(struct file* f) {
    if (!f) return NULL;
    struct ofile* o = ofile_alloc(FD_VFS);
    if (o) o->file = f;
    return o;
}
struct ofile* ofile_from_shm(struct shm* s) {
    if (!s) return NULL;
    struct ofile* o = ofile_alloc(FD_SHM);
    if (o) o->shm = shm_ref(s);
    return o;
}
struct ofile* ofile_from_sock(struct usock* s) {
    struct ofile* o = ofile_alloc(FD_SOCK);
    if (o) o->sock = s;
    return o;
}
struct ofile* ofile_from_netsock(struct netsock* s) {
    struct ofile* o = ofile_alloc(FD_NETSOCK);
    if (o) o->nsock = s;
    return o;
}

struct ofile* ofile_ref(struct ofile* o) {
    if (o) o->refcount++;
    return o;
}

void ofile_unref(struct ofile* o) {
    if (!o) return;
    if (--o->refcount > 0) return;
    switch (o->kind) {
        case FD_VFS:  if (o->file) vfs_close(o->file);   break;
        case FD_SHM:  if (o->shm)  shm_unref(o->shm);    break;
        case FD_SOCK: if (o->sock) usock_close(o->sock); break;
        case FD_NETSOCK: if (o->nsock) netsock_close(o->nsock); break;
    }
    kfree(o);
}

/* ---- shared memory -------------------------------------------------------- */

struct shm* shm_create(size_t size) {
    int n = (int)((size + 4095) / 4096);
    if (n <= 0) n = 1;
    if (n > SHM_MAX_FRAMES) return NULL;

    struct shm* s = (struct shm*)kcalloc(1, sizeof *s);
    if (!s) return NULL;
    s->refcount = 1;
    s->nframes  = n;
    for (int i = 0; i < n; i++) {
        uint32_t f = pmm_alloc_frame();
        if (!f) {                               /* OOM — unwind */
            for (int j = 0; j < i; j++) pmm_free_frame(s->frames[j]);
            kfree(s);
            return NULL;
        }
        /* Zero the frame through the identity map (frames are < 1 GiB). */
        uint8_t* p = (uint8_t*)(uintptr_t)f;
        for (int b = 0; b < 4096; b++) p[b] = 0;
        s->frames[i] = f;
    }
    return s;
}

struct shm* shm_ref(struct shm* s) {
    if (s) s->refcount++;
    return s;
}

void shm_unref(struct shm* s) {
    if (!s) return;
    if (--s->refcount > 0) return;
    for (int i = 0; i < s->nframes; i++) pmm_free_frame(s->frames[i]);
    kfree(s);
}
