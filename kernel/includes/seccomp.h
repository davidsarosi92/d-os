/* =============================================================================
 * seccomp.h — Linux's system-call filter (seccomp mode 2), §M90.  See seccomp.c.
 * ============================================================================= */
#ifndef SECCOMP_H
#define SECCOMP_H
#include <stdint.h>

struct seccomp_filter;
struct task;

/* Reference counting of a chain (NULL is fine for both). */
void seccomp_get(struct seccomp_filter* f);
void seccomp_put(struct seccomp_filter* f);

/* The guest's audit architecture (AUDIT_ARCH_*), what seccomp_data.arch says. */
#define SECCOMP_ARCH_I386     0x40000003u
#define SECCOMP_ARCH_X86_64   0xC000003Eu
#define SECCOMP_ARCH_AARCH64  0xC00000B7u

/* At the top of a Linux-personality system call: run the caller's filters.
 * 0 = proceed with the call; 1 = do NOT run it, return *ret to the program
 * (ERRNO, TRAP, TRACE and USER_NOTIF without a tracer/listener).  A KILL
 * action does not return.  Only called when the caller HAS a filter (the
 * dispatchers test task->cred.seccomp first, so an unfiltered task pays one
 * pointer test). */
int seccomp_enter(struct task* t, unsigned long nr, const unsigned long a[6],
                  uintptr_t ip, uint32_t arch, long* ret);

/* seccomp(op, flags, uargs) and prctl(PR_SET_SECCOMP / PR_GET_SECCOMP). */
long seccomp_syscall(unsigned op, unsigned long flags, uintptr_t uargs, unsigned word_bytes);
long seccomp_prctl_set(unsigned long mode, uintptr_t uprog, unsigned word_bytes);
int  seccomp_mode(const struct task* t);          /* 0 none, 2 filter (1 strict) */

#endif
