/* =============================================================================
 * hello.c — a real compiled-C user program linked against the d-os in-tree
 * libc (M25 stage 7).  Proves the whole userland substrate end-to-end: the
 * kernel loads this as a static ELF, drops to ring 3, and the program runs
 * standard C — printf, malloc, string ops — entirely over the syscall ABI.
 * ============================================================================= */

#include "libc.h"

int main(void) {
    puts("hello from libc (compiled C, ring 3)");
    printf("libc printf: answer=%d hex=0x%x char=%c str=%s\n", 42, 0xBEEF, 'X', "ok");

    char* p = (char*)malloc(64);
    if (p) {
        memcpy(p, "malloc+memcpy work", 19);
        printf("heap: %s\n", p);
    }
    return 0;
}
