/* Throwaway hello-world for testing the on-device compiler (§M43).
 * Provisioned at /hi.c on d-os; compile + run it with:
 *     tcc /hi.c -o /hi
 *     exec /hi
 * (Or open it in the Editor and click "Run".)  Safe to delete after testing. */
#include <stdio.h>

int main(void) {
    printf("Hello, World! (compiled + run on d-os)\n");
    return 0;
}
