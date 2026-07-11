/* =============================================================================
 * fd.h — generic open-file object behind a file descriptor (M25 stage 4+).
 *
 * Stage 3 stored raw `struct file*` (VFS handles) in the per-process fd
 * table.  Stage 4 (shared memory) and stage 5 (unix sockets) put *non-file*
 * objects behind descriptors too, so the table now holds a `struct ofile` —
 * a tagged handle wrapping exactly one of: a VFS file, a shared-memory
 * object, or a unix socket endpoint.  It carries a refcount so a descriptor
 * can be duplicated / passed between processes (SCM_RIGHTS, stage 5) while
 * the underlying object lives until the last reference closes.
 * ============================================================================= */

#ifndef FD_H
#define FD_H

#include <stdint.h>
#include <stddef.h>

struct file;                    /* vfs.h  */
struct shm;                     /* below  */
struct usock;                   /* unix socket endpoint (stage 5)          */

enum fd_kind { FD_VFS, FD_SHM, FD_SOCK, FD_NETSOCK };

struct netsock;                 /* network (AF_INET) socket — usyscall.c        */

struct ofile {
    enum fd_kind kind;
    int          refcount;      /* # of descriptors referencing this object */
    struct file* file;          /* FD_VFS  */
    struct shm*  shm;           /* FD_SHM  */
    struct usock* sock;         /* FD_SOCK */
    struct netsock* nsock;      /* FD_NETSOCK (M24 socket API) */
};

/* Wrap a resource in a fresh ofile (refcount 1), or NULL on OOM. */
struct ofile* ofile_from_file(struct file* f);
struct ofile* ofile_from_shm (struct shm* s);
struct ofile* ofile_from_sock(struct usock* s);
struct ofile* ofile_from_netsock(struct netsock* s);

/* Refcount management.  ofile_unref drops the last reference → closes the
 * wrapped resource + frees the ofile. */
struct ofile* ofile_ref  (struct ofile* o);
void          ofile_unref(struct ofile* o);

/* ---- shared-memory object (stage 4) --------------------------------------- */

#define SHM_MAX_FRAMES 64       /* 64 × 4 KiB = 256 KiB max per object (plenty) */

struct shm {
    int      refcount;          /* independent of the ofile refcount: a frame
                                 * set can outlive an fd once mmap'd */
    int      nframes;
    uint32_t frames[SHM_MAX_FRAMES];   /* physical frame addresses */
};

/* Create a shared-memory object of `size` bytes (rounded up to pages), frames
 * zeroed.  Returns NULL on OOM / too large. */
struct shm* shm_create(size_t size);
struct shm* shm_ref   (struct shm* s);
void        shm_unref (struct shm* s);   /* frees frames at refcount 0 */

/* ---- unix socket pair + fd passing (stage 5) ------------------------------ */

int  usock_pair (struct usock** a, struct usock** b);
long usock_send (struct usock* s, const void* buf, size_t n, struct ofile* passfile);
/* Tier A.3 — `block`: when non-zero and the endpoint has nothing to receive
 * (no bytes, no passed fd) but the peer is still open, park the caller on the
 * endpoint's read wait-queue until usock_send/usock_close wakes it, then
 * re-drain.  block == 0 keeps the original non-blocking snapshot behaviour
 * (poll's drain path, single-task self-tests). */
long usock_recv (struct usock* s, void* buf, size_t n, int block,
                 struct ofile** passfile_out);
void usock_close(struct usock* s);
int  usock_can_read (struct usock* s);   /* bytes buffered? (poll POLLIN)  */
int  usock_can_write(struct usock* s);   /* peer open + space? (POLLOUT)   */

/* Tier A.3 — poll readiness signal.  usock_send / usock_close call this
 * after changing an fd's readiness so a task blocked in a (timeout < 0)
 * poll() wakes and re-scans.  Defined in usyscall.c (owns the global
 * readiness wait-queue); declared here so the socket layer can raise it. */
void fd_readiness_signal(void);

#endif
