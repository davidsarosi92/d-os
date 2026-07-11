# =============================================================================
# d-os Makefile — multi-arch build.
#
# Default arch is i386 (the M1-M19+M18.5 reference port).  Build the
# x86_64 (long mode) port with `make ARCH=x86_64`.  Each ARCH gets its
# own object-output tree under `build/$(ARCH)/`, so the two builds do
# not collide and you can ping-pong between them without `make clean`.
#
# Toolchain assumptions:
#   - gcc with multilib (-m32 + -m64 both available).
#   - nasm.
#   - GNU ld; we pick the emulation via -m (elf_i386 / elf_x86_64).
#   - grub-mkrescue + xorriso for ISO assembly.
# All of these come from the Dockerfile at the repo root, so
# `./scripts/build.sh` works on a host that only has Docker.
# =============================================================================

ARCH ?= i386

CC      := gcc
AS      := nasm
LD      := ld

INCLUDES      := -Ikernel/includes
COMMON_CFLAGS := -ffreestanding -fno-stack-protector -fno-pie -nostdlib \
                 -Wall -Wextra -Wno-unused-parameter -std=c11 $(INCLUDES)

# -----------------------------------------------------------------------------
# Per-arch toolchain knobs + source lists.
#
# The shared (arch-agnostic) source list lives below the ifeq block; each
# arch contributes its own HAL sources + asm.  Anything that touches
# x86-specific instructions, port I/O, descriptor tables, or page-table
# layout MUST live under kernel/hal/<arch>/ and be added here only on
# the corresponding ARCH branch.
# -----------------------------------------------------------------------------

ifeq ($(ARCH),i386)
  CFLAGS  := -m32 $(COMMON_CFLAGS)
  ASFLAGS := -f elf32
  LINKER_SCRIPT := linker-i386.ld
  LDFLAGS := -m elf_i386 -T $(LINKER_SCRIPT) -nostdlib
  LIBGCC  := $(shell $(CC) -m32 -print-libgcc-file-name)
  QEMU    := qemu-system-i386

  # i386 HAL implementation.
  ARCH_C_SRCS := \
      kernel/hal/x86/io.c \
      kernel/hal/x86/gdt.c \
      kernel/hal/x86/idt.c \
      kernel/hal/x86/tss.c \
      kernel/hal/x86/vmm.c \
      kernel/hal/x86/fb_present.c \
      kernel/hal/x86/ringtest.c \
      kernel/hal/x86/pci.c \
      kernel/hal/x86/hal_arch.c \
      kernel/hal/x86/task_arch.c \
      kernel/hal/x86/lapic.c \
      kernel/hal/x86/ioapic.c \
      kernel/hal/x86/smp.c \
      kernel/hal/x86/syscall.c

  ARCH_ASM_SRCS := \
      kernel/hal/x86/boot.s \
      kernel/hal/x86/isr_stubs.s \
      kernel/hal/x86/usermode.s \
      kernel/hal/x86/switch.s

  ARCH_EXTRA_OBJS := kernel/hal/x86/ap_trampoline_blob.o user/hello_blob.o \
                     user/spin_blob.o user/args_blob.o

  # Tier B — in-tree user libc build knobs (i386 reference).
  USER_CFLAGS   := -m32 -ffreestanding -fno-pie -fno-stack-protector \
                   -fno-builtin -nostdlib -Os -Wall -std=c11 -Iuser
  USER_LDEMU    := -m elf_i386
  USER_BASE     := 0x40000000
  USER_OCARGS   := --output-target=elf32-i386 --binary-architecture=i386
  USER_CRT0_BUILD = nasm -f elf32 user/crt0.s -o $(OBJ_DIR)/user/crt0.o

