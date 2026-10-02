/* flock.h — BSD advisory file locks (flock(2)), §M90.  See core/flock.c. */
#ifndef FLOCK_H
#define FLOCK_H

struct ofile;

/* flock(2) on an open file description: LOCK_SH 1 / LOCK_EX 2 / LOCK_UN 8,
 * optionally | LOCK_NB 4.  0, or a negative Linux errno (EWOULDBLOCK -11,
 * EINTR -4, EINVAL -22, ENOLCK -37, EBADF -9). */
int  flock_op(struct ofile* o, int op);

/* Drop every lock `o` holds — called when its last descriptor closes. */
void flock_release(struct ofile* o);

#endif
