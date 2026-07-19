/* =============================================================================
 * hbtest.c — dynamically-linked musl program exercising HarfBuzz (§M38 support
 * libs).  Creates a buffer, adds UTF-8 text, and runs segment-property guessing
 * (real HarfBuzz script/direction detection), then reports the version.  Proves
 * the ported libharfbuzz.so.0 (a big C++ store package, DT_NEEDED
 * libharfbuzz.so.0 -> libstdc++.so.6) loads, relocates, and runs.  Full shaping
 * against a FreeType face awaits a font file in the VFS — a follow-up.
 * ============================================================================= */
#include <stdio.h>
#include <hb.h>

int main(void) {
    hb_buffer_t* buf = hb_buffer_create();
    if (!hb_buffer_allocation_successful(buf)) {
        printf("hbtest: hb_buffer_create failed\n");
        return 1;
    }
    hb_buffer_add_utf8(buf, "d-os harfbuzz shaping", -1, 0, -1);
    hb_buffer_guess_segment_properties(buf);

    unsigned int n = hb_buffer_get_length(buf);
    hb_direction_t dir = hb_buffer_get_direction(buf);
    hb_script_t script = hb_buffer_get_script(buf);
    char tag[5] = {0};
    hb_tag_to_string(hb_script_to_iso15924_tag(script), tag);

    printf("hbtest: harfbuzz %s, %u code points, dir=%s, script=%s\n",
           hb_version_string(), n,
           dir == HB_DIRECTION_LTR ? "LTR" : "other", tag);

    hb_buffer_destroy(buf);
    return 0;
}