else ifeq ($(ARCH),x86_64)
  # mcmodel=large: kernel can be linked anywhere in 64-bit address space.
  # mno-red-zone: x86_64 ABI's 128-byte red zone below RSP is unsafe in
  #   kernel context because IRQs use the same stack and would clobber it.
  # mno-{mmx,sse,sse2,sse3,3dnow}: don't emit SIMD instructions; we have
  #   not initialised FPU/XMM state on AP entry, and the savearea isn't
  #   in our task struct yet.
  CFLAGS  := -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-sse3 \
             -mno-3dnow -mcmodel=large $(COMMON_CFLAGS)
  ASFLAGS := -f elf64
  LINKER_SCRIPT := linker-x86_64.ld
  LDFLAGS := -m elf_x86_64 -T $(LINKER_SCRIPT) -nostdlib -z max-page-size=0x1000
  LIBGCC  := $(shell $(CC) -m64 -print-libgcc-file-name)
  QEMU    := qemu-system-x86_64

  # x86_64 HAL implementation.  Phase A of M20.5 reuses the i386
  # lapic.c + ioapic.c verbatim (both files are pure MMIO + MSR with
  # no port I/O), widening their `phys` params to uintptr_t.
  # M20.6.2/3 added pci.c (also a port-I/O-only file, identically
  # encoded on both archs) so xHCI + virtio-blk can compile here.
  #
  # Phase 3 of M20 adds the GDT/IDT/TSS
  # + context-switch + isr stubs needed for kernel_main to link (the
  # final wiring happens in Phase 5 once vmm.c is ported).  SMP-side
  # files (lapic, ioapic, smp, ap_trampoline) come in a later phase /
  # M20.5 milestone; UP boot is the M20 DoD.
  ARCH_C_SRCS := \
      kernel/hal/x86_64/io.c \
      kernel/hal/x86_64/hal_arch.c \
      kernel/hal/x86_64/gdt.c \
      kernel/hal/x86_64/idt.c \
      kernel/hal/x86_64/tss.c \
      kernel/hal/x86_64/vmm.c \
      kernel/hal/x86_64/task_arch.c \
      kernel/hal/x86_64/mb2.c \
      kernel/hal/x86_64/main_entry.c \
      kernel/hal/x86_64/m20_stubs.c \
      kernel/hal/x86_64/smp.c \
      kernel/hal/x86_64/syscall.c \
      kernel/hal/x86/lapic.c \
      kernel/hal/x86/ioapic.c \
      kernel/hal/x86/pci.c \
      kernel/hal/x86/fb_present.c \
      kernel/hal/x86/ringtest.c

  ARCH_ASM_SRCS := \
      kernel/hal/x86_64/boot.s \
      kernel/hal/x86_64/isr_stubs.s \
      kernel/hal/x86_64/switch.s \
      kernel/hal/x86_64/usermode.s

  ARCH_EXTRA_OBJS := kernel/hal/x86_64/ap_trampoline_blob.o \
                     user/hello_blob.o user/spin_blob.o

  # Tier B — in-tree user libc build knobs (x86_64).  -mno-sse* because the
  # kernel does not init/save FPU/XMM state for user tasks.
  USER_CFLAGS   := -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-sse3 \
                   -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
                   -nostdlib -Os -Wall -std=c11 -Iuser
  USER_LDEMU    := -m elf_x86_64
  USER_BASE     := 0x40000000
  USER_OCARGS   := --output-target=elf64-x86-64 --binary-architecture=i386
  USER_CRT0_BUILD = nasm -f elf64 user/crt0_x86_64.s -o $(OBJ_DIR)/user/crt0.o

