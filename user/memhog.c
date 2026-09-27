/* =============================================================================
 * memhog.c — the falsifier for §M75's memory column.
 *
 * §M71 rule 1: a check nobody has seen fail is a check nobody has tested.  The
 * memory column has a weaker version of the same problem — on a bare boot every
 * task is a kernel thread, so the whole column reads 0, which is CORRECT and
 * proves nothing about the page-table walk behind it.
 *
 * A program that merely holds "some" memory would not fix that either: an
 * absolute figure has an unknown constant in it (the ELF image, the stack, the
 * libc's first heap chunk), so "3412 KB" is a number nobody can check.
 *
 * **SO THIS GROWS IN KNOWN STEPS AND THE TEST READS THE DIFFERENCE.**  One
 * mebibyte every two seconds, announced on stdout as it happens.  Two `ps`
 * runs N seconds apart must show this pid's MEMKB rise by exactly N/2 MiB —
 * the unknown constant cancels, and a walker that reported a plausible-looking
 * wrong number is caught by arithmetic instead of by opinion.
 *
 * EVERY PAGE IS TOUCHED, and that is not a formality: `mmap` reserves address
 * space, and a resident-page count is supposed to report what is actually
 * BACKED.  Writing one byte per 4 KiB is what makes the frame real.  (This
 * kernel maps eagerly today, so the touch is currently redundant — which is
 * exactly why it is here: when §M74's demand paging lands, this program is
 * already asking the right question, and the day the two disagree it will be
 * because something changed, not because the test was sloppy.)
 *
 * It sleeps rather than spins between steps, so it is killable with plain
 * `kill` and costs nothing while it is being measured — a hog that also burnt
 * a core would put its own noise into the CPU% column being read beside it.
 * ============================================================================= */

#include "libc.h"

#define STEP_BYTES   (1024u * 1024u)      /* 1 MiB per step   */
#define STEP_MS      2000u                /* every 2 seconds  */
#define MAX_STEPS    16u                  /* 16 MiB, then hold */

/* §M72 — `memhog fill`: grow as fast as it can until the kernel REFUSES, then
 * say at what size and hold.  The falsifier for the reserve: with the reserve
 * working, this process is refused while the shell, the compositor and `crash`
 * still get memory; without it, the machine runs dry wherever the next
 * allocation happens to be. */
static int fill_mode(void) {
    printf("memhog: pid %d - filling 4 MiB at a time until refused\n", getpid());
    unsigned held_kb = 0;
    for (;;) {
        unsigned char* p = (unsigned char*)mmap(4u * STEP_BYTES, -1);
        if (!p) break;
        for (unsigned off = 0; off < 4u * STEP_BYTES; off += 4096) p[off] = 1;
        held_kb += 4u * STEP_BYTES / 1024u;
    }
    printf("memhog: REFUSED after %u KB - holding it; the shell must still answer\n", held_kb);
    for (;;) nanosleep_ms(1000);
    return 0;
}

/* §M72 stage 3 — `memhog verify`: the falsifier for eviction.  A pattern that
 * depends on every word's ADDRESS (so a page brought back in the wrong place,
 * or a page of zeros, cannot pass) is re-checked twice a second, forever.  A
 * program paused, evicted and resumed must go on printing "ok"; one byte
 * wrong prints where.
 *
 * Before the private region, a small one is written and then FORKED over: the
 * child keeps reading it, so in the parent those pages stay copy-on-write —
 * shared with another process — and eviction must leave them alone (the
 * report counts them as kept).  The child checks its copy too, so a page that
 * was evicted anyway and came back wrong would show on the OTHER side. */
