/* fifo.h — named pipes (mkfifo / mknod S_IFIFO), §M90; see fifo.c. */
#ifndef DOS_FIFO_H
#define DOS_FIFO_H
#include <stdint.h>
#include <stddef.h>

struct ofile;
struct file;

#define FIFO_R 0x1u                 /* this open file description reads   */
#define FIFO_W 0x2u                 /* ...and/or writes                    */

/* Attach `o` (an FD_FIFO ofile whose `file` is the VFS open of the FIFO's
 * inode) to the inode's pipe, as a reader and/or writer (FIFO_R|FIFO_W).
 * Blocks as POSIX open(2) does unless `nonblock`: a reader until a writer
 * opens, a writer until a reader opens; O_RDWR never waits.
 * 0, or a NEGATIVE Linux errno: -6 ENXIO (non-blocking writer, no reader),
 * -4 EINTR (killed while waiting), -12 ENOMEM.  On failure `o` is NOT
 * attached and the caller releases it as an ordinary file. */
int  fifo_attach(struct ofile* o, unsigned role, int nonblock);
/* The last reference to `o` is going: detach it (wakes the other side). */
void fifo_detach(struct ofile* o);

/* Bytes, 0 at end of file (no writer left), or a NEGATIVE Linux errno:
 * -11 EAGAIN, -32 EPIPE (no reader left), -9 EBADF (wrong direction),
 * -4 EINTR (killed while blocked). */
long fifo_read (struct ofile* o, void* buf, size_t n, int block);
long fifo_write(struct ofile* o, const void* buf, size_t n, int block);
/* POLL* bits for poll/epoll (Linux pipe_poll's rules). */
uint32_t fifo_readiness(struct ofile* o);
#endif