else ifeq ($(ARCH),aarch64)
  # ARM64 port (M21).  Fundamentally different from x86: no port I/O (every
  # device is MMIO), GIC instead of APIC, exception levels (EL1/EL0) instead
  # of rings, and a raw-ELF boot handed straight to QEMU `-M virt -kernel`
  # (no GRUB / no multiboot).  Uses the aarch64-linux-gnu cross toolchain
  # from the Dockerfile; assembly is GNU `as` syntax (.S, run through the C
  # compiler for cpp + gas), NOT nasm.
  #
  # -mgeneral-regs-only: never emit FP/NEON — we do not save the SIMD/FP
  #   register file on exception entry or context switch (mirrors the x86_64
  #   -mno-sse decision).
  CROSS   := aarch64-linux-gnu-
  CC      := $(CROSS)gcc
  LD      := $(CROSS)ld
  # -mno-outline-atomics: emit atomics inline instead of via libgcc's runtime
  #   LSE-detection helpers, which pull in glibc's __getauxval (unavailable
  #   freestanding).
  # -fno-tree-loop-distribute-patterns: stop gcc turning the hand-written
  #   memset/memcpy loops in lib.c into calls to themselves (infinite
  #   recursion) — the standard freestanding-libc footgun.
  CFLAGS  := -mgeneral-regs-only -mno-outline-atomics \
             -fno-tree-loop-distribute-patterns $(COMMON_CFLAGS)
  LINKER_SCRIPT := linker-aarch64.ld
  LDFLAGS := -T $(LINKER_SCRIPT) -nostdlib
  LIBGCC  := $(shell $(CC) -print-libgcc-file-name)
  QEMU    := qemu-system-aarch64

  # AArch64 HAL implementation.  Phase A = boot + UART + exception vectors +
  # MMU identity map (enough to reach a C entry with the MMU on).  Later
  # phases add the GIC, generic timer, context switch, and the console/shell.
  ARCH_C_SRCS := \
      kernel/hal/aarch64/uart.c \
      kernel/hal/aarch64/exceptions.c \
      kernel/hal/aarch64/mmu.c \
      kernel/hal/aarch64/gic.c \
      kernel/hal/aarch64/timer.c \
      kernel/hal/aarch64/hal_arch.c \
      kernel/hal/aarch64/task_arch.c \
      kernel/hal/aarch64/stubs.c \
      kernel/hal/aarch64/lib.c \
      kernel/hal/aarch64/smp.c \
      kernel/hal/aarch64/vmm.c \
      kernel/hal/aarch64/syscall.c \
      kernel/hal/aarch64/pci.c \
      kernel/hal/aarch64/virtio_mmio_blk.c \
      kernel/hal/aarch64/virtio_gpu.c \
      kernel/hal/aarch64/virtio_input.c \
      kernel/hal/aarch64/pl031_rtc.c \
      kernel/hal/aarch64/dtb.c \
      kernel/hal/aarch64/serial_shell.c \
      kernel/hal/aarch64/main_entry.c

  ARCH_ASM_SRCS := \
      kernel/hal/aarch64/boot.S \
      kernel/hal/aarch64/vectors.S \
      kernel/hal/aarch64/switch.S \
      kernel/hal/aarch64/smp_entry.S \
      kernel/hal/aarch64/usermode.S

  ARCH_EXTRA_OBJS := user/hello_blob.o user/spin_blob.o

  # Tier B — in-tree user libc build knobs (aarch64).  Uses the cross toolchain
  # ($(CC)/$(LD)/$(CROSS)objcopy); user base is 4 GiB (above the identity map).
  USER_CFLAGS   := -mgeneral-regs-only -ffreestanding -fno-pie \
                   -fno-stack-protector -fno-builtin -nostdlib -Os -Wall \
                   -std=c11 -Iuser
  USER_LDEMU    :=
  USER_BASE     := 0x100000000
  USER_OCARGS   := --output-target=elf64-littleaarch64 --binary-architecture=aarch64
  USER_OBJCOPY  := $(CROSS)objcopy
  USER_CRT0_BUILD = $(CC) $(USER_CFLAGS) -c user/crt0_aarch64.S -o $(OBJ_DIR)/user/crt0.o

else
  $(error Unsupported ARCH "$(ARCH)" — supported: i386, x86_64, aarch64)
endif

# -----------------------------------------------------------------------------
# Shared (arch-agnostic) source list.
#
# These compile under BOTH archs.  Anything that breaks under x86_64 is
# a HAL leak — fix it by moving the arch bits to kernel/hal/<arch>/ and
# routing the core caller through hal_api.h.
#
# NOTE: during M20 phases 1-4 we deliberately keep CORE_C_SRCS empty
# under the x86_64 build — there's no kernel_main to link to yet.  When
# phase 5 lands, the full list activates for both archs.
# -----------------------------------------------------------------------------

