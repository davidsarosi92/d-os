/* =============================================================================
 * drvrt_user.c — §M33 Tier 1: the USER-MODE backend of the driver-runtime API.
 *
 * The same drvrt.h a driver in ring 0 is written against, implemented on the
 * other side of the boundary.  A driver that includes drvrt.h and is compiled
 * with -DDRV_USERSPACE links against this instead of kernel/core/drvrt.c, and
 * nothing in its own source changes — which is the claim §M33 made and this
 * file is half the proof of.
 *
 * -----------------------------------------------------------------------------
 * THE PROPERTY WORTH NOTICING: PORT I/O IS NOT A SYSCALL
 *
 * `drv_ports_request` is a syscall, once.  After it, `drv_in8`/`drv_out8`
 * execute the `in`/`out` instruction DIRECTLY, in ring 3, at the same speed the
 * kernel driver ran at — because the grant lives in the CPU's I/O permission
 * bitmap rather than in a check the kernel performs per access.
 *
 * That is what makes moving a driver out of the kernel affordable.  A design
 * where every port access trapped would make the placement a performance
 * decision as well as a safety one, and the answer would always be "leave it in
 * the kernel".  Here the only per-access cost is the same bounds check the
 * in-kernel backend does — which is deliberate, because a driver must behave
 * identically in both, including when it asks for something out of range.
 *
 * The interrupt is the opposite and unavoidably so: `drv_irq_wait` blocks, and
 * blocking is the kernel's to do.  One syscall per interrupt is the price of
 * the boundary, and it is the price §M33's plan quoted.
 * ============================================================================= */

#include "libc.h"
#include "drvrt.h"

#define SYS_DRV_PORTS    0xD060
#define SYS_DRV_IRQ      0xD061
#define SYS_DRV_IRQ_WAIT 0xD062
#define SYS_DRV_INPUT    0xD063
#define SYS_DRV_LOG      0xD064

/* Mirror of the grant, so the bounds check is local.  Small and fixed: a
 * ring-3 driver holds a handful of resources, and an allocation here would be
 * a failure path on the one journey that has no way to report anything. */
struct u_res { int used; uint16_t base, count; };
static struct u_res g_res[DRV_MAX_RES];

void drv_rt_init(struct drv_rt* rt, const char* owner) {
    if (!rt) return;
    rt->owner    = owner;
    rt->nres     = 0;
    rt->body_pid = 0;              /* in ring 3 the body IS this process */
    for (int i = 0; i < DRV_MAX_RES; i++) { rt->res[i] = 0; g_res[i].used = 0; }
}

void drv_release_all(struct drv_rt* rt) {
    if (!rt) return;
    /* The kernel releases the real resources when the process dies, which is
     * the ONLY teardown that can be trusted here: a driver that crashed did not
     * get to call this.  So this clears the local mirror and nothing else —
     * and that asymmetry is a feature of the placement rather than a gap.  In
     * ring 0 a shutdown hook is the only way resources come back; in ring 3 the
     * process exiting is. */
    for (int i = 0; i < DRV_MAX_RES; i++) g_res[i].used = 0;
    rt->nres = 0;
}

drv_handle drv_ports_request(struct drv_rt* rt, uint16_t base, uint16_t count,
                             const char* why) {
    (void)rt; (void)why;
    long h = dos_syscall3(SYS_DRV_PORTS, base, count, 0);
    if (h < 0) return (drv_handle)h;
    if (h > 0 && h <= DRV_MAX_RES) {
        g_res[h - 1].used  = 1;
        g_res[h - 1].base  = base;
        g_res[h - 1].count = count;
    }
    return (drv_handle)h;
}

/* The bound check, in one place — the same shape as the in-kernel backend's,
 * for the same reason: a driver must get the same answer on both sides,
 * including when it asks for something outside its window. */
static long port_at(drv_handle h, uint16_t off) {
    if (h <= 0 || h > DRV_MAX_RES || !g_res[h - 1].used) return DRV_EBAD;
    if (off >= g_res[h - 1].count) return DRV_ERANGE;
    return (long)(g_res[h - 1].base + off);
}

