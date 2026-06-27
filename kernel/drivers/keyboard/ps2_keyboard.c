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
#include "idt.h"
#include "module.h"
#include "task.h"
#include "devfs.h"
#include "vc.h"
#include <stdint.h>
#include <stddef.h>

#define KBD_DATA     0x60
#define KBD_STATUS   0x64
#define KBD_OUT_FULL 0x01

/* Same scancode-set-1 translation tables as the old polled driver.  Moved
 * here verbatim; interpretation happens now inside the IRQ handler. */
static const char sc_lower[128] = {
    /* 0x00-0x0E */  0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    /* 0x0F-0x1C */ '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    /* 0x1D-0x29 */  0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`',
    /* 0x2A-0x35 */  0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    /* 0x36-0x39 */  0,   '*', 0,   ' ',
};

static const char sc_upper[128] = {
    /* 0x00-0x0E */  0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    /* 0x0F-0x1C */ '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    /* 0x1D-0x29 */  0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    /* 0x2A-0x35 */  0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    /* 0x36-0x39 */  0,   '*', 0,   ' ',
};

static int shift_down = 0;
/* M14: Alt modifier — Alt+1..9 switches focus to the Nth VC instead of
 * producing a character.  Alt scancode (set 1): 0x38 make / 0xB8 break. */
static int alt_down = 0;

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

    if (sc == 0x2A || sc == 0x36) { shift_down = 1; return; }   /* shift make */
    if (sc == 0xAA || sc == 0xB6) { shift_down = 0; return; }   /* shift break */
    if (sc == 0x38)               { alt_down   = 1; return; }   /* Alt make  */
    if (sc == 0xB8)               { alt_down   = 0; return; }   /* Alt break */
    if (sc & 0x80) return;                                      /* any other break */

    /* M14: Alt+1..9 switches focus to the Nth VC.  Scancodes (set 1)
     * for the digit row: '1' = 0x02 .. '9' = 0x0A.  We intercept these
     * BEFORE the translation table so they never reach the shell as
     * literal digits while Alt is held. */
    if (alt_down && sc >= 0x02 && sc <= 0x0A) {
        int n = (int)sc - 0x02 + 1;
        vc_focus_by_id(n);
        return;
    }

    char c = shift_down ? sc_upper[sc] : sc_lower[sc];
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
        __asm__ volatile ("sti; hlt");
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

