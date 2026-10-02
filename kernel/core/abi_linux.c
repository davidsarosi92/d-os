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
    { 192, ABI_MMAP_PGOFF },
    { 145, ABI_READV    },
    { 146, ABI_WRITEV   },
    {  54, ABI_IOCTL    },
    {  45, ABI_BRK      },
    { 258, ABI_SET_TID_ADDRESS },
    { 224, ABI_GETTID   },
    /* §M53 stage 3 — timing. */
    { 322, ABI_TIMERFD_CREATE  },
    {  60, ABI_UMASK           },
    /* §M90 — setpgid 57, getpgrp 65, setsid 66, getpgid 132, getsid 147. */
    {  57, ABI_SETPGID }, {  65, ABI_GETPGRP }, {  66, ABI_SETSID },
    { 132, ABI_GETPGID }, { 147, ABI_GETSID  },
    { 328, ABI_EVENTFD         },
    { 323, ABI_EVENTFD_OLD     },
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
    /* §M24 — sockets.  i386 has BOTH: these direct numbers (since 4.3) and the
     * old `socketcall(102)` multiplexer, which the i386 shim demultiplexes into
     * the SAME canonical ops.  102 is deliberately absent from this table: it
     * is not an operation, it is an envelope. */
    { 359, ABI_SOCKET      },
    { 361, ABI_BIND        },
    { 362, ABI_CONNECT     },
    { 363, ABI_LISTEN      },
    { 364, ABI_ACCEPT4     },   /* i386 has no plain accept(2)                */
    { 365, ABI_GETSOCKOPT  },
    { 366, ABI_SETSOCKOPT  },
    /* §M73 — directory calls (i386: mkdir 39, link 9, chmod 15, unlink 10,
     * mkdirat 296, linkat 303, fchmodat 306, unlinkat 301). */
    {  39, ABI_MKDIR }, { 296, ABI_MKDIRAT }, {   9, ABI_LINK }, { 303, ABI_LINKAT },
    /* §M90 — chown32 212, lchown32 198, fchownat 298 (the 16-bit-uid
     * chown 182 / lchown 16 are left unmapped: musl never calls them). */
    { 212, ABI_CHOWN }, { 198, ABI_CHOWN }, { 298, ABI_FCHOWNAT },
    /* §M90 — rename 38, renameat 302, renameat2 353. */
    {  38, ABI_RENAME }, { 302, ABI_RENAMEAT }, { 353, ABI_RENAMEAT2 },
    { 143, ABI_FLOCK },
    /* §M90 — fsync 118, fdatasync 148, syncfs 344; rmdir 40. */
    { 118, ABI_FSYNC }, { 148, ABI_FSYNC }, { 344, ABI_FSYNC }, {  40, ABI_RMDIR },
    {  15, ABI_CHMOD }, { 306, ABI_FCHMODAT }, { 10, ABI_UNLINK }, { 301, ABI_UNLINKAT },
    /* §M73 — i386 identity: the 32-bit-uid calls (getuid32 199, getgid32 200,
     * geteuid32 201, getegid32 202, setuid32 213, setgid32 214). */
    { 199, ABI_GETUID }, { 200, ABI_GETGID }, { 201, ABI_GETEUID }, { 202, ABI_GETEGID },
    { 213, ABI_SETUID }, { 214, ABI_SETGID }, { 205, ABI_GETGROUPS },
    /* §M73 — files and time (i386 numbers; the *64 stat calls are the ones
     * musl uses, with struct stat64). */
    {   5, ABI_OPEN }, { 295, ABI_OPENAT },
    { 195, ABI_STAT }, { 196, ABI_LSTAT /* lstat64 */ }, { 197, ABI_FSTAT }, { 300, ABI_FSTATAT },
    { 220, ABI_GETDENTS64 }, {  55, ABI_FCNTL }, { 221, ABI_FCNTL /* fcntl64 */ },
    {  33, ABI_ACCESS }, { 307, ABI_FACCESSAT }, { 439, ABI_FACCESSAT } /* §M90 faccessat2 */, {  85, ABI_READLINK }, { 305, ABI_READLINKAT },
    { 239, ABI_SENDFILE /* sendfile64 */ }, { 122, ABI_UNAME },
    {  41, ABI_DUP }, {  63, ABI_DUP2 }, { 330, ABI_DUP3 },
    { 265, ABI_CLOCK_GETTIME }, { 403, ABI_CLOCK_GETTIME64 }, {  78, ABI_GETTIMEOFDAY },
    { 162, ABI_NANOSLEEP }, { 267, ABI_CLOCK_NANOSLEEP },
    { 183, ABI_GETCWD }, {  12, ABI_CHDIR }, { 174, ABI_LNX_SIGACTION },
    {  37, ABI_KILL }, { 238, ABI_TKILL }, { 270, ABI_TGKILL }, { 186, ABI_SIGALTSTACK },
    { 240, ABI_FUTEX }, { 318, ABI_GETCPU }, { 116, ABI_SYSINFO }, { 191, ABI_GETRLIMIT },
    {  76, ABI_GETRLIMIT }, {  75, ABI_SETRLIMIT }, { 340, ABI_PRLIMIT64 },
    { 266, ABI_CLOCK_GETRES }, { 172, ABI_PRCTL }, {  77, ABI_GETRUSAGE },
    { 158, ABI_SCHED_YIELD }, { 355, ABI_GETRANDOM }, { 219, ABI_MADVISE }, { 375, ABI_MEMBARRIER },
    { 356, ABI_MEMFD_CREATE }, { 218, ABI_MINCORE }, { 242, ABI_SCHED_GETAFFINITY },
    { 241, ABI_SCHED_SETAFFINITY }, { 311, ABI_SET_ROBUST_LIST },
    { 268, ABI_STATFS64 }, { 269, ABI_FSTATFS64 }, {  83, ABI_SYMLINK }, { 304, ABI_SYMLINKAT },
    { 367, ABI_GETSOCKNAME },
    { 368, ABI_GETPEERNAME },
    { 369, ABI_SENDTO      },
    { 371, ABI_RECVFROM    },
    { 373, ABI_SHUTDOWN    },
    /* 360 is socketpair and 370/372 are sendmsg/recvmsg — deliberately absent.
     * These numbers are NOT sequential by name: 360 sits between socket and
     * bind, and getsockopt comes before setsockopt.  Reciting them from memory
     * put ABI_CONNECT on bind's number, and the failure was a bind that
     * returned ECONNREFUSED — the one errno that names the handler that
     * actually ran.  Verified against arch/x86/entry/syscalls/syscall_32.tbl. */
    /* §M65 — d-os display-bridge op; the SAME number on every guest,
     * because it is ours to choose (Linux has no such call). */
    { 0xD054, ABI_UI_BUILD },
    /* §M59 — pipe was missing on i386 entirely: musl's pipe() returned
     * ENOSYS and every program that pipes (a Wayland paste, a shell
     * pipeline) failed on the one arch that had never been asked. */
    {  42, ABI_PIPE   },
    { 331, ABI_PIPE2  },
};

