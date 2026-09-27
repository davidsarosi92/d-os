/* =============================================================================
 * icons.h — the system icon set (§M64 / §M63).
 *
 * There was no icon anywhere in this system: the only graphic with a shape in
 * it was the 8×8 glyph font.  A desktop with shortcuts and a control panel with
 * categories both need pictures, so this is the primitive both build on.
 *
 * WHY PROCEDURAL RATHER THAN BITMAPS.  The obvious implementation is a set of
 * PNG/BMP blobs linked in with objcopy (the pattern userland programs already
 * use).  Drawing them instead buys three things that matter more here than
 * fidelity does:
 *
 *   1. **Any size, one definition.**  A 24 px taskbar icon, a 48 px desktop
 *      icon and a 64 px control-panel tile are the same code with a different
 *      `size`.  A bitmap set means three files per icon, or scaling artefacts.
 *   2. **No build plumbing and no arch question.**  Blobs mean Makefile rules
 *      per arch and symbols renamed per arch (§M47.5's lesson) for what is,
 *      here, a few hundred bytes of shapes.
 *   3. **It cannot fail at runtime.**  There is no file to be missing, no
 *      decoder to refuse, no allocation to lose — an icon always draws.
 *
 * The cost is honest and worth stating: these are flat geometric glyphs, not
 * artwork.  When somebody wants real artwork the seam to add it is a new
 * `icon_draw` case that blits a bitmap — callers pass an id, and none of them
 * know how it is painted.
 *
 * Drawing contract: `icon_draw` paints inside the box (x, y, size, size) of the
 * destination surface, clipped by the surface's own clip rect like every gfx
 * primitive.  It never allocates and may be called from the compositor task.
 * ============================================================================= */

#ifndef ICONS_H
#define ICONS_H

#include <stdint.h>

struct gfx_surface;

enum icon_id {
    ICON_NONE = 0,
    ICON_APP,           /* generic application window                  */
    ICON_FOLDER,        /* file manager, a directory                   */
    ICON_DOC,           /* a document / the editor                     */
    ICON_TERMINAL,      /* a shell                                     */
    ICON_SETTINGS,      /* control panel (gear)                        */
    ICON_DISPLAY,       /* display settings (monitor)                  */
    ICON_BRUSH,         /* personalisation                             */
    ICON_GLOBE,         /* browser / network                           */
    ICON_CHART,         /* task manager                                */
    ICON_PACKAGE,       /* the §M35.5 store                            */
    ICON_KEYBOARD,      /* region / input                              */
    ICON_CLOCK,         /* date + time                                 */
    ICON_WARN,          /* crash reports                               */
    ICON_CODE,          /* BASIC / code                                */
    ICON_INFO,          /* about                                       */
    /* §M23 — the taskbar's sound indicator.  THREE states, not two: a device
     * that is missing or failed must look different from one that is merely
     * muted, or "I turned it off" and "it is broken" are the same picture and
     * the user goes looking for the wrong problem. */
    ICON_VOLUME,        /* audio available, audible                    */
    ICON_VOLUME_MUTED,  /* audio available, silenced by the user       */
    ICON_VOLUME_OFF,    /* no device / audio unavailable               */
    ICON_CHIP,          /* a device / the device manager               */
    /* From the Console Plate icon set (design/icons/).  These have vector
     * artwork and no drawn fallback — an id here that lost its vpath would
     * render as nothing, which is why `icon_draw` says so rather than leaving
     * an empty box for somebody to mistake for a layout bug. */
    ICON_FIREWALL,      /* firewall                                    */
    ICON_USERS,         /* users / accounts (§M32)                     */
    ICON_STORAGE,       /* disks, volumes                              */
    ICON_PRINTER,       /* printers                                    */
    ICON_BLUETOOTH,     /* bluetooth                                   */
    ICON_UPDATE,        /* updates                                     */
    ICON_POWER,         /* power management                            */
    ICON_MOUSE,         /* pointer settings                            */
    ICON_LOCALE,        /* language and region                         */
    ICON_ACCESS,        /* accessibility                               */
    ICON_BACKUP,        /* backup                                      */
    ICON_REMOTE,        /* remote desktop                              */
    /* §M87 — the taskbar's network indicator.  SIX pictures for six answers
     * (net_get_state), for §M23's reason: "the cable is out", "I turned it
     * off", "there is no adapter" and "no address" each call for a different
     * fix, and one "offline" glyph would send the user looking for the wrong
     * one.  Drawn in COLOUR, so no vector artwork (see icon_draw). */
    ICON_NET_WIRED,     /* online through a cable                      */
    ICON_NET_NOLINK,    /* adapter present, no carrier                 */
    ICON_NET_NOADDR,    /* link up, no IPv4 address                    */
    ICON_NET_DISABLED,  /* the user disabled the adapter               */
    ICON_NET_NONE,      /* no network adapter at all                   */
    ICON_WIFI_1,        /* Wi-Fi, weak / fair / strong signal          */
    ICON_WIFI_2,
    ICON_WIFI_3,
    ICON_WIFI_OFF,      /* wireless adapter, not associated            */
    ICON_MEMORY,        /* memory: swap, page cache, the reserve       */
    ICON__COUNT
};

/* Paint icon `id` into the size×size box at (x,y). */
struct vpath;
/* Vector artwork per id, NULL where the design set has none.  Generated into
 * assets/icons_vector.c by scripts/svgset2icons.py from design/icons/. */
extern const struct vpath* const icon_vpaths[ICON__COUNT];

void icon_draw(struct gfx_surface* s, int x, int y, int size, int id);

/* Map a name to an id — used by shortcut files and config values, which are
 * text.  Unknown names return ICON_APP rather than nothing: a shortcut with a
 * misspelt icon should still be visible and clickable. */
int  icon_by_name(const char* name);
const char* icon_name(int id);

#endif
