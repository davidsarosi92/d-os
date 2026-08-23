/* =============================================================================
 * splash.c — the boot screen (§M62).  See splash.h for the four rules.
 *
 * Drawn with `gfx` primitives straight onto a framebuffer surface: a vertical
 * gradient, the product name, the milestone label and a progress bar.  No
 * decoder, no file, no allocation — the boot screen is the one thing that must
 * not be able to fail while the machine is still deciding whether it works.
 *
 * On ARM the framebuffer is a virtio-gpu scanout rather than a linear window,
 * so every paint ends with `fb_present_flush` — the same seam M21 carved, which
 * is why this file is arch-independent.
 * ============================================================================= */

#include "splash.h"
#include "gfx.h"
#include "fb_present.h"
#include "console.h"
#include "vc.h"       /* vc_screen_suppress — the OTHER path to the screen */
#include "config.h"
#include "printf.h"
#include "version.h"
#include "vpath.h"
#include "klog.h"
#include "printf.h"
#include "timer.h"
#include <stddef.h>
#include <stdint.h>

/* §M62 — the boot screen is BLACK with the white logo on it.  A gradient was
 * the placeholder while there was nothing to show; with real artwork the
 * background has to be the artwork's own, or the logo sits in a frame that does
 * not belong to it. */
#define COL_TOP     0xFF000000u
#define COL_BOT     0xFF000000u
#define COL_TEXT    0xFFEAF2FBu
#define COL_DIM     0xFF7E9AB8u
#define COL_BAR_BG  0xFF16324Eu
#define COL_BAR_FG  0xFF4FA3D8u

static int   active = 0;
static int   quiet  = 0;               /* `quiet` also shows the phase line */
static struct gfx_surface s;
static int   last_pct = -1;
static char  last_phase[24] = "";

/* EVERY screen sink, with the state each had before we touched it.
 *
 * There is more than one: the framebuffer console and the VGA text fallback
 * both register under the category "screen", and which of them is ACTIVE
 * depends on how far boot got.  The first version remembered the first match
 * and disabled that — which was the (already inactive) VGA sink, while the
 * framebuffer one went on printing boot lines over the splash.
 *
 * Kept as a small fixed array with the previous flags, because the restore
 * path runs in FAULT context: no lookup, no allocation, no iteration over a
 * registry somebody may be modifying. */
#define MAX_SCREEN_SINKS 4
static struct console_sink* screen_sinks[MAX_SCREEN_SINKS];
static int  screen_prev[MAX_SCREEN_SINKS];
static int  screen_n = 0;

static int str_len(const char* p) { int n = 0; while (p && p[n]) n++; return n; }

/* §M62 — the logo is a VECTOR, not a bitmap.
 *
 * Asked from use: *"could it be SVG rather than BMP — it has to look good at
 * every resolution?"*  It has to, and a bitmap cannot: it is sharp at exactly
 * one size, and §M61 made the resolution a runtime choice.  So the artwork is
 * flattened to polygons at BUILD time (scripts/svg2paths.py) and rasterised
 * here at whatever size the screen is — 2.5 KB of points instead of a 786 KB
 * bitmap, and crisp at 640×480 and at 4 K alike.  The SVG parser stays on the
 * host, where XML belongs. */
extern const struct vpath splash_logo;

/* Where the progress bar lives, derived from the surface so both painters
 * agree without either owning the number. */
static void bar_rect(int* bx, int* by, int* bw, int* bh) {
    *bw = s.w / 3;
    *bx = (s.w - *bw) / 2;
    *by = s.h * 5 / 6;
    *bh = 6;
}

/* Repaint ONLY the bar.  The logo is a vector fill over a large box — cheap on
 * hardware, and measurably not cheap under emulation — so redrawing the whole
 * screen for a progress update wasted most of the boot's drawing time and, on
 * the first attempt, was still running when a screendump caught it (the logo
 * appeared cut in half, which looked like a rasteriser bug and was not). */
static void draw_bar(void) {
    if (!active) return;
    int bx, by, bw, bh;
    bar_rect(&bx, &by, &bw, &bh);
    gfx_fill(&s, bx, by, bw, bh, COL_BAR_BG);
    int pct = last_pct < 0 ? 0 : (last_pct > 100 ? 100 : last_pct);
    gfx_fill(&s, bx, by, bw * pct / 100, bh, COL_BAR_FG);
    if (quiet && last_phase[0]) {
        gfx_fill(&s, 0, by + bh + 8, s.w, GFX_GLYPH_H + 2, COL_TOP);
        int pl = str_len(last_phase);
        gfx_text(&s, (s.w - pl * GFX_GLYPH_W) / 2, by + bh + 10, last_phase, COL_DIM);
    }
    fb_present_flush((uint32_t)bx, (uint32_t)by, (uint32_t)bw,
                     (uint32_t)(bh + GFX_GLYPH_H + 12));
}

