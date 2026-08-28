/* =============================================================================
 * drvtest.c — §M33 Tier 1: does a ring-3 driver really get exactly its ports?
 *
 * Three questions, and the third is the one that matters:
 *
 *   1. Can a granted port be read FROM RING 3?  (the grant works)
 *   2. Is a port outside the MANIFEST refused?   (the bound is real)
 *   3. Is a raw `in` on an ungranted port a FAULT?  (the CPU enforces it, not
 *      a check somebody could forget)
 *
 * The third is why this program exists.  A grant that is merely recorded is a
 * comment; the test is that the hardware says no — and this project has learnt
 * twice that a deliberate fault which quietly succeeds makes a test whose pass
 * and fail look identical (§M62's `*(int*)0x4`).  So the ungranted read is
 * attempted LAST: if the bitmap works, this program dies there and the kernel
 * reports the #GP, which IS the pass.
 * ============================================================================= */

#include "libc.h"

/* No printf in this libc, and the values matter — so a two-digit hex by hand.
 * Small enough that pulling in formatting for it would be the larger cost. */
static int  strlen_(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void put(const char* s) { write(1, s, (size_t)strlen_(s)); }
static void puthex(unsigned v) {
    static const char* d = "0123456789abcdef";
    char b[3] = { d[(v >> 4) & 0xF], d[v & 0xF], 0 };
    put(b);
}

#define SYS_DRV_PORTS_LOCK   0xD065
#define SYS_DRV_PORTS_UNLOCK 0xD066
#define SYS_DRV_PORTS    0xD060
#define SYS_DRV_IRQ      0xD061

static inline unsigned char raw_inb(unsigned short port) {
    unsigned char v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

int main(void) {
    put("drvtest: ring-3 driver resource test\n");

    /* 1. Ask for the window the manifest allows. */
    long h = dos_syscall3(SYS_DRV_PORTS, 0x60, 5, 0);
    if (h < 0) { put("drvtest: FAIL — granted request refused\n"); return 1; }
    put("drvtest: ports 0x60..0x64 granted\n");

    /* The 8042 status register, read from RING 3.  Any value will do — what is
     * being tested is that the instruction EXECUTES rather than faults. */
    unsigned char st = raw_inb(0x64);
    put("drvtest: read 0x64 from ring 3 = ");
    puthex(st);
    put("\n");

    /* 2. A port outside the manifest must be refused by the kernel. */
    long bad = dos_syscall3(SYS_DRV_PORTS, 0x3F8, 8, 0);
    if (bad >= 0) { put("drvtest: FAIL — serial ports granted, manifest ignored\n"); return 1; }
    put("drvtest: 0x3F8 refused by the manifest — good\n");

    /* 3. THE EXCLUSIVE CLAIM IS BOUNDED, and that is what is falsified here.
     *
     * A claim masks the KEYBOARD's interrupt line, so a driver that takes one
     * and dies — or is simply wrong — would leave the machine with no keyboard.
     * The kernel therefore takes the claim back on a deadline.  Proving it
     * needs the reclaim to be OBSERVED rather than assumed: take a claim with a
     * short deadline, never release it, sleep past it, and ask again.  A second
     * claim can only succeed if the first one is gone. */
    long lk = dos_syscall3(SYS_DRV_PORTS_LOCK, h, 40, 0);
    if (lk != 0) { put("drvtest: FAIL — exclusive claim refused\n"); return 1; }
    put("drvtest: exclusive claim taken (40 ms) and deliberately NOT released\n");

    /* A second claim while the first stands must be refused — otherwise the
     * success after the sleep would prove nothing about the deadline, only
     * that claims are always granted. */
    if (dos_syscall3(SYS_DRV_PORTS_LOCK, h, 40, 0) == 0) {
        put("drvtest: FAIL — a second claim was granted while the first stood\n");
        return 1;
    }
    put("drvtest: a second claim while the first stands is refused — good\n");

    nanosleep_ms(250);
    if (dos_syscall3(SYS_DRV_PORTS_LOCK, h, 40, 0) != 0) {
        put("drvtest: FAIL — claim never reclaimed, the keyboard would be dead\n");
        return 1;
    }
    dos_syscall3(SYS_DRV_PORTS_UNLOCK, h, 0, 0);
    put("drvtest: the abandoned claim was reclaimed on its deadline — good\n");

    /* 4. And the CPU must refuse the one we were never given.  If the bitmap
     * works this line does not return; the kernel prints the fault and kills
     * the process, and THAT is the pass. */
    put("drvtest: now reading ungranted 0x3F8 directly — expect a fault\n");
    unsigned char bogus = raw_inb(0x3F8);
    put("drvtest: FAIL — ungranted read SUCCEEDED, value ");
    puthex(bogus);
    put("\n");
    return 1;
}
