/* =============================================================================
 * tss.c — per-CPU Task State Segments for the kernel (x86_64).
 *
 * SMP (2026-08-01): there is one TSS PER CPU, not one for the machine.  The TSS
 * holds RSP0 — the stack the CPU switches to on a ring-3 → ring-0 transition —
 * and TR is a per-CPU register, so two CPUs sharing one TSS would land ring-3
 * traps from both cores on the SAME kernel stack and corrupt each other.  Worse,
 * an AP that never executes `ltr` has TR = 0: the first interrupt or exception
 * taken while it runs a ring-3 task cannot find a stack at all, which escalates
 * straight to #DF and then a triple fault (this is exactly how x86_64 `-smp 2`
 * died as soon as NetSurf got load-balanced onto the AP — see DOCS §8).  Mirrors
 * what M35 did for i386: an array here, one GDT descriptor per CPU in gdt.c, and
 * every CPU LTRs its own via gdt_load_cpu_tss().
 *
 * The 64-bit TSS is laid out very differently from the 32-bit one:
 *
 *   offset  field         size
 *   0x00    reserved       4 B
 *   0x04    RSP0           8 B  ← kernel stack pointer for ring 3 → ring 0
 *   0x0C    RSP1           8 B
 *   0x14    RSP2           8 B
 *   0x1C    reserved       8 B
 *   0x24    IST1           8 B  ← per-vector kernel stack (optional)
 *   ...     IST2..IST7     8 B each
 *   0x5C    reserved       8 B
 *   0x64    reserved      16 b
 *   0x66    iomap_base    16 b
 *   total                104 B
 *
 * Most fields are 8-byte values starting at 4-byte boundaries — the
 * struct MUST be packed or the compiler will pad to natural alignment
 * and the CPU will read garbage.
 *
 * Why no ESP1/ESP2 array like i386: long mode dropped hardware task
 * switching entirely.  The CPU only ever reads RSP0/1/2 (for the
 * three privilege levels it actually supports) and IST1..7 (for IDT
 * entries that opt into a per-vector stack via their IST field).
 *
 * Reference: Intel SDM Vol 3 §7.7 (Task Management in 64-bit Mode).
 * ============================================================================= */

#include "tss.h"
#include "gdt.h"
#include "hal_api.h"
#include "acpi.h"          /* ACPI_MAX_CPUS — sizes the per-CPU array */
#include "percpu.h"        /* this_cpu_id                              */
#include <stdint.h>

#define IOMAP_PORTS 0x400
#define IOMAP_BYTES (IOMAP_PORTS / 8)

struct tss64 {
    uint32_t reserved0;             /* 0x00 */
    uint64_t rsp0;                  /* 0x04 — kernel stack pointer */
    uint64_t rsp1;                  /* 0x0C */
    uint64_t rsp2;                  /* 0x14 */
    uint64_t reserved1;             /* 0x1C */
    uint64_t ist1;                  /* 0x24 */
    uint64_t ist2;                  /* 0x2C */
    uint64_t ist3;                  /* 0x34 */
    uint64_t ist4;                  /* 0x3C */
    uint64_t ist5;                  /* 0x44 */
    uint64_t ist6;                  /* 0x4C */
    uint64_t ist7;                  /* 0x54 */
    uint64_t reserved2;             /* 0x5C */
    uint16_t reserved3;             /* 0x64 */
    uint16_t iomap_base;            /* 0x66 */

    /* §M33 Tier 1 — the I/O permission bitmap.  Identical in purpose and size
     * to the i386 twin; see kernel/hal/x86/tss.c for why 0x400 ports is a
     * deliberate ceiling rather than a shortcut.  The mechanism is unchanged in
     * long mode: IN/OUT from ring 3 consults it, and a port past the segment
     * limit is denied. */
    uint8_t  iomap[IOMAP_BYTES];
    uint8_t  iomap_tail;
} __attribute__((packed));

#define TSS_MAX_CPUS ACPI_MAX_CPUS
static struct tss64 tss[TSS_MAX_CPUS];

/* Mirror of the RUNNING CPU's tss.rsp0, read by the SYSCALL-instruction entry
 * stub (syscall_entry.s): `syscall` does not switch stacks the way an int-gate
 * does, so the stub loads the kernel stack from here.  Kept in lock-step with
 * tss[this_cpu].rsp0 by tss_init / hal_set_kernel_stack below.
 *
 * NOTE (SMP): this single mirror is correct only because it is rewritten on
 * every context switch by hal_set_kernel_stack — the scheduler sets it for the
 * task about to run on THIS CPU.  If SYSCALL entry ever needs to work without
 * that guarantee it must become per-CPU (e.g. via %gs and swapgs). */
/* §M52 — syscall.c owns the per-CPU SYSCALL scratch area; the entry stub
 * reaches it through swapgs.  Was a single global, which made SYSCALL
 * UP-only long after ring-3 tasks started running on APs. */
void syscall_set_kernel_rsp(int cpu, uintptr_t top);

