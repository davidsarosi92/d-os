/* =============================================================================
 * fb_terminal.c — linear-framebuffer console with an 8x8 bitmap font.
 *
 * Activated when the bootloader hands us a framebuffer through multiboot
 * (fields `framebuffer_*` in mboot_info, flag bit 12).  We support the
 * common "RGB direct-color" layout at 32 bpp — the only one GRUB yields
 * under QEMU with our current header.  Other layouts (paletted / RGB565)
 * are rejected at init and we fall back to VGA text mode.
 *
 * Character cells are GLYPH_W × GLYPH_H pixels.  On a 1024×768 display
 * that yields a 128 × 96 character grid — about 5× the row count of
 * classic 80×25 VGA text, so there is room both for output and for
 * future multi-pane UIs.
 *
 * Scrolling is a single memmove of (rows-1)*pitch bytes.  On a 1024×768×32
 * display that is ~3 MiB per scroll — perceptible but acceptable for a
 * simple terminal; later we can add a scrollback ring or hardware
 * acceleration.
 *
 * Font: embedded 8×8 bitmap, derived from the classic public-domain IBM
 * PC CGA/VGA ROM font.  Each glyph is 8 bytes; each byte is one horizontal
 * row; within a byte, the MSB is the leftmost pixel.  ASCII 0x00–0x1F
 * are left as zeros (control codes don't render); 0x20–0x7E are the
 * printable range; 0x7F renders as a solid block used as the fallback
 * glyph for out-of-range characters.
 * ============================================================================= */

#include "multiboot.h"
#include "vmm.h"
#include "printf.h"
#include "console.h"
#include "module.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Framebuffer state.                                                         */
/* -------------------------------------------------------------------------- */

static volatile uint32_t* fb_pixels = 0;        /* pointer into the mapped FB */
static uint32_t fb_width  = 0;                  /* pixels per row */
static uint32_t fb_height = 0;                  /* pixel rows */
static uint32_t fb_pitch_bytes = 0;             /* bytes per pixel row */

#define GLYPH_W 8
#define GLYPH_H 8

/* Cell grid derived from the framebuffer dimensions at init. */
static uint32_t cell_cols = 0;
static uint32_t cell_rows = 0;
static uint32_t cur_col = 0;
static uint32_t cur_row = 0;

/* Colors.  Packed as 0x00RRGGBB for our 32-bpp RGB target. */
#define FG_COLOR  0xFFE0E0E0u                   /* bright grey */
#define BG_COLOR  0xFF101828u                   /* dark navy-ish black */