static void draw_all(void) {
    if (!active) return;

    int w = s.w, h = s.h;
    gfx_fill(&s, 0, 0, w, h, COL_TOP);

    /* The logo, centred, at a size derived from the SCREEN — this is the whole
     * point of it being vector artwork.  Half the shorter axis reads well from
     * 640×480 up; the table is the same either way. */
    int have_logo = (splash_logo.npoly > 0);
    if (have_logo) {
        /* MILLISECONDS from the tick, not `timer_now_ns()`.  The splash now
         * draws immediately after module_init_all, and §M53's nanosecond clock
         * is established later in boot — it returns ZERO there, so the timing
         * printed "0 us" for something that takes tens of milliseconds.  The
         * same instrument failure §4.72 recorded: a measurement taken before
         * its clock exists reports the work as free. */
        uint32_t t0 = timer_ticks_ms();
        int side = (w < h ? w : h) / 2;
        int lx = (w - side) / 2, ly = (h - side) / 2 - h / 12;

        /* Rasterise OFF-SCREEN, then blit once.
         *
         * Reported from use: *"it loads in visibly, top to bottom."*  It did —
         * the filler writes rows straight into the scanned-out framebuffer, so
         * the eye follows the scanline down the logo.  Painting into a private
         * surface and copying the finished result makes the logo APPEAR rather
         * than arrive, which is the same argument M22.6 made for the
         * compositor's page flip, one layer down.
         *
         * If the allocation fails we draw directly — a splash that refuses to
         * paint because it could not get a nicety is worse than a visible
         * sweep (rule 1: it must ALWAYS paint something). */
        struct gfx_surface tmp;
        if (gfx_surface_init(&tmp, side, side) == 0) {
            gfx_fill(&tmp, 0, 0, side, side, COL_TOP);
            vpath_fill(&tmp, &splash_logo, 0, 0, side, COL_TEXT);
            gfx_blit(&s, lx, ly, &tmp, 0, 0, side, side);
            gfx_surface_free(&tmp);
        } else {
            vpath_fill(&s, &splash_logo, lx, ly, side, COL_TEXT);
        }
        /* Reported to the log (which the splash suppresses on screen but klog
         * keeps): a boot screen whose own cost is unmeasured is a boot screen
         * nobody can defend when boot gets slower. */
        /* The splash now draws before interrupts are running, so the tick has
         * not moved and neither clock can measure this — say THAT rather than
         * print a zero, which would read as "free" (§4.72's instrument lesson,
         * one boot phase earlier).  When the splash is raised later (`splash
         * on`, or a boot where the early path declined) the number is real. */
        uint32_t took = timer_ticks_ms() - t0;
        if (took) klog(KLOG_INFO, "splash", "logo %dx%d rasterised in %u ms\n",
                       side, side, (unsigned)took);
        else      klog(KLOG_INFO, "splash", "logo %dx%d rasterised (too early "
                       "to time - the tick is not running yet)\n", side, side);
        draw_bar();
        fb_present_flush(0, 0, (uint32_t)w, (uint32_t)h);
        return;
    }

    /* Product name, drawn as blocky text by scaling the 8×8 font: the console
     * font is the only glyph source this early, and a 1× title on a 1920-wide
     * screen is a whisper. */
    const char* name = "d-os";
    int nl = str_len(name);
    int scale = w / 320; if (scale < 2) scale = 2; if (scale > 8) scale = 8;
    int tw = nl * GFX_GLYPH_W * scale;
    int tx = (w - tw) / 2, ty = h / 2 - GFX_GLYPH_H * scale;
    for (int i = 0; i < nl; i++) {
        char one[2] = { name[i], 0 };
        /* gfx_text has no scale, so a scaled glyph is drawn as a block grid:
         * render once into the top-left of a cell and expand by hand. */
        for (int gy = 0; gy < GFX_GLYPH_H; gy++)
            for (int gx = 0; gx < GFX_GLYPH_W; gx++) {
                /* Sample the font through gfx_text by drawing a 1× copy into a
                 * scratch row would need a surface; instead use the font table
                 * the console exposes. */
                extern const uint8_t* fb_font_glyph(unsigned char ch);
                const uint8_t* g = fb_font_glyph((unsigned char)one[0]);
                if (!(g[gy] & (0x80u >> gx))) continue;
                gfx_fill(&s, tx + (i * GFX_GLYPH_W + gx) * scale,
                         ty + gy * scale, scale, scale, COL_TEXT);
            }
    }

    /* Milestone + arch, small, under the name. */
    int ll = str_len(DOS_LABEL);
    gfx_text(&s, (w - ll * GFX_GLYPH_W) / 2, ty + GFX_GLYPH_H * scale + 12,
             DOS_LABEL, COL_DIM);

    /* Progress bar. */
    int bw = w / 3, bx = (w - bw) / 2, by = h * 2 / 3, bh = 10;
    gfx_fill(&s, bx, by, bw, bh, COL_BAR_BG);
    int pct = last_pct < 0 ? 0 : (last_pct > 100 ? 100 : last_pct);
    gfx_fill(&s, bx, by, bw * pct / 100, bh, COL_BAR_FG);

    if (quiet && last_phase[0]) {
        int pl = str_len(last_phase);
        gfx_text(&s, (w - pl * GFX_GLYPH_W) / 2, by + bh + 10,
                 last_phase, COL_DIM);
    }

    fb_present_flush(0, 0, (uint32_t)w, (uint32_t)h);
}

