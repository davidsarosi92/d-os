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
};

#define ARRAY_N(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

const struct abi_map abi_map_linux_i386 = {
    "linux/i386",  linux_i386_ents,  ARRAY_N(linux_i386_ents)
};
const struct abi_map abi_map_linux_amd64 = {
    "linux/amd64", linux_amd64_ents, ARRAY_N(linux_amd64_ents)
};
const struct abi_map abi_map_linux_arm64 = {
    "linux/arm64", linux_arm64_ents, ARRAY_N(linux_arm64_ents)
};
