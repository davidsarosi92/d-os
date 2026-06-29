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
 * Length modifier support (M20.5 Phase A):
 *   - `l`   = long           (`%lx`, `%lu`, `%ld`)
 *   - `ll`  = long long      (`%llx`, `%llu`, `%lld`)
 *   - `z`   = size_t         (`%zx`, `%zu`, `%zd`)
 *
 * On i386 long is 32-bit; on x86_64 long is 64-bit.  `unsigned long long`
 * is 64-bit on both.  Argument fetch sizes itself accordingly via
 * va_arg's matching type.  Pointer width (`%p`) is uintptr_t — 8 hex
 * digits on i386, 16 on x86_64 — so addresses line up regardless of
 * arch.
 *
 * Intentional simplifications:
 *   - No width, precision, padding, sign, or %.Nf.
 *   - Not reentrant; fine because the kernel is single-threaded today.
 * =========================================================================== */

#include "printf.h"
#include "console.h"
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

/* All output goes through the console sink registry.  Each registered +
 * active sink receives every byte; this is how a single kprintf can land
 * on the framebuffer AND on the serial debug port simultaneously.  See
 * kernel/core/console.c. */
static void emit(char c) {
    console_putchar(c);
}

/* Write a non-negative decimal integer.  Build digits right-to-left into
 * a buffer, then flush left-to-right.  Buffer sized for the worst case
 * of a 64-bit unsigned value (20 decimal digits). */
static void put_dec(uint64_t v) {
    char buf[24];
    int n = 0;
    if (v == 0) { emit('0'); return; }
    while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) emit(buf[n]);
}

/* Write a lowercase hexadecimal integer.  `min_digits` is the minimum
 * number of digits to emit — useful for pointer formatting (%p) so
 * addresses line up.  Buffer sized for the worst case of 16 hex
 * digits (64-bit value). */
static void put_hex(uint64_t v, int min_digits) {
    static const char hex[] = "0123456789abcdef";
    char buf[24];
    int n = 0;
    if (v == 0 && min_digits == 0) { emit('0'); return; }
    while (v || n < min_digits) { buf[n++] = hex[v & 0xF]; v >>= 4; }
    while (n--) emit(buf[n]);
}

/* Length-modifier enum.  Encodes how wide the matching va_arg fetch
 * should be (and, for signed decimals, how to interpret the sign). */
enum length_mod {
    LM_DEFAULT,        /* int / unsigned int (or char-promoted-to-int) */
    LM_LONG,           /* long / unsigned long */
    LM_LONGLONG,       /* long long / unsigned long long */
    LM_SIZE_T,         /* size_t / ssize_t */
};

/* Fetch the next unsigned argument matching `lm` from `ap`.  We always
 * widen to uint64_t internally so put_dec/put_hex have a single code
 * path; va_arg takes the right-sized slot off the stack regardless. */
static uint64_t fetch_unsigned(va_list* ap, enum length_mod lm) {
    switch (lm) {
        case LM_DEFAULT:  return (uint64_t)va_arg(*ap, unsigned int);
        case LM_LONG:     return (uint64_t)va_arg(*ap, unsigned long);
        case LM_LONGLONG: return (uint64_t)va_arg(*ap, unsigned long long);
        case LM_SIZE_T:   return (uint64_t)va_arg(*ap, size_t);
    }
    return 0;
}

/* Fetch the next signed argument matching `lm` from `ap`, sign-extending
 * to int64_t.  Mirrors fetch_unsigned. */
static int64_t fetch_signed(va_list* ap, enum length_mod lm) {
    switch (lm) {
        case LM_DEFAULT:  return (int64_t)va_arg(*ap, int);
        case LM_LONG:     return (int64_t)va_arg(*ap, long);
        case LM_LONGLONG: return (int64_t)va_arg(*ap, long long);
        case LM_SIZE_T:   return (int64_t)va_arg(*ap, long);   /* ssize_t == long */
    }
    return 0;
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

        /* Parse optional length modifier (`l`, `ll`, `z`).  No state
         * machine — just look ahead once.  Anything we don't recognise
         * falls through to the default-width path. */
        enum length_mod lm = LM_DEFAULT;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { lm = LM_LONGLONG; fmt++; }
            else             { lm = LM_LONG; }
        } else if (*fmt == 'z') {
            lm = LM_SIZE_T;
            fmt++;
        }

        switch (*fmt) {
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s) emit(*s++);
                break;
            }
            case 'c':
                /* char arg is promoted to int by the variadic ABI; any
                 * length modifier is meaningless here but harmless. */
                emit((char)va_arg(ap, int));
                break;
            case 'd':
            case 'i': {
                int64_t v = fetch_signed(&ap, lm);
                if (v < 0) { emit('-'); put_dec((uint64_t)(-v)); }
                else       put_dec((uint64_t)v);
                break;
            }
            case 'u':
                put_dec(fetch_unsigned(&ap, lm));
                break;
            case 'x':
                put_hex(fetch_unsigned(&ap, lm), 0);
                break;
            case 'p': {
                /* Print pointer as 0x + uintptr_t-width hex digits so
                 * addresses line up visually regardless of arch.  i386:
                 * 8 digits, x86_64: 16 digits. */
                emit('0');
                emit('x');
                uintptr_t v = (uintptr_t)va_arg(ap, void*);
                put_hex((uint64_t)v, (int)(sizeof(uintptr_t) * 2));
                break;
            }
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