/* Find the active SCREEN sink through the registry's iterator.  Only the screen
 * category is ever touched: serial keeps writing and klog keeps recording, so
 * `dmesg` afterwards has every line the console would have shown. */
static void find_screen_sink(const struct console_sink* c, void* ctx) {
    (void)ctx;
    if (!c || !c->category || screen_n >= MAX_SCREEN_SINKS) return;
    const char* k = c->category;
    if (k[0] == 's' && k[1] == 'c' && k[2] == 'r') {        /* "screen" */
        screen_sinks[screen_n] = (struct console_sink*)c;
        screen_prev[screen_n]  = c->active;
        screen_n++;
    }
}

static void console_screen_set(int on) {
    /* TWO paths reach the screen in this system, and suppressing one is not
     * suppressing output:
     *
     *   - the console SINK (`fb`), used before vc_init;
     *   - the per-task emit hook into the focused VC, which is how everything
     *     reaches the screen AFTER vc_init — and vc_init deactivates the fb
     *     sink precisely so the two do not both draw.
     *
     * The first version only cleared the sink flag, and the splash appeared
     * with boot-log lines printed on top of it: the sink was already inactive
     * and every line was going through the VC.  `vc_screen_suppress` is the
     * switch the GUI already uses for exactly this, and it is the one that
     * matters at this point in boot. */
    if (!screen_n) console_for_each(find_screen_sink, NULL);
    for (int i = 0; i < screen_n; i++)
        screen_sinks[i]->active = on ? screen_prev[i] : 0;
    vc_screen_suppress(on ? 0 : 1);
}

/* ------------------------------------------------------------------- */
/* Early suppression.                                                    */
/*                                                                       */
/* Reported from use: *"a few lines are printed before the splash comes  */
/* up."*  They were: `boot.splash` lives in the PERSISTENT store, and    */
/* that store is the disk mount, so the decision cannot be made until    */
/* the disk is there — while the console has been printing since the     */
/* framebuffer came up.                                                  */
/*                                                                       */
/* The fix is to invert it: go QUIET as early as the framebuffer exists, */
/* decide later, and if the answer is "no splash", REPLAY everything the */
/* screen missed out of klog.  Nothing is lost either way, which is the  */
/* same promise the splash itself makes — suppressed, never discarded.   */
/* ------------------------------------------------------------------- */

static int early_quiet = 0;

static void replay_one(const struct klog_record* r, void* ctx) {
    struct vc* v = (struct vc*)ctx;
    /* Written straight into the ROOT VC, not through kprintf.
     *
     * After `vc_init` the framebuffer SINK is deactivated on purpose and screen
     * output flows through the per-task emit hook — and the boot task has no
     * console bound, so a kprintf here reached the serial line and nothing
     * else.  The replay has to name the destination it means. */
    for (const char* p = r->msg; *p; p++) vc_putchar(v, *p);
    vc_putchar(v, '\n');
}

