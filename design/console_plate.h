/* Console Plate — OS widget tokens, v0.1
 *
 * Logikai pixelek 1x skálán. HiDPI: a CP_SCALE makróval skálázd a
 * hosszméreteket (egész szám!), a színek nem skálázódnak.
 *
 * Szín formátum: 0xAARRGGBB (premultiply nélkül).
 * A tokenek forrása: tokens.json — a kettőt tartsd szinkronban.
 */
#ifndef CONSOLE_PLATE_H
#define CONSOLE_PLATE_H

#include <stdint.h>

#ifndef CP_SCALE
#define CP_SCALE 1
#endif
#define CP_PX(v) ((int)((v) * CP_SCALE))

typedef uint32_t cp_color; /* 0xAARRGGBB */

typedef struct {
    int dy, blur;
    cp_color color; /* alfát tartalmaz */
} cp_shadow;

typedef struct {
    cp_color bg;         /* asztal háttér */
    cp_color surface;    /* ablak / panel törzs */
    cp_color sunken;     /* input, lista, sáv-vályú */
    cp_color raised;     /* címsor, menü, dialógus, toast */
    cp_color tray;       /* tab-tálca, táblázat fejléc, scrollbar vályú */
    cp_color line;       /* 1px keret */
    cp_color line_soft;  /* sorválasztó, szeparátor */
    cp_color text;
    cp_color muted;      /* másodlagos szöveg, ikon, scrollbar hüvely */
    cp_color accent;
    cp_color on_accent;  /* szöveg/pipa az accent felületen */
    cp_color hover;
    cp_color press;
    cp_color switch_off;
    cp_color knob;
    cp_color sel_bg;     /* kijelölt listasor */
    cp_color sel_fg;
    cp_color focus;      /* fókuszgyűrű (alfás) */
    cp_shadow shadow_sm, shadow_md, shadow_lg;
} cp_theme;

/* --- DARK (alapértelmezett) --- */
static const cp_theme CP_DARK = {
    .bg = 0xFF16232F, .surface = 0xFF1D2D3D, .sunken = 0xFF16232F, .raised = 0xFF22374A,
    .tray = 0xFF1A2937, .line = 0xFF416180, .line_soft = 0xFF2C455D,
    .text = 0xFFEEF6FF, .muted = 0xFF94BCE3,
    .accent = 0xFF94BCE3, .on_accent = 0xFF16232F,
    .hover = 0xFF2C455D, .press = 0xFF416180,
    .switch_off = 0xFF416180, .knob = 0xFFFFFFFF,
    .sel_bg = 0xFF416180, .sel_fg = 0xFFEEF6FF,
    .focus = 0x8094BCE3,
    .shadow_sm = { 1,  2, 0x66000000 },
    .shadow_md = { 3, 10, 0x6B000000 },
    .shadow_lg = { 14, 34, 0x8C000000 }
};

/* --- LIGHT --- */
static const cp_theme CP_LIGHT = {
    .bg = 0xFFE7E7EA, .surface = 0xFFF5F5F8, .sunken = 0xFFFFFFFF, .raised = 0xFFFFFFFF,
    .tray = 0xFFE7E7EA, .line = 0xFFD4D4D7, .line_soft = 0xFFE7E7EA,
    .text = 0xFF1D1F20, .muted = 0xFF5D5D60,
    .accent = 0xFF5980A6, .on_accent = 0xFFFFFFFF,
    .hover = 0xFFEEF6FF, .press = 0xFFD6EBFF,
    .switch_off = 0xFFB7B7BA, .knob = 0xFFFFFFFF,
    .sel_bg = 0xFFD6EBFF, .sel_fg = 0xFF1D1F20,
    .focus = 0x735980A6,
    .shadow_sm = { 1,  2, 0x242B2B2D },
    .shadow_md = { 3, 10, 0x292B2B2D },
    .shadow_lg = { 12, 32, 0x382B2B2D }
};

/* --- sűrűség --- */
typedef struct {
    int control_h, control_pad_x, row_h, font_body, font_mono;
} cp_density;

static const cp_density CP_COMFORT = { 40, 18, 40, 15, 14 };
static const cp_density CP_COMPACT = { 32, 14, 32, 14, 13 };

/* --- geometria (logikai px) --- */
#define CP_BORDER               1
#define CP_RADIUS_OUTER         4   /* ablak, panel, gomb, input, tálca */
#define CP_RADIUS_INNER         3   /* tálcán belüli elem, ablakgomb, menüelem */

#define CP_CHECKBOX             20
#define CP_CHECKBOX_RADIUS      4
#define CP_RADIO                20
#define CP_RADIO_DOT            10  /* középre, accent szín */

#define CP_SWITCH_W             48
#define CP_SWITCH_H             26  /* radius = H/2 = 13 */
#define CP_SWITCH_KNOB          18
#define CP_SWITCH_INSET         3
#define CP_SWITCH_TRAVEL        22
#define CP_SWITCH_MS            120

#define CP_SLIDER_TRACK_H       6
#define CP_SLIDER_THUMB         22
#define CP_PROGRESS_H           8
#define CP_SPINNER              20
#define CP_SPINNER_STROKE       2
#define CP_SPINNER_MS           850
#define CP_INDETERMINATE_MS     1400

#define CP_SCROLLBAR_W          12
#define CP_SCROLLBAR_INSET      2
#define CP_SCROLLBAR_THUMB_MIN  28

#define CP_TITLEBAR_H           38
#define CP_PANEL_H              34
#define CP_TASKBAR_H            46
#define CP_WINDOW_BTN           26
#define CP_MENU_ITEM_H          34
#define CP_MENU_PAD             6
#define CP_TAB_H                32
#define CP_TAB_PAD_X            16
#define CP_TRAY_PAD             3
#define CP_TABLE_HEADER_H       30

#define CP_FOCUS_RING           2
#define CP_FOCUS_OFFSET         1
#define CP_MIN_TOUCH_TARGET     44  /* touch buildben minden kattintható elem */

/* --- tipográfia (px) --- */
#define CP_FONT_LABEL           11  /* Barlow Condensed 600, UPPERCASE, ls 0.14em */
#define CP_FONT_TITLE           22  /* Barlow Condensed 600, ls 0.02em */
#define CP_FONT_SECTION         26  /* Barlow Condensed 600, UPPERCASE, ls 0.04em */
#define CP_LINE_HEIGHT_BODY     145 /* százalék */

#endif /* CONSOLE_PLATE_H */
