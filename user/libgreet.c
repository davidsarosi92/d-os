/* =============================================================================
 * libgreet.c — a tiny SHARED LIBRARY (§M37 stage 5).
 *
 * Built into user/libgreet.so and provisioned at /lib/libgreet.so.  A program
 * (user/solibtest.c) links against it by name (-lgreet), so its DT_NEEDED lists
 * "libgreet.so".  When d-os runs that program, musl's ld.so must:
 *   - find libgreet.so via the search path (/lib),
 *   - map it, apply its relocations,
 *   - resolve the program's call to greet_add() (a JMP_SLOT into this .so),
 *   - resolve THIS library's own call to snprintf() (a JMP_SLOT into libc).
 * If greet_add() returns the right value AND greet_msg() formats a string via
 * libc, cross-object symbol resolution works both directions.  greet_tag is an
 * exported global to also exercise a data (GLOB_DAT) reference.
 * ============================================================================= */
#include <stdio.h>

const char* greet_tag = "libgreet";

/* §M37 stage 6 — a thread-local in a SHARED library.  Because libgreet is a
 * .so, this uses the general-dynamic TLS model: each reference goes through
 * __tls_get_addr(module, offset), and ld.so applies DTPMOD/DTPOFF relocations
 * so the module id + offset are filled in at load time.  Exercises the dynamic
 * TLS path on top of the §M35 %gs thread pointer. */
static __thread int greet_tls = 100;

int greet_tls_bump(void) { return ++greet_tls; }

int greet_add(int a, int b) {
    return a + b;
}

int greet_msg(char* buf, int cap, int n) {
    /* Calls into libc (snprintf) — a .so → libc cross-object resolution. */
    return snprintf(buf, (size_t)cap, "%s says %d", greet_tag, n);
}
