/* =============================================================================
 * irq_attach.c — hal_irq_attach on x86 (i386 and x86_64; see hal_api.h).
 *
 * irq_install hands its handler the trap frame, which a portable driver has no
 * business reading; this keeps a small table of plain callbacks per legacy
 * line and one trampoline that finds them by the frame's vector.  Several
 * callbacks per line, because PCI INTx lines are shared.
 * ============================================================================= */

#include "hal_api.h"
#include "idt.h"
#include <stddef.h>

#define IRQA_LINES 24
#define IRQA_PER    4

static void (*g_fn[IRQA_LINES][IRQA_PER])(void);

static void irqa_tramp(struct int_frame* f) {
    int line = (int)f->int_no - 32;
    if (line < 0 || line >= IRQA_LINES) return;
    for (int k = 0; k < IRQA_PER; k++)
        if (g_fn[line][k]) g_fn[line][k]();
}

int hal_irq_attach(int line, void (*fn)(void)) {
    if (line < 0 || line >= IRQA_LINES || !fn) return -1;
    int first = 1;
    for (int k = 0; k < IRQA_PER; k++) if (g_fn[line][k]) first = 0;
    for (int k = 0; k < IRQA_PER; k++) {
        if (g_fn[line][k]) continue;
        g_fn[line][k] = fn;
        if (first) irq_install(line, irqa_tramp);   /* chains with others */
        return 0;
    }
    return -1;
}