/* ---- Linux / amd64 -------------------------------------------------------- */
static const struct abi_nument linux_amd64_ents[] = {
    {   0, ABI_READ     },
    {   1, ABI_WRITE    },
    {   3, ABI_CLOSE    },
    {   8, ABI_SEEK     },
    {   9, ABI_MMAP     },
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
    {  22, ABI_PIPE     },
    { 293, ABI_PIPE2    },
    /* §M53 stage 3 — timing. */
    { 283, ABI_TIMERFD_CREATE  },
    {  95, ABI_UMASK           },
    /* §M90 — setpgid 109, getpgrp 111, setsid 112, getpgid 121, getsid 124. */
    { 109, ABI_SETPGID }, { 111, ABI_GETPGRP }, { 112, ABI_SETSID },
    { 121, ABI_GETPGID }, { 124, ABI_GETSID  },
    { 290, ABI_EVENTFD         },
    { 284, ABI_EVENTFD_OLD     },
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
    /* §M24 — sockets. */
    {  41, ABI_SOCKET      },
    {  42, ABI_CONNECT     },
    {  43, ABI_ACCEPT      },
    {  44, ABI_SENDTO      },
    {  45, ABI_RECVFROM    },
    {  48, ABI_SHUTDOWN    },
    {  49, ABI_BIND        },
    {  50, ABI_LISTEN      },
    {  51, ABI_GETSOCKNAME },
    {  52, ABI_GETPEERNAME },
    {  54, ABI_SETSOCKOPT  },
    {  55, ABI_GETSOCKOPT  },
    /* §M73 — x86_64: mkdir 83, link 86, chmod 90, unlink 87, mkdirat 258,
     * linkat 265, fchmodat 268, unlinkat 263. */
    {  83, ABI_MKDIR }, { 258, ABI_MKDIRAT }, {  86, ABI_LINK }, { 265, ABI_LINKAT },
    /* §M90 — chown 92, lchown 94, fchownat 260. */
    {  92, ABI_CHOWN }, {  94, ABI_CHOWN }, { 260, ABI_FCHOWNAT },
    /* §M90 — rename 82, renameat 264, renameat2 316. */
    {  82, ABI_RENAME }, { 264, ABI_RENAMEAT }, { 316, ABI_RENAMEAT2 },
    {  73, ABI_FLOCK },
    /* §M90 — fsync 74, fdatasync 75, syncfs 306; rmdir 84. */
    {  74, ABI_FSYNC }, {  75, ABI_FSYNC }, { 306, ABI_FSYNC }, {  84, ABI_RMDIR },
    {  90, ABI_CHMOD }, { 268, ABI_FCHMODAT }, { 87, ABI_UNLINK }, { 263, ABI_UNLINKAT },
    /* §M73 — x86_64 identity: getuid 102, getgid 104, setuid 105, setgid 106,
     * geteuid 107, getegid 108. */
    { 102, ABI_GETUID }, { 104, ABI_GETGID }, { 105, ABI_SETUID }, { 106, ABI_SETGID },
    { 107, ABI_GETEUID }, { 108, ABI_GETEGID }, { 115, ABI_GETGROUPS },
    /* §M73 — files and time (x86_64 numbers). */
    {   2, ABI_OPEN }, { 257, ABI_OPENAT },
    {   4, ABI_STAT }, {   6, ABI_LSTAT }, {   5, ABI_FSTAT }, { 262, ABI_FSTATAT },
    { 217, ABI_GETDENTS64 }, {  72, ABI_FCNTL },
    {  21, ABI_ACCESS }, { 269, ABI_FACCESSAT }, { 439, ABI_FACCESSAT } /* §M90 faccessat2 */, {  89, ABI_READLINK }, { 267, ABI_READLINKAT },
    {  40, ABI_SENDFILE }, {  63, ABI_UNAME },
    {  32, ABI_DUP }, {  33, ABI_DUP2 }, { 292, ABI_DUP3 },
    { 228, ABI_CLOCK_GETTIME }, {  96, ABI_GETTIMEOFDAY },
    {  35, ABI_NANOSLEEP }, { 230, ABI_CLOCK_NANOSLEEP },
    {  79, ABI_GETCWD }, {  80, ABI_CHDIR }, {  13, ABI_LNX_SIGACTION },
    {  62, ABI_KILL }, { 200, ABI_TKILL }, { 234, ABI_TGKILL }, { 131, ABI_SIGALTSTACK },
    { 202, ABI_FUTEX }, { 309, ABI_GETCPU }, {  99, ABI_SYSINFO }, {  97, ABI_GETRLIMIT },
    { 160, ABI_SETRLIMIT }, { 302, ABI_PRLIMIT64 }, { 229, ABI_CLOCK_GETRES },
    { 137, ABI_STATFS }, { 138, ABI_FSTATFS }, { 157, ABI_PRCTL }, {  98, ABI_GETRUSAGE },
    {  24, ABI_SCHED_YIELD }, { 318, ABI_GETRANDOM }, {  28, ABI_MADVISE }, { 324, ABI_MEMBARRIER },
    { 319, ABI_MEMFD_CREATE }, {  77, ABI_FTRUNCATE }, {  27, ABI_MINCORE }, {  17, ABI_PREAD64 },
    {  18, ABI_PWRITE64 }, { 204, ABI_SCHED_GETAFFINITY }, { 203, ABI_SCHED_SETAFFINITY },
    { 273, ABI_SET_ROBUST_LIST }, { 271, ABI_PPOLL }, {  88, ABI_SYMLINK }, { 266, ABI_SYMLINKAT },
    { 288, ABI_ACCEPT4     },
    /* §M65 — d-os display-bridge op; the SAME number on every guest,
     * because it is ours to choose (Linux has no such call). */
    { 0xD054, ABI_UI_BUILD },
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
    {  59, ABI_PIPE2    },          /* arm64 has only pipe2 */
    /* §M53 stage 3 — timing. */
    {  85, ABI_TIMERFD_CREATE  },
    { 166, ABI_UMASK           },
    /* §M90 — setpgid 154, getpgid 155, getsid 156, setsid 157 (no getpgrp). */
    { 154, ABI_SETPGID }, { 155, ABI_GETPGID }, { 156, ABI_GETSID }, { 157, ABI_SETSID },
    {  19, ABI_EVENTFD         },
    {  86, ABI_TIMERFD_SETTIME },
    {  20, ABI_EPOLL_CREATE },      /* epoll_create1 — arm64 has no plain
                                     * epoll_create, and no plain epoll_wait
                                     * either: glibc/musl call epoll_pwait. */
    {  21, ABI_EPOLL_CTL },
    {  22, ABI_EPOLL_WAIT },        /* epoll_pwait */
    { 136, ABI_SIGPENDING },        /* rt_sigpending */
    {  87, ABI_TIMERFD_GETTIME },
    { 103, ABI_SETITIMER       },
    /* §M24 — sockets.  arm64 gets the whole server surface in the same change
     * as the two x86 arches, which is the entire claim §M50 made: a new
     * operation is one handler and one row per guest, not one implementation
     * per architecture. */
    { 198, ABI_SOCKET      },
    { 200, ABI_BIND        },
    { 201, ABI_LISTEN      },
    { 202, ABI_ACCEPT      },
    { 203, ABI_CONNECT     },
    { 204, ABI_GETSOCKNAME },
    { 205, ABI_GETPEERNAME },
    { 206, ABI_SENDTO      },
    { 207, ABI_RECVFROM    },
    { 208, ABI_SETSOCKOPT  },
    { 209, ABI_GETSOCKOPT  },
    /* §M73 — arm64 has only the *at forms: mkdirat 34, linkat 37,
     * fchmodat 53, unlinkat 35. */
    /* §M90 — fchownat 54 (arm64 has no plain chown/lchown). */
    {  54, ABI_FCHOWNAT },
    /* §M90 — renameat 38, renameat2 276 (no plain rename on arm64). */
    {  38, ABI_RENAMEAT }, { 276, ABI_RENAMEAT2 },
    {  32, ABI_FLOCK },
    /* §M90 — fsync 82, fdatasync 83, syncfs 267 (no plain rmdir on arm64). */
    {  82, ABI_FSYNC }, {  83, ABI_FSYNC }, { 267, ABI_FSYNC },
    {  34, ABI_MKDIRAT }, {  37, ABI_LINKAT }, {  53, ABI_FCHMODAT }, {  35, ABI_UNLINKAT },
    /* §M73 — arm64 identity: setgid 144, setuid 146, getuid 174, geteuid 175,
     * getgid 176, getegid 177. */
    { 144, ABI_SETGID }, { 146, ABI_SETUID }, { 174, ABI_GETUID }, { 175, ABI_GETEUID },
    { 176, ABI_GETGID }, { 177, ABI_GETEGID }, { 158, ABI_GETGROUPS },
    /* §M73 — files and time (arm64 numbers: only the *at forms exist). */
    {  56, ABI_OPENAT }, {  79, ABI_FSTATAT }, {  80, ABI_FSTAT },
    {  61, ABI_GETDENTS64 }, {  25, ABI_FCNTL },
    {  48, ABI_FACCESSAT }, { 439, ABI_FACCESSAT } /* §M90 faccessat2 */, {  78, ABI_READLINKAT },
    {  71, ABI_SENDFILE }, { 160, ABI_UNAME },
    {  23, ABI_DUP }, {  24, ABI_DUP3 },
    { 113, ABI_CLOCK_GETTIME }, { 169, ABI_GETTIMEOFDAY },
    { 101, ABI_NANOSLEEP }, { 115, ABI_CLOCK_NANOSLEEP },
    {  17, ABI_GETCWD }, {  49, ABI_CHDIR }, { 134, ABI_LNX_SIGACTION },
    { 129, ABI_KILL }, { 130, ABI_TKILL }, { 131, ABI_TGKILL }, { 132, ABI_SIGALTSTACK },
    {  98, ABI_FUTEX }, { 168, ABI_GETCPU }, { 179, ABI_SYSINFO }, { 163, ABI_GETRLIMIT },
    { 164, ABI_SETRLIMIT }, { 261, ABI_PRLIMIT64 }, { 114, ABI_CLOCK_GETRES },
    {  43, ABI_STATFS }, {  44, ABI_FSTATFS }, { 167, ABI_PRCTL }, { 165, ABI_GETRUSAGE },
    { 124, ABI_SCHED_YIELD }, { 278, ABI_GETRANDOM }, { 233, ABI_MADVISE }, { 283, ABI_MEMBARRIER },
    { 279, ABI_MEMFD_CREATE }, {  46, ABI_FTRUNCATE }, { 232, ABI_MINCORE }, {  67, ABI_PREAD64 },
    {  68, ABI_PWRITE64 }, { 123, ABI_SCHED_GETAFFINITY }, { 122, ABI_SCHED_SETAFFINITY },
    {  99, ABI_SET_ROBUST_LIST }, {  73, ABI_PPOLL }, {  36, ABI_SYMLINKAT },
    { 210, ABI_SHUTDOWN    },
    { 242, ABI_ACCEPT4     },
    /* §M65 — d-os display-bridge op; the SAME number on every guest,
     * because it is ours to choose (Linux has no such call). */
    { 0xD054, ABI_UI_BUILD },
};

