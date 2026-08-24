/* =============================================================================
 * icons.c — the system icon set, drawn rather than stored.  See icons.h for
 * why, and for the seam that lets real artwork replace this later.
 *
 * Every icon is built from the same three pieces so the set looks like a set:
 *
 *   - a TILE: a rounded-ish square in the icon's accent colour, drawn as three
 *     stacked rectangles (there is no circle primitive in gfx.c, and adding one
 *     for corner rounding would be a bigger change than the corners are worth);
 *   - a GLYPH in white or a light tint on top of it;
 *   - proportions computed from `size`, never from constants, so one definition
 *     serves 24 px in a taskbar and 64 px in a control panel.
 *
 * Coordinates are integer and derived with /16 fractions of the box.  At very
 * small sizes some features collapse to a single pixel; that is deliberate —
 * the alternative is sub-pixel maths in a kernel with no floating point.
 * ============================================================================= */

#include "icons.h"
#include "gfx.h"
#include <stddef.h>

/* ------------------------------------------------------------------- */
/* Palette.  One accent per icon, chosen so a grid of them is readable  */
/* at a glance rather than pretty in isolation.                         */
/* ------------------------------------------------------------------- */
#define C_BLUE      0xFF3D6FB8u
#define C_TEAL      0xFF2E8C8Cu
#define C_AMBER     0xFFC8912Eu
#define C_SLATE     0xFF44506Au
#define C_GREEN     0xFF3E8E52u
#define C_PURPLE    0xFF6A5AA8u
#define C_RED       0xFFB4453Du
#define C_GREY      0xFF5A6478u
#define C_WHITE     0xFFF2F5FAu
#define C_DIM       0xFFC9D3E4u
#define C_DARK      0xFF1B2333u

/* A "rounded" tile: the middle band full width, top and bottom insets by one
 * step.  Three fills, no per-pixel work, and it reads as a rounded square from
 * 24 px upwards. */
static void tile(struct gfx_surface* s, int x, int y, int n, uint32_t col) {
    int r = n / 8; if (r < 1) r = 1;
    gfx_fill(s, x + r, y,         n - 2 * r, r,         col);
    gfx_fill(s, x,     y + r,     n,         n - 2 * r, col);
    gfx_fill(s, x + r, y + n - r, n - 2 * r, r,         col);
}

/* Horizontal "text" lines — used by several glyphs (document, list, chart). */
static void bars(struct gfx_surface* s, int x, int y, int w, int n,
                 int step, int th, uint32_t col) {
    for (int i = 0; i < n; i++)
        gfx_fill(s, x, y + i * step, w, th, col);
}

