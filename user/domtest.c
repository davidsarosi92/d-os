/* =============================================================================
 * domtest.c — dyn musl program exercising libdom (§M42 NetSurf libs).
 * Uses the core DOM string type (interned via libwapcaplet under the hood).
 * Proves the ported libdom.so.0 (a store package: libdom -> libwapcaplet +
 * libhubbub + libparserutils) loads, relocates, and runs.
 *
 * (The full HTML->DOM path via the libdom hubbub binding needs the binding
 * headers staged under a non-colliding prefix — dom-hubbub/ — to avoid clashing
 * with libhubbub's own hubbub/*.h; a follow-up.  The core DOM API is enough to
 * prove the library here.)
 * ============================================================================= */
#include <stdio.h>
#include <dom/dom.h>

int main(void) {
    dom_string *s = NULL;
    if (dom_string_create((const uint8_t *)"NetSurf-on-d-os", 15, &s) != DOM_NO_ERR || !s) {
        printf("domtest: dom_string_create failed\n");
        return 1;
    }
    size_t len = dom_string_length(s);
    printf("domtest: libdom dom_string len=%lu data=\"%.*s\"\n",
           (unsigned long)len, (int)len, dom_string_data(s));

    dom_string *t = NULL;
    dom_string_create((const uint8_t *)"NetSurf-on-d-os", 15, &t);
    bool eq = dom_string_isequal(s, t);

    dom_string_unref(s);
    dom_string_unref(t);
    printf("domtest: libdom dom_string equality: %s\n", eq ? "yes" : "no");
    return (len == 15 && eq) ? 0 : 1;
}
