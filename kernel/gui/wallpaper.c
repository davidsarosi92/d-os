/* =============================================================================
 * wallpaper.c — desktop background sources (§M60).  See wallpaper.h for the
 * contract; this file is the implementation and the traps.
 *
 * Structure:
 *   - spec parsing            ("gradient" / "solid:RRGGBB" / a VFS path)
 *   - a streaming BMP reader  (header validation + one row at a time)
 *   - the fit transform       (fill / stretch / center / tile)
 *   - the gradient fallback   (also the backdrop behind `center`)
 *
 * The scaler is nearest-neighbour with a precomputed X map.  Two reasons, both
 * about what this kernel has: there is no floating point available in kernel
 * context on any of the three arches without saving FP state (§A2's lesson
 * from the other direction), and a divide per pixel at 1920×1200 is 2.3 M
 * divisions per repaint — the X map turns that into 1920 divisions plus a
 * table lookup per pixel.  Bilinear filtering would look better and is a
 * deliberate non-goal here: it needs a second source row live at all times,
 * which is exactly the streaming property this file is built around.
 * ============================================================================= */

#include "wallpaper.h"
#include "gfx.h"
#include "gui.h"
#include "vfs.h"
#include "config.h"
#include "settings.h"
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include <stddef.h>
#include <stdint.h>

/* The built-in gradient — the same two colours the desktop shipped with, kept
 * here so `gui.c` no longer needs to know what a background looks like. */
#define COL_WALL_TOP    0xFF10243Eu
#define COL_WALL_BOT    0xFF1B5E63u

/* An image wider or taller than this is refused rather than trusted: the row
 * buffer is sized from the header, and a corrupt header is the classic way to
 * turn an image loader into an allocator attack. */
#define WALL_MAX_DIM    8192

enum fit_mode { FIT_FILL, FIT_STRETCH, FIT_CENTER, FIT_TILE };

/* Status line, rebuilt on every render.  Static because the shell (and later
 * the §M63 panel) asks for it long after the render returned. */
static char status[192];

/* ------------------------------------------------------------------- */
/* Small string helpers — no libc in the kernel.                        */
/* ------------------------------------------------------------------- */

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int starts_(const char* s, const char* pfx) {
    while (*pfx) { if (*s++ != *pfx++) return 0; }
    return 1;
}
/* Append to `status` at *pos, NUL-terminating.  Silently truncates. */
static void st_add(int* pos, const char* s) {
    while (*s && *pos < (int)sizeof status - 1) status[(*pos)++] = *s++;
    status[*pos] = '\0';
}
static void st_num(int* pos, long v) {
    char tmp[24];
    int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) tmp[n++] = '-';
    while (n-- > 0 && *pos < (int)sizeof status - 1) status[(*pos)++] = tmp[n];
    status[*pos] = '\0';
}