void icon_draw(struct gfx_surface* s, int x, int y, int n, int id) {
    if (!s || n < 8) return;

    const int u = n / 16 > 0 ? n / 16 : 1;      /* one unit = 1/16 of the box */
    const int q = n / 4;

    switch (id) {

    case ICON_FOLDER: {
        tile(s, x, y, n, C_AMBER);
        /* Folder: a tab on the left of the back panel, then the front panel. */
        gfx_fill(s, x + 3 * u, y + 4 * u, 5 * u, 2 * u, C_WHITE);
        gfx_fill(s, x + 3 * u, y + 5 * u, 10 * u, 7 * u, C_WHITE);
        gfx_fill(s, x + 4 * u, y + 7 * u, 8 * u, 4 * u, C_AMBER);
        break;
    }

    case ICON_DOC: {
        tile(s, x, y, n, C_BLUE);
        gfx_fill(s, x + 4 * u, y + 3 * u, 8 * u, 10 * u, C_WHITE);
        /* The dog-ear, faked with a triangle of shrinking rows. */
        for (int i = 0; i < 3 * u; i++)
            gfx_fill(s, x + 12 * u - i, y + 3 * u + i, i + 1, 1, C_DIM);
        bars(s, x + 5 * u, y + 6 * u, 6 * u, 3, 2 * u, u, C_BLUE);
        break;
    }

    case ICON_TERMINAL: {
        tile(s, x, y, n, C_DARK);
        gfx_fill(s, x + 2 * u, y + 3 * u, 12 * u, 10 * u, 0xFF0E1420u);
        /* A prompt: ">" as two strokes, then a cursor bar. */
        gfx_line(s, x + 4 * u, y + 6 * u, x + 6 * u, y + 8 * u, 0xFF6BE07Au);
        gfx_line(s, x + 6 * u, y + 8 * u, x + 4 * u, y + 10 * u, 0xFF6BE07Au);
        gfx_fill(s, x + 7 * u, y + 9 * u, 4 * u, u, 0xFF6BE07Au);
        break;
    }

    case ICON_SETTINGS: {
        tile(s, x, y, n, C_GREY);
        /* Gear: a ring plus four teeth.  Four, not eight — at 32 px eight
         * teeth merge into a blur and read as a circle. */
        gfx_fill(s, x + 7 * u, y + 2 * u, 2 * u, 3 * u, C_WHITE);
        gfx_fill(s, x + 7 * u, y + 11 * u, 2 * u, 3 * u, C_WHITE);
        gfx_fill(s, x + 2 * u, y + 7 * u, 3 * u, 2 * u, C_WHITE);
        gfx_fill(s, x + 11 * u, y + 7 * u, 3 * u, 2 * u, C_WHITE);
        gfx_fill(s, x + 5 * u, y + 5 * u, 6 * u, 6 * u, C_WHITE);
        gfx_fill(s, x + 7 * u, y + 7 * u, 2 * u, 2 * u, C_GREY);
        break;
    }

    case ICON_DISPLAY: {
        tile(s, x, y, n, C_SLATE);
        gfx_fill(s, x + 2 * u, y + 3 * u, 12 * u, 8 * u, C_WHITE);
        gfx_fill(s, x + 3 * u, y + 4 * u, 10 * u, 6 * u, 0xFF3D6FB8u);
        gfx_fill(s, x + 6 * u, y + 11 * u, 4 * u, 2 * u, C_WHITE);  /* stand */
        gfx_fill(s, x + 4 * u, y + 13 * u, 8 * u, u, C_WHITE);      /* foot  */
        break;
    }

    case ICON_BRUSH: {
        tile(s, x, y, n, C_PURPLE);
        /* Brush: a diagonal handle with a wide head at the bottom left. */
        for (int i = 0; i < 7 * u; i++)
            gfx_fill(s, x + 4 * u + i, y + 10 * u - i, 2 * u, 2 * u, C_WHITE);
        gfx_fill(s, x + 3 * u, y + 10 * u, 4 * u, 3 * u, 0xFFE0C56Cu);
        break;
    }

    case ICON_GLOBE: {
        tile(s, x, y, n, C_TEAL);
        /* A sphere suggested by a ROUNDED body with meridian + parallels.
         * The first version used a plain square and read as a table rather
         * than a globe — at this size the silhouette is what carries the
         * meaning, so the corners are worth three extra fills. */
        tile(s, x + 3 * u, y + 3 * u, 10 * u, C_WHITE);
        /* ONE meridian and THREE thin parallels, inset at the poles.  The
         * first version used two-unit bars at full width, which cut the body
         * into four squares and read as a table — the lines have to be
         * thinner than the gaps for a sphere to survive. */
        gfx_fill(s, x + 7 * u, y + 4 * u, u, 8 * u, C_TEAL);
        gfx_fill(s, x + 4 * u, y + 7 * u, 8 * u, u, C_TEAL);
        gfx_fill(s, x + 5 * u, y + 5 * u, 6 * u, u, C_TEAL);
        gfx_fill(s, x + 5 * u, y + 9 * u, 6 * u, u, C_TEAL);
        break;
    }

    case ICON_CHART: {
        tile(s, x, y, n, C_GREEN);
        gfx_fill(s, x + 3 * u, y + 9 * u, 2 * u, 4 * u, C_WHITE);
        gfx_fill(s, x + 6 * u, y + 6 * u, 2 * u, 7 * u, C_WHITE);
        gfx_fill(s, x + 9 * u, y + 3 * u, 2 * u, 10 * u, C_WHITE);
        gfx_fill(s, x + 2 * u, y + 13 * u, 11 * u, u, C_WHITE);
        break;
    }

    case ICON_PACKAGE: {
        tile(s, x, y, n, C_AMBER);
        gfx_fill(s, x + 3 * u, y + 5 * u, 10 * u, 8 * u, C_WHITE);
        gfx_fill(s, x + 3 * u, y + 5 * u, 10 * u, 2 * u, C_DIM);   /* lid   */
        gfx_fill(s, x + 7 * u, y + 5 * u, 2 * u, 8 * u, C_AMBER);  /* strap */
        break;
    }

    case ICON_KEYBOARD: {
        tile(s, x, y, n, C_SLATE);
        gfx_fill(s, x + 2 * u, y + 5 * u, 12 * u, 7 * u, C_WHITE);
        bars(s, x + 3 * u, y + 6 * u, 10 * u, 2, 2 * u, u, C_SLATE);
        gfx_fill(s, x + 5 * u, y + 10 * u, 6 * u, u, C_SLATE);     /* space */
        break;
    }

    case ICON_CLOCK: {
        tile(s, x, y, n, C_BLUE);
        gfx_fill(s, x + 3 * u, y + 3 * u, 10 * u, 10 * u, C_WHITE);
        gfx_fill(s, x + 7 * u, y + 5 * u, u, 4 * u, C_BLUE);       /* hour  */
        gfx_fill(s, x + 8 * u, y + 8 * u, 3 * u, u, C_BLUE);       /* minute*/
        break;
    }

    case ICON_WARN: {
        tile(s, x, y, n, C_RED);
        /* Triangle, drawn as widening rows. */
        for (int i = 0; i < 9 * u; i++)
            gfx_fill(s, x + 8 * u - i / 2, y + 3 * u + i, i + 1, 1, C_WHITE);
        gfx_fill(s, x + 7 * u, y + 7 * u, 2 * u, 3 * u, C_RED);
        gfx_fill(s, x + 7 * u, y + 11 * u, 2 * u, u, C_RED);
        break;
    }

    case ICON_CODE: {
        tile(s, x, y, n, C_PURPLE);
        /* "< >" — two chevrons. */
        gfx_line(s, x + 6 * u, y + 5 * u, x + 3 * u, y + 8 * u, C_WHITE);
        gfx_line(s, x + 3 * u, y + 8 * u, x + 6 * u, y + 11 * u, C_WHITE);
        gfx_line(s, x + 10 * u, y + 5 * u, x + 13 * u, y + 8 * u, C_WHITE);
        gfx_line(s, x + 13 * u, y + 8 * u, x + 10 * u, y + 11 * u, C_WHITE);
        break;
    }

    /* §M23 — a speaker: a small box plus a flared cone, then the state mark.
     * Drawn from primitives like every other icon here (§M64's argument: one
     * definition serves 24/48/64 px, there is no file to be missing, and an
     * icon cannot fail at runtime). */
    case ICON_VOLUME:
    case ICON_VOLUME_MUTED:
    case ICON_VOLUME_OFF: {
        uint32_t body = (id == ICON_VOLUME_OFF) ? C_GREY : C_BLUE;
        tile(s, x, y, n, body);
        /* speaker box */
        gfx_fill(s, x + 3 * u, y + 6 * u, 3 * u, 4 * u, C_WHITE);
        /* cone: widening steps */
        gfx_fill(s, x + 6 * u, y + 5 * u, u, 6 * u, C_WHITE);
        gfx_fill(s, x + 7 * u, y + 4 * u, u, 8 * u, C_WHITE);
        if (id == ICON_VOLUME) {
            /* two arcs = sound coming out */
            gfx_fill(s, x + 9 * u,  y + 5 * u, u, 6 * u, C_WHITE);
            gfx_fill(s, x + 11 * u, y + 3 * u, u, 10 * u, C_WHITE);
        } else if (id == ICON_VOLUME_MUTED) {
            /* a cross where the sound would be — the universal "off" mark */
            for (int i = 0; i < 5; i++) {
                gfx_fill(s, x + (9 + i) * u, y + (4 + i) * u, u, u, C_WHITE);
                gfx_fill(s, x + (13 - i) * u, y + (4 + i) * u, u, u, C_WHITE);
            }
        } else {
            /* unavailable: a bar through the whole glyph, and a dimmed body,
             * so it reads as "not present" rather than "switched off". */
            for (int i = 0; i < 12; i++)
                gfx_fill(s, x + (2 + i) * u, y + (2 + i) * u, u, u, C_RED);
        }
        break;
    }

    case ICON_INFO: {
        tile(s, x, y, n, C_TEAL);
        gfx_fill(s, x + 7 * u, y + 3 * u, 2 * u, 2 * u, C_WHITE);
        gfx_fill(s, x + 7 * u, y + 6 * u, 2 * u, 7 * u, C_WHITE);
        break;
    }

    case ICON_APP:
    default: {
        tile(s, x, y, n, C_GREY);
        /* A window: title bar + body, the most neutral thing an unknown
         * launchable can be. */
        gfx_fill(s, x + 3 * u, y + 4 * u, 10 * u, 8 * u, C_WHITE);
        gfx_fill(s, x + 3 * u, y + 4 * u, 10 * u, 2 * u, 0xFF3D6FB8u);
        break;
    }
    }
    (void)q;
}

