/* =============================================================================
 * ps2_keyboard.c — PS/2 keyboard driver, IRQ-driven.
 *
 * On boot, `keyboard_init` registers `keyboard_irq` as the handler for
 * IRQ 1.  Every time the user presses or releases a key, the 8259 PIC
 * delivers vector 33 (= 32 + 1), the ISR machinery in idt.c funnels it
 * into `keyboard_irq`, which reads one scancode byte from port 0x60,
 * decodes it to ASCII, and pushes the result into a small ring buffer.
 *
 * `keyboard_getchar` is now a consumer for that buffer: when empty it
 * `hlt`s with interrupts enabled, so the CPU sleeps until the next IRQ
 * rather than spinning.
 *
 * Reference: Intel 8042 PS/2 controller datasheet, OSDev Wiki
 * "PS/2 Keyboard" for scancode set 1.
 * ============================================================================= */

#include "keyboard.h"
#include "hal.h"
#include "hal_api.h"
#include "idt.h"
#include "module.h"
#include "task.h"
#include "devfs.h"
#include "vc.h"
#include "keymap.h"
#include <stdint.h>
#include <stddef.h>

#define KBD_DATA     0x60
#define KBD_STATUS   0x64
#define KBD_OUT_FULL 0x01

/* PS/2 scancode set 1 → USB HID Usage ID (M16).
 *
 * From M16 onward the keyboard pipeline is two-stage:
 *
 *   PS/2 scancode  ──[this table]──►  HID Usage ID
 *                                            │
 *                                            ▼
 *                            keymap_translate(usage, modifiers)
 *                                            │
 *                                            ▼
 *                                       ASCII char
 *
 * This puts the layout-specific knowledge (US vs HU vs ...) in
 * `kernel/core/layouts.c`, not in each input driver.  We only carry
 * the hardware-specific scancode → usage mapping here.
 *
 * Set-1 scancodes are bytes 0x01..0x53 on the main keyboard; multi-byte
 * extended scancodes (0xE0 prefix) cover modifiers like RCtrl/RAlt and
 * the cursor cluster — we handle RAlt (0xE0 0x38) below since it
 * doubles as AltGr.
 *
 * Indices outside the populated range map to 0 ("unknown key", silently
 * dropped). */
static const uint8_t sc1_to_hid[128] = {
    [0x01] = KC_ESC,
    [0x02] = KC_1, [0x03] = 0x1F, [0x04] = 0x20, [0x05] = 0x21, [0x06] = 0x22,
    [0x07] = 0x23, [0x08] = 0x24, [0x09] = 0x25, [0x0A] = KC_9, [0x0B] = KC_0,
    [0x0C] = 0x2D /* - */, [0x0D] = 0x2E /* = */, [0x0E] = KC_BACKSPACE,
    [0x0F] = KC_TAB,
    [0x10] = 0x14 /* q */, [0x11] = 0x1A /* w */, [0x12] = 0x08 /* e */,
    [0x13] = 0x15 /* r */, [0x14] = 0x17 /* t */, [0x15] = 0x1C /* y */,
    [0x16] = 0x18 /* u */, [0x17] = 0x0C /* i */, [0x18] = 0x12 /* o */,
    [0x19] = 0x13 /* p */,
    [0x1A] = 0x2F /* [ */, [0x1B] = 0x30 /* ] */,
    [0x1C] = KC_ENTER,
    /* 0x1D = LCtrl — handled as modifier */
    [0x1E] = KC_A,           [0x1F] = 0x16 /* s */, [0x20] = 0x07 /* d */,
    [0x21] = 0x09 /* f */, [0x22] = 0x0A /* g */, [0x23] = 0x0B /* h */,
    [0x24] = 0x0D /* j */, [0x25] = 0x0E /* k */, [0x26] = 0x0F /* l */,
    [0x27] = 0x33 /* ; */, [0x28] = 0x34 /* ' */,
    [0x29] = 0x35 /* ` */,
    /* 0x2A = LShift — handled as modifier */
    [0x2B] = 0x31 /* \ */,
    [0x2C] = KC_Z,           [0x2D] = 0x1B /* x */, [0x2E] = 0x06 /* c */,
    [0x2F] = 0x19 /* v */, [0x30] = 0x05 /* b */, [0x31] = 0x11 /* n */,
    [0x32] = 0x10 /* m */, [0x33] = 0x36 /* , */, [0x34] = 0x37 /* . */,
    [0x35] = 0x38 /* / */,
    /* 0x36 = RShift — handled as modifier */
    /* 0x38 = LAlt — handled as modifier */
    [0x39] = KC_SPACE,
    [0x56] = KC_NONUS_BSLASH,
};

