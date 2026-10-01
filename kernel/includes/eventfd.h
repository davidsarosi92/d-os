/* eventfd.h — a counter behind a descriptor (§M90); see eventfd.c. */
#ifndef DOS_EVENTFD_H
#define DOS_EVENTFD_H
#include <stdint.h>
#include <stddef.h>
struct eventfd;
struct ofile;
struct eventfd* eventfd_create_obj(uint64_t init, int semaphore);
void eventfd_set_owner(struct eventfd* e, struct ofile* o);
void eventfd_close(struct eventfd* e);
/* 8 bytes, or a NEGATIVE Linux errno: -11 EAGAIN (non-blocking), -22 EINVAL. */
long eventfd_read (struct eventfd* e, void* buf, size_t n, int block);
long eventfd_write(struct eventfd* e, const void* buf, size_t n, int block);
int  eventfd_can_read(struct eventfd* e);
int  eventfd_can_write(struct eventfd* e);
#endif
