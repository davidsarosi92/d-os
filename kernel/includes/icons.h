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
    ICON__COUNT
};

/* Paint icon `id` into the size×size box at (x,y). */
void icon_draw(struct gfx_surface* s, int x, int y, int size, int id);

/* Map a name to an id — used by shortcut files and config values, which are
 * text.  Unknown names return ICON_APP rather than nothing: a shortcut with a
 * misspelt icon should still be visible and clickable. */
int  icon_by_name(const char* name);
const char* icon_name(int id);

#endif
