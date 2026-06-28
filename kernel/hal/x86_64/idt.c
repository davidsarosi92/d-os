/* =============================================================================
 * idt.c — IDT construction, PIC remap, and interrupt dispatch (x86_64).
 *
 * Mirror of the i386 version with two structural changes:
 *
 *   1. Each gate descriptor is 16 bytes (not 8): the offset is now
 *      64-bit, split across three fields, and there's a 3-bit IST
 *      selector that lets a vector pick one of TSS.IST[1..7] as its
 *      kernel stack on entry.
 *   2. The IDTR pointer is 10 bytes (2-byte limit + 8-byte base) so
 *      `lidt` loads the full 64-bit address.
 *
 * Everything portable — PIC remap, IRQ registration, APIC routing —
 * is line-for-line ported.  The C dispatcher (`isr_handler`) walks
 * the (now 64-bit) int_frame fields with their new names (rip vs eip,
 * rax vs eax) but the dispatch logic is unchanged.
 *
 * Reference: AMD64 APM Vol 2 §8.9 (Long-Mode Interrupt Descriptors).
 * ============================================================================= */

#include "idt.h"
#include "hal.h"
#include "printf.h"
#include "gdt.h"
#include "syscall.h"
#include "task.h"
#include "lapic.h"
#include "ioapic.h"
#include <stdint.h>

/* --------------------------------------------------------------------------
 * 64-bit gate descriptor.
 *
 *   bits  0..15  offset_low      — handler offset bits  0..15
 *   bits 16..31  selector        — code segment to jump through
 *   bits 32..34  ist             — TSS.IST index (0 = no IST switch)
 *   bits 35..39  zero            — reserved (must be 0)
 *   bits 40..43  type            — 0xE = interrupt gate, 0xF = trap gate
 *   bit  44      zero
 *   bits 45..46  dpl             — required privilege to invoke via int N
 *   bit  47      present
 *   bits 48..63  offset_mid      — handler offset bits 16..31
 *   bits 64..95  offset_high     — handler offset bits 32..63
 *   bits 96..127 reserved
 * -------------------------------------------------------------------------- */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;                /* low 3 bits = IST index; upper 5 reserved */
    uint8_t  type_attr;          /* P | DPL(2) | 0 | type(4) */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define IDT_ENTRIES 256
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtr;

static irq_handler_t irq_handlers[16] = { 0 };

/* APIC mode flag, same semantics as i386. */
static int g_apic_mode = 0;
static uint8_t g_bsp_apic_id = 0;

/* --------------------------------------------------------------------------
 * asm stubs — declared so we can take their addresses in set_gate().
 * Same layout as i386: one stub per vector 0..47 + APIC vectors +
 * the syscall vector.
 * -------------------------------------------------------------------------- */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void isr32(void); extern void isr33(void); extern void isr34(void); extern void isr35(void);
extern void isr36(void); extern void isr37(void); extern void isr38(void); extern void isr39(void);
extern void isr40(void); extern void isr41(void); extern void isr42(void); extern void isr43(void);
extern void isr44(void); extern void isr45(void); extern void isr46(void); extern void isr47(void);
extern void isr64(void);                           /* LAPIC timer (M18.5) */
extern void isr65(void);                           /* reserved: cross-CPU preempt IPI */
extern void isr128(void);                          /* int 0x80 — syscall */

static void (*const isr_table[48])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
    isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
};

static const char* const exception_name[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "BOUND Range", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection",
    "Page Fault", "reserved", "x87 FP", "Alignment Check",
    "Machine Check", "SIMD FP", "Virtualization", "Control Protection",
    "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
};

/* --------------------------------------------------------------------------
 * PIC remap — identical to i386.  The legacy 8259A still exists on
 * x86_64 PCs (and in QEMU's pc machine) and uses the same port map.
 * -------------------------------------------------------------------------- */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static void pic_remap(void) {
    outb(PIC1_CMD,  0x11);  outb(PIC2_CMD,  0x11);
    outb(PIC1_DATA, 0x20);  outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);  outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);  outb(PIC2_DATA, 0x01);

    outb(PIC1_DATA, 0xFB);
    outb(PIC2_DATA, 0xFF);
}

static void pic_unmask(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  mask = inb(port);
    mask &= ~(1 << (irq & 7));
    outb(port, mask);
}

