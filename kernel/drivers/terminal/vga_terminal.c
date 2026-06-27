/* =============================================================================
 * vga_terminal.c — VGA text-mode console driver (fallback path).
 *
 * Used when the bootloader leaves us in text mode (no framebuffer available).
 * The kernel normally runs with a framebuffer terminal (fb_terminal.c) but
 * this driver stays in the tree as a safety net for loaders that ignore our
 * multiboot video request.
 *
 * Hardware: IBM VGA text mode, 80×25 cells, 2 bytes per cell at physical
 * 0xB8000.  Byte 0 = ASCII, byte 1 = attribute (low nibble FG, high BG).
 * Public entry points are `vga_term_*`; the top-level dispatcher in
 * terminal.c decides which driver gets called.
 * ============================================================================= */

#include "console.h"
#include "module.h"
#include <stdint.h>
#include <stddef.h>

#define VGA_MEMORY ((volatile uint16_t*)0xB8000)
#define VGA_COLS   80
#define VGA_ROWS   25
#define VGA_ATTR   0x07                         /* light grey on black */

static int row = 0;
static int col = 0;

static uint16_t vga_entry(char c) {
    return (uint16_t)(uint8_t)c | ((uint16_t)VGA_ATTR << 8);
}

void vga_term_clear(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) VGA_MEMORY[i] = vga_entry(' ');
    row = 0;
    col = 0;
}

void vga_term_init(void) {
    vga_term_clear();
}

static void scroll_if_needed(void) {
    if (row < VGA_ROWS) return;

    for (int r = 1; r < VGA_ROWS; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            VGA_MEMORY[(r - 1) * VGA_COLS + c] = VGA_MEMORY[r * VGA_COLS + c];
        }
    }
    for (int c = 0; c < VGA_COLS; c++) {
        VGA_MEMORY[(VGA_ROWS - 1) * VGA_COLS + c] = vga_entry(' ');
    }
    row = VGA_ROWS - 1;
}

void vga_term_putchar(char c) {
    if (c == '\n') { col = 0; row++; scroll_if_needed(); return; }
    if (c == '\r') { col = 0; return; }
    if (c == '\b') {
        if (col > 0) {
            col--;
            VGA_MEMORY[row * VGA_COLS + col] = vga_entry(' ');
        }
        return;
    }

    VGA_MEMORY[row * VGA_COLS + col] = vga_entry(c);
    col++;
    if (col >= VGA_COLS) { col = 0; row++; scroll_if_needed(); }
}

void vga_term_write(const char* str) {
    while (*str) vga_term_putchar(*str++);
}

/* -------------------------------------------------------------------------- */
/* Module registration.  Category "screen" — same as FB; only one of the two  */
/* should be active at a time.  This module runs second (link order: see      */
/* Makefile C_SRCS) so by the time we get here, the FB sink already           */
/* attempted to activate.  We defer if FB took the slot.                      */
/* -------------------------------------------------------------------------- */

static struct console_sink vga_sink = {
    .name     = "vga-text",
    .category = "screen",
    .putchar  = vga_term_putchar,
    .clear    = vga_term_clear,
    .active   = 0,
    .next     = NULL,
};

static int vga_module_init(void) {
    console_sink_register(&vga_sink);
    if (!console_sink_any_active("screen")) {
        vga_term_init();
        vga_sink.active = 1;
    }
    return 0;
}

MODULE("vga-text", "console", vga_module_init);

