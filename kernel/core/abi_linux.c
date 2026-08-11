/* =============================================================================
 * abi_linux.c — the Linux guest ABIs as DATA (§M50).
 *
 * Three number spaces, one meaning.  Linux numbers its syscalls differently on
 * every architecture — `read` is 3 on i386, 0 on amd64 and 63 on arm64 — but
 * it is the same `read` in all three.  That is the entire reason this file is
 * tables and not code: the difference between the ports is numbering, and
 * numbering is data.
 *
 * These tables are also the answer to "how do we support a NEW architecture":
 * add a table.  And to "how do we support a new GUEST" (a BSD ABI, a different
 * Linux generation, a bespoke one): add a table.  Neither requires touching a
 * handler, because a handler never learns which number brought it here.
 *
 * The arm64 table is filled in even though no aarch64 shim consumes it yet —
 * deliberately.  Writing it beside its siblings is where the numbering is
 * easiest to get right, and it makes the claim "a new arch is a table"
 * checkable rather than aspirational.
 *
 * NOT mapped here yet, on purpose:
 *
 *   `exit` / `exit_group`.  The x86 layers' exit does two things — terminate a
 *   user process, and, when the ELF was run as a synchronous EXCURSION from the
 *   kernel (`proc_exec_elf`, which is how `musltest` and every self-test runs),
 *   teleport back to the kernel stack it came from.  That second half is
 *   arch-coupled (a saved SP/PC pair per arch), so exit cannot be a shared
 *   handler until the excursion path is unified.  Mapping it before then would
 *   have silently broken every self-test on both x86 arches, which is the kind
 *   of thing the "engine may decline, the switch is the fallback" design exists
 *   to make survivable — but declining on purpose is better than finding out.
 *
 *   amd64 `mmap`.  Its existing case translates a failure into -ENOMEM; until
 *   the canonical handler does exactly that, routing it here would change
 *   behaviour rather than move it.
 *
 * `rt_sigprocmask` is mapped for arm64 only so far.  The x86 layers fold it
 * into a shared "best-effort success" case alongside rt_sigaction, membarrier
 * and fcntl; splitting one number out of that group is a behaviour change until
 * each is checked individually, and the engine declining is cheaper than
 * guessing.
 *
 * Numbers verified against the Linux kernel's own tables:
 *   i386   arch/x86/entry/syscalls/syscall_32.tbl
 *   amd64  arch/x86/entry/syscalls/syscall_64.tbl
 *   arm64  include/uapi/asm-generic/unistd.h (the generic ABI arm64 uses)
 * ============================================================================= */

#include "abi.h"

/* ---- Linux / i386 (int 0x80, the classic i386 numbering) ------------------ */
static const struct abi_nument linux_i386_ents[] = {
    {   3, ABI_READ     },
    {   4, ABI_WRITE    },
    {   6, ABI_CLOSE    },
    {  19, ABI_SEEK     },
    {  20, ABI_GETPID   },
    {  64, ABI_GETPPID  },
    {  91, ABI_MUNMAP   },
    { 125, ABI_MPROTECT },
    { 145, ABI_READV    },
    { 146, ABI_WRITEV   },
    {  54, ABI_IOCTL    },
    {  45, ABI_BRK      },
    { 258, ABI_SET_TID_ADDRESS },
    { 224, ABI_GETTID   },
    /* §M53 stage 3 — timing. */
    { 322, ABI_TIMERFD_CREATE  },
    { 325, ABI_TIMERFD_SETTIME },
    { 254, ABI_EPOLL_CREATE },      /* epoll_create  */
    { 329, ABI_EPOLL_CREATE },      /* epoll_create1 */
    { 255, ABI_EPOLL_CTL },
    { 256, ABI_EPOLL_WAIT },
    { 319, ABI_EPOLL_WAIT },        /* epoll_pwait */
    { 176, ABI_SIGPENDING },        /* rt_sigpending  */
    { 175, ABI_SIGPROCMASK },       /* rt_sigprocmask */
    { 326, ABI_TIMERFD_GETTIME },
    { 104, ABI_SETITIMER       },
};

