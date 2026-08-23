/* =============================================================================
 * abi.h — the guest-ABI translation engine (§M50).
 *
 * WHAT PROBLEM THIS SOLVES.  d-os services foreign binaries by translating
 * their syscall ABI onto its own primitives.  That translation was written by
 * hand, per architecture: `hal/x86/linux_abi.c` and `hal/x86_64/linux_abi.c`
 * are 2275 lines and ~160 `case` labels between them, and they are two copies
 * of ONE idea.  aarch64 would have been a third.  The numbers differ per arch
 * (Linux/i386, Linux/amd64 and Linux/arm64 are three different number spaces),
 * but what each number MEANS does not.
 *
 * So the meaning is factored out and the numbering is left as data:
 *
 *      guest trap ──► arch shim ──► number map ──► canonical op ──► handler
 *      (regs)        (frame→args)  (per guest ABI) (arch-neutral)  (shared)
 *
 * Each stage is replaceable on its own.  A new ARCHITECTURE is a new shim
 * (~20 lines: which registers hold the number, the arguments and the result).
 * A new GUEST ABI is a new number map — a table, not a file.  A new SYSCALL is
 * one handler, and every arch and every guest ABI that names it gets it at
 * once.
 *
 * WHY A CANONICAL OP AND NOT "just call sys_* directly".  Because the mapping
 * is not always one-to-one: several guest numbers can mean the same operation
 * (`exit` / `exit_group`), one guest number can need argument reshaping before
 * it means anything native (`openat` vs `open`), and error values need
 * translating into the guest's own errno space.  A canonical op is the place
 * where those differences are resolved ONCE instead of once per arch.
 *
 * WHAT THIS IS NOT.  Not an emulator: the guest's instructions run natively.
 * This translates the SYSCALL BOUNDARY only — the point where a foreign binary
 * asks the kernel for something.
 * ============================================================================= */

#ifndef ABI_H
#define ABI_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Canonical operations.
 *
 * The vocabulary every guest ABI is translated INTO.  Deliberately named after
 * what the operation does, not after any one system's spelling of it — this is
 * the interlingua, and tying it to Linux's names would quietly make Linux the
 * only guest that ever fits.
 *
 * Numbering is internal and may be reordered freely; nothing outside the kernel
 * sees these values.  Add to the end when adding an operation.
 * ------------------------------------------------------------------------- */
enum abi_op {
    ABI_OP_NONE = 0,        /* not mapped — the caller falls back */

    /* File descriptors + I/O */
    ABI_READ,
    ABI_WRITE,
    ABI_CLOSE,
    ABI_SEEK,

    /* Memory */
    ABI_MPROTECT,
    ABI_MUNMAP,

    /* Process identity */
    ABI_GETPID,
    ABI_GETPPID,
    ABI_GETTID,

    /* Process lifetime.  ABI_EXIT never returns — a handler is allowed not to,
     * and the shim must not assume it will. */
    ABI_EXIT,

    /* Scatter/gather I/O.  Named separately from READ/WRITE rather than
     * folded into them: the argument SHAPE differs (a vector, not a buffer),
     * and hiding that behind the same op would push the difference back into
     * the per-arch shims, which is exactly what this vocabulary exists to
     * prevent. */
    ABI_READV,
    ABI_WRITEV,

    /* Terminal control.  Answered, not implemented: see the handler. */
    ABI_IOCTL,

    /* Memory */
    ABI_BRK,
    ABI_MMAP,

    /* Threading identity used at libc startup. */
    ABI_SET_TID_ADDRESS,

    /* Signal mask.  Answered as best-effort success: d-os has no per-task
     * blocked-signal set yet, and a libc that cannot mask signals still runs
     * correctly — it just cannot defer them.  Reporting failure instead would
     * abort startup in libcs that treat it as fatal. */
    ABI_SIGPROCMASK,

    /* Process control.  Both are arch-neutral: they take pointers and return a
     * value, with none of exit's or clone's coupling to the trap frame. */
    ABI_WAIT,
    ABI_EXECVE,

    /* §M53 stage 3 — timing.  A deadline behind a descriptor, so a guest event
     * loop can wait for time and for I/O in one place, plus the interval timer
     * that delivers SIGALRM for the guest that wants to be interrupted instead.
     * Both arrive as `struct itimerspec`, whose WORD WIDTH is a property of the
     * guest ABI — which is why the map now carries it (see abi_map.word_bytes)
     * rather than the handler guessing. */
    ABI_TIMERFD_CREATE,
    ABI_TIMERFD_SETTIME,
    ABI_TIMERFD_GETTIME,
    ABI_SETITIMER,

    /* §M56 — a readiness SET kept by the kernel.  `struct epoll_event` is the
     * sharpest example yet of why the map has to describe the guest: it is 12
     * bytes on i386 AND on amd64 — Linux packs it on x86_64 specifically so the
     * 32- and 64-bit layouts agree — but 16 on arm64, where it is not packed
     * and the u64 aligns to 8.  So its size does NOT follow word_bytes, and a
     * handler that derived it from the word size would get amd64 wrong.  The
     * map carries the size itself (abi_map.epoll_event_bytes). */
    ABI_EPOLL_CREATE,
    ABI_EPOLL_CTL,
    ABI_EPOLL_WAIT,

    /* §M56.1 — the query half of the signal mask.  Without it a program can
     * block a signal but never find out that one arrived, which makes
     * "defer this signal" indistinguishable from "discard it" from the
     * inside — and it is the only way to assert that the mask DEFERS. */
    ABI_SIGPENDING,