static inline unsigned char raw_in8(unsigned short p) {
    unsigned char v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v;
}
static inline unsigned short raw_in16(unsigned short p) {
    unsigned short v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(p)); return v;
}
static inline unsigned raw_in32(unsigned short p) {
    unsigned v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p)); return v;
}
static inline void raw_out8 (unsigned short p, unsigned char v)  { __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline void raw_out16(unsigned short p, unsigned short v) { __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline void raw_out32(unsigned short p, unsigned v)       { __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(p)); }

int  drv_in8 (drv_handle h, uint16_t off) { long p = port_at(h, off); return p < 0 ? (int)p : (int)raw_in8((unsigned short)p); }
int  drv_in16(drv_handle h, uint16_t off) { long p = port_at(h, off); return p < 0 ? (int)p : (int)raw_in16((unsigned short)p); }
long drv_in32(drv_handle h, uint16_t off) { long p = port_at(h, off); return p < 0 ? p : (long)raw_in32((unsigned short)p); }
int  drv_out8 (drv_handle h, uint16_t off, uint8_t v)  { long p = port_at(h, off); if (p < 0) return (int)p; raw_out8((unsigned short)p, v); return 0; }
int  drv_out16(drv_handle h, uint16_t off, uint16_t v) { long p = port_at(h, off); if (p < 0) return (int)p; raw_out16((unsigned short)p, v); return 0; }
int  drv_out32(drv_handle h, uint16_t off, uint32_t v) { long p = port_at(h, off); if (p < 0) return (int)p; raw_out32((unsigned short)p, v); return 0; }

drv_handle drv_irq_request(struct drv_rt* rt, int line, const char* why) {
    (void)rt; (void)why;
    return (drv_handle)dos_syscall3(SYS_DRV_IRQ, line, 0, 0);
}

/* Set once the kernel has asked this driver to stop.  Sticky on purpose: the
 * request is delivered exactly once, on whichever wait happened to be in
 * flight, and a driver that checked the flag one loop later would miss it. */
static int g_stopping;

int drv_irq_wait(drv_handle h, int timeout_ms) {
    int r = (int)dos_syscall3(SYS_DRV_IRQ_WAIT, h, timeout_ms, 0);
    if (r == DRV_ESTOP) g_stopping = 1;
    return r;
}

uint32_t drv_irq_count(drv_handle h) { (void)h; return 0; }   /* kernel-side stat */

/* MMIO and DMA: not reachable from ring 3 yet.  REFUSED rather than faked —
 * a driver that needs either is a driver Tier 1 cannot place, and finding that
 * out at the request is the whole reason drvrt.h returns errors instead of
 * assuming success. */
drv_handle drv_mmio_request(struct drv_rt* rt, uint64_t phys, size_t len, const char* why) {
    (void)rt; (void)phys; (void)len; (void)why; return DRV_ENOSYS;
}
volatile void* drv_mmio_ptr(drv_handle h) { (void)h; return 0; }
drv_handle drv_dma_request(struct drv_rt* rt, size_t bytes, const char* why) {
    (void)rt; (void)bytes; (void)why; return DRV_ENOSYS;
}
void*    drv_dma_cpu(drv_handle h)    { (void)h; return 0; }
uint64_t drv_dma_device(drv_handle h) { (void)h; return 0; }

/* In ring 3 the "driver body" is simply the program.  `drv_run` calls it
 * instead of spawning, so the driver's own source does not have to know which
 * side of the boundary it is on. */
int drv_run(struct drv_rt* rt, const char* name, void (*body)(void)) {
    (void)rt; (void)name;
    if (body) body();
    return 0;
}

void drvrt_start_deferred(void) { }
void drv_res_dump(void) { }

/* The kernel-side helpers a ported driver still names.  Each is the smallest
 * honest ring-3 equivalent. */
void mouse_publish(int dx, int dy, unsigned buttons, int dz) {
    dos_syscall4(SYS_DRV_INPUT, dx, dy, (long)buttons, dz);
}

/* THE COOPERATIVE STOP, ACROSS A PROCESS BOUNDARY.
 *
 * This used to `return 0` with a comment claiming a ring-3 driver is stopped by
 * being killed.  That was wrong in a way only the stop path could show: the
 * driver spends its life blocked in drv_irq_wait, and the kernel's force-kill
 * only takes a task caught IN USER MODE at a timer preemption — which a driver
 * on a one-second timeout reaches roughly once a second.  So `drv stop` on a
 * placed driver appeared to do nothing, and the honest-looking stub was the
 * reason.
 *
 * Now the kernel's request arrives as DRV_ESTOP on the wait itself, and the
 * driver's own loop — the same line the in-kernel build runs — sees it. */
int task_should_stop(void) { return g_stopping; }

/* A driver's own messages, rendered here and sent to the kernel's log.
 *
 * THE FIRST VERSION WROTE TO FD 1 AND WAS INVISIBLE.  A driver process is
 * spawned by the registry rather than by a shell, so it has no console bound
 * and its writes went nowhere — the first ring-3 bring-up failure of this
 * milestone printed "controller timeout" into a void, and the fault had to be
 * inferred from a CPU-time column belonging to a different task.  SYS_DRV_LOG
 * puts the line in klog next to the in-kernel driver's, attributed by the
 * kernel from the slot.
 *
 * %d / %x / %s ONLY, and the conversions are here rather than skipped because
 * the messages that matter are the ones carrying a number: "no port grant (-1)"
 * and "no port grant (-5)" are different diagnoses, and a formatter that drops
 * the argument makes them the same sentence. */
static int u_fmt_num(char* out, int cap, long v, int base) {
    char tmp[24]; int n = 0, neg = 0;
    unsigned long u;
    if (v < 0 && base == 10) { neg = 1; u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (!u) tmp[n++] = '0';
    while (u) { int d = (int)(u % (unsigned long)base); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (unsigned long)base; }
    if (neg) tmp[n++] = '-';
    int k = 0;
    while (n && k < cap) out[k++] = tmp[--n];
    return k;
}

void kprintf(const char* fmt, ...) {
    char buf[128];
    int  n = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char* p = fmt; *p && n < (int)sizeof buf - 1; p++) {
        if (*p != '%') { if (*p != '\n') buf[n++] = *p; continue; }
        p++;
        if (*p == 'd')      n += u_fmt_num(buf + n, (int)sizeof buf - 1 - n,
                                           __builtin_va_arg(ap, int), 10);
        else if (*p == 'x') n += u_fmt_num(buf + n, (int)sizeof buf - 1 - n,
                                           (long)__builtin_va_arg(ap, unsigned), 16);
        else if (*p == 's') { const char* s = __builtin_va_arg(ap, const char*);
                              while (s && *s && n < (int)sizeof buf - 1) buf[n++] = *s++; }
        else if (*p == '%') buf[n++] = '%';
        else if (!*p)       break;
    }
    __builtin_va_end(ap);
    buf[n] = 0;
    dos_syscall3(SYS_DRV_LOG, (long)(uintptr_t)buf, n, 0);
}
