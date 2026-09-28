/* =============================================================================
 * display.c — the output registry and its arrangement (§M88).  See display.h.
 * ============================================================================= */

#include "display.h"
#include "config.h"
#include "printf.h"
#include "shellcmd.h"
#include "lock.h"
#include "settings.h"
#include <stddef.h>

/* Slot 0 is RESERVED for the primary.  Secondary drivers come up at boot,
 * long before the compositor registers the primary from fb_present — first
 * come, first served would make a secondary output 0, and "output 0 is the
 * boot display at (0,0)" is what every coordinate here rests on. */
static struct display_output* g_out[DISPLAY_MAX];
static int g_n = 1;                   /* next secondary slot; 0 is the primary's */
static spinlock_t g_lock = SPINLOCK_INIT;

int display_register(struct display_output* o) {
    if (!o) return -1;
    uint32_t fl = spin_lock_irqsave(&g_lock);
    int i = -1;
    if (o->primary) { i = 0; g_out[0] = o; }
    else if (g_n < DISPLAY_MAX) { i = g_n; g_out[g_n++] = o; }
    spin_unlock_irqrestore(&g_lock, fl);
    if (i < 0) { kprintf("display: '%s' ignored - %d outputs at most\n", o->name, DISPLAY_MAX); return -1; }
    kprintf("display: output %d '%s' %dx%d%s\n", i, o->name, o->w, o->h,
            o->primary ? " (primary)" : "");
    return i;
}

int display_count(void) { return g_n; }
struct display_output* display_at(int i) { return (i >= 0 && i < g_n) ? g_out[i] : NULL; }

static int sames(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int display_arrange(void) {
    int right_edge = 0, bottom_edge = 0, n = 0;
    for (int i = 0; i < g_n; i++) {
        struct display_output* o = g_out[i];
        if (!o) continue;
        if (i == 0) {
            o->x = 0; o->y = 0; o->enabled = 1;
            right_edge = o->w; bottom_edge = o->h;
            n++;
            continue;
        }
        char key[48] = "display.";
        int k = 8;
        for (int j = 0; o->name[j] && k < 36; j++) key[k++] = o->name[j];
        const char* suf = ".position";
        for (int j = 0; suf[j] && k < 47; j++) key[k++] = suf[j];
        key[k] = 0;
        const char* pos = config_get(key, "right");
        if (sames(pos, "off")) { o->enabled = 0; continue; }
        o->enabled = 1;
        if (sames(pos, "below")) {
            o->x = 0; o->y = bottom_edge;
            bottom_edge += o->h;
        } else {                                  /* "right" and anything unknown */
            o->x = right_edge; o->y = 0;
            right_edge += o->w;
        }
        n++;
    }
    return n;
}

void display_union(int* w, int* h) {
    int uw = 0, uh = 0;
    for (int i = 0; i < g_n; i++) {
        struct display_output* o = g_out[i];
        if (!o || !o->enabled) continue;
        if (o->x + o->w > uw) uw = o->x + o->w;
        if (o->y + o->h > uh) uh = o->y + o->h;
    }
    if (w) *w = uw;
    if (h) *h = uh;
}

int display_at_point(int x, int y) {
    for (int i = 0; i < g_n; i++) {
        struct display_output* o = g_out[i];
        if (o && o->enabled && x >= o->x && x < o->x + o->w && y >= o->y && y < o->y + o->h)
            return i;
    }
    return -1;
}

int display_rect(int i, int* x, int* y, int* w, int* h) {
    struct display_output* o = display_at(i);
    if (!o) return -1;
    if (x) *x = o->x;
    if (y) *y = o->y;
    if (w) *w = o->w;
    if (h) *h = o->h;
    return 0;
}

static void cmd_displays(const char* args) {
    (void)args;
    for (int i = 0; i < g_n; i++) {
        struct display_output* o = g_out[i];
        if (!o) { kprintf("  0  (the primary registers when the desktop starts)\n"); continue; }
        kprintf("  %d  %s  %dx%d  at %d,%d  %s%s\n", i, o->name, o->w, o->h, o->x, o->y,
                o->enabled ? "on" : "off", o->primary ? "  (primary)" : "");
    }
    int uw, uh;
    display_union(&uw, &uh);
    kprintf("  desktop: %dx%d\n", uw, uh);
}
SHELL_CMD(displays) = { "displays", "", "the monitors, their sizes and where each sits in the desktop",
                        SHELL_G_DEV, cmd_displays, SHELL_P_ANY };

/* The arrangement key, declared once for every output name the drivers use
 * today — `display.bochs1.position` for the first secondary head.  The generic
 * Display panel lists it with no per-key code. */
/* The Monitors page: the generic key panel plus one live line naming every
 * output and where it sits — the arrangement is easier to choose next to the
 * picture of what is there now. */
static void mon_live(char* b, int cap) {
    int n = 0;
    b[0] = 0;
    for (int i = 0; i < g_n && n < cap - 1; i++) {
        struct display_output* o = g_out[i];
        if (!o) continue;
        char tmp[64]; int k = 0;
        const char* nm = o->name;
        while (*nm && k < 20) tmp[k++] = *nm++;
        tmp[k++] = ' ';
        int v = o->w; char d[8]; int dn = 0; do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (dn) tmp[k++] = d[--dn];
        tmp[k++] = 'x';
        v = o->h; dn = 0; do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (dn) tmp[k++] = d[--dn];
        if (!o->enabled) { const char* off = " (off)"; while (*off) tmp[k++] = *off++; }
        tmp[k] = 0;
        if (n) { b[n++] = ' '; b[n++] = ' '; b[n++] = '|'; b[n++] = ' '; b[n++] = ' '; }
        for (int j = 0; tmp[j] && n < cap - 1; j++) b[n++] = tmp[j];
        b[n] = 0;
    }
}
#include "icons.h"
SETTINGS_PANEL(sp_monitors) = {
    .name    = "Monitors",
    .summary = "more than one screen: where each one sits",
    .icon    = ICON_MONITORS,
    .live    = mon_live,
};

CONFIG_KEY(ck_disp1_pos) = {
    .key = "display.bochs1.position", .group = "Monitors", .type = CFG_ENUM,
    .values = "right below off", .def = "right",
    .help = "where the second monitor sits: right of the primary, below it, or off (applies when the desktop starts)",
};
