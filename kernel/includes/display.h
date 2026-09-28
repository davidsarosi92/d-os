/* =============================================================================
 * display.h — the monitors attached to this machine (§M88).
 *
 * fb_present.h describes ONE framebuffer: the boot display, the console's and
 * the compositor's.  A second monitor is an OUTPUT: a size, a buffer the
 * compositor draws into, a way to push a rectangle of it to the screen, and a
 * POSITION in one virtual desktop that spans all outputs.
 *
 * Output 0 is the boot display, registered by the compositor from fb_present;
 * others are registered by their drivers (bochs_out.c) as they are found.  The
 * compositor composes the UNION of the outputs into one back buffer and, when
 * it presents, hands each output the part of each damaged rectangle that falls
 * on it — so windows, damage and the pointer live in ONE coordinate space and a
 * window may straddle two monitors.
 *
 * ARRANGEMENT is a setting (display.<name>.position = right | below | off),
 * applied by display_arrange().  The primary sits at (0,0); the others are
 * placed to its right or below it, in registration order.  Left and above are
 * deliberately absent in the first cut: they would move the primary off the
 * origin, and every coordinate the compositor has ever produced assumes the
 * primary's top-left is (0,0).
 * ============================================================================= */
#ifndef DOS_DISPLAY_H
#define DOS_DISPLAY_H

#include <stdint.h>

#define DISPLAY_MAX 4

struct display_output {
    char      name[16];          /* "primary", "bochs1"                        */
    int       w, h;              /* the mode, in pixels                        */
    uint32_t* px;                /* what the compositor draws into             */
    int       stride;            /* pixels per row of `px`                     */
    /* Push [x,y,w,h] (output coordinates) of `px` to the screen.  NULL when a
     * write to `px` is already visible (a live framebuffer). */
    void    (*flush)(struct display_output* o, int x, int y, int w, int h);
    int       x, y;              /* position in the virtual desktop            */
    int       primary;           /* output 0                                   */
    int       enabled;           /* position != off                            */
    void*     priv;
};

/* Register an output; returns its index or -1 (full / NULL). */
int  display_register(struct display_output* o);
int  display_count(void);
struct display_output* display_at(int i);

/* Place every output by its display.<name>.position setting and return how
 * many are enabled.  The primary is always enabled and at (0,0). */
int  display_arrange(void);

/* The bounding box of the enabled outputs (the virtual desktop). */
void display_union(int* w, int* h);

/* Which enabled output holds the point (x,y); -1 for none (a gap between
 * outputs of different sizes). */
int  display_at_point(int x, int y);

/* The rectangle of output `i` in desktop coordinates; 0 on success. */
int  display_rect(int i, int* x, int* y, int* w, int* h);

#endif