static void pic_eoi(int irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

/* --------------------------------------------------------------------------
 * IDT construction.  Each gate is 16 bytes; offset is 64-bit split
 * across three fields.
 * -------------------------------------------------------------------------- */

static void set_gate(int vector, void (*handler)(void), uint8_t type_attr) {
    uintptr_t addr = (uintptr_t)handler;
    idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vector].selector    = GDT_KERNEL_CS;
    idt[vector].ist         = 0;                           /* no IST switch */
    idt[vector].type_attr   = type_attr;
    idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)(addr >> 32);
    idt[vector].reserved    = 0;
}

void idt_init(void) {
    /* Zero everything so any vector we don't install is P=0. */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low  = 0;
        idt[i].selector    = 0;
        idt[i].ist         = 0;
        idt[i].type_attr   = 0;
        idt[i].offset_mid  = 0;
        idt[i].offset_high = 0;
        idt[i].reserved    = 0;
    }

    /* Install 48 gates: 32 exceptions + 16 IRQs.  0x8E = P=1, DPL=0,
     * 64-bit interrupt gate (the type-attr byte is identical to i386 —
     * the gate type byte 0xE means "interrupt gate" in both modes, the
     * CPU just interprets it according to the 64-bit descriptor
     * layout). */
    for (int i = 0; i < 48; i++) set_gate(i, isr_table[i], 0x8E);

    /* APIC vectors (M18.5).  Same numbering as i386. */
    set_gate(0x40, isr64, 0x8E);
    set_gate(0x41, isr65, 0x8E);

    /* Syscall: DPL=3 so ring 3 can invoke via int 0x80.  On x86_64 we
     * also plan to add a SYSCALL/SYSRET path in Phase 7; the IDT
     * gate stays as a fallback. */
    set_gate(0x80, isr128, 0xEE);

    pic_remap();

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)(uintptr_t)&idt[0];
    __asm__ volatile ("lidt %0" : : "m"(idtr));

    kprintf("IDT: %d entries @ %p, PIC remapped to 0x20..0x2F\n",
            IDT_ENTRIES, (void*)idtr.base);
}

/* APs call this to lidt the shared IDT on their own CPU. */
void idt_load(void) {
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

void irq_install(int irq, irq_handler_t handler) {
    if (irq < 0 || irq > 15) return;
    irq_handlers[irq] = handler;
    if (!handler) return;
    if (g_apic_mode) {
        ioapic_route_isa(irq, (uint8_t)(0x20 + irq), g_bsp_apic_id);
    } else {
        pic_unmask(irq);
    }
}

void idt_use_apic(uint8_t bsp_apic_id) {
    g_bsp_apic_id = bsp_apic_id;
    g_apic_mode   = 1;

    for (int irq = 0; irq < 16; irq++) {
        if (irq_handlers[irq]) {
            ioapic_route_isa(irq, (uint8_t)(0x20 + irq), bsp_apic_id);
        }
    }

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    kprintf("apic: routing live (bsp_apic_id=%u), 8259 disabled\n",
            bsp_apic_id);
}

/* --------------------------------------------------------------------------
 * C-side dispatch.  Same logic as i386, with field-name updates for
 * the 64-bit int_frame (rip vs eip, etc.).
 * -------------------------------------------------------------------------- */
void isr_handler(struct int_frame* f) {
    if (f->int_no < 32) {
        kprintf("\n!! EXCEPTION %u (%s) at cs:rip=%lx:%p err=%lx\n",
                (unsigned)f->int_no, exception_name[f->int_no],
                (unsigned long)f->cs, (void*)(uintptr_t)f->rip,
                (unsigned long)f->err_code);
        for (;;) __asm__ volatile ("cli; hlt");
    }

    if (f->int_no >= 32 && f->int_no < 48) {
        int irq = (int)f->int_no - 32;
        if (irq_handlers[irq]) irq_handlers[irq](f);
        if (g_apic_mode) lapic_eoi();
        else             pic_eoi(irq);
        schedule_check();
        return;
    }

    if (f->int_no == 0x40 || f->int_no == 0x41) {
        if (f->int_no == 0x40) schedule_request();
        lapic_eoi();
        schedule_check();
        return;
    }

    if (f->int_no == 0x80) {
        syscall_dispatch(f);
        return;
    }

    kprintf("unexpected vector %u at %p\n",
            (unsigned)f->int_no, (void*)(uintptr_t)f->rip);
}