/* ------------------------------------------------------------------- */
/* Names.  Shortcut files and config values are TEXT, so the mapping    */
/* has to exist somewhere; keeping it next to the drawing means a new   */
/* icon is one enum, one case and one row.                              */
/* ------------------------------------------------------------------- */

static const struct { const char* name; int id; } names[] = {
    { "app",       ICON_APP       },
    { "folder",    ICON_FOLDER    },
    { "doc",       ICON_DOC       },
    { "terminal",  ICON_TERMINAL  },
    { "settings",  ICON_SETTINGS  },
    { "display",   ICON_DISPLAY   },
    { "brush",     ICON_BRUSH     },
    { "globe",     ICON_GLOBE     },
    { "chart",     ICON_CHART     },
    { "package",   ICON_PACKAGE   },
    { "keyboard",  ICON_KEYBOARD  },
    { "clock",     ICON_CLOCK     },
    { "warn",      ICON_WARN      },
    { "code",      ICON_CODE      },
    { "info",      ICON_INFO      },
    { "volume",    ICON_VOLUME    },
    { "muted",     ICON_VOLUME_MUTED },
    { "noaudio",   ICON_VOLUME_OFF },
    { NULL, 0 }
};

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int icon_by_name(const char* name) {
    if (!name || !*name) return ICON_APP;
    for (int i = 0; names[i].name; i++)
        if (streq_(names[i].name, name)) return names[i].id;
    /* Unknown → the generic app icon, deliberately not "nothing": a shortcut
     * with a misspelt icon must still be visible and clickable, and an invisible
     * icon is a bug report nobody can describe. */
    return ICON_APP;
}

const char* icon_name(int id) {
    for (int i = 0; names[i].name; i++)
        if (names[i].id == id) return names[i].name;
    return "app";
}
