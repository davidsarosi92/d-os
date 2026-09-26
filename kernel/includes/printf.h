/* printf.h — kernel-side formatted output.
 *
 * Supports: %s  %c  %d  %u  %x  %p  %%
 * Does NOT support: width / precision / padding / signed hex / long / float.
 * Output is broadcast through `console_putchar` to every active console
 * sink (typically FB terminal + serial debug). */

#ifndef PRINTF_H
#define PRINTF_H

#include <stdarg.h>

/* Format-checked (2026-09-26): making pmm_phys_t 64-bit on i386 (§M86 PAE)
 * turned every `%x` of a physical address into an argument that shifts every
 * later one — a `%s` after it reads garbage as a pointer.  The compiler can
 * find all of those; nothing else reliably can. */
void kprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* va_list form of kprintf — same formatting + console/klog teeing.
 * Used by klog() (klog.c) so structured log lines format through the
 * one and only formatter. */
void kvprintf(const char* fmt, va_list ap);

#endif