/* ---- Linux / amd64 -------------------------------------------------------- */
static const struct abi_nument linux_amd64_ents[] = {
    {   0, ABI_READ     },
    {   1, ABI_WRITE    },
    {   3, ABI_CLOSE    },
    {   8, ABI_SEEK     },
    {  10, ABI_MPROTECT },
    {  11, ABI_MUNMAP   },
    {  39, ABI_GETPID   },
    { 110, ABI_GETPPID  },
    {  19, ABI_READV    },
    {  20, ABI_WRITEV   },
    {  16, ABI_IOCTL    },
    {  12, ABI_BRK      },
    { 218, ABI_SET_TID_ADDRESS },
    { 186, ABI_GETTID   },
    /* §M53 stage 3 — timing. */
    { 283, ABI_TIMERFD_CREATE  },
    { 286, ABI_TIMERFD_SETTIME },
    { 213, ABI_EPOLL_CREATE },      /* epoll_create  */
    { 291, ABI_EPOLL_CREATE },      /* epoll_create1 */
    { 233, ABI_EPOLL_CTL },
    { 232, ABI_EPOLL_WAIT },
    { 281, ABI_EPOLL_WAIT },        /* epoll_pwait */
    { 127, ABI_SIGPENDING },        /* rt_sigpending  */
    {  14, ABI_SIGPROCMASK },       /* rt_sigprocmask */
    { 287, ABI_TIMERFD_GETTIME },
    {  38, ABI_SETITIMER       },
};

/* ---- Linux / arm64 (the asm-generic numbering) ---------------------------- */
static const struct abi_nument linux_arm64_ents[] = {
    {  57, ABI_CLOSE    },
    {  62, ABI_SEEK     },
    {  63, ABI_READ     },
    {  64, ABI_WRITE    },
    { 172, ABI_GETPID   },
    { 173, ABI_GETPPID  },
    { 215, ABI_MUNMAP   },
    { 226, ABI_MPROTECT },
    {  65, ABI_READV    },
    {  66, ABI_WRITEV   },
    {  29, ABI_IOCTL    },
    { 214, ABI_BRK      },
    { 222, ABI_MMAP     },
    {  96, ABI_SET_TID_ADDRESS },
    { 178, ABI_GETTID   },
    { 135, ABI_SIGPROCMASK },
    { 260, ABI_WAIT     },          /* wait4 */
    { 221, ABI_EXECVE   },
    /* §M53 stage 3 — timing. */
    {  85, ABI_TIMERFD_CREATE  },
    {  86, ABI_TIMERFD_SETTIME },
    {  20, ABI_EPOLL_CREATE },      /* epoll_create1 — arm64 has no plain
                                     * epoll_create, and no plain epoll_wait
                                     * either: glibc/musl call epoll_pwait. */
    {  21, ABI_EPOLL_CTL },
    {  22, ABI_EPOLL_WAIT },        /* epoll_pwait */
    { 136, ABI_SIGPENDING },        /* rt_sigpending */
    {  87, ABI_TIMERFD_GETTIME },
    { 103, ABI_SETITIMER       },
};

#define ARRAY_N(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* The trailing two numbers are word_bytes and epoll_event_bytes.  Note that
 * they do NOT track each other: `struct epoll_event` is 12 bytes on i386 and
 * ALSO 12 on amd64 (Linux packs it there precisely so the layouts agree), but
 * 16 on arm64, where it is unpacked and the u64 aligns to 8.  Deriving the
 * struct size from the word size would therefore be correct on exactly one of
 * these three. */
const struct abi_map abi_map_linux_i386 = {
    "linux/i386",  linux_i386_ents,  ARRAY_N(linux_i386_ents),  4, 12
};
const struct abi_map abi_map_linux_amd64 = {
    "linux/amd64", linux_amd64_ents, ARRAY_N(linux_amd64_ents), 8, 12
};
const struct abi_map abi_map_linux_arm64 = {
    "linux/arm64", linux_arm64_ents, ARRAY_N(linux_arm64_ents), 8, 16
};
