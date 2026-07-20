/* =============================================================================
 * wctest.c — dyn musl program exercising libwapcaplet (§M42 NetSurf libs).
 * libwapcaplet interns strings: two interns of the same text return the SAME
 * handle.  Proves the ported libwapcaplet.so.0 (a store package) loads + runs.
 * ============================================================================= */
#include <stdio.h>
#include <libwapcaplet/libwapcaplet.h>

int main(void) {
    lwc_string *a = NULL, *b = NULL, *c = NULL;
    lwc_intern_string("netsurf", 7, &a);
    lwc_intern_string("netsurf", 7, &b);
    lwc_intern_string("d-os",    4, &c);

    bool eq_ab = false, eq_ac = false;
    lwc_string_isequal(a, b, &eq_ab);
    lwc_string_isequal(a, c, &eq_ac);

    printf("wctest: libwapcaplet interned; \"netsurf\"==\"netsurf\": %s (same handle: %s), "
           "\"netsurf\"==\"d-os\": %s\n",
           eq_ab ? "yes" : "no", a == b ? "yes" : "no", eq_ac ? "yes" : "no");

    lwc_string_unref(a); lwc_string_unref(b); lwc_string_unref(c);
    return (eq_ab && a == b && !eq_ac) ? 0 : 1;
}
