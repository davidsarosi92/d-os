/* =============================================================================
 * csstest.c — dyn musl program exercising libcss (§M42 NetSurf libs).
 * Creates a stylesheet and parses real CSS, driving libcss's parser + the
 * generated property parsers.  Proves the ported libcss.so.0 (a store package,
 * DT_NEEDED libcss.so.0 -> libwapcaplet.so.0 + libparserutils.so.0) loads +
 * parses.
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <libcss/libcss.h>

static css_error resolve_url(void *pw, const char *base,
                             lwc_string *rel, lwc_string **abs) {
    (void)pw; (void)base;
    *abs = lwc_string_ref(rel);
    return CSS_OK;
}

int main(void) {
    css_stylesheet_params p;
    memset(&p, 0, sizeof p);
    p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    p.level   = CSS_LEVEL_DEFAULT;
    p.charset = "UTF-8";
    p.url     = "dos:test.css";
    p.resolve = resolve_url;

    css_stylesheet *sheet = NULL;
    if (css_stylesheet_create(&p, &sheet) != CSS_OK || !sheet) {
        printf("csstest: css_stylesheet_create failed\n");
        return 1;
    }
    const char *css = "body { color: #ff0000; margin: 8px; } p { display: block; }";
    css_stylesheet_append_data(sheet, (const uint8_t *)css, strlen(css));
    css_error done = css_stylesheet_data_done(sheet);

    size_t sz = 0;
    css_stylesheet_size(sheet, &sz);
    printf("csstest: libcss parsed a stylesheet (data_done=%d), %lu bytes in-memory\n",
           (int)done, (unsigned long)sz);

    css_stylesheet_destroy(sheet);
    return (done == CSS_OK) ? 0 : 1;
}