ifeq ($(ARCH),i386)
CORE_C_SRCS := \
    kernel/core/kernel.c \
    kernel/core/shell.c \
    kernel/core/rescue_shell.c \
    kernel/core/printf.c \
    kernel/core/klog.c \
    kernel/core/elf.c \
    kernel/core/proc.c \
    kernel/core/usyscall.c \
    kernel/core/fd.c \
    kernel/core/usock.c \
    kernel/core/service.c \
    kernel/core/bus.c \
    kernel/core/svc_demo.c \
    kernel/core/watchdog.c \
    kernel/core/cron.c \
    kernel/core/multiboot.c \
    kernel/core/console.c \
    kernel/core/module.c \
    kernel/core/driver.c \
    kernel/core/config.c \
    kernel/core/task.c \
    kernel/core/block.c \
    kernel/core/block_cache.c \
    kernel/core/lock.c \
    kernel/core/vc.c \
    kernel/gui/gfx.c \
    kernel/gui/gui.c \
    kernel/gui/widget.c \
    kernel/gui/w_editor.c \
    kernel/gui/clipboard.c \
    kernel/gui/shell_vista.c \
    kernel/gui/shell_bare.c \
    kernel/gui/apps/fileman.c \
    kernel/gui/apps/about.c \
    kernel/gui/apps/newshell.c \
    kernel/gui/apps/hello.c \
    kernel/gui/apps/taskman.c \
    kernel/gui/apps/editor.c \
    kernel/gui/apps/basic.c \
    kernel/core/basic.c \
    kernel/drivers/rtc/cmos_rtc.c \
    kernel/drivers/mouse/ps2_mouse.c \
    kernel/core/keymap.c \
    kernel/core/layouts.c \
    kernel/core/percpu.c \
    kernel/drivers/serial/serial.c \
    kernel/drivers/terminal/fb_terminal.c \
    kernel/drivers/terminal/vga_terminal.c \
    kernel/drivers/keyboard/ps2_keyboard.c \
    kernel/drivers/timer/pit.c \
    kernel/drivers/null/null.c \
    kernel/drivers/block/virtio_blk.c \
    kernel/drivers/net/virtio_net.c \
    kernel/core/net.c \
    kernel/drivers/audio/ac97.c \
    kernel/core/audio.c \
    kernel/drivers/usb/xhci.c \
    kernel/drivers/usb/usb_hid.c \
    kernel/acpi/acpi.c \
    kernel/fs/vfs.c \
    kernel/fs/ramfs.c \
    kernel/fs/devfs.c \
    kernel/fs/procfs.c \
    kernel/fs/exfat.c \
    kernel/mem/pmm.c \
    kernel/mem/kmalloc.c \
    kernel/mem/slab.c
else ifeq ($(ARCH),aarch64)
# M21 Phase C+D: the AArch64 build links the PORTABLE slice of the core it
# needs — printf/console (serial out), spinlocks + per-CPU, the PMM/slab/
# kmalloc heap, the preemptive scheduler (C), and the module registry + VFS +
# ramfs for the interactive serial shell (D).  It deliberately does NOT pull
# the x86-coupled shell.c (welded to the framebuffer VC + GUI + block/USB +
# usermode) or the x86-coupled kernel_main; aarch64/main_entry.c runs its own
# bring-up and aarch64/serial_shell.c is the REPL.  This list grows as later
# phases port more of the core.  M21 Phase I adds the PORTABLE framebuffer
# terminal (fb_terminal.c) — the same 8x8-font renderer x86 uses — driven by
# the aarch64 virtio-gpu present backend (kernel/hal/aarch64/virtio_gpu.c).
CORE_C_SRCS := \
    kernel/core/printf.c \
    kernel/core/klog.c \
    kernel/core/elf.c \
    kernel/core/proc.c \
    kernel/core/usyscall.c \
    kernel/core/fd.c \
    kernel/core/usock.c \
    kernel/core/service.c \
    kernel/core/bus.c \
    kernel/core/svc_demo.c \
    kernel/core/watchdog.c \
    kernel/core/cron.c \
    kernel/core/console.c \
    kernel/core/lock.c \
    kernel/core/percpu.c \
    kernel/core/multiboot.c \
    kernel/core/module.c \
    kernel/core/block.c \
    kernel/core/task.c \
    kernel/core/block_cache.c \
    kernel/core/config.c \
    kernel/core/driver.c \
    kernel/core/keymap.c \
    kernel/core/layouts.c \
    kernel/core/vc.c \
    kernel/core/shell.c \
    kernel/core/rescue_shell.c \
    kernel/core/basic.c \
    kernel/drivers/terminal/fb_terminal.c \
    kernel/drivers/usb/xhci.c \
    kernel/drivers/usb/usb_hid.c \
    kernel/gui/gfx.c \
    kernel/gui/gui.c \
    kernel/gui/widget.c \
    kernel/gui/w_editor.c \
    kernel/gui/clipboard.c \
    kernel/gui/shell_vista.c \
    kernel/gui/shell_bare.c \
    kernel/gui/apps/fileman.c \
    kernel/gui/apps/about.c \
    kernel/gui/apps/newshell.c \
    kernel/gui/apps/hello.c \
    kernel/gui/apps/taskman.c \
    kernel/gui/apps/editor.c \
    kernel/gui/apps/basic.c \
    kernel/mem/pmm.c \
    kernel/mem/slab.c \
    kernel/mem/kmalloc.c \
    kernel/fs/vfs.c \
    kernel/fs/ramfs.c \
    kernel/fs/procfs.c \
    kernel/fs/exfat.c