/* Dedicated kernel stack used for ring-3 → ring-0 transitions, mirror
 * of the i386 syscall_stack.  4 KiB is plenty for a syscall handler;
 * deeper paths (nested IRQs etc.) use the regular kernel stack via
 * IST entries instead.  Aligned to 16 bytes to satisfy System V's
 * stack-alignment requirement on call.  ONE PER CPU — two cores taking a
 * ring-3 trap at the same time must not share it. */
#define KSTACK_SIZE 4096
static uint8_t syscall_stack[TSS_MAX_CPUS][KSTACK_SIZE] __attribute__((aligned(16)));

static inline uint64_t default_rsp0(int cpu) {
    return (uint64_t)(uintptr_t)(syscall_stack[cpu] + KSTACK_SIZE);
}

void tss_init(void) {
    /* Zero everything — the per-CPU init code below sets the slots we
     * actually use.  Every CPU's TSS is prepared up front so an AP only has to
     * `ltr` its own selector (gdt_load_cpu_tss) with nothing left to allocate. */
    for (int c = 0; c < TSS_MAX_CPUS; c++) {
        uint8_t* b = (uint8_t*)&tss[c];
        for (uint32_t i = 0; i < sizeof(struct tss64); i++) b[i] = 0;
        tss[c].rsp0 = default_rsp0(c);
        /* iomap_base == sizeof(tss) means "no I/O permission bitmap" —
         * any ring-3 IN/OUT traps with #GP (which is what we want; user
         * mode has no business doing port I/O). */
        tss[c].iomap_base = sizeof(struct tss64);
        for (uint32_t i = 0; i < IOMAP_BYTES; i++) tss[c].iomap[i] = 0xFF;
        tss[c].iomap_tail = 0xFF;
    }
    for (int c = 0; c < TSS_MAX_CPUS; c++)
        syscall_set_kernel_rsp(c, (uintptr_t)tss[c].rsp0);
}

void tss_set_kernel_stack(uintptr_t sp) {
    tss[this_cpu_id()].rsp0 = (uint64_t)sp;
}

/* Tier B — per-task ring-3→ring-0 stack.  `top != 0` = an independent user
 * task's own kernel-stack top; `top == 0` restores this CPU's fixed syscall
 * stack (kernel threads + the excursion-model self-tests). */
void hal_set_kernel_stack(uintptr_t top) {
    int c = this_cpu_id();
    tss[c].rsp0 = top ? (uint64_t)top : default_rsp0(c);
    syscall_set_kernel_rsp(c, (uintptr_t)tss[c].rsp0);
}

/* M35 TLS (x86_64) — set the thread pointer.  On x86_64, thread-local storage
 * lives at %fs (musl's __init_tls + arch_prctl(ARCH_SET_FS, p)), unlike i386's
 * %gs GDT descriptor.  We program the FS.base MSR directly.  The scheduler
 * calls this for the incoming task on every switch (task.c: `if (has_tls)
 * hal_set_tls_base(tls_base)`), so per-task FS bases are restored for free —
 * no per-CPU GDT descriptor juggling like i386 needs. */
#define MSR_FS_BASE 0xC0000100u
void hal_set_tls_base(uintptr_t base) {
    __asm__ volatile ("wrmsr"
                      :: "c"(MSR_FS_BASE),
                         "a"((uint32_t)(uint64_t)base),
                         "d"((uint32_t)((uint64_t)base >> 32)));
}

uintptr_t tss_get_addr(void)        { return (uintptr_t)&tss[0]; }
uintptr_t tss_get_addr_cpu(int cpu) { return (uintptr_t)&tss[cpu]; }
uint32_t  tss_get_limit(void)       { return sizeof(struct tss64) - 1; }

/* §M33 Tier 1 — install a task's port grant.  See the i386 twin for the
 * reasoning, including why the copy is skipped when nothing changed. */
static const void* iomap_loaded[TSS_MAX_CPUS];

void hal_set_io_bitmap(const void* bm) {
    int c = this_cpu_id();
    if (iomap_loaded[c] == bm) return;
    iomap_loaded[c] = bm;
    if (!bm) { tss[c].iomap_base = sizeof(struct tss64); return; }
    const uint8_t* src = (const uint8_t*)bm;
    for (uint32_t i = 0; i < IOMAP_BYTES; i++) tss[c].iomap[i] = src[i];
    tss[c].iomap_tail = 0xFF;
    tss[c].iomap_base = (uint16_t)__builtin_offsetof(struct tss64, iomap);
}

/* §M33 Tier 2 — drop a cache entry for a bitmap about to be freed.  See the
 * i386 twin for why: the cache compares POINTERS, and a restarted driver's
 * fresh allocation lands on the freed one's address, so without this the TSS
 * would keep the dead driver's permissions and look correct while doing it. */
void hal_io_bitmap_forget(const void* bm) {
    for (int c = 0; c < TSS_MAX_CPUS; c++)
        if (iomap_loaded[c] == bm) iomap_loaded[c] = NULL;
}

uint32_t hal_io_bitmap_bytes(void) { return IOMAP_BYTES; }
int       tss_max_cpus(void)        { return TSS_MAX_CPUS; }
