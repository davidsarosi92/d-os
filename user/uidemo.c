/* =============================================================================
 * uidemo.c — the widget toolkit, driven from RING 3 (§M65 stage 4).
 *
 * This is the proof that the toolkit's API shape was worth it.  Everything the
 * kernel-side Control Panel does — a label, a checkbox, a radio group, a
 * button, laid out by the same engine — is done here by a user program that
 * shares no pointers with the kernel at all:
 *
 *   - the WIDGETS are described as DATA (a header, fixed-size records, a
 *     string pool addressed by OFFSET) and sent through the display bridge;
 *   - the EVENTS come back on the queue this program already polls for keys
 *     and clicks, as (widget id, event type, value).
 *
 * If either half needed a function pointer, none of this would be possible —
 * which is why `struct ui_spec` is ints and strings and why events go to ONE
 * sink per window instead of a callback per widget.
 *
 * It prints what it receives, so the check is a grep on the serial log rather
 * than somebody watching a screen.
 * ============================================================================= */

#include "libc.h"

/* ---- the display bridge (d-os operations, same numbers on every arch) ---- */
#define SYS_DOSGUI_CREATE   0xD050
#define SYS_DOSGUI_PRESENT  0xD051
#define SYS_DOSGUI_POLL     0xD052
#define SYS_DOSGUI_DESTROY  0xD053
#define SYS_DOSGUI_UI_BUILD 0xD054

struct dosgui_event {
    int type;       /* 0 key, 1 motion, 2 button, 3 close, 4 resize, 5 UI */
    int keycode;    /* UI: the widget id                                  */
    int pressed;    /* UI: UI_EV_* — 1 click, 2 toggle, 3 change          */
    int x, y;       /* UI: x = the value                                  */
    int ch;
};

/* ---- the wire format (mirrors struct dg_ui_rec in kernel/gui/dosgui.c) --- */
#define UI_MAGIC   0x49554F44u          /* "DOUI" */
#define UI_VERSION 1

#define UI_ROW          0x0001
#define UI_FILL_W       0x0004
#define UI_GRID         0x0040

struct ui_rec {
    int id, parent;
    int cls_off, text_off;
    int value, min, max, weight, flags;
};

struct ui_hdr {
    unsigned magic, version;
    int count;
    int str_off, str_len;
};

/* Widget ids we will recognise when the events come back. */
#define ID_GRID   1
#define ID_CHECK  10
#define ID_RADIO  11
#define ID_SLIDER 12

static char blob[2048];
static int  str_used;

/* Add a string to the pool, returning its OFFSET.  Offsets, not pointers: an
 * address means nothing on the other side of an address space. */
static int pool_add(char* pool, const char* s) {
    int off = str_used;
    int i = 0;
    for (; s[i]; i++) pool[str_used++] = s[i];
    pool[str_used++] = 0;
    return off;
}

int main(void) {
    int h = (int)dos_syscall3(SYS_DOSGUI_CREATE, 420, 260, (long)"Ring-3 toolkit");
    if (h < 0) { puts("uidemo: no window (is the GUI running?)"); return 1; }

    /* ---- build the blob ------------------------------------------------- */
    struct ui_hdr* hdr = (struct ui_hdr*)blob;
    struct ui_rec* rec = (struct ui_rec*)(blob + sizeof *hdr);
    int n = 0;
    /* The pool sits after the records; its size is fixed here because the
     * program knows its own strings. */
    int str_base = (int)sizeof *hdr + 8 * (int)sizeof *rec;
    char* pool = blob + str_base;
    str_used = 0;

    int off_label  = pool_add(pool, "label");
    int off_check  = pool_add(pool, "checkbox");
    int off_radio  = pool_add(pool, "radio");
    int off_slider = pool_add(pool, "slider");
    int off_box    = pool_add(pool, "box");

    int t_title = pool_add(pool, "built by a ring-3 program");
    int t_check = pool_add(pool, "a checkbox it does not own");
    int t_radio = pool_add(pool, "one two three");
    int t_k1    = pool_add(pool, "checkbox");
    int t_k2    = pool_add(pool, "radio");
    int t_k3    = pool_add(pool, "slider");

    rec[n++] = (struct ui_rec){ .id = 2, .cls_off = off_label,
                                .text_off = t_title, .flags = UI_FILL_W };
    rec[n++] = (struct ui_rec){ .id = ID_GRID, .cls_off = off_box,
                                .flags = UI_GRID | UI_FILL_W };
    rec[n++] = (struct ui_rec){ .parent = ID_GRID, .cls_off = off_label,
                                .text_off = t_k1 };
    rec[n++] = (struct ui_rec){ .id = ID_CHECK, .parent = ID_GRID,
                                .cls_off = off_check, .text_off = t_check,
                                .flags = UI_FILL_W };
    rec[n++] = (struct ui_rec){ .parent = ID_GRID, .cls_off = off_label,
                                .text_off = t_k2 };
    rec[n++] = (struct ui_rec){ .id = ID_RADIO, .parent = ID_GRID,
                                .cls_off = off_radio, .text_off = t_radio,
                                .flags = UI_FILL_W };
    rec[n++] = (struct ui_rec){ .parent = ID_GRID, .cls_off = off_label,
                                .text_off = t_k3 };
    rec[n++] = (struct ui_rec){ .id = ID_SLIDER, .parent = ID_GRID,
                                .cls_off = off_slider, .min = 0, .max = 100,
                                .value = 40, .flags = UI_FILL_W };

    hdr->magic   = UI_MAGIC;
    hdr->version = UI_VERSION;
    hdr->count   = n;
    hdr->str_off = str_base;
    hdr->str_len = str_used;

    int built = (int)dos_syscall3(SYS_DOSGUI_UI_BUILD, h, (long)blob,
                              str_base + str_used);
    if (built <= 0) {
        puts("uidemo: FAIL — the kernel refused the widget blob");
        dos_syscall3(SYS_DOSGUI_DESTROY, h, 0, 0);
        return 1;
    }
    puts("uidemo: widgets built from ring 3 — click them; the events print here");

    /* ---- the event loop -------------------------------------------------- */
    struct dosgui_event e;
    int seen = 0;
    for (;;) {
        while ((int)dos_syscall3(SYS_DOSGUI_POLL, h, (long)&e, 0) == 1) {
            if (e.type == 3) goto done;                 /* window closed */
            if (e.type != 5) continue;                  /* not a widget event */
            seen++;
            puts("uidemo: UI event");
            printf("  widget id %d, type %d, value %d\n", e.keycode, e.pressed, e.x);
        }
        /* Nothing to do but wait; the kernel's toolkit draws the window. */
        nanosleep_ms(50);
        if (seen >= 64) break;                          /* bounded demo */
    }
done:
    dos_syscall3(SYS_DOSGUI_DESTROY, h, 0, 0);
    puts("uidemo: done");
    return 0;
}