#define ARRAY_N(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* The trailing two numbers are word_bytes and epoll_event_bytes.  Note that
 * they do NOT track each other: `struct epoll_event` is 12 bytes on i386 and
 * ALSO 12 on amd64 (Linux packs it there precisely so the layouts agree), but
 * 16 on arm64, where it is unpacked and the u64 aligns to 8.  Deriving the
 * struct size from the word size would therefore be correct on exactly one of
 * these three. */
/* §M73 — the three `struct stat` layouts, as data (see abi_stat_layout).
 *               bytes dev ino w  ino32 mode nlink w  uid gid size blksz w blocks atime mtime ctime w */
static const struct abi_stat_layout stat_i386_stat64 =   /* packed, asm/stat.h */
               {  96,   0, 88, 8, 12,  16,  20,  4,  24, 28,  44,  52,  4,  56,   64,   72,   80, 4 };
static const struct abi_stat_layout stat_amd64 =
               { 144,   0,  8, 8, 0xFF, 24, 16,  8,  28, 32,  48,  56,  8,  64,   72,   88,  104, 8 };
static const struct abi_stat_layout stat_generic64 =     /* asm-generic: arm64 */
               { 128,   0,  8, 8, 0xFF, 16, 20,  4,  24, 28,  48,  56,  4,  64,   72,   88,  104, 8 };

const struct abi_map abi_map_linux_i386 = {
    "linux/i386",  linux_i386_ents,  ARRAY_N(linux_i386_ents),  4, 12, &stat_i386_stat64, 0200000
};
const struct abi_map abi_map_linux_amd64 = {
    "linux/amd64", linux_amd64_ents, ARRAY_N(linux_amd64_ents), 8, 12, &stat_amd64, 0200000
};
const struct abi_map abi_map_linux_arm64 = {
    "linux/arm64", linux_arm64_ents, ARRAY_N(linux_arm64_ents), 8, 16, &stat_generic64, 040000
};
