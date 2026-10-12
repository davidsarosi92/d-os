/* tty.h — terminals: pseudo-terminal pairs, the line discipline, devpts
 * instances and the controlling terminal (§M90 rung 6, kernel/core/tty.c).
 *
 * The Linux contract, because the programs using it are Linux programs:
 * `docker run -it` (runc opens /dev/ptmx in the container's devpts, hands the
 * master to the shim over a unix socket and makes the slave the container
 * init's controlling terminal), every shell's job control, and isatty(). */
#ifndef TTY_H
#define TTY_H
#include <stdint.h>
#include <stddef.h>

struct ofile;
struct task;
struct tty;

/* Linux's struct termios as the TCGETS/TCSETS ioctls carry it: identical on
 * i386, x86_64 and arm64 (NCCS 19, 36 bytes) — the one terminal structure
 * that needs no per-guest layout. */
#define TTY_NCCS 19
struct ktermios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[TTY_NCCS];
} __attribute__((packed));

/* An ofile is a terminal end: master (the ptmx side) or slave (/dev/pts/N). */
#define TTY_SLAVE  0
#define TTY_MASTER 1

/* Open hooks (devfs nodes call these): a new pty from a ptmx node of devpts
 * instance `inst`, the slave `index` of an instance, the caller's controlling
 * terminal.  `noctty` is O_NOCTTY.  0 or a negative errno. */
int  tty_open_ptmx(void* inst, struct ofile* o, int noctty);
int  tty_open_slave(void* slave_ctx, struct ofile* o, int noctty);
int  tty_open_ctty(struct ofile* o);

/* The ofile's end is going away (fd.c's last unref). */
void tty_release(struct ofile* o);

/* read / write / readiness / ioctl on an end.  `block` is !O_NONBLOCK.
 * Errors are negative Linux errnos. */
long     tty_read(struct ofile* o, void* buf, size_t n, int block);
long     tty_write(struct ofile* o, const void* buf, size_t n, int block);
uint32_t tty_readiness(struct ofile* o);
/* `arg` is the ioctl's third argument: a KERNEL buffer for the commands that
 * carry a structure (the engine copies it in and out — see tty_ioctl_size),
 * the value itself for the ones that carry an int.  Returns >= 0 or -errno. */
long     tty_ioctl(struct ofile* o, unsigned cmd, void* kbuf, unsigned long val);
/* How the engine marshals `cmd`'s argument: bytes and direction
 * (1 = the kernel writes it, 2 = the kernel reads it, 0 = a value). */
int      tty_ioctl_size(unsigned cmd, int* dir);

/* /proc/<pid>/fd and /proc/<pid>/stat: the end's name and Linux device number. */
int      tty_name(struct ofile* o, char* out, size_t cap);
uint32_t tty_devnum(struct ofile* o, int* is_master);
/* The controlling terminal's device number (stat's tty_nr), 0 = none. */
uint32_t tty_ctty_devnum(struct task* t);

/* devpts: a mount of type devpts made the anonymous directory `path` (its
 * ptmx node is created here).  The machine's own instance is /dev/pts with
 * /dev/ptmx, created by tty_init. */
int  devpts_instance_new(const char* path);
void tty_init(void);

/* Process lifecycle: a session leader exiting hangs its terminal up; setsid
 * leaves the old one. */
void tty_task_exit(struct task* t);

/* The console (a VC) as a terminal: termios, raw reads, size (tty.c). */
int  tty_console_cooked(void);                  /* 1 = the shell's cooked reader applies */
struct vc;
int  tty_vc_raw(struct vc* v);                  /* a program holds it raw: send key sequences */
long tty_console_read(void* buf, size_t n, int block);
int  tty_console_readable(void);
long tty_console_ioctl(unsigned cmd, void* kbuf, unsigned long val);

/* Linux pgrp signal delivery (lnx_signal.c): `sig` to every process of group
 * `pgid` (global numbering, 0 = the boot group).  `check` = the caller's
 * permission applies (kill), else it is the kernel's own signal (a tty's ^C).
 * Returns the number of processes signalled. */
int  lnx_sig_pgrp(int pgid, int sig, int check);

#endif