/* -------------------------------------------------------------------------- */
/* Font data — 128 × 8 bytes.  Rows laid out top-to-bottom, MSB = left pixel. */
/* Glyphs derived from the classic CP437 8×8 ROM font (public domain).        */
/* -------------------------------------------------------------------------- */
static const uint8_t font8x8[128][8] = {
    /* 0x00-0x1F control codes: all zero glyphs */
    [' ']  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!']  = {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    ['"']  = {0x6C,0x6C,0x48,0x00,0x00,0x00,0x00,0x00},
    ['#']  = {0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x00},
    ['$']  = {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00},
    ['%']  = {0xC6,0xCC,0x18,0x30,0x60,0xC6,0x86,0x00},
    ['&']  = {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
    ['\''] = {0x18,0x18,0x10,0x00,0x00,0x00,0x00,0x00},
    ['(']  = {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    [')']  = {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    ['*']  = {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    ['+']  = {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    [',']  = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    ['-']  = {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    ['.']  = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    ['/']  = {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    ['0']  = {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00},
    ['1']  = {0x30,0x70,0x30,0x30,0x30,0x30,0xFC,0x00},
    ['2']  = {0x78,0xCC,0x0C,0x38,0x60,0xCC,0xFC,0x00},
    ['3']  = {0x78,0xCC,0x0C,0x38,0x0C,0xCC,0x78,0x00},
    ['4']  = {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00},
    ['5']  = {0xFC,0xC0,0xF8,0x0C,0x0C,0xCC,0x78,0x00},
    ['6']  = {0x38,0x60,0xC0,0xF8,0xCC,0xCC,0x78,0x00},
    ['7']  = {0xFC,0xCC,0x0C,0x18,0x30,0x30,0x30,0x00},
    ['8']  = {0x78,0xCC,0xCC,0x78,0xCC,0xCC,0x78,0x00},
    ['9']  = {0x78,0xCC,0xCC,0x7C,0x0C,0x18,0x70,0x00},
    [':']  = {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    [';']  = {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    ['<']  = {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},
    ['=']  = {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00},
    ['>']  = {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    ['?']  = {0x78,0xCC,0x0C,0x18,0x30,0x00,0x30,0x00},
    ['@']  = {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00},
    ['A']  = {0x30,0x78,0xCC,0xCC,0xFC,0xCC,0xCC,0x00},
    ['B']  = {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
    ['C']  = {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
    ['D']  = {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
    ['E']  = {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
    ['F']  = {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
    ['G']  = {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00},
    ['H']  = {0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0x00},
    ['I']  = {0x78,0x30,0x30,0x30,0x30,0x30,0x78,0x00},
    ['J']  = {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
    ['K']  = {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
    ['L']  = {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
    ['M']  = {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00},
    ['N']  = {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
    ['O']  = {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
    ['P']  = {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    ['Q']  = {0x78,0xCC,0xCC,0xCC,0xDC,0x78,0x1C,0x00},
    ['R']  = {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
    ['S']  = {0x78,0xCC,0xE0,0x70,0x1C,0xCC,0x78,0x00},
    ['T']  = {0xFC,0xB4,0x30,0x30,0x30,0x30,0x78,0x00},
    ['U']  = {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xFC,0x00},
    ['V']  = {0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00},
    ['W']  = {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},
    ['X']  = {0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00},
    ['Y']  = {0xCC,0xCC,0xCC,0x78,0x30,0x30,0x78,0x00},
    ['Z']  = {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
    ['[']  = {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    ['\\'] = {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},
    [']']  = {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    ['^']  = {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    ['_']  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    ['`']  = {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00},
    ['a']  = {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},
    ['b']  = {0xE0,0x60,0x60,0x7C,0x66,0x66,0xDC,0x00},
    ['c']  = {0x00,0x00,0x78,0xCC,0xC0,0xCC,0x78,0x00},
    ['d']  = {0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0x76,0x00},
    ['e']  = {0x00,0x00,0x78,0xCC,0xFC,0xC0,0x78,0x00},
    ['f']  = {0x38,0x6C,0x60,0xF0,0x60,0x60,0xF0,0x00},
    ['g']  = {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8},
    ['h']  = {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},
    ['i']  = {0x30,0x00,0x70,0x30,0x30,0x30,0x78,0x00},
    ['j']  = {0x0C,0x00,0x0C,0x0C,0x0C,0xCC,0xCC,0x78},
    ['k']  = {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},
    ['l']  = {0x70,0x30,0x30,0x30,0x30,0x30,0x78,0x00},
    ['m']  = {0x00,0x00,0xCC,0xFE,0xFE,0xD6,0xC6,0x00},
    ['n']  = {0x00,0x00,0xF8,0xCC,0xCC,0xCC,0xCC,0x00},
    ['o']  = {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x78,0x00},
    ['p']  = {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},
    ['q']  = {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},
    ['r']  = {0x00,0x00,0xDC,0x76,0x66,0x60,0xF0,0x00},
    ['s']  = {0x00,0x00,0x7C,0xC0,0x78,0x0C,0xF8,0x00},
    ['t']  = {0x10,0x30,0x7C,0x30,0x30,0x34,0x18,0x00},
    ['u']  = {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
    ['v']  = {0x00,0x00,0xCC,0xCC,0xCC,0x78,0x30,0x00},
    ['w']  = {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00},
    ['x']  = {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},
    ['y']  = {0x00,0x00,0xCC,0xCC,0xCC,0x7C,0x0C,0xF8},
    ['z']  = {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00},
    ['{']  = {0x1C,0x30,0x30,0xE0,0x30,0x30,0x1C,0x00},
    ['|']  = {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    ['}']  = {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    ['~']  = {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x7F] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},     /* fallback block */
};

/* -------------------------------------------------------------------------- */
/* Pixel-level helpers.                                                       */
/* -------------------------------------------------------------------------- */

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    /* `fb_pitch_bytes / 4` = pixels per scanline for 32-bpp.  Our caller
     * guarantees x < fb_width and y < fb_height, so no range check here. */
    fb_pixels[y * (fb_pitch_bytes / 4) + x] = color;
}

/* Paint the entire framebuffer with the background color and home the
 * cursor to the top-left cell. */
static void fb_fill_bg(void) {
    uint32_t stride = fb_pitch_bytes / 4;
    for (uint32_t y = 0; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) fb_pixels[y * stride + x] = BG_COLOR;
    }
}

/* Render one glyph at cell (col, row).  Characters outside 0..127 land on
 * the fallback glyph at 0x7F so nothing disappears silently. */
static void draw_glyph(unsigned char ch, uint32_t col, uint32_t row) {
    const uint8_t* g = font8x8[ch < 128 ? ch : 0x7F];
    uint32_t ox = col * GLYPH_W;
    uint32_t oy = row * GLYPH_H;
    for (int py = 0; py < GLYPH_H; py++) {
        uint8_t bits = g[py];
        for (int px = 0; px < GLYPH_W; px++) {
            uint32_t color = (bits & (0x80u >> px)) ? FG_COLOR : BG_COLOR;
            put_pixel(ox + px, oy + py, color);
        }
    }
}

/* Shift the framebuffer up by one character row and clear the new bottom
 * row.  Cheaper than re-drawing every cell. */
static void scroll_one_row(void) {
    uint32_t stride = fb_pitch_bytes / 4;
    uint32_t src_offset_px = GLYPH_H * stride;

    /* Move `(fb_height - GLYPH_H)` pixel rows up by GLYPH_H.  A plain loop
     * is enough — memmove isn't available and the volatile on fb_pixels
     * keeps the compiler from reordering the copy. */
    for (uint32_t y = 0; y < fb_height - GLYPH_H; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            fb_pixels[y * stride + x] = fb_pixels[y * stride + x + src_offset_px];
        }
    }
    /* Clear the now-exposed bottom band. */
    for (uint32_t y = fb_height - GLYPH_H; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) fb_pixels[y * stride + x] = BG_COLOR;
    }
}

/* -------------------------------------------------------------------------- */
/* Rect-aware primitives (M14).                                               */
/*                                                                            */
/* The original `fb_term_putchar` operates on a single global cursor that     */
/* spans the entire screen.  M14 carves the screen into VC panes, each       */
/* with its own rect and cursor.  The functions below let vc.c render and    */
/* scroll inside an arbitrary cell rect without disturbing other panes.      */
/*                                                                            */
/* Coordinates are in CELLS, not pixels.  (col, row) starts at (0,0).        */
/* The legacy `fb_term_putchar` API still works and is used by the boot      */
/* log; once vc.c is up, the legacy fb_sink is deactivated so kprintf no    */
/* longer wanders across pane boundaries.                                    */
/* -------------------------------------------------------------------------- */

int fb_cell_cols(void) { return (int)cell_cols; }
int fb_cell_rows(void) { return (int)cell_rows; }

/* Paint a cell rect (col,row)..(col+w-1,row+h-1) with `bg` color. */
void fb_clear_cells(int col, int row, int w, int h, uint32_t bg) {
    if (!fb_pixels) return;
    if (col < 0 || row < 0) return;
    if (col + w > (int)cell_cols) w = cell_cols - col;
    if (row + h > (int)cell_rows) h = cell_rows - row;
    if (w <= 0 || h <= 0) return;

    uint32_t stride = fb_pitch_bytes / 4;
    uint32_t px0 = col * GLYPH_W;
    uint32_t py0 = row * GLYPH_H;
    uint32_t pxN = px0 + w * GLYPH_W;
    uint32_t pyN = py0 + h * GLYPH_H;
    for (uint32_t y = py0; y < pyN; y++) {
        for (uint32_t x = px0; x < pxN; x++) fb_pixels[y * stride + x] = bg;
    }
}

/* Render glyph `ch` at cell (col,row) using fg/bg colors. */
void fb_draw_glyph_at(int col, int row, char ch, uint32_t fg, uint32_t bg) {
    if (!fb_pixels) return;
    if (col < 0 || row < 0 || col >= (int)cell_cols || row >= (int)cell_rows) return;
    const uint8_t* g = font8x8[(unsigned char)ch < 128 ? (unsigned char)ch : 0x7F];
    uint32_t ox = col * GLYPH_W;
    uint32_t oy = row * GLYPH_H;
    for (int py = 0; py < GLYPH_H; py++) {
        uint8_t bits = g[py];
        for (int px = 0; px < GLYPH_W; px++) {
            uint32_t color = (bits & (0x80u >> px)) ? fg : bg;
            put_pixel(ox + px, oy + py, color);
        }
    }
}

/* Scroll a cell rect up by ONE cell row.  The bottom cell row is cleared
 * to `bg`.  Implemented by moving pixel rows up by GLYPH_H pixels and
 * blanking the freed band. */
void fb_scroll_cells_up(int col, int row, int w, int h, uint32_t bg) {
    if (!fb_pixels) return;
    if (col < 0 || row < 0) return;
    if (col + w > (int)cell_cols) w = cell_cols - col;
    if (row + h > (int)cell_rows) h = cell_rows - row;
    if (w <= 0 || h <= 1) return;

    uint32_t stride = fb_pitch_bytes / 4;
    uint32_t px0    = col * GLYPH_W;
    uint32_t pxN    = px0 + w * GLYPH_W;
    uint32_t py0    = row * GLYPH_H;
    uint32_t pyN    = py0 + h * GLYPH_H;
    uint32_t src_lift = GLYPH_H * stride;

    /* Move pixel rows up by GLYPH_H within the rect.  Top-to-bottom
     * iteration is safe because src is below dst in the FB. */
    for (uint32_t y = py0; y < pyN - GLYPH_H; y++) {
        for (uint32_t x = px0; x < pxN; x++) {
            fb_pixels[y * stride + x] = fb_pixels[y * stride + x + src_lift];
        }
    }
    /* Clear the newly exposed bottom band. */
    for (uint32_t y = pyN - GLYPH_H; y < pyN; y++) {
        for (uint32_t x = px0; x < pxN; x++) fb_pixels[y * stride + x] = bg;
    }
}

/* -------------------------------------------------------------------------- */
/* M22 exports — the GUI's gfx layer needs raw framebuffer access and the    */
/* embedded font.  Kept as plain accessors so the dependency stays one-way    */
/* (this driver knows nothing about surfaces or windows).                     */
/* -------------------------------------------------------------------------- */

/* Fill out the framebuffer geometry.  Returns 0 on success, -1 if no
 * usable 32-bpp FB was mapped (VGA text fallback boot). */
int fb_get_info(volatile uint32_t** px, uint32_t* w, uint32_t* h,
                uint32_t* pitch_bytes) {
    if (!fb_pixels) return -1;
    if (px)          *px          = fb_pixels;
    if (w)           *w           = fb_width;
    if (h)           *h           = fb_height;
    if (pitch_bytes) *pitch_bytes = fb_pitch_bytes;
    return 0;
}

/* One glyph = 8 bytes, row-major, MSB = leftmost pixel.  Out-of-range
 * characters land on the 0x7F fallback block, same as draw_glyph. */
const uint8_t* fb_font_glyph(unsigned char ch) {
    return font8x8[ch < 128 ? ch : 0x7F];
}

/* -------------------------------------------------------------------------- */
/* Public entry points (legacy whole-screen API).                             */
/* -------------------------------------------------------------------------- */

/* Attempt to stand up the framebuffer terminal.  Returns 0 on success,
 * non-zero if the bootloader didn't give us a usable RGB 32-bpp FB or we
 * couldn't map it into our address space.  On success the caller (the
 * dispatcher in terminal.c) should start routing output to fb_term_*. */
int fb_term_init_if_available(const struct mboot_info* mbi) {
    if (!mbi) return -1;
    if ((mbi->flags & MBI_FLAG_FB) == 0) return -2;
    if (mbi->framebuffer_type != 1) return -3;          /* 1 = RGB direct */
    if (mbi->framebuffer_bpp != 32)   return -4;

    /* Physical FB base.  Truncate the 64-bit address to 32-bit — it always
     * fits on i386 (QEMU puts it at 0xFD000000 on our setup). */
    uint32_t fb_phys = (uint32_t)mbi->framebuffer_addr;
    uint32_t fb_size = (uint32_t)mbi->framebuffer_pitch * mbi->framebuffer_height;

    /* Identity-map the FB window with 4 MiB PSE PDEs.  The region is
     * usually a few MiB and aligned to 4 MiB, so one or two PDEs cover it. */
    uint32_t base_aligned = fb_phys & 0xFFC00000u;
    uint32_t end          = fb_phys + fb_size;
    for (uint32_t a = base_aligned; a < end; a += 0x00400000u) {
        int r = vmm_map_4mib(a, a, VMM_WRITABLE);
        /* r == -2 means a regular PT is already there — skip and hope the
         * mapping is already present (will be the case if FB overlaps the
         * initial 256 MiB identity range, which it doesn't on QEMU). */
        if (r != 0 && r != -2) {
            kprintf("fb: vmm_map_4mib(%p) failed: %d\n", (void*)a, r);
            return -5;
        }
    }

    fb_pixels      = (volatile uint32_t*)(uintptr_t)fb_phys;
    fb_width       = mbi->framebuffer_width;
    fb_height      = mbi->framebuffer_height;
    fb_pitch_bytes = mbi->framebuffer_pitch;

    cell_cols = fb_width  / GLYPH_W;
    cell_rows = fb_height / GLYPH_H;
    cur_col   = 0;
    cur_row   = 0;

    fb_fill_bg();

    kprintf("fb: %dx%d@%dbpp mapped at %p, grid %dx%d cells\n",
            fb_width, fb_height, mbi->framebuffer_bpp, (void*)fb_phys,
            cell_cols, cell_rows);
    return 0;
}

void fb_term_clear(void) {
    fb_fill_bg();
    cur_col = 0;
    cur_row = 0;
}

void fb_term_putchar(char c) {
    if (!fb_pixels) return;                             /* never initialized */

    if (c == '\n') {
        cur_col = 0;
        cur_row++;
        if (cur_row >= cell_rows) { scroll_one_row(); cur_row = cell_rows - 1; }
        return;
    }
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            draw_glyph(' ', cur_col, cur_row);
        }
        return;
    }

    draw_glyph((unsigned char)c, cur_col, cur_row);
    cur_col++;
    if (cur_col >= cell_cols) {
        cur_col = 0;
        cur_row++;
        if (cur_row >= cell_rows) { scroll_one_row(); cur_row = cell_rows - 1; }
    }
}

void fb_term_write(const char* s) {
    while (*s) fb_term_putchar(*s++);
}

/* -------------------------------------------------------------------------- */
/* Module registration.  Category "screen" — mutually exclusive with VGA.     */
/* The sink starts inactive; module init flips it on only if the FB probe     */
/* succeeds.  VGA's module init checks for any active "screen" sink and       */
/* defers if it sees one.                                                     */
/* -------------------------------------------------------------------------- */

static struct console_sink fb_sink = {
    .name     = "fb",
    .category = "screen",
    .putchar  = fb_term_putchar,
    .clear    = fb_term_clear,
    .active   = 0,
    .next     = NULL,
};

static int fb_module_init(void) {
    /* Always register so it shows up in `lsmod`/console_list, even if
     * the bootloader didn't give us a usable framebuffer.  The active
     * flag controls whether output actually flows. */
    console_sink_register(&fb_sink);
    if (fb_term_init_if_available(mboot_get_info()) == 0) {
        fb_sink.active = 1;
    }
    return 0;
}

/* M14: vc_init() calls this to take over the screen.  After this point
 * kprintf no longer wanders across the whole FB — per-task routing in
 * console_putchar sends each shell-task's output into its bound VC. */
void fb_sink_disable(void) {
    fb_sink.active = 0;
}

MODULE("vesa-fb", "console", fb_module_init);