/* Modifier state.  Bits match keymap.h's KBD_MOD_* layout so we can
 * hand `mods` straight to keymap_translate without conversion. */
static uint8_t mods = 0;

/* RAlt (AltGr) comes in as the 2-byte sequence 0xE0 0x38.  Track that
 * we just saw the 0xE0 prefix so the next byte is interpreted as the
 * "extended" variant.  Same prefix marks RCtrl, cursor keys, keypad
 * Enter, etc. — for M16 we only care about RAlt. */
static int e0_pending = 0;

/* --------------------------------------------------------------------------
 * Input ring buffer shared between the ISR (producer) and the shell
 * (consumer).  Power-of-two size so (index mod SIZE) is a cheap `& MASK`
 * and head/tail comparisons are unambiguous.
 *
 * We declare `head` and `tail` volatile because one is written by the
 * IRQ and read by the main context, and vice versa — without volatile the
 * compiler is free to cache them in registers across the `hlt` loop.
 * -------------------------------------------------------------------------- */
#define KBD_BUF_SIZE 64
#define KBD_BUF_MASK (KBD_BUF_SIZE - 1)

static volatile char     kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0;   /* ISR writes here */
static volatile uint32_t kbd_tail = 0;   /* getchar reads from here */

/* Push one decoded ASCII byte into the ring.  If the buffer is full the
 * byte is silently dropped — we don't try to apply backpressure to the
 * hardware.  Called only from the IRQ context, so no lock is needed. */
static void kbd_push(char c) {
    uint32_t next = (kbd_head + 1) & KBD_BUF_MASK;
    if (next == kbd_tail) return;          /* buffer full — drop */
    kbd_buf[kbd_head] = c;
    kbd_head = next;
}

/* --------------------------------------------------------------------------
 * IRQ1 handler.  Runs with interrupts disabled, so access to ring buffer
 * indices is race-free relative to the consumer (getchar simply sees the
 * updated `kbd_head` the next time it's polled).
 * -------------------------------------------------------------------------- */
static void keyboard_irq(struct int_frame* f) {
    (void)f;                                /* we don't inspect CPU state */

    /* On some controllers a spurious IRQ may fire with no byte ready;
     * check the status bit instead of blindly reading. */
    if ((inb(KBD_STATUS) & KBD_OUT_FULL) == 0) return;

    uint8_t sc = inb(KBD_DATA);

    /* Extended (0xE0-prefixed) scancodes: stash the prefix and handle
     * the follow-up byte on the next IRQ.  We only care about RAlt
     * (0xE0 0x38 make / 0xE0 0xB8 break) — everything else gets
     * dropped via the e0_pending consume below. */
    if (sc == 0xE0) { e0_pending = 1; return; }
    int was_e0 = e0_pending;
    e0_pending = 0;

    /* Modifier handling — these MUST come before the break-bit check
     * because we care about both make AND break for modifiers. */
    if (was_e0) {
        /* Extended modifier set. */
        if (sc == 0x38) { mods |=  KBD_MOD_RALT;  return; }
        if (sc == 0xB8) { mods &= ~KBD_MOD_RALT;  return; }
        if (sc == 0x1D) { mods |=  KBD_MOD_RCTRL; return; }
        if (sc == 0x9D) { mods &= ~KBD_MOD_RCTRL; return; }
        /* All other extended keys: drop for M16 (cursor keys, etc). */
        return;
    }
    if (sc == 0x2A) { mods |=  KBD_MOD_LSHIFT; return; }
    if (sc == 0xAA) { mods &= ~KBD_MOD_LSHIFT; return; }
    if (sc == 0x36) { mods |=  KBD_MOD_RSHIFT; return; }
    if (sc == 0xB6) { mods &= ~KBD_MOD_RSHIFT; return; }
    if (sc == 0x1D) { mods |=  KBD_MOD_LCTRL;  return; }
    if (sc == 0x9D) { mods &= ~KBD_MOD_LCTRL;  return; }
    if (sc == 0x38) { mods |=  KBD_MOD_LALT;   return; }
    if (sc == 0xB8) { mods &= ~KBD_MOD_LALT;   return; }

    if (sc & 0x80) return;                                      /* other break */

    /* M14: LAlt+1..9 switches focus to the Nth VC.  Scancodes (set 1)
     * for the digit row: '1' = 0x02 .. '9' = 0x0A.  Intercept BEFORE
     * keymap_translate so they never reach the shell as literal digits.
     * Use LAlt specifically — RAlt is AltGr and should produce layout
     * symbols, not pane-switch. */
    if ((mods & KBD_MOD_LALT) && sc >= 0x02 && sc <= 0x0A) {
        int n = (int)sc - 0x02 + 1;
        vc_focus_by_id(n);
        return;
    }

    /* Hardware → universal keycode → ASCII via the active layout. */
    uint8_t keycode = sc1_to_hid[sc & 0x7F];
    if (!keycode) return;
    char c = keymap_translate(keycode, mods);
    if (!c) return;

    /* M14 routing: prefer the focused VC's ring; fall back to the
     * legacy central ring during early boot (before vc_init runs) so
     * keyboard_getchar in the kernel main path still works. */
    if (vc_focused()) vc_kbd_push(c);
    else              kbd_push(c);
}

