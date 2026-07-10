/* printf.h — kernel-side formatted output.
 *
 * Supports: %s  %c  %d  %u  %x  %p  %%
 * Does NOT support: width / precision / padding / signed hex / long / float.
 * Output is broadcast through `console_putchar` to every active console
 * sink (typically FB terminal + serial debug). */

#ifndef PRINTF_H
#define PRINTF_H

#include <stdarg.h>

void kprintf(const char* fmt, ...);

/* va_list form of kprintf — same formatting + console/klog teeing.
 * Used by klog() (klog.c) so structured log lines format through the
 * one and only formatter. */
void kvprintf(const char* fmt, va_list ap);

#endif