#define VFY_PRIV   (8u * STEP_BYTES)
#define VFY_SHARED (64u * 1024u)
static unsigned vfy_word(const unsigned* p) { return (unsigned)(unsigned long)p ^ 0x5A17C0DEu; }
static int vfy_check(unsigned* base, unsigned bytes, const char* who, int pass) {
    for (unsigned i = 0; i < bytes / 4; i++)
        if (base[i] != vfy_word(&base[i])) {
            printf("memhog: %s CORRUPT at +%u (pass %d)\n", who, i * 4, pass);
            return -1;
        }
    return 0;
}
static int verify_mode(void) {
    unsigned* sh = (unsigned*)mmap(VFY_SHARED, -1);
    if (!sh) { printf("memhog: no memory\n"); return 1; }
    for (unsigned i = 0; i < VFY_SHARED / 4; i++) sh[i] = vfy_word(&sh[i]);
    int child = fork();
    if (child == 0) {
        for (int pass = 1;; pass++) {
            if (vfy_check(sh, VFY_SHARED, "child", pass) == 0 && pass % 10 == 0)
                printf("memhog: child pass %d ok\n", pass);
            nanosleep_ms(500);
        }
    }
    unsigned* pv = (unsigned*)mmap(VFY_PRIV, -1);
    if (!pv) { printf("memhog: no memory\n"); return 1; }
    for (unsigned i = 0; i < VFY_PRIV / 4; i++) pv[i] = vfy_word(&pv[i]);
    printf("memhog: pid %d verifying %u KB private + %u KB shared with child %d\n",
           getpid(), VFY_PRIV / 1024u, VFY_SHARED / 1024u, child);
    for (int pass = 1;; pass++) {
        int bad = vfy_check(pv, VFY_PRIV, "private", pass) | vfy_check(sh, VFY_SHARED, "shared", pass);
        if (!bad) printf("memhog: pass %d ok\n", pass);
        nanosleep_ms(500);
    }
    return 0;
}

/* §M74 rung 1 — `memhog age`: the accessed-bit sweep's falsifier.  8 MiB,
 * all written once; then the first 25 % is touched every 50 ms for ~6 s, and
 * after that the first 75 %.  The kernel's per-process split must follow. */
#define AGE_BYTES (8u * STEP_BYTES)
static int age_mode(void) {
    unsigned char* p = (unsigned char*)mmap(AGE_BYTES, -1);
    if (!p) { printf("memhog: no memory\n"); return 1; }
    for (unsigned off = 0; off < AGE_BYTES; off += 4096) p[off] = 1;
    printf("memhog: pid %d touching 25%% of %u KB, then 75%%\n", getpid(), AGE_BYTES / 1024u);
    volatile unsigned sink = 0;
    for (unsigned it = 0;; it++) {
        unsigned span = it < 120 ? AGE_BYTES / 4u : (AGE_BYTES / 4u) * 3u;
        if (it == 120) printf("memhog: now touching 75%%\n");
        for (unsigned off = 0; off < span; off += 4096) sink += p[off];   /* a READ */
        nanosleep_ms(50);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && argv[1][0] == 'f') return fill_mode();
    if (argc > 1 && argv[1][0] == 'v') return verify_mode();
    if (argc > 1 && argv[1][0] == 'a') return age_mode();
    unsigned held_kb = 0;

    printf("memhog: pid %d — 1 MiB every 2 s, up to %d MiB\n",
           getpid(), (int)MAX_STEPS);

    for (unsigned step = 0; step < MAX_STEPS; step++) {
        unsigned char* p = (unsigned char*)mmap(STEP_BYTES, -1);
        if (!p) {
            printf("memhog: mmap failed at %u KB — holding\n", held_kb);
            break;
        }
        /* One byte per page: the write is what makes the frame resident. */
        for (unsigned off = 0; off < STEP_BYTES; off += 4096) p[off] = (unsigned char)step;

        held_kb += STEP_BYTES / 1024u;
        printf("memhog: touched %u KB\n", held_kb);
        nanosleep_ms(STEP_MS);
    }

    printf("memhog: holding %u KB — `kill` me\n", held_kb);
    for (;;) nanosleep_ms(1000);
    return 0;
}