else
# Phase 5 of M20: x86_64 path now links the full kernel core.  M20.6.2/3
# enabled the device drivers (virtio-blk, xHCI, USB HID) and exFAT;
# these were i386-only until then because the i386 driver code assumed
# <4 GiB DMA.  The audit found the assumption holds today (PMM only
# manages low memory, well below 4 GiB), so the drivers compile here
# unchanged.  Real high-memory DMA support is gated on M19.5.1 (HIGHMEM
# zone population + kmap) plus widening the `phys` fields to uintptr_t
# in xhci.c/virtio_blk.c — both deferred.
CORE_C_SRCS := \
    kernel/core/kernel.c \
    kernel/core/shell.c \
    kernel/core/rescue_shell.c \
    kernel/core/printf.c \
    kernel/core/klog.c \
    kernel/core/elf.c \
    kernel/core/proc.c \
    kernel/core/usyscall.c \
    kernel/core/fd.c \
    kernel/core/usock.c \
    kernel/core/service.c \
    kernel/core/bus.c \
    kernel/core/svc_demo.c \
    kernel/core/watchdog.c \
    kernel/core/cron.c \
    kernel/core/multiboot.c \
    kernel/core/console.c \
    kernel/core/module.c \
    kernel/core/driver.c \
    kernel/core/config.c \
    kernel/core/task.c \
    kernel/core/block.c \
    kernel/core/block_cache.c \
    kernel/core/lock.c \
    kernel/core/vc.c \
    kernel/gui/gfx.c \
    kernel/gui/gui.c \
    kernel/gui/widget.c \
    kernel/gui/w_editor.c \
    kernel/gui/clipboard.c \
    kernel/gui/shell_vista.c \
    kernel/gui/shell_bare.c \
    kernel/gui/apps/fileman.c \
    kernel/gui/apps/about.c \
    kernel/gui/apps/newshell.c \
    kernel/gui/apps/hello.c \
    kernel/gui/apps/taskman.c \
    kernel/gui/apps/editor.c \
    kernel/gui/apps/basic.c \
    kernel/core/basic.c \
    kernel/drivers/rtc/cmos_rtc.c \
    kernel/drivers/mouse/ps2_mouse.c \
    kernel/core/keymap.c \
    kernel/core/layouts.c \
    kernel/core/percpu.c \
    kernel/drivers/serial/serial.c \
    kernel/drivers/terminal/fb_terminal.c \
    kernel/drivers/terminal/vga_terminal.c \
    kernel/drivers/keyboard/ps2_keyboard.c \
    kernel/drivers/timer/pit.c \
    kernel/drivers/null/null.c \
    kernel/drivers/block/virtio_blk.c \
    kernel/drivers/usb/xhci.c \
    kernel/drivers/usb/usb_hid.c \
    kernel/acpi/acpi.c \
    kernel/fs/vfs.c \
    kernel/fs/ramfs.c \
    kernel/fs/devfs.c \
    kernel/fs/procfs.c \
    kernel/fs/exfat.c \
    kernel/mem/pmm.c \
    kernel/mem/kmalloc.c \
    kernel/mem/slab.c
endif

C_SRCS   := $(CORE_C_SRCS) $(ARCH_C_SRCS)
ASM_SRCS := $(ARCH_ASM_SRCS)

# Object files mirror sources under build/$(ARCH)/obj/<original_path>.o
# so the i386 and x86_64 builds never share .o files.  We do this with
# a per-source substitution rather than VPATH because the Makefile is
# clearer when every object's path is obvious.
BUILD_DIR := build/$(ARCH)
OBJ_DIR   := $(BUILD_DIR)/obj
# ASM sources may be nasm `.s` (x86) or GNU-as `.S` (aarch64); map both to .o.
ASM_OBJS  := $(patsubst %.s,%.o,$(patsubst %.S,%.o,$(ASM_SRCS)))
OBJS      := $(addprefix $(OBJ_DIR)/,$(C_SRCS:.c=.o) $(ASM_OBJS)) \
             $(addprefix $(OBJ_DIR)/,$(ARCH_EXTRA_OBJS))

KERNEL_BIN := $(BUILD_DIR)/kernel.bin
ISO_DIR    := $(BUILD_DIR)/iso
ISO        := $(BUILD_DIR)/d-os.iso

