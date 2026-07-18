/* =============================================================================
 * muslhellodyn.c — a DYNAMICALLY-linked musl program (§M37).
 *
 * Same source shape as user/muslhello.c, but the Makefile links it as a PIE
 * (-pie) against the SHARED libc.so with the musl dynamic linker as its
 * PT_INTERP (/lib/ld-musl-i386.so.1).  When d-os execs it (under the Linux
 * personality) the kernel maps the PIE main object + the interpreter, hands
 * control to ld.so with a full auxv (AT_PHDR/AT_PHNUM/AT_BASE/AT_ENTRY/…), and
 * ld.so — running in ring 3 — self-relocates, relocates + resolves symbols in
 * the main object, runs the init array, then jumps to main.  If this prints,
 * dynamic linking works end to end.
 * ============================================================================= */
#include <stdio.h>

int main(void) {
    printf("hello from DYNAMICALLY-LINKED musl on d-os (ld.so resolved this)\n");
    printf("  dynamic printf: 6 * 7 = %d, string = %s\n", 6 * 7, "ok");
    return 0;
}
