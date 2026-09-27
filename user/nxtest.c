/* =============================================================================
 * nxtest.c — is a DATA page really not code?  (§M86, no-execute / W^X)
 *
 * Runs as an UNMODIFIED musl binary under the Linux personality, on all three
 * architectures, and makes THREE claims, each checked, in an order chosen so a
 * failure cannot be mistaken for the thing it tests:
 *
 *   1. CONTROL — a page asked for with PROT_EXEC (written, then mprotect'ed
 *      to READ|EXEC) runs.  Without this, "the data page faulted" could mean
 *      "nothing at all can be executed from mmap", which is a broken loader
 *      and not a working NX.
 *   2. a data page (a static array: .data/.bss, mapped RW) must NOT run;
 *   3. a buffer on the STACK must NOT run — the classic overflow target.
 *
 * 2 and 3 run in a CHILD (fork): the correct outcome is that the child is
 * killed by the fault, so the parent reads the answer from the wait status.
 * A child that comes back and prints is the failure, and says so in words
 * that cannot be confused with success.
 *
 * The one instruction written is `ret` — 0xC3 on x86, 0xd65f03c0 on arm64 —
 * so a page that DOES run returns straight to the caller.  On arm64 the
 * instruction cache must be told about code written through the data side
 * (__builtin___clear_cache); x86 keeps them coherent itself.
 * ============================================================================= */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdlib.h>

static unsigned char data_page[8192] __attribute__((aligned(4096)));

static void put_ret(unsigned char* p) {
#if defined(__aarch64__)
    uint32_t insn = 0xd65f03c0u;              /* ret */
    memcpy(p, &insn, 4);
    __builtin___clear_cache((char*)p, (char*)p + 4);
#else
    p[0] = 0xC3;                              /* ret */
#endif
    /* A COMPILER BARRIER, and it is load-bearing: executing a buffer is not a
     * READ as far as the compiler knows, so without this GCC dropped the store
     * into the stack buffer as dead — and the child took "Invalid Opcode" on
     * whatever garbage was there, which looked like a pass for the wrong
     * reason (found on the first run). */
    __asm__ volatile ("" ::: "memory");
}

typedef void (*fn_t)(void);

/* The kernel says whether it enforces no-execute (DOS_NX=1/0 in the
 * environment, set by the `nxtest` command).  On a machine without it a data
 * page RUNNING is the correct outcome, and reporting that as a failure would
 * make the test red on hardware that is behaving exactly as documented. */
static int g_expect_nx = 1;

/* Run `what` in a child; report whether the child survived. */
static int must_not_run(const char* what, int stack) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (stack) {
            unsigned char buf[64];
            put_ret(buf);
            ((fn_t)(void*)buf)();
        } else {
            put_ret(data_page);
            ((fn_t)(void*)data_page)();
        }
        if (!g_expect_nx)
            printf("nxtest: skip - %s executed, as it must on a machine "
                   "without no-execute\n", what);
        else
            printf("nxtest: FAIL - %s EXECUTED (no-execute is not enforced)\n", what);
        fflush(stdout);
        _exit(0);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
        printf("nxtest: FAIL - waitpid on the %s child\n", what);
        return 1;
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return g_expect_nx ? 1 : 0;
    printf("nxtest: ok - executing %s was REFUSED (child status 0x%x)\n", what, st);
    return 0;
}

int main(void) {
    int fails = 0;
    const char* e = getenv("DOS_NX");
    if (e && e[0] == '0') g_expect_nx = 0;

    /* `nxtest excursion`: fault in THIS process, not a child.  The command
     * runs us as a synchronous excursion on the shell's own task, so this is
     * the falsifier for "a faulting excursion returns to its caller instead of
     * taking the shell with it" — a write near address 0, which faults on
     * every architecture whether or not no-execute exists. */
    const char* m = getenv("DOS_NXTEST");
    if (m && m[0] == 'e') {
        printf("nxtest: faulting on purpose inside the excursion...\n");
        fflush(stdout);
        *(volatile int*)(uintptr_t)0x10 = 1;
        printf("nxtest: FAIL - the write to 0x10 did not fault\n");
        return 1;
    }

    unsigned char* x = mmap(0, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (x == MAP_FAILED) { printf("nxtest: FAIL - mmap\n"); return 1; }
    put_ret(x);
    if (mprotect(x, 4096, PROT_READ | PROT_EXEC) != 0) {
        printf("nxtest: FAIL - mprotect PROT_EXEC\n");
        return 1;
    }
    ((fn_t)(void*)x)();
    printf("nxtest: ok - code in a PROT_EXEC page ran (control)\n");

    fails += must_not_run("a data page", 0);
    fails += must_not_run("a stack buffer", 1);

    printf(fails ? "nxtest: FAIL (%d)\n" : "nxtest: PASS\n", fails);
    return fails ? 1 : 0;
}