.PHONY: all kernel iso run clean clean-all

all: $(KERNEL_BIN)

kernel: $(KERNEL_BIN)

# Per-source compile rule.  The `@mkdir -p $(@D)` ensures the
# build/<arch>/obj/<dir>/ tree exists before each invocation; without
# it gcc would fail trying to write into a nonexistent directory.
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.s
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) $< -o $@

# GNU-as assembly (.S) — used by the aarch64 port.  Run through the C
# compiler so the C preprocessor + gas both apply; $(CC) is the cross gcc.
$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# AP boot trampoline — assembled as a flat binary so `org 0x8000` works,
# then wrapped in an ELF object via objcopy.  Each arch has its own
# trampoline source (i386 needs only real→protected; x86_64 chains
# real→protected→long-mode) so the blob lives in arch-specific source
# trees.  M20.5 Phase B added the x86_64 variant.
#
# objcopy mints symbol names from the input filename (replacing '/' and
# '.' with '_'), and smp.c hard-references those names — so we MUST
# keep the .bin files at their source-relative paths.  Only the wrapper
# .o goes into the per-arch build tree.
kernel/hal/x86/ap_trampoline.bin: kernel/hal/x86/ap_trampoline.s
	nasm -f bin $< -o $@

$(OBJ_DIR)/kernel/hal/x86/ap_trampoline_blob.o: kernel/hal/x86/ap_trampoline.bin
	@mkdir -p $(@D)
	objcopy --input-target=binary --output-target=elf32-i386 \
	         --binary-architecture=i386 \
	         $< $@

kernel/hal/x86_64/ap_trampoline.bin: kernel/hal/x86_64/ap_trampoline.s
	nasm -f bin $< -o $@

$(OBJ_DIR)/kernel/hal/x86_64/ap_trampoline_blob.o: kernel/hal/x86_64/ap_trampoline.bin
	@mkdir -p $(@D)
	objcopy --input-target=binary --output-target=elf64-x86-64 \
	         --binary-architecture=i386:x86-64 \
	         $< $@

# M25 stage 7 + Tier B — in-tree user libc + compiled-C programs (hello, spin),
# built PER ARCH from the USER_* knobs set in the ifeq block above: crt0
# ($(USER_CRT0_BUILD)), compile flags ($(USER_CFLAGS)), link emulation
# ($(USER_LDEMU)) + base ($(USER_BASE)), and blob objcopy target ($(USER_OCARGS)
# via $(USER_OBJCOPY)).  Each program links as a static ELF at USER_BASE with
# OMAGIC (-N) → one RWX PT_LOAD the elf.c loader maps directly, then is wrapped
# as a binary blob.  Per-arch ELF names (user/<prog>_$(ARCH).elf) yield per-arch
# blob symbols (_binary_user_<prog>_<arch>_elf_*); shell.c picks the live one.
USER_OBJCOPY ?= objcopy

user/hello_$(ARCH).elf: user/libc.c user/hello.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c  -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/hello.c -o $(OBJ_DIR)/user/hello.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/hello.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/hello_blob.o: user/hello_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

user/spin_$(ARCH).elf: user/libc.c user/spin.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/spin.c -o $(OBJ_DIR)/user/spin.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/spin.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/spin_blob.o: user/spin_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

user/args_$(ARCH).elf: user/libc.c user/args.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/args.c -o $(OBJ_DIR)/user/args.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/args.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/args_blob.o: user/args_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

$(KERNEL_BIN): $(OBJS) $(LINKER_SCRIPT)
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

iso: $(ISO)

# grub.cfg per arch — i386 uses multiboot1 (`multiboot`), x86_64 uses
# multiboot2 (`multiboot2`).  Both live under boot/grub/ as named
# variants and the iso target picks the right one.
ifeq ($(ARCH),i386)
  GRUB_CFG := boot/grub/grub.cfg
else
  GRUB_CFG := boot/grub/grub-x86_64.cfg
endif

$(ISO): $(KERNEL_BIN) $(GRUB_CFG)
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	cp $(GRUB_CFG) $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR)

run: $(ISO)
	$(QEMU) -cdrom $(ISO)

# `clean` removes only the current ARCH's tree (so you can wipe x86_64
# without disturbing a working i386 build).  `clean-all` wipes both.
clean:
	rm -rf $(BUILD_DIR)

clean-all:
	rm -rf build
