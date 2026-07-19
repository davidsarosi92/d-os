/* A sample C source provisioned at /hello.c on d-os, for the on-device
 * compiler (§M43): `tcc /hello.c -o /hello` compiles + links it against musl
 * ON d-os, then it runs.  The editor "Compile & Run" does the same with the
 * buffer.  Uses stdio to prove the full libc path (headers + crt + libc). */
#include <stdio.h>

int main(void) {
    printf("hello, compiled on d-os by tcc!\n");
    for (int i = 1; i <= 3; i++)
        printf("  count %d\n", i);
    return 0;
}
