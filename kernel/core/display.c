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
    int right_edge = 0, bottom_edge = 0, left_edge = 0, top_edge = 0, n = 0;
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
        } else if (sames(pos, "left")) {
            left_edge -= o->w;
            o->x = left_edge; o->y = 0;
        } else if (sames(pos, "above")) {
            top_edge -= o->h;
            o->x = 0; o->y = top_edge;
        } else {                                  /* "right" and anything unknown */
            o->x = right_edge; o->y = 0;
            right_edge += o->w;
        }
        n++;
    }
    /* §M88 rung (2026-09-29) — LEFT and ABOVE put an output at a negative
     * position relative to the primary; the desktop's pixels start at (0,0),
     * so everything is shifted until the leftmost/topmost edge is 0.  The
     * primary then sits at (-left_edge, -top_edge) — the compositor reads that
     * back as its `prim_x/prim_y` and keeps the shell, new windows and the
     * taskbar in the primary's own coordinates (gui_priv.h). */
    if (left_edge || top_edge)
        for (int i = 0; i < g_n; i++)
            if (g_out[i] && g_out[i]->enabled) { g_out[i]->x -= left_edge; g_out[i]->y -= top_edge; }
    return n;
}

/* "1920x1200" -> 1920, 1200; 0 on success. */
static int parse_mode(const char* s, int* w, int* h) {
    int a = 0, b = 0;
    if (!s || *s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') a = a * 10 + (*s++ - '0');
    if (*s != 'x' && *s != 'X') return -1;
    s++;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') b = b * 10 + (*s++ - '0');
    if (*s || a < 320 || b < 200 || a > 4096 || b > 4096) return -1;
    *w = a; *h = b;
    return 0;
}

void display_apply_modes(void) {
    for (int i = 1; i < g_n; i++) {
        struct display_output* o = g_out[i];
        if (!o || !o->set_mode) continue;
        char key[48] = "display.";
        int k = 8;
        for (int j = 0; o->name[j] && k < 40; j++) key[k++] = o->name[j];
        const char* suf = ".mode";
        for (int j = 0; suf[j]; j++) key[k++] = suf[j];
        key[k] = 0;
        const char* m = config_get(key, "");
        int w, h;
        if (!m || !m[0] || parse_mode(m, &w, &h) != 0) continue;
        if (w == o->w && h == o->h) continue;
        if (o->set_mode(o, w, h) == 0)
            kprintf("display: '%s' now %dx%d (%s)\n", o->name, o->w, o->h, key);
        else
            kprintf("display: '%s' refused %dx%d - keeping %dx%d\n", o->name, w, h, o->w, o->h);
    }
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

CONFIG_KEY(ck_disp1_mode) = {
    .key = "display.bochs1.mode", .group = "Monitors", .type = CFG_ENUM,
    .values = "1280x800 1024x768 1600x900 1920x1080 1920x1200", .def = "1280x800",
    .help = "the second monitor's resolution",
};

CONFIG_KEY(ck_disp1_pos) = {
    .key = "display.bochs1.position", .group = "Monitors", .type = CFG_ENUM,
    .values = "right left below above off", .def = "right",
    .help = "where the second monitor sits beside the primary, or off",
};
