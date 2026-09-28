/* =============================================================================
 * mmaptest.c — does the address space behave the way a JVM needs it to?
 * (§M89 rung 1: reservations, demand-zero pages, true PROT_NONE, lazy file
 * mappings, address reuse.)
 *
 * An UNMODIFIED musl binary under the Linux personality, on all three arches.
 * Each claim is built so that the pre-§M89 kernel FAILS it, which is what makes
 * a pass mean something:
 *
 *   1. RESERVE — a PROT_NONE, MAP_NORESERVE range far larger than the machine's
 *      memory (4 GiB on 64-bit, 512 MiB on 32-bit) is granted.  The old mmap
 *      capped a mapping at 256 MiB and allocated every frame up front.
 *   2. DEMAND-ZERO — a READ-WRITE anonymous mapping of the same size is granted
 *      too, three scattered pages are touched, and an untouched page reads as
 *      zero.  Eagerly this is gigabytes of zeroed frames on a 2 GiB machine.
 *   3. COMMIT — mprotect of pieces of the reservation to READ|WRITE makes them
 *      usable; contents survive a trip through PROT_NONE and back.
 *   4. PROT_NONE FAULTS — a child writes into the reservation and must DIE
 *      (the old kernel mapped a present, readable page for PROT_NONE).  Until
 *      rung 2 delivers SIGSEGV to a handler, dying is the observable outcome.
 *   5. REUSE — munmap then mmap of the same size returns the same address
 *      (the old allocator was a bump cursor that never went back).
 *   6. NOREPLACE — MAP_FIXED_NOREPLACE over a live mapping is refused EEXIST.
 *   7. LAZY FILE — a private file mapping reads the file, a write to it stays
 *      private (the file is unchanged afterwards).
 *
 * Output: one PASS/FAIL line per claim and a final verdict line the harness
 * greps for.
 * ============================================================================= */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0x4000
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PG 4096UL

static int g_fail;

static void verdict(int ok, const char* what) {
    printf("mmaptest: %s - %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

int main(void) {
    const size_t big = sizeof(void*) == 8 ? ((size_t)4 << 30) : ((size_t)512 << 20);
    const int anon = MAP_PRIVATE | MAP_ANONYMOUS;

    /* 1. reserve */
    unsigned char* r = mmap(NULL, big, PROT_NONE, anon | MAP_NORESERVE, -1, 0);
    verdict(r != MAP_FAILED, "a PROT_NONE reservation larger than RAM is granted");
    if (r == MAP_FAILED) { printf("mmaptest: errno %d\n", errno); goto out; }
    printf("mmaptest: reserved %zu MiB at %p\n", big >> 20, (void*)r);

    /* 2. demand-zero */
    {
        unsigned char* w = mmap(NULL, big, PROT_READ | PROT_WRITE, anon | MAP_NORESERVE, -1, 0);
        int ok = w != MAP_FAILED;
        if (ok) {
            size_t offs[3] = { 0, big / 2, big - PG };
            for (int i = 0; i < 3; i++) w[offs[i]] = (unsigned char)(0x40 + i);
            for (int i = 0; i < 3; i++) ok &= w[offs[i]] == (unsigned char)(0x40 + i);
            ok &= w[PG * 7 + 123] == 0;            /* untouched: zero */
            munmap(w, big);
        }
        verdict(ok, "a read-write mapping larger than RAM is granted and filled on touch");
    }

    /* 3. commit pieces of the reservation, and a round trip through PROT_NONE */
    {
        unsigned char* c = r + big / 4;
        int ok = mprotect(c, 3 * PG, PROT_READ | PROT_WRITE) == 0;
        if (ok) {
            memset(c, 0x5A, 3 * PG);
            ok &= mprotect(c, 3 * PG, PROT_NONE) == 0;
            ok &= mprotect(c, 3 * PG, PROT_READ | PROT_WRITE) == 0;
            ok &= c[0] == 0x5A && c[3 * PG - 1] == 0x5A;
        }
        verdict(ok, "a committed piece keeps its contents through PROT_NONE and back");
    }

    /* 4. PROT_NONE must fault — in a child, since it is supposed to die */
    {
        pid_t p = fork();
        if (p == 0) {
            r[PG * 2] = 1;                         /* must not return */
            printf("mmaptest: child wrote into PROT_NONE and survived\n");
            _exit(0);
        }
        int st = 0;
        waitpid(p, &st, 0);
        int died = !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        verdict(p > 0 && died, "a write into a PROT_NONE page faults");
    }

    /* 5. address reuse */
    {
        void* a = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, anon, -1, 0);
        munmap(a, 1 << 20);
        void* b = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, anon, -1, 0);
        verdict(a != MAP_FAILED && a == b, "munmap gives the address back for reuse");
        if (b != MAP_FAILED) munmap(b, 1 << 20);
    }

    /* 6. MAP_FIXED_NOREPLACE */
    {
        void* x = mmap(r, PG, PROT_READ, anon | MAP_FIXED_NOREPLACE, -1, 0);
        verdict(x == MAP_FAILED && errno == EEXIST, "MAP_FIXED_NOREPLACE over a mapping is EEXIST");
    }

    /* 7. a lazy private file mapping */
    {
        const char* path = "/tmp/mmaptest.dat";
        int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
        unsigned char buf[PG];
        int ok = fd >= 0;
        for (int i = 0; ok && i < 3; i++) {
            memset(buf, 'A' + i, sizeof buf);
            ok &= write(fd, buf, sizeof buf) == (ssize_t)sizeof buf;
        }
        unsigned char* m = ok ? mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)
                              : MAP_FAILED;
        ok &= m != MAP_FAILED;
        if (ok) {
            ok &= m[0] == 'A' && m[PG] == 'B' && m[3 * PG - 1] == 'C';
            m[PG] = 'z';                            /* private: must not reach the file */
            ok &= m[PG] == 'z';
            unsigned char c = 0;
            lseek(fd, PG, SEEK_SET);
            ok &= read(fd, &c, 1) == 1 && c == 'B';
            munmap(m, 3 * PG);
        }
        if (fd >= 0) { close(fd); unlink(path); }
        verdict(ok, "a private file mapping reads the file and keeps its writes private");
    }

    munmap(r, big);
out:
    printf("mmaptest: %s (%d failed)\n", g_fail ? "FAIL" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