/* Parse up to 8 hex digits.  Returns 0 on success (value in *out). */
static int parse_hex(const char* s, uint32_t* out) {
    uint32_t v = 0;
    int n = 0;
    if (!*s) return -1;
    for (; *s; s++, n++) {
        char c = *s;
        uint32_t d;
        if      (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return -1;
        if (n >= 8) return -1;
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

static enum fit_mode fit_from_name(const char* s) {
    if (streq_(s, "stretch")) return FIT_STRETCH;
    if (streq_(s, "center"))  return FIT_CENTER;
    if (streq_(s, "centre"))  return FIT_CENTER;
    if (streq_(s, "tile"))    return FIT_TILE;
    return FIT_FILL;                        /* default, and what a photo wants */
}
static const char* fit_name(enum fit_mode f) {
    switch (f) {
        case FIT_STRETCH: return "stretch";
        case FIT_CENTER:  return "center";
        case FIT_TILE:    return "tile";
        default:          return "fill";
    }
}

/* ------------------------------------------------------------------- */
/* Gradient + solid — the sources that cannot fail.                     */
/* ------------------------------------------------------------------- */

static void paint_gradient(struct gfx_surface* dst) {
    gfx_vgradient(dst, 0, 0, dst->w, dst->h, COL_WALL_TOP, COL_WALL_BOT);
}

/* ------------------------------------------------------------------- */
/* BMP.                                                                 */
/*                                                                      */
/* Layout of the two headers we accept (little-endian throughout):      */
/*   BITMAPFILEHEADER  14 bytes: 'BM', file size, 2 reserved, pixel off */
/*   BITMAPINFOHEADER  40 bytes: size, w, h, planes, bpp, compression…  */
/*                                                                      */
/* Traps handled below, each of which produces a plausible-looking but  */
/* wrong image if missed:                                               */
/*   - HEIGHT MAY BE NEGATIVE.  A positive height means the rows are    */
/*     stored BOTTOM-UP (the original OS/2 convention); negative means  */
/*     top-down.  Ignoring the sign flips the picture vertically, which */
/*     looks like a bug in the scaler rather than in the parser.        */
/*   - ROWS ARE PADDED TO 4 BYTES.  A 3-pixel-wide 24 bpp row is 9 data */
/*     bytes and 12 stored bytes.  Using w*3 as the stride shears the   */
/*     image progressively — the classic "diagonal" corruption.         */
/*   - 32 bpp BMPs carry an alpha byte that is very often ZERO.  We     */
/*     force alpha to 0xFF: the compositor blits the wallpaper opaquely */
/*     today, but a future blend path would turn the whole desktop      */
/*     transparent, and the bug would surface milestones away from here.*/
/* ------------------------------------------------------------------- */

struct bmp {
    struct file* f;
    uint32_t     data_off;      /* byte offset of the pixel array         */
    int          w, h;          /* dimensions, h already sign-corrected   */
    int          bpp;           /* 24 or 32                               */
    int          bottom_up;     /* 1 = last stored row is the top row     */
    uint32_t     row_bytes;     /* padded stored size of one row          */
    uint8_t*     row;           /* one-row staging buffer                 */
};

static int rd_at(struct file* f, uint64_t off, void* buf, int n) {
    f->pos = off;
    ssize_t got = vfs_read(f, buf, (size_t)n);
    return (got == (ssize_t)n) ? 0 : -1;
}
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* Open + validate.  Returns 0 on success; `why` is set to a short reason on
 * failure so the status line can say something more useful than "failed". */
static int bmp_open(struct bmp* b, const char* path, const char** why) {
    uint8_t hdr[54];

    b->f = NULL; b->row = NULL;
    *why = "unreadable";

    b->f = vfs_open(path, VFS_RDONLY);
    if (!b->f) return -1;
    if (rd_at(b->f, 0, hdr, (int)sizeof hdr) != 0) { *why = "truncated"; goto fail; }

    if (hdr[0] != 'B' || hdr[1] != 'M') { *why = "not a BMP"; goto fail; }

    b->data_off = le32(hdr + 10);
    uint32_t ih  = le32(hdr + 14);          /* BITMAPINFOHEADER size */
    if (ih < 40) { *why = "unsupported BMP header"; goto fail; }

    int32_t  w   = (int32_t)le32(hdr + 18);
    int32_t  h   = (int32_t)le32(hdr + 22);
    uint16_t bpp = le16(hdr + 28);
    uint32_t comp= le32(hdr + 30);

    /* BI_RGB (0) only.  BI_BITFIELDS (3) is common for 32 bpp and usually
     * carries the same BGRA order, but "usually" is not a format: accepting it
     * without reading the masks would render some files with red and blue
     * swapped, which is exactly the kind of failure a user reports as "the
     * colours are wrong" and nobody can reproduce. */
    if (comp != 0)               { *why = "compressed BMP";   goto fail; }
    if (bpp != 24 && bpp != 32)  { *why = "not 24/32 bpp";    goto fail; }

    b->bottom_up = (h > 0);
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > WALL_MAX_DIM || h > WALL_MAX_DIM) {
        *why = "implausible dimensions"; goto fail;
    }

    b->w = (int)w;
    b->h = (int)h;
    b->bpp = (int)bpp;
    /* Stored row size, padded up to a 4-byte boundary. */
    b->row_bytes = (((uint32_t)w * (uint32_t)(bpp / 8)) + 3u) & ~3u;

    b->row = (uint8_t*)kmalloc(b->row_bytes);
    if (!b->row) { *why = "out of memory"; goto fail; }
    return 0;

fail:
    if (b->f) { vfs_close(b->f); b->f = NULL; }
    return -1;
}

static void bmp_close(struct bmp* b) {
    if (b->row) { kfree(b->row); b->row = NULL; }
    if (b->f)   { vfs_close(b->f); b->f = NULL; }
}

/* Read source row `sy` (0 = TOP of the image, whatever the storage order) into
 * b->row.  Returns 0 on success. */
static int bmp_row(struct bmp* b, int sy) {
    int stored = b->bottom_up ? (b->h - 1 - sy) : sy;
    uint64_t off = (uint64_t)b->data_off + (uint64_t)stored * b->row_bytes;
    return rd_at(b->f, off, b->row, (int)b->row_bytes);
}

/* Pixel `sx` of the staged row, as opaque ARGB. */
static uint32_t bmp_px(const struct bmp* b, int sx) {
    const uint8_t* p = b->row + (uint32_t)sx * (uint32_t)(b->bpp / 8);
    /* BMP stores B,G,R (,A) — the byte order is why this looks reversed. */
    return 0xFF000000u | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

/* ------------------------------------------------------------------- */
/* The fit transform.                                                   */
/*                                                                      */
/* Every mode reduces to the same loop: for each destination row, decide  */
/* which source row feeds it (or that none does), then walk the row      */
/* through a precomputed X map.  Keeping all four modes in one loop is   */
/* deliberate — they differ only in two mappings, and four separate      */
/* loops would be four places to get the clipping wrong.                 */
/* ------------------------------------------------------------------- */

static int render_image(struct gfx_surface* dst, const char* path,
                        enum fit_mode fit, const char** why) {
    struct bmp b;
    if (bmp_open(&b, path, why) != 0) return -1;

    int dw = dst->w, dh = dst->h;

    /* Scaled size + top-left offset in destination space. */
    int sw = b.w, sh = b.h;
    int tw, th, ox, oy;

    switch (fit) {
        case FIT_STRETCH:
            tw = dw; th = dh; ox = 0; oy = 0;
            break;
        case FIT_CENTER:
        case FIT_TILE:
            tw = sw; th = sh;
            ox = (dw - tw) / 2; oy = (dh - th) / 2;
            break;
        default: {   /* FIT_FILL — cover the screen, crop the overflow. */
            /* Compare aspect ratios without division: sw/sh vs dw/dh. */
            if ((int64_t)sw * dh > (int64_t)dw * sh) {
                th = dh;                                  /* height-limited */
                tw = (int)(((int64_t)sw * dh) / sh);
            } else {
                tw = dw;                                  /* width-limited  */
                th = (int)(((int64_t)sh * dw) / sw);
            }
            if (tw < 1) tw = 1;
            if (th < 1) th = 1;
            ox = (dw - tw) / 2; oy = (dh - th) / 2;
            break;
        }
    }

    /* X map: destination column → source column.  Built once for the whole
     * image; FIT_TILE wraps, the others clamp to the scaled rectangle. */
    int* xmap = (int*)kmalloc((size_t)dw * sizeof(int));
    if (!xmap) { bmp_close(&b); *why = "out of memory"; return -1; }
    for (int x = 0; x < dw; x++) {
        int rel = x - ox;
        int sx;
        if (fit == FIT_TILE) {
            /* C's % keeps the sign of the dividend — a negative rel would
             * index before the row.  Bias it positive first. */
            rel %= sw;
            if (rel < 0) rel += sw;
            sx = rel;
        } else if (rel < 0 || rel >= tw) {
            sx = -1;                                     /* outside the image */
        } else {
            sx = (int)(((int64_t)rel * sw) / tw);
            if (sx >= sw) sx = sw - 1;
        }
        xmap[x] = sx;
    }

    /* Anything the image does not cover gets the gradient underneath, so
     * `center` on a small picture looks intentional instead of unpainted. */
    if (fit == FIT_CENTER || (fit != FIT_TILE && (tw < dw || th < dh)))
        paint_gradient(dst);

    int ok = 0;
    int last_src = -2;
    for (int y = 0; y < dh; y++) {
        int rel = y - oy;
        int sy;
        if (fit == FIT_TILE) {
            rel %= sh;
            if (rel < 0) rel += sh;
            sy = rel;
        } else if (rel < 0 || rel >= th) {
            continue;                                    /* leave the backdrop */
        } else {
            sy = (int)(((int64_t)rel * sh) / th);
            if (sy >= sh) sy = sh - 1;
        }

        /* Re-read only when the source row actually changes: scaling UP means
         * many destination rows share one source row, and a seek+read per
         * destination row would multiply the file traffic by the scale factor. */
        if (sy != last_src) {
            if (bmp_row(&b, sy) != 0) { *why = "truncated"; ok = -1; break; }
            last_src = sy;
        }

        uint32_t* drow = dst->px + (size_t)y * (size_t)dst->stride;
        for (int x = 0; x < dw; x++) {
            int sx = xmap[x];
            if (sx < 0) continue;
            drow[x] = bmp_px(&b, sx);
        }
    }

    kfree(xmap);
    bmp_close(&b);
    return ok;
}

/* ------------------------------------------------------------------- */
/* Public entry points.                                                 */
/* ------------------------------------------------------------------- */

/* ------------------------------------------------------------------- */
/* The embedded default wallpaper.                                      */
/*                                                                      */
/* The system SHIPS a picture, so a fresh boot shows one instead of a   */
/* gradient and an instruction to go and find an image.  It is written  */
/* to the VFS once, at GUI start, rather than decoded from the blob     */
/* directly, for two reasons: the decoder reads through `struct file`   */
/* (so a blob would need a second, parallel read path), and once it is  */
/* a file the user can point `gui.wallpaper` at it, copy it, replace it */
/* or delete it like any other — an embedded default that cannot be     */
/* replaced is a hardcoded background wearing a config key.             */
/* ------------------------------------------------------------------- */

extern const unsigned char _binary_assets_wallpaper_default_bmp_start[]
    __attribute__((weak));
extern const unsigned char _binary_assets_wallpaper_default_bmp_end[]
    __attribute__((weak));

int wallpaper_provision(void) {
    const unsigned char* b = _binary_assets_wallpaper_default_bmp_start;
    const unsigned char* e = _binary_assets_wallpaper_default_bmp_end;
    /* WEAK symbols: a build without the asset links fine and simply has no
     * default picture.  Testing them is what keeps `assets/` optional rather
     * than a hard build dependency. */
    if (!b || !e || e <= b) return -1;

    struct file* f = vfs_open(WALLPAPER_DEFAULT_PATH, VFS_RDONLY);
    if (f) { vfs_close(f); return 0; }          /* already there */

    vfs_mkdir("/usr");
    vfs_mkdir("/usr/share");
    vfs_mkdir("/usr/share/wallpapers");

    f = vfs_open(WALLPAPER_DEFAULT_PATH, VFS_WRONLY | VFS_CREATE);
    if (!f) return -1;
    ssize_t n = vfs_write(f, b, (size_t)(e - b));
    vfs_close(f);
    if (n != (ssize_t)(e - b)) {
        vfs_unlink(WALLPAPER_DEFAULT_PATH);     /* a half-written BMP is worse */
        klog(KLOG_WARN, "gui", "wallpaper: could not provision the default image\n");
        return -1;
    }
    kprintf("wallpaper: default image provisioned at %s (%u bytes)\n",
            WALLPAPER_DEFAULT_PATH, (unsigned)(e - b));
    return 0;
}

int wallpaper_draw_bmp(struct gfx_surface* dst, const char* path, int centered) {
    if (!dst || !path) return -1;
    const char* why = "unreadable";
    /* Reuses the same streaming decoder the wallpaper uses — one BMP reader in
     * the kernel, not two.  §M62's splash is the second caller. */
    return render_image(dst, path, centered ? FIT_CENTER : FIT_FILL, &why);
}

int wallpaper_render(struct gfx_surface* dst) {
    if (!dst || !dst->px || dst->w <= 0 || dst->h <= 0) return -1;

    const char* spec = config_get("gui.wallpaper", WALLPAPER_DEFAULT_PATH);
    enum fit_mode fit = fit_from_name(config_get("gui.wallpaper_fit", "fill"));
    int pos = 0;
    status[0] = '\0';

    if (!spec || !*spec || streq_(spec, "gradient")) {
        paint_gradient(dst);
        st_add(&pos, "gradient");
        return 0;
    }

    if (starts_(spec, "solid:")) {
        uint32_t rgb;
        if (parse_hex(spec + 6, &rgb) == 0) {
            gfx_fill(dst, 0, 0, dst->w, dst->h, 0xFF000000u | (rgb & 0x00FFFFFFu));
            st_add(&pos, "solid ");
            st_add(&pos, spec + 6);
            return 0;
        }
        paint_gradient(dst);
        st_add(&pos, "gradient (fallback: \"");
        st_add(&pos, spec);
        st_add(&pos, "\" is not a hex colour)");
        klog(KLOG_WARN, "gui", "wallpaper: bad solid colour '%s' — using gradient\n", spec);
        return -1;
    }

    /* Anything else is a path. */
    const char* why = "unreadable";
    if (render_image(dst, spec, fit, &why) == 0) {
        st_add(&pos, spec);
        st_add(&pos, " (");
        st_add(&pos, fit_name(fit));
        st_add(&pos, ", ");
        st_num(&pos, dst->w);
        st_add(&pos, "x");
        st_num(&pos, dst->h);
        st_add(&pos, ")");
        return 0;
    }

    /* Failure path.  render_image may have painted a partial image before the
     * read failed, so repaint the whole thing rather than leaving a torn one. */
    paint_gradient(dst);
    st_add(&pos, "gradient (fallback: ");
    st_add(&pos, spec);
    st_add(&pos, " — ");
    st_add(&pos, why);
    st_add(&pos, ")");
    klog(KLOG_WARN, "gui", "wallpaper: %s — %s; using gradient\n", spec, why);
    return -1;
}

const char* wallpaper_status(void) {
    if (status[0]) return status;

    /* Nothing has been rendered yet — which is the normal state before `gui`
     * starts, and the first version of this function answered "gradient" there.
     * That is a LIE with a straight face: the user has just set a picture, the
     * config holds the picture, and the status line names the fallback.  Report
     * what boot will use instead, and mark it as not-yet-painted. */
    int pos = 0;
    status[0] = '\0';
    st_add(&pos, "not rendered yet — configured: ");
    st_add(&pos, config_get("gui.wallpaper", "gradient"));
    st_add(&pos, " (");
    st_add(&pos, config_get("gui.wallpaper_fit", "fill"));
    st_add(&pos, ")");
    /* Deliberately NOT cached: this is a snapshot of config, and the next call
     * must re-read it.  Clearing on entry above is what keeps a real render's
     * status from being shadowed by this one. */
    static char snap[sizeof status];
    for (int i = 0; i <= pos; i++) snap[i] = status[i];
    status[0] = '\0';
    return snap;
}

/* §M63 — DECLARE the keys, so the Control Panel can render them with no UI
 * code and `conf set` can refuse a value the renderer would only discover
 * later.  The declaration lives next to the code that READS the key, which is
 * the whole point of a registry: adding a setting is a line here, not an edit
 * to the panel. */
CONFIG_KEY(ck_wallpaper) = {
    .key = "gui.wallpaper", .group = "Personalisation", .type = CFG_STRING,
    .def = WALLPAPER_DEFAULT_PATH,
    .help = "gradient | solid:RRGGBB | a path to a BMP",
};
CONFIG_KEY(ck_wallpaper_fit) = {
    .key = "gui.wallpaper_fit", .group = "Personalisation", .type = CFG_ENUM,
    .values = "fill stretch center tile", .def = "fill",
    .help = "how the image is fitted to the screen",
};

/* §M63 stage 0 — react to the keys changing by ANY route: the persistent store
 * being overlaid once the disk mounts, `setconf`, or the Personalisation panel.
 * Without this, `setconf gui.wallpaper …` would be a setting that only takes
 * effect at the next boot while `wallpaper …` works immediately — one config
 * key with two behaviours depending on which command touched it, which is the
 * kind of inconsistency nobody can debug from the outside.
 *
 * Note the ORDER this creates: wallpaper_set() writes the key with config_set
 * (not config_apply) and repaints itself, precisely so this callback does not
 * fire a second, redundant repaint of the image it just drew. */
static void wallpaper_conf_changed(const char* key, const char* value) {
    (void)key; (void)value;
    gui_wallpaper_reload();
}
CONFIG_WATCH(wallpaper_watch) = {
    .prefix  = "gui.wallpaper",     /* covers gui.wallpaper AND gui.wallpaper_fit */
    .changed = wallpaper_conf_changed,
};

int wallpaper_set(const char* spec) {
    if (!spec || !*spec) return -2;
    /* Validate the cheap cases before committing them to config: a rejected
     * spec must leave the desktop exactly as it was. */
    if (starts_(spec, "solid:")) {
        uint32_t rgb;
        if (parse_hex(spec + 6, &rgb) != 0) return -2;
    }
    if (config_set("gui.wallpaper", spec) != 0) return -2;
    return gui_wallpaper_reload();
}

int wallpaper_set_fit(const char* fit) {
    if (!fit || !*fit) return -2;
    if (!streq_(fit, "fill") && !streq_(fit, "stretch") &&
        !streq_(fit, "center") && !streq_(fit, "centre") && !streq_(fit, "tile"))
        return -2;
    if (config_set("gui.wallpaper_fit", fit) != 0) return -2;
    return gui_wallpaper_reload();
}

/* ------------------------------------------------------------------- */
/* The shell command (shared by both shells — see wallpaper.h).          */
/* ------------------------------------------------------------------- */

/* Parse a small non-negative decimal; returns -1 if there is no digit. */
static int parse_int(const char** s) {
    const char* p = *s;
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return -1;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *s = p;
    return v;
}
/* Copy the next whitespace-delimited word; returns the remainder. */
static const char* next_word(const char* s, char* out, int cap) {
    int n = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && n < cap - 1) out[n++] = *s++;
    out[n] = '\0';
    while (*s == ' ') s++;
    return s;
}

void wallpaper_cmd(const char* args) {
    char word[128];
    const char* rest = next_word(args ? args : "", word, (int)sizeof word);

    if (!word[0]) {
        kprintf("wallpaper: %s\n", wallpaper_status());
        kprintf("  source: %s   fit: %s\n",
                config_get("gui.wallpaper", "gradient"),
                config_get("gui.wallpaper_fit", "fill"));
        kprintf("  usage: wallpaper gradient | solid:RRGGBB | <path.bmp>\n");
        kprintf("         wallpaper fit fill|stretch|center|tile\n");
        kprintf("         wallpaper testimg <path> [w h]   (generate a BMP)\n");
        kprintf("         wallpaper check [w h]            (render off-screen, print pixels)\n");
        return;
    }

    if (streq_(word, "fit")) {
        char mode[32];
        next_word(rest, mode, (int)sizeof mode);
        if (!mode[0]) { kprintf("wallpaper: fit fill|stretch|center|tile\n"); return; }
        if (wallpaper_set_fit(mode) == -2) {
            kprintf("wallpaper: unknown fit mode '%s'\n", mode);
            return;
        }
        kprintf("wallpaper: %s\n", wallpaper_status());
        return;
    }

    if (streq_(word, "check")) {
        /* Render the CURRENT configuration into an off-screen surface and
         * report what came out.  This exists because the display is the one
         * part of this system that cannot be tested the way everything else
         * is — every automated check here is a grep over a serial log, and the
         * aarch64 test harness has no framebuffer device at all, so on that
         * arch a screendump is not merely inconvenient, it is impossible.
         * Rendering into memory and printing the pixels makes the decoder and
         * the fit transform falsifiable on every arch, and the numbers are
         * comparable ACROSS arches, which a screenshot never is. */
        const char* p = rest;
        int w = parse_int(&p); if (w < 0) w = 320;
        int h = parse_int(&p); if (h < 0) h = 200;
        struct gfx_surface tmp;
        if (w <= 0 || h <= 0 || gfx_surface_init(&tmp, w, h) != 0) {
            kprintf("wallpaper: check %dx%d — cannot allocate surface\n", w, h);
            return;
        }
        int rc = wallpaper_render(&tmp);
        /* A checksum over every pixel (order-sensitive: a rotation or a row
         * flip changes it), plus the four corners and the centre by name so a
         * mismatch says WHERE rather than just "different". */
        uint32_t sum = 0;
        for (int y = 0; y < h; y++) {
            const uint32_t* row = tmp.px + (size_t)y * (size_t)tmp.stride;
            for (int x = 0; x < w; x++) sum = sum * 31u + row[x];
        }
        #define PX(xx, yy) (tmp.px[(size_t)(yy) * (size_t)tmp.stride + (xx)])
        /* NOTE: this kernel's printf has NO width or padding (printf.c says so
         * in its header) — the first version of this used %08x and printed the
         * format string itself, which is exactly the kind of "test passes,
         * evidence is garbage" outcome a harness must not produce. */
        kprintf("wallpaper check: %dx%d rc=%d sum=%x\n", w, h, rc, sum);
        kprintf("  tl=%x tr=%x bl=%x br=%x mid=%x\n",
                PX(0, 0), PX(w - 1, 0), PX(0, h - 1), PX(w - 1, h - 1),
                PX(w / 2, h / 2));
        #undef PX
        kprintf("  %s\n", wallpaper_status());
        gfx_surface_free(&tmp);
        return;
    }

    if (streq_(word, "testimg")) {
        char path[128];
        const char* p = next_word(rest, path, (int)sizeof path);
        if (!path[0]) { kprintf("wallpaper: testimg <path> [w h]\n"); return; }
        int w = parse_int(&p); if (w < 0) w = 320;
        int h = parse_int(&p); if (h < 0) h = 200;
        if (wallpaper_write_test_bmp(path, w, h) != 0)
            kprintf("wallpaper: could not write %s\n", path);
        return;
    }

    /* Anything else is a source spec. */
    int rc = wallpaper_set(args);
    if (rc == -2) {
        kprintf("wallpaper: rejected '%s' (bad spec — nothing changed)\n", args);
        return;
    }
    /* rc == -1 means it fell back; the status line says why, so print it
     * either way rather than a bare "ok"/"failed" that hides the reason. */
    kprintf("wallpaper: %s\n", wallpaper_status());
    if (!gui_is_active())
        kprintf("  (GUI not running — takes effect when `gui` starts)\n");
}

/* ------------------------------------------------------------------- */
/* Test image generator.                                                */
/*                                                                      */
/* Writes a 24 bpp bottom-up BMP with a pattern chosen to make the three   */
/* things that can go wrong VISIBLE rather than subtle:                   */
/*   - a colour ramp across both axes  → row/column order, and a mirrored  */
/*     image is obvious at a glance;                                      */
/*   - a one-pixel white border        → cropping and off-by-one edges;    */
/*   - a red marker block in the TOP-LEFT corner only → the bottom-up row  */
/*     order, which is the single most likely parser bug.                 */
/* The width is deliberately NOT a multiple of 4 when odd sizes are asked  */
/* for, so the row padding path gets exercised by the test rather than by  */
/* a user's photo.                                                        */
/* ------------------------------------------------------------------- */

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);  p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int wallpaper_write_test_bmp(const char* path, int w, int h) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;

    uint32_t row_bytes = ((uint32_t)w * 3u + 3u) & ~3u;
    uint32_t data_sz   = row_bytes * (uint32_t)h;

    uint8_t hdr[54];
    for (int i = 0; i < 54; i++) hdr[i] = 0;
    hdr[0] = 'B'; hdr[1] = 'M';
    put32(hdr + 2,  54u + data_sz);         /* file size            */
    put32(hdr + 10, 54u);                   /* pixel data offset    */
    put32(hdr + 14, 40u);                   /* info header size     */
    put32(hdr + 18, (uint32_t)w);
    put32(hdr + 22, (uint32_t)h);           /* positive = bottom-up */
    hdr[26] = 1;                            /* planes               */
    hdr[28] = 24;                           /* bpp                  */
    put32(hdr + 34, data_sz);

    uint8_t* row = (uint8_t*)kmalloc(row_bytes);
    if (!row) return -1;

    vfs_unlink(path);
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE);
    if (!f) { kfree(row); return -1; }
    if (vfs_write(f, hdr, sizeof hdr) != (ssize_t)sizeof hdr) {
        vfs_close(f); kfree(row); return -1;
    }

    for (int stored = 0; stored < h; stored++) {
        int y = h - 1 - stored;                     /* bottom-up on disk */
        for (uint32_t i = 0; i < row_bytes; i++) row[i] = 0;
        for (int x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 255) / (w > 1 ? w - 1 : 1));
            uint8_t g = (uint8_t)((y * 255) / (h > 1 ? h - 1 : 1));
            uint8_t bl = (uint8_t)(((x + y) * 255) / ((w + h) > 1 ? (w + h - 2) : 1));
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) { r = g = bl = 255; }
            if (x < w / 8 && y < h / 8) { r = 255; g = 0; bl = 0; }
            row[x * 3 + 0] = bl;                    /* BMP order: B, G, R */
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        if (vfs_write(f, row, row_bytes) != (ssize_t)row_bytes) {
            vfs_close(f); kfree(row); return -1;
        }
    }

    vfs_close(f);
    kfree(row);
    kprintf("wallpaper: wrote test image %s (%dx%d, %u bytes)\n",
            path, w, h, 54u + data_sz);
    return 0;
}