/* --------------------------------------------------------------------------
 * Public API.
 * -------------------------------------------------------------------------- */
void keyboard_init(void) {
    /* Drain any stale byte the controller may already have queued so our
     * first real keypress isn't preceded by BIOS noise. */
    while (inb(KBD_STATUS) & KBD_OUT_FULL) (void)inb(KBD_DATA);

    irq_install(1, keyboard_irq);           /* unmasks IRQ1 on the PIC */
}

/* Block until a character is available in the ring.
 *
 * The `sti; hlt` pair is atomic on x86 — between the two instructions the
 * CPU cannot service a pending interrupt, so there is no window where we
 * could sleep past an already-delivered IRQ.  After the hlt wakes (because
 * an IRQ arrived), we re-check the buffer: it might still be empty if the
 * IRQ was for a different source, in which case we loop and sleep again. */
char keyboard_getchar(void) {
    for (;;) {
        if (kbd_head != kbd_tail) {
            char c = kbd_buf[kbd_tail];
            kbd_tail = (kbd_tail + 1) & KBD_BUF_MASK;
            return c;
        }
        /* Sleep until the next interrupt arrives, then offer the CPU
         * to any other RUNNABLE task before checking the buffer again.
         * This is how a parallel kernel task gets a slice of CPU while
         * the shell is at the prompt. */
        hal_cpu_idle();
        task_yield();
    }
}

/* -------------------------------------------------------------------------- */
/* devfs adapter — `cat /dev/keyboard` blocks reading keystrokes until EOF    */
/* (which we never send).  Each call returns up to `n` characters, blocking   */
/* until at least one is available.                                            */
/* -------------------------------------------------------------------------- */

static ssize_t kbd_devfs_read(void* ctx, void* buf, size_t n, uint64_t off) {
    (void)ctx; (void)off;
    if (n == 0) return 0;
    char* b = (char*)buf;
    /* Block for the first character, then drain whatever else is in the
     * ring without sleeping so a paste-style burst comes back as one read. */
    b[0] = keyboard_getchar();
    size_t i = 1;
    while (i < n && kbd_head != kbd_tail) {
        b[i++] = kbd_buf[kbd_tail];
        kbd_tail = (kbd_tail + 1) & KBD_BUF_MASK;
    }
    return (ssize_t)i;
}

static struct devfs_node kbd_devfs_node = {
    .name = "keyboard", .kind = DEVFS_CHAR,
    .read = kbd_devfs_read, .write = NULL, .ioctl = NULL, .ctx = NULL,
    ._next = NULL,
};

/* -------------------------------------------------------------------------- */
/* Module registration.                                                        */
/* Class is "input" so future input drivers (USB HID, serial console, ...)    */
/* coexist under the same umbrella.                                            */
/* -------------------------------------------------------------------------- */

static int kbd_module_init(void) {
    keyboard_init();
    devfs_register(&kbd_devfs_node);            /* queued; flushed by devfs_init */
    return 0;
}

MODULE("ps2-keyboard", "input", kbd_module_init);

