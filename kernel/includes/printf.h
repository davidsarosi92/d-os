/* printf.h — kernel-side formatted output.
 *
 * Supports: %s  %c  %d  %u  %x  %p  %%
 * Does NOT support: width / precision / padding / signed hex / long / float.
 * Output is broadcast through `console_putchar` to every active console
 * sink (typically FB terminal + serial debug). */

#ifndef PRINTF_H
#define PRINTF_H

void kprintf(const char* fmt, ...);

#endif