void splash_early_quiet(void) {
    /* Called the moment the framebuffer console is up — as early as there is
     * anything to draw on.  Cheap and reversible: the serial sink and klog are
     * untouched, so a boot that never reaches the decision point still has a
     * full log on the wire.
     *
     * IT RAISES THE SPLASH IMMEDIATELY, ON THE DEFAULT, and the stored setting
     * either confirms or cancels that later (splash_begin).  Reported from use:
     * *"there is still a bit of text before the boot screen, and it comes up a
     * little slowly."*  Both had the same cause — the DECISION needs the disk
     * (`boot.splash` lives in the persistent store, §4.63), so the first
     * version went quiet here and left a BLANK screen until the mount.  A blank
     * screen is not a neutral state to somebody watching a machine boot; it is
     * what a hung machine shows.
     *
     * Only the OVERRIDE has to wait for the disk; the default does not.  So:
     * draw now, revisit at the mount.  The cost of being wrong is that a
     * machine configured `boot.splash=off` shows the logo for the second it
     * takes to read the setting, and then drops to the log with every missed
     * line replayed.  The cost of the other order is a blank screen on every
     * boot of the DEFAULT configuration. */
    if (early_quiet || active) return;
    if (gfx_fb_surface(&s) != 0) return;        /* no framebuffer → nothing to hide */
    early_quiet = 1;
    console_screen_set(0);

    active   = 1;
    last_pct = 0;
    draw_all();
}

/* Replay is DEFERRED to `splash_screen_ready()`, not done here.
 *
 * Doing it at the decision point looked right and achieved nothing: `vc_init`
 * runs later, and it paints over the boot log by design (the comment in
 * kernel.c says so) — so the replayed lines were drawn and then wiped by the
 * thing that takes over the screen a moment afterwards.  *Restoring output
 * before the final owner of the screen exists is restoring it to nobody.* */
static int replay_pending = 0;

static void early_quiet_end(int splashing) {
    if (!early_quiet) return;
    early_quiet = 0;
    if (splashing) return;                      /* the splash owns the screen */
    console_screen_set(1);
    replay_pending = 1;
}

void splash_screen_ready(void) {
    /* THE SCREEN HAS A NEW OWNER, so the state we remembered for it is stale.
     *
     * `screen_prev` was snapshotted before `vc_init` ran, when the framebuffer
     * console SINK was the path to the screen and therefore active.  `vc_init`
     * has since called `fb_sink_disable()` — after that point output reaches
     * the screen through the per-task VC hook, and the sink must stay off.
     * Restoring the remembered flag would switch it back on, and every kprintf
     * from a task with no VC (the boot task, the compositor) would then paint
     * across the framebuffer.  That is exactly what it did: `meminfo` typed at
     * the desktop printed its output over the wallpaper.
     *
     * So the state to restore for a screen SINK is now "off", unconditionally,
     * and `vc_screen_suppress(0)` alone is what "give the screen back" means
     * from here on.  A remembered flag is only valid while nobody else is
     * allowed to change what it describes. */
    for (int i = 0; i < screen_n; i++) screen_prev[i] = 0;

    if (!replay_pending) return;
    replay_pending = 0;
    struct vc* v = vc_root();
    if (!v) return;
    /* A boot log that exists only on a serial cable is not a boot log for the
     * person sitting at the machine. */
    klog_for_each(replay_one, v);
}

void splash_begin(void) {
    /* THE DECISION, not the raising: splash_early_quiet already put the default
     * on the screen.  This is where the STORED setting gets its say — confirm
     * (nothing to do, the logo is up) or cancel (take it down, replay what the
     * screen missed). */
    const char* v = config_get("boot.splash", "on");   /* §M62 — on by default now */
    int on = 0;
    if (v) {
        if (v[0] == 'o' && v[1] == 'n') on = 1;
        else if (v[0] == 'q')           { on = 1; quiet = 1; }
    }
    if (!on) {
        /* Cancelled by the stored setting.  Wipe the logo before handing the
         * screen back — for the same reason splash_abort does: giving the sinks
         * back is not clearing the screen, and the log would start printing on
         * top of the artwork. */
        if (active && s.px) {
            gfx_fill(&s, 0, 0, s.w, s.h, 0xFF101828u);
            fb_present_flush(0, 0, (uint32_t)s.w, (uint32_t)s.h);
        }
        active = 0;
        early_quiet_end(0);
        return;
    }

    if (active) { early_quiet_end(1); return; }   /* already up — confirmed */

    /* Not up: either there was no framebuffer when the early call ran, or this
     * is `splash on` typed at a running system. */
    if (gfx_fb_surface(&s) != 0) { early_quiet_end(0); return; }
    early_quiet_end(1);
    active = 1;
    last_pct = 0;
    console_screen_set(0);                   /* suppress, never discard */
    draw_all();
}

