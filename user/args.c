/* =============================================================================
 * args.c — a user program that prints its argv (M34 slice A test).
 *
 * Proves the System V initial stack the kernel builds (proc.c
 * build_initial_stack) reaches the program through crt0: main() receives
 * argc/argv and walks them over the in-tree libc.
 * ============================================================================= */

#include "libc.h"

int main(int argc, char** argv) {
    printf("args: argc=%d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("args: argv[%d]=%s\n", i, argv[i]);
    return argc;                       /* exit code = argc, so we can check it */
}
