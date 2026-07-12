/* =============================================================================
 * muslhello.c — a NORMAL C program linked against REAL, pristine musl (§M36).
 *
 * Unlike user/linuxhello.c (hand-written Linux-ABI, no libc), this is an
 * ordinary hosted program: it includes <stdio.h> and calls printf().  It is
 * compiled with musl's headers and statically linked with musl's crt1 + libc.a
 * (see the `muslhello` rule in the Makefile), producing a stock Linux i386 ELF.
 * d-os runs it under the Linux personality (task->linux_abi), so every syscall
 * musl issues at startup + runtime is serviced by kernel/hal/x86/linux_abi.c.
 * This is the end goal of §M36 stage 2: an unmodified musl binary on d-os.
 * ============================================================================= */
#include <stdio.h>

int main(void) {
    printf("hello from REAL musl on d-os (unmodified, static libc)\n");
    printf("  1 + 2 = %d, and %%s works: %s\n", 1 + 2, "yes");
    return 0;
}
