/* =============================================================================
 * printf.c — kprintf(), a very small formatter for kernel diagnostics.
 *
 * No libc is available, so we implement enough of printf to be useful for
 * boot logs and debugging: decimal, unsigned decimal, lowercase hex, string,
 * char, pointer, and the literal percent sign.
 *
 * Every byte is broadcast through `console_putchar` (kernel/core/console.c)
 * to whichever sinks have registered as active — typically the
 * framebuffer terminal AND the serial debug port simultaneously.  No
 * sinks active means the bytes go nowhere; that's fine for the brief
 * window between boot entry and the first sink activating.
 *
 * Freestanding C gives us <stdarg.h> even without a libc, so variadic
 * argument walking is available as normal.
 *
 * Intentional simplifications:
 *   - No width, precision, padding, sign, or %.Nf.
 *   - 32-bit arguments only (%d/%u/%x take int / unsigned int).
 *   - Not reentrant; fine because the kernel is single-threaded today.
 * =========================================================================== */

#include "printf.h"
#include "console.h"
#include <stdarg.h>
#include <stdint.h>

/* All output goes through the console sink registry.  Each registered +
 * active sink receives every byte; this is how a single kprintf can land
 * on the framebuffer AND on the serial debug port simultaneously.  See
 * kernel/core/console.c. */
static void emit(char c) {
    console_putchar(c);
}

/* Write a non-negative decimal integer.  Build digits right-to-left into
 * a buffer, then flush left-to-right. */
static void put_dec(unsigned int v) {
    char buf[16];
    int n = 0;
    if (v == 0) { emit('0'); return; }
    while (v) { buf[n++] = '0' + (v % 10); v /= 10; }
    while (n--) emit(buf[n]);
}

/* Write a lowercase hexadecimal integer.  `min_digits` is the minimum
 * number of digits to emit — useful for pointer formatting (%p). */
static void put_hex(unsigned int v, int min_digits) {
    static const char hex[] = "0123456789abcdef";
    char buf[16];
    int n = 0;
    if (v == 0 && min_digits == 0) { emit('0'); return; }
    while (v || n < min_digits) { buf[n++] = hex[v & 0xF]; v >>= 4; }
    while (n--) emit(buf[n]);
}

void kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(*fmt);
            continue;
        }
        fmt++;                                  /* step past '%' onto the specifier */
        switch (*fmt) {
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s) emit(*s++);
                break;
            }
            case 'c':
                emit((char)va_arg(ap, int));
                break;
            case 'd': {
                int v = va_arg(ap, int);
                if (v < 0) { emit('-'); put_dec((unsigned)-v); }
                else       put_dec((unsigned)v);
                break;
            }
            case 'u':
                put_dec(va_arg(ap, unsigned int));
                break;
            case 'x':
                put_hex(va_arg(ap, unsigned int), 0);
                break;
            case 'p':
                /* Print pointer as 0x + 8 hex digits so addresses line up
                 * visually in boot logs and diagnostic dumps. */
                emit('0');
                emit('x');
                put_hex((uintptr_t)va_arg(ap, void*), 8);
                break;
            case '%':
                emit('%');
                break;
            case 0:
                /* Trailing lone '%' — bail out cleanly. */
                va_end(ap);
                return;
            default:
                /* Unknown specifier: echo it verbatim so bugs are visible. */
                emit('%');
                emit(*fmt);
                break;
        }
    }

    va_end(ap);
}
