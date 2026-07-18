/* =============================================================================
 * solibtest.c — a program that uses a SEPARATE shared library (§M37 stage 5).
 *
 * Linked against libgreet.so by name (-lgreet), so its DT_NEEDED carries
 * "libgreet.so" (in addition to "libc.so").  Running it exercises the real
 * dynamic-linking machinery beyond the libc-is-the-interpreter shortcut: ld.so
 * has to locate + map a genuinely separate .so and resolve symbols across three
 * objects (main → libgreet, libgreet → libc).
 * ============================================================================= */
#include <stdio.h>

extern const char* greet_tag;
int greet_add(int a, int b);
int greet_msg(char* buf, int cap, int n);
int greet_tls_bump(void);                       /* touches a .so __thread var    */

int main(void) {
    int sum = greet_add(40, 2);                 /* main → libgreet (JMP_SLOT)   */
    char buf[64];
    greet_msg(buf, sizeof buf, sum);            /* libgreet → libc (snprintf)   */
    printf("solibtest: greet_add(40,2)=%d, tag=%s\n", sum, greet_tag);
    printf("solibtest: greet_msg -> \"%s\"\n", buf);
    /* §M37 stage 6: three bumps of a .so thread-local → 101,102,103 proves the
     * general-dynamic TLS path (__tls_get_addr + DTPMOD/DTPOFF) resolves. */
    int a = greet_tls_bump(), b = greet_tls_bump(), c = greet_tls_bump();
    printf("solibtest: .so __thread bumps = %d,%d,%d\n", a, b, c);
    return 0;
}