    /* §M24 second half — the BSD socket surface.
     *
     * These arrive one operation per number on amd64 and arm64, and on i386
     * both as direct numbers AND multiplexed through `socketcall(102)`; the
     * i386 shim demultiplexes into exactly these ops, so there is one
     * implementation rather than one per calling convention.
     *
     * ACCEPT and ACCEPT4 are SEPARATE ops although accept4 is accept plus a
     * flag word, and SEND/SENDTO likewise.  The reason is arity: the arch shim
     * fills all six argument slots from registers, so a handler that reads a
     * fourth argument a three-argument call never passed reads whatever the
     * guest happened to leave there.  One op per ARITY is the only version
     * that cannot silently take a garbage flag word for an instruction. */
    /* §M65 — the display bridge's toolkit build.  A d-os operation, not a
     * Linux one, which is exactly why it goes through the ENGINE: the number
     * is the same on every guest, and a shared handler means all three arches
     * gain it at once instead of three switch arms drifting apart. */
    ABI_UI_BUILD,

    ABI_SOCKET,
    ABI_BIND,
    ABI_CONNECT,
    ABI_LISTEN,
    ABI_ACCEPT,
    ABI_ACCEPT4,
    ABI_GETSOCKNAME,
    ABI_GETPEERNAME,
    ABI_SEND,
    ABI_SENDTO,
    ABI_RECV,
    ABI_RECVFROM,
    ABI_SHUTDOWN,
    ABI_SETSOCKOPT,
    ABI_GETSOCKOPT,

    ABI_OP_MAX
};

/* ---------------------------------------------------------------------------
 * Call context.
 *
 * The normalised form of a guest syscall: six machine-word arguments and a
 * result.  Everything arch-specific has already been stripped off by the shim
 * — which register held what, how the guest packs a 64-bit offset on a 32-bit
 * machine, whether the number arrives in eax or x8.  A handler sees only the
 * arguments and never learns which architecture it is running on, which is the
 * property that makes it shareable at all.
 * ------------------------------------------------------------------------- */
struct abi_ctx {
    unsigned long a[6];         /* arguments, guest order */
    unsigned long nr;           /* the guest's own syscall number (diagnostics) */
    const struct abi_map* map;  /* the ABI this call arrived through */
};

/* A handler returns the value the guest should see, already in the guest's
 * error convention (negative errno for Linux-shaped ABIs).  It must not touch
 * the trap frame: the shim owns that. */
typedef long (*abi_handler_fn)(struct abi_ctx* c);

/* ---------------------------------------------------------------------------
 * Number map — one per GUEST ABI (Linux/i386, Linux/amd64, Linux/arm64, ...).
 *
 * `nr` is the guest's number, `op` what it means.  Entries need not be sorted
 * or dense: the lookup is linear over a table small enough that it does not
 * matter, and keeping the table in a readable order is worth more than the few
 * cycles a binary search would save on a path that is already a trap.
 * ------------------------------------------------------------------------- */
struct abi_nument {
    uint32_t nr;
    uint16_t op;                /* enum abi_op */
};

struct abi_map {
    const char*             name;       /* "linux/amd64" — appears in diagnostics */
    const struct abi_nument* ents;
    uint32_t                n_ents;
    /* §M53 stage 3 — the guest's word size in bytes (4 or 8).
     *
     * Most operations never need it: a pointer is a pointer and an int is an
     * int.  But some pass STRUCTS whose layout is `long`-shaped — timespec,
     * itimerspec, stat — and those are 16 bytes on a 32-bit guest and 32 on a
     * 64-bit one.  Putting the width in the MAP keeps that where the rest of
     * the guest's description already lives; a handler that inferred it from
     * the host's own word size would be right only by coincidence, and would
     * silently break the first time a 32-bit guest ran on a 64-bit kernel. */
    uint8_t                 word_bytes;
    /* §M56 — sizeof(struct epoll_event) IN THE GUEST.  Kept separate from
     * word_bytes on purpose: it is 12 on both i386 and amd64 but 16 on arm64,
     * so it is not derivable from the word size and never was.  See the
     * ABI_EPOLL_* note above. */
    uint8_t                 epoll_event_bytes;
};

/* Look up a guest number.  Returns ABI_OP_NONE when the map does not name it,
 * which is a normal outcome, not an error: a partially migrated layer answers
 * what it knows and lets its caller handle the rest. */
enum abi_op abi_lookup(const struct abi_map* map, unsigned long nr);

/* Execute a canonical operation.  `*out` receives the guest-visible result.
 * Returns 1 if the operation ran, 0 if no handler is registered for it (again:
 * a normal outcome while the vocabulary is being filled in). */
int abi_invoke(enum abi_op op, struct abi_ctx* c, long* out);

/* Convenience: look up and invoke.  Returns 1 if handled. */
int abi_dispatch(const struct abi_map* map, unsigned long nr,
                 unsigned long a0, unsigned long a1, unsigned long a2,
                 unsigned long a3, unsigned long a4, unsigned long a5,
                 long* out);

/* The guest ABIs the kernel knows.  Defined in kernel/core/abi_linux.c. */
extern const struct abi_map abi_map_linux_i386;
extern const struct abi_map abi_map_linux_amd64;
extern const struct abi_map abi_map_linux_arm64;

/* Diagnostics for `abi` / /proc: how many operations have handlers, and how
 * many numbers each known map translates. */
void abi_stats(int* ops_with_handlers, int* ops_total);

#endif