void splash_progress(const char* phase, int pct) {
    if (!active) return;
    last_pct = pct;
    if (phase) {
        int i = 0;
        while (phase[i] && i < (int)sizeof last_phase - 1) { last_phase[i] = phase[i]; i++; }
        last_phase[i] = 0;
    }
    draw_bar();          /* not draw_all: see the note on draw_bar */
}

void splash_end(void) {
    if (!active) return;
    active = 0;
    /* WIPE THE FRAMEBUFFER, not just the console.  `console_clear()` broadcasts
     * to ACTIVE sinks, and by this point in boot the framebuffer sink has been
     * deactivated by `vc_init` — so it clears nothing, and the splash's last
     * progress bar stayed on screen underneath the shell's first prompt.  The
     * same lesson splash_abort already carries: handing the console back is not
     * clearing the screen. */
    if (s.px) {
        gfx_fill(&s, 0, 0, s.w, s.h, COL_TOP);
        fb_present_flush(0, 0, (uint32_t)s.w, (uint32_t)s.h);
    }
    console_screen_set(1);
    console_clear();
}

void splash_abort(void) {
    /* Fault context: no locks, no allocation.  Hand the screen back so whatever
     * is about to be printed is READABLE — the whole safety argument of the
     * feature (splash.h rule 3).
     *
     * It also WIPES the framebuffer, which the first version did not: handing
     * the sinks back made the report appear, but on top of the splash's
     * gradient, because `console_clear()` resets the console's own state and
     * the VC repaints only what it draws.  A panic report over a logo is still
     * a panic report over a logo.  A flat fill is safe here — it is a loop over
     * pixels, no different from what draw_all already does, and it takes
     * nothing. */
    if (!active) return;
    active = 0;
    if (s.px) {
        gfx_fill(&s, 0, 0, s.w, s.h, 0xFF101828u);      /* console background */
        fb_present_flush(0, 0, (uint32_t)s.w, (uint32_t)s.h);
    }
    for (int i = 0; i < screen_n; i++) screen_sinks[i]->active = screen_prev[i];
    vc_screen_suppress(0);
    console_clear();
}

int splash_active(void) { return active; }

/* `splash` — show/dismiss at runtime.  It exists for the TEST as much as for
 * the user: the fault teardown (rule 3) can only be falsified if the splash
 * can be put back up on a running system and then crashed into.  *A safety
 * property nobody can trigger on purpose is one nobody has verified.* */
void splash_cmd(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args || args[0] == 's') {
        kprintf("splash: %s (boot.splash = %s)\n",
                active ? "up" : "not shown", config_get("boot.splash", "off"));
        kprintf("  splash on | off | status | faultkernel (deliberate ring-0 fault)\n");
        return;
    }
    if (args[0] == 'f') { splash_faultkernel(); return; }
    if (args[0] == 'o' && args[1] == 'n') {
        if (active) { kprintf("splash: already up\n"); return; }
        if (gfx_fb_surface(&s) != 0) { kprintf("splash: no framebuffer\n"); return; }
            active = 1;
        console_screen_set(0);
        draw_all();
        return;
    }
    splash_end();
    kprintf("splash: down\n");
}

/* Deliberately fault in RING 0, so the splash's most important promise can be
 * demonstrated rather than asserted: with `kernel.fault_policy = kill` the box
 * survives, and the screendump afterwards either shows the fault report or
 * shows a logo — there is no third outcome, and that is what makes it a test.
 * (`hardlock` is the same idea for the NMI path, which always reboots.) */
void splash_faultkernel(void) {
    kprintf("splash: deliberate ring-0 fault — the report must be VISIBLE, "
            "not hidden behind the splash\n");
    /* Put the splash up HERE, in the same command: any keystroke dismisses it
     * (rule 4), so a test that types `splash on` and then types the fault
     * command has already dismissed the thing it meant to test.  The two steps
     * have to be one. */
    if (!active && gfx_fb_surface(&s) == 0) {
            active = 1;
        last_pct = 100;
        console_screen_set(0);
        draw_all();
    }
    /* NOT a null-ish address: low memory is IDENTITY-MAPPED on both x86
     * arches, so `*(int*)0x4 = …` succeeds silently and the "test" proves
     * nothing — the first version of this did exactly that, and the screendump
     * showed the splash still up for the innocent reason that no fault had
     * happened.  This address is above the identity map on every arch here. */
    volatile int* p = (volatile int*)(uintptr_t)0xDEADB000u;
    *p = 0xDEAD;
}

int splash_key(void) {
    if (!active) return 0;
    splash_end();
    kprintf("splash: dismissed — showing the boot log\n");
    return 1;
}
