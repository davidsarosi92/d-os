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
      kernel/hal/x86/syscall.c \
      kernel/hal/x86/fork.c \
      kernel/hal/x86/signal.c \
      kernel/hal/x86/linux_abi.c

  ARCH_ASM_SRCS := \
      kernel/hal/x86/boot.s \
      kernel/hal/x86/isr_stubs.s \
      kernel/hal/x86/usermode.s \
      kernel/hal/x86/switch.s

  ARCH_EXTRA_OBJS := kernel/hal/x86/ap_trampoline_blob.o user/hello_blob.o \
                     user/spin_blob.o user/args_blob.o user/forktest_blob.o \
                     user/forkexec_blob.o user/pipetest_blob.o \
                     user/sigtest_blob.o user/dnstest_blob.o \
                     user/httptest_blob.o user/threadtest_blob.o \
                     user/tlstest_blob.o user/posixtest_blob.o \
                     user/linuxhello_blob.o user/wlclient_blob.o user/wlapp_blob.o

  # REAL musl-linked programs are embedded ONLY when musl has been built
  # (`make musl`); otherwise the kernel builds without them.  This keeps the
  # default build independent of the (fetched, on-demand) musl toolchain.
  # MUSL_COREUTILS is the modular list — add a coreutil by adding its name here
  # (+ a user/<name>.c) and a recipe in pkg.c; the build + blob are generic.
  MUSL_COREUTILS := echo cat ls env sh
  ifneq ($(wildcard third_party/musl-i386/lib/libc.a),)
    ARCH_EXTRA_OBJS += user/muslhello_muslblob.o \
                       $(patsubst %,user/%_muslblob.o,$(MUSL_COREUTILS))
  endif

  # §M37: dynamic-linking artifacts need the SHARED musl (libc.so, produced by
  # the same `make musl`).  ldmusl = the dynamic linker itself (embedded so the
  # kernel can install it at /lib/ld-musl-i386.so.1); muslhellodyn = a
  # dynamically-linked test program (PT_INTERP set, PIE main).
  ifneq ($(wildcard third_party/musl-i386/lib/libc.so),)
    ARCH_EXTRA_OBJS += user/ldmusl_blob.o user/muslhellodyn_dynblob.o \
                       user/libgreet_blob.o user/solibtest_dynblob.o \
                       user/dlopentest_dynblob.o
  endif

  # §M39 stage 2+3: crypttest + ssltest link against the ported Mbed TLS.
  ifneq ($(wildcard third_party/mbedtls-i686/lib/libmbedcrypto.a),)
    ARCH_EXTRA_OBJS += user/crypttest_muslblob.o user/ssltest_muslblob.o
  endif

  # §M38: C++ runtime artifacts, present only once the musl C++ toolchain was
  # built (make musl-cross-i686).  cpptest = the DoD (exceptions across a .so);
  # libcpplib/libstdcxx/libgccs are the .so's provisioned into /lib at boot.
  ifneq ($(wildcard third_party/musl-cross-i686/bin/i686-linux-musl-g++),)
    ARCH_EXTRA_OBJS += user/cpptest_cxxblob.o user/libcpplib_blob.o \
                       user/libstdcxx_blob.o user/libgccs_blob.o
  endif

  # §M43: the on-device C compiler (make tcc) — the tcc binary + a rootfs
  # archive (tcc/musl headers + crt) unpacked into the VFS at boot.
  ifneq ($(wildcard third_party/tinycc-i686/bin/tcc),)
    ARCH_EXTRA_OBJS += user/dostcc_blob.o user/rootfs_blob.o
  endif

  # Tier B — in-tree user libc build knobs (i386 reference).
  USER_CFLAGS   := -m32 -ffreestanding -fno-pie -fno-stack-protector \
                   -fno-builtin -nostdlib -Os -Wall -std=c11 -Iuser
  USER_LDEMU    := -m elf_i386
  USER_BASE     := 0x40000000
  USER_OCARGS   := --output-target=elf32-i386 --binary-architecture=i386
  USER_CRT0_BUILD = nasm -f elf32 user/crt0.s -o $(OBJ_DIR)/user/crt0.o
  # The canonical PT_INTERP path an i386 musl dynamic binary carries.
  DOS_LDSO      := /lib/ld-musl-i386.so.1

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
      kernel/hal/x86_64/linux_abi.c \
      kernel/hal/x86_64/fork.c \
      kernel/hal/x86/lapic.c \
      kernel/hal/x86/ioapic.c \
      kernel/hal/x86/pci.c \
      kernel/hal/x86/fb_present.c \
      kernel/hal/x86/ringtest.c

  ARCH_ASM_SRCS := \
      kernel/hal/x86_64/boot.s \
      kernel/hal/x86_64/isr_stubs.s \
      kernel/hal/x86_64/switch.s \
      kernel/hal/x86_64/usermode.s \
      kernel/hal/x86_64/syscall_entry.s

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

  # §M36/§M37 (x86_64) — the musl userland for x86_64 comes from the PREBUILT
  # musl.cc cross-toolchain (third_party/musl-cross-x86_64), whose sysroot IS a
  # complete x86_64 musl (static libc.a + shared libc.so=ld.so + crt + a musl
  # libstdc++).  No separate `make musl` needed — the prebuilt sysroot is it.
  MUSL_SYSROOT := third_party/musl-cross-x86_64/x86_64-linux-musl
  MUSL_ELF_CC  := third_party/musl-cross-x86_64/bin/x86_64-linux-musl-gcc
  MUSL_ELF_CXX := third_party/musl-cross-x86_64/bin/x86_64-linux-musl-g++
  # The canonical PT_INTERP path an x86_64 musl dynamic binary carries; pkg.c
  # provisions the ld.so (== libc.so) there at boot.
  DOS_LDSO     := /lib/ld-musl-x86_64.so.1
  ifneq ($(wildcard $(MUSL_SYSROOT)/lib/libc.a),)
    ARCH_EXTRA_OBJS += user/muslhello_muslblob.o user/forktest64_muslblob.o
  endif
  # §M37 dynamic-linking artifacts (x86_64): the shared libc.so=ld.so blob +
  # dynamically-linked test programs.  The prebuilt sysroot's libc.so IS the
  # dynamic linker, so the same wildcard gate as the static libc.a applies.
  ifneq ($(wildcard $(MUSL_SYSROOT)/lib/libc.so),)
    ARCH_EXTRA_OBJS += user/ldmusl_blob.o user/muslhellodyn_dynblob.o \
                       user/libgreet_blob.o user/solibtest_dynblob.o \
                       user/dlopentest_dynblob.o
  endif
  # §M38 C++ runtime (x86_64) — the prebuilt sysroot ships a musl libstdc++.so.6
  # + libgcc_s.so.1, so cpptest (exceptions across a .so) works once the g++
  # driver exists.  Same artifacts as i386.
  ifneq ($(wildcard $(MUSL_ELF_CXX)),)
    ARCH_EXTRA_OBJS += user/cpptest_cxxblob.o user/libcpplib_blob.o \
                       user/libstdcxx_blob.o user/libgccs_blob.o
  endif
  # §M38 support libs (toward NetSurf), x86_64.  zlib → a store package
  # (libz.so.1) + a dyn test.  Present only once the vendored source is fetched
  # (scripts/fetch-zlib.sh) and the musl toolchain exists.
  ifneq ($(wildcard third_party/zlib/zlib.h),)
    ARCH_EXTRA_OBJS += user/libz_blob.o user/ztest_dynblob.o
  endif
  # libpng → a store package (libpng16.so.16) whose closure pins zlib.
  ifneq ($(wildcard third_party/libpng/png.h),)
    ARCH_EXTRA_OBJS += user/libpng16_blob.o user/pngtest_dynblob.o
  endif
  # freetype → a store package (libfreetype.so.6).  Guarded on the PREBUILT .so
  # (not the source) because compiling FreeType's ~40 amalgamated modules under
  # amd64 emulation is minutes-long — build it once with `make freetype`, then
  # it is embedded; a plain build never triggers the slow compile.
  ifneq ($(wildcard user/libfreetype.so.6),)
    ARCH_EXTRA_OBJS += user/libfreetype6_blob.o user/fttest_dynblob.o
  endif
  # harfbuzz → a big C++ store package (libharfbuzz.so.0, DT_NEEDED
  # libstdc++.so.6).  Prebuilt-.so guard, same slow-C++-build reasoning.
  ifneq ($(wildcard user/libharfbuzz.so.0),)
    ARCH_EXTRA_OBJS += user/libharfbuzz0_blob.o user/hbtest_dynblob.o
  endif
  # §M42 — NetSurf's own component libraries, each a store package (built from
  # git.netsurf-browser.org sources; scripts/fetch-netsurf-libs.sh).
  ifneq ($(wildcard third_party/libwapcaplet/src/libwapcaplet.c),)
    ARCH_EXTRA_OBJS += user/libwapcaplet0_blob.o user/wctest_dynblob.o
  endif
  ifneq ($(wildcard third_party/libparserutils/Makefile),)
    ARCH_EXTRA_OBJS += user/libparserutils0_blob.o user/putest_dynblob.o
  endif
  ifneq ($(wildcard third_party/libhubbub/Makefile),)
    ARCH_EXTRA_OBJS += user/libhubbub0_blob.o user/hbbtest_dynblob.o
  endif
  ifneq ($(wildcard third_party/libnsgif/src/gif.c),)
    ARCH_EXTRA_OBJS += user/libnsgif0_blob.o user/gtest_dynblob.o
  endif
  # libcss — CSS engine; heavy codegen (gen_parser host tool + per-property
  # parsers + python select generator) → slow, guarded on the PREBUILT .so
  # (run `make libcss`).
  ifneq ($(wildcard user/libcss.so.0),)
    ARCH_EXTRA_OBJS += user/libcss0_blob.o user/csstest_dynblob.o
  endif

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
    kernel/gui/wayland.c \
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
    kernel/core/futex.c \
    kernel/core/pkg.c \
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
    kernel/mem/slab.c \
    kernel/core/random.c
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
    kernel/gui/wayland.c \
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
    kernel/gui/wayland.c \
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
    kernel/core/net.c \
    kernel/core/audio.c \
    kernel/core/futex.c \
    kernel/core/pkg.c \
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
    kernel/mem/slab.c \
    kernel/core/random.c
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

.PHONY: all kernel iso run clean clean-all musl musl-clean \
        musl-cross-i686 musl-cross-x86_64 mbedtls tcc

all: $(KERNEL_BIN)

# -----------------------------------------------------------------------------
# musl (§M36 stage 2 + §M37) — build the vendored, PRISTINE musl as an i386 libc,
# BOTH static (libc.a) AND shared (libc.so).
#
# musl is fetched (not committed) by scripts/fetch-musl.sh into third_party/musl
# and built here into third_party/musl-i386/ (also gitignored).  We do NOT patch
# musl — d-os provides the Linux i386 syscall ABI it targets (linux_abi.c).  Run
# this INSIDE the build container (gcc-multilib), e.g.:
#     docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build make musl
#
# §M36 needs the static libc.a (statically-linked musl programs — muslhello,
# coreutils).  §M37 (dynamic linking) additionally needs the SHARED build:
# musl's libc.so IS the dynamic linker (the interpreter /lib/ld-musl-i386.so.1
# is a symlink to libc.so), so `--enable-shared` (musl's default — we simply
# stopped passing --disable-shared) yields both:
#   lib/libc.a               — static archive (§M36)
#   lib/libc.so              — shared library == the dynamic linker (§M37)
#   lib/ld-musl-i386.so.1    — symlink → libc.so (the PT_INTERP target)
# Produces third_party/musl-i386/lib/{libc.a,libc.so,crt1.o,Scrt1.o,crti.o,
# crtn.o} + include/.
# -----------------------------------------------------------------------------
MUSL_SRC    := third_party/musl
MUSL_PREFIX := third_party/musl-i386
MUSL_LIBC   := $(MUSL_PREFIX)/lib/libc.a
MUSL_LIBSO  := $(MUSL_PREFIX)/lib/libc.so

musl: $(MUSL_LIBSO)

# The shared library is the newer artifact; depending on it (and having its
# recipe also produce libc.a) makes `make musl` build both in one configure.
$(MUSL_LIBSO): $(MUSL_LIBC)

$(MUSL_LIBC):
	@test -f $(MUSL_SRC)/configure || { \
	  echo "musl source missing — run ./scripts/fetch-musl.sh first"; exit 1; }
	cd $(MUSL_SRC) && CC='gcc -m32' ./configure \
	    --target=i386 --prefix=$(CURDIR)/$(MUSL_PREFIX) \
	    AR=ar RANLIB=ranlib

	$(MAKE) -C $(MUSL_SRC) -j
	$(MAKE) -C $(MUSL_SRC) install
	@echo "musl i386 libc (static + shared) built → $(MUSL_PREFIX)/lib/"

musl-clean:
	-$(MAKE) -C $(MUSL_SRC) clean 2>/dev/null || true
	rm -rf $(MUSL_PREFIX)

# -----------------------------------------------------------------------------
# musl C++ cross-toolchain (§M38) — build a from-source gcc/g++ + binutils +
# musl that TARGETS musl, so we get a musl libstdc++ + libgcc (with DWARF
# exception unwinding) for d-os.  Fetched by scripts/fetch-musl-cross.sh; built
# INSIDE the container (needs network + wget to pull gcc/binutils sources):
#     docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build make musl-cross-i686
# Produces third_party/musl-cross-i686/bin/i686-linux-musl-{gcc,g++,...} and a
# musl sysroot with libstdc++.so.6 + libgcc_s.so.1.  Long build (gcc from src).
# -----------------------------------------------------------------------------
# NB: gcc is built on the CONTAINER's native filesystem (/tmp), NOT on the
# Docker-mounted host volume.  Two reasons: (1) tar's directory-metadata restore
# fails on the macOS virtiofs mount ("Directory renamed before its status could
# be extracted") when extracting the linux-headers tarball; (2) a mounted-volume
# gcc build is painfully slow.  We copy the (fetched, source-cached) tree to
# /tmp, build there, then copy just the finished toolchain back to the mount.
MCM_DIR := third_party/musl-cross-make
define MUSL_CROSS_BUILD
	@test -f $(MCM_DIR)/Makefile || { \
	  echo "musl-cross-make missing — run ./scripts/fetch-musl-cross.sh first"; exit 1; }
	rm -rf /tmp/mcm && cp -a $(MCM_DIR) /tmp/mcm
	$(MAKE) -C /tmp/mcm TARGET=$(1) OUTPUT=/tmp/mcm/out install
	rm -rf third_party/musl-cross-$(2)
	cp -a /tmp/mcm/out third_party/musl-cross-$(2)
	rm -rf /tmp/mcm
	@echo "$(2) musl C++ toolchain → third_party/musl-cross-$(2)/bin/"
endef

musl-cross-i686:
	$(call MUSL_CROSS_BUILD,i686-linux-musl,i686)

musl-cross-x86_64:
	$(call MUSL_CROSS_BUILD,x86_64-linux-musl,x86_64)

# -----------------------------------------------------------------------------
# Mbed TLS (§M39 stage 2) — build the vendored crypto/TLS library for i686-musl.
# Pure C: compiled with the host gcc -m32 + our musl headers (same path as the
# musl coreutils), NOT the C++ toolchain.  Built on the container-local fs (the
# PSA driver-wrapper generation writes many files; keep it off the slow mount).
# The image must have python3-jsonschema/jinja2 (Dockerfile, §M39) for the PSA
# wrapper generation.  Produces third_party/mbedtls-i686/{lib,include}.
#     docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build make mbedtls
# -----------------------------------------------------------------------------
MBEDTLS_DIR    := third_party/mbedtls
MBEDTLS_PREFIX := third_party/mbedtls-i686
MBEDTLS_CFLAGS := -I$(CURDIR)/$(MUSL_PREFIX)/include -Os -fno-stack-protector -w

mbedtls:
	@test -f $(MBEDTLS_DIR)/Makefile || { \
	  echo "Mbed TLS missing — run ./scripts/fetch-mbedtls.sh first"; exit 1; }
	rm -rf /tmp/mb && cp -a $(MBEDTLS_DIR) /tmp/mb
	$(MAKE) -C /tmp/mb/library CC='gcc -m32' CFLAGS='$(MBEDTLS_CFLAGS)' \
	    libmbedcrypto.a libmbedx509.a libmbedtls.a
	rm -rf $(MBEDTLS_PREFIX)
	mkdir -p $(MBEDTLS_PREFIX)/lib
	cp /tmp/mb/library/lib*.a $(MBEDTLS_PREFIX)/lib/
	cp -a /tmp/mb/include $(MBEDTLS_PREFIX)/include
	rm -rf /tmp/mb
	@echo "Mbed TLS i686 libs → $(MBEDTLS_PREFIX)/lib/"

# -----------------------------------------------------------------------------
# TinyCC (§M43) — an on-device C compiler.  Cross-built with the musl C++
# toolchain so the `tcc` binary is an i686-musl ELF that RUNS on d-os (under
# §M37) and compiles C → runnable ELF on d-os.  Built on the container-local fs;
# the interpreter is provisioned into the build container's /lib so qemu-i386
# binfmt can run the musl build helpers.  PROVEN: `make tcc` builds the compiler
# and it compiles a .c to a valid i386 .o under emulation.  (libtcc1.a + the
# on-d-os provisioning of headers/crt/libc + a `tcc` shell command + the Editor
# "Compile & Run" button are the remaining §M43 steps — see fetch-tinycc.sh.)
#     docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build make tcc
# -----------------------------------------------------------------------------
TINYCC_DIR := third_party/tinycc
tcc:
	@test -f $(TINYCC_DIR)/configure || { \
	  echo "TinyCC missing — run ./scripts/fetch-tinycc.sh first"; exit 1; }
	@test -x $(MUSL_CXX_DIR)/bin/i686-linux-musl-gcc || { \
	  echo "musl toolchain missing — run make musl-cross-i686 first"; exit 1; }
	cp $(MUSL_CXX_DIR)/i686-linux-musl/lib/libc.so /lib/ld-musl-i386.so.1
	# Clean musl headers at /usr/include so tcc (built here, run under qemu-i386)
	# can compile libtcc1.a — the host's glibc /usr/include otherwise conflicts.
	# (The musl-cross gcc is unaffected: it uses its own sysroot.)
	rm -rf /usr/include && cp -a $(CURDIR)/$(MUSL_PREFIX)/include /usr/include
	rm -rf /tmp/tcc && cp -a $(TINYCC_DIR) /tmp/tcc
	cd /tmp/tcc && PATH=$(CURDIR)/$(MUSL_CXX_DIR)/bin:$$PATH \
	    CC=i686-linux-musl-gcc ./configure --cpu=i386 --config-musl \
	      --elfinterp=/lib/ld-musl-i386.so.1 --crtprefix=/lib --libpaths="{B}:/lib" \
	      --sysincludepaths="{B}/include:/usr/include" --prefix=/usr \
	      --extra-cflags="-fPIE" --extra-ldflags="-pie" --config-pie \
	      --config-bcheck=no --config-backtrace=no
	cd /tmp/tcc && PATH=$(CURDIR)/$(MUSL_CXX_DIR)/bin:$$PATH $(MAKE) tcc libtcc1.a
	rm -rf third_party/tinycc-i686 && mkdir -p third_party/tinycc-i686/bin third_party/tinycc-i686/lib
	cp /tmp/tcc/tcc third_party/tinycc-i686/bin/tcc
	cp /tmp/tcc/libtcc1.a third_party/tinycc-i686/lib/libtcc1.a
	cp -a /tmp/tcc/include third_party/tinycc-i686/include
	rm -rf /tmp/tcc
	@echo "tcc (on-device C compiler) → third_party/tinycc-i686/bin/tcc"

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

user/forktest_$(ARCH).elf: user/libc.c user/forktest.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/forktest.c -o $(OBJ_DIR)/user/forktest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/forktest.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/forktest_blob.o: user/forktest_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

user/forkexec_$(ARCH).elf: user/libc.c user/forkexec.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/forkexec.c -o $(OBJ_DIR)/user/forkexec.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/forkexec.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/forkexec_blob.o: user/forkexec_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

user/pipetest_$(ARCH).elf: user/libc.c user/pipetest.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/pipetest.c -o $(OBJ_DIR)/user/pipetest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/pipetest.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/pipetest_blob.o: user/pipetest_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

user/sigtest_$(ARCH).elf: user/libc.c user/sigtest.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c    -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/sigtest.c -o $(OBJ_DIR)/user/sigtest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/sigtest.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/sigtest_blob.o: user/sigtest_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

user/dnstest_$(ARCH).elf: user/libc.c user/dnstest.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c    -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/dnstest.c -o $(OBJ_DIR)/user/dnstest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/dnstest.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/dnstest_blob.o: user/dnstest_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

user/httptest_$(ARCH).elf: user/libc.c user/httptest.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/httptest.c -o $(OBJ_DIR)/user/httptest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/httptest.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/httptest_blob.o: user/httptest_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

user/threadtest_$(ARCH).elf: user/libc.c user/threadtest.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c       -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/threadtest.c -o $(OBJ_DIR)/user/threadtest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/threadtest.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/threadtest_blob.o: user/threadtest_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

user/tlstest_$(ARCH).elf: user/libc.c user/tlstest.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c    -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/tlstest.c -o $(OBJ_DIR)/user/tlstest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/tlstest.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/tlstest_blob.o: user/tlstest_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

user/posixtest_$(ARCH).elf: user/libc.c user/posixtest.c user/libc.h
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c      -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/posixtest.c -o $(OBJ_DIR)/user/posixtest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/posixtest.o $(OBJ_DIR)/user/libc.o

$(OBJ_DIR)/user/posixtest_blob.o: user/posixtest_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

# Standalone Linux-ABI test program — NO d-os crt0/libc (entry = _start), uses
# Linux syscall numbers directly.  Run under the Linux personality.
user/linuxhello_$(ARCH).elf: user/linuxhello.c
	@mkdir -p $(OBJ_DIR)/user
	$(CC) $(USER_CFLAGS) -c user/linuxhello.c -o $(OBJ_DIR)/user/linuxhello.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ $(OBJ_DIR)/user/linuxhello.o

$(OBJ_DIR)/user/linuxhello_blob.o: user/linuxhello_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

# wlclient — a freestanding NATIVE-ABI ring-3 Wayland client (int 0x80 with d-os
# syscall numbers, no libc); speaks the Wayland wire protocol over fd 3.
user/wlclient_$(ARCH).elf: user/wlclient.c
	@mkdir -p $(OBJ_DIR)/user
	$(CC) $(USER_CFLAGS) -c user/wlclient.c -o $(OBJ_DIR)/user/wlclient.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ $(OBJ_DIR)/user/wlclient.o

$(OBJ_DIR)/user/wlclient_blob.o: user/wlclient_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

# wlapp — a ring-3 app that speaks Wayland via the libwl client library.
user/wlapp_$(ARCH).elf: user/wlapp.c user/libwl.c user/libwl.h
	@mkdir -p $(OBJ_DIR)/user
	$(CC) $(USER_CFLAGS) -c user/wlapp.c -o $(OBJ_DIR)/user/wlapp.o
	$(CC) $(USER_CFLAGS) -c user/libwl.c -o $(OBJ_DIR)/user/libwl.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/wlapp.o $(OBJ_DIR)/user/libwl.o

$(OBJ_DIR)/user/wlapp_blob.o: user/wlapp_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

# NORMAL C programs linked against REAL, pristine musl (need `make musl` first;
# blobs wired in only when musl-i386/lib/libc.a exists, see the i386 block).
# Compiled with musl's headers, statically linked with musl crt1/crti/libc.a/
# crtn into a stock Linux i386 ELF, relocated to the d-os user base via
# -Ttext-segment (moves the ELF headers too → one contiguous image below the
# user stack) + libgcc (musl printf pulls in the 64-bit __udivmoddi4 helper).
# Linked with `ld` directly (no gcc PIE/spec interference).  Generic pattern:
# any user/<name>.c → user/<name>.muslelf → <name>_muslblob.o (symbol
# _binary_user_<name>_muslelf_start).  Add coreutils via MUSL_COREUTILS above.
MUSL_CC_FLAGS := -m32 -static -fno-pie -Os -Wall
ifeq ($(ARCH),x86_64)
# x86_64: the prebuilt musl.cc cross-gcc driver links crt1/crti/libc.a/libgcc/
# crtn itself; -static -no-pie + -Wl,-Ttext-segment relocates the whole image
# (ELF headers included) to the d-os user base — same trick the i386 rule uses.
user/%.muslelf: user/%.c $(MUSL_SYSROOT)/lib/libc.a
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -static -no-pie -Os -Wall \
	    -Wl,-Ttext-segment=$(USER_BASE) $< -o $@
else
user/%.muslelf: user/%.c $(MUSL_LIBC)
	@mkdir -p $(OBJ_DIR)/user
	gcc $(MUSL_CC_FLAGS) -c user/$*.c -I$(MUSL_PREFIX)/include \
	    -o $(OBJ_DIR)/user/$*.muslo
	ld -m elf_i386 -static -Ttext-segment=$(USER_BASE) -e _start -o $@ \
	    $(MUSL_PREFIX)/lib/crt1.o $(MUSL_PREFIX)/lib/crti.o \
	    $(OBJ_DIR)/user/$*.muslo \
	    --start-group $(MUSL_PREFIX)/lib/libc.a \
	    `gcc -m32 -print-libgcc-file-name` --end-group \
	    $(MUSL_PREFIX)/lib/crtn.o
endif

# §M39 stage 2 — crypttest overrides the generic %.muslelf rule to ALSO compile
# with Mbed TLS's headers and link its static libs (libmbedcrypto for the
# crypto primitives; x509+tls linked too so the same rule serves stage 3).
user/crypttest.muslelf: user/crypttest.c $(MUSL_LIBC)
	@mkdir -p $(OBJ_DIR)/user
	gcc $(MUSL_CC_FLAGS) -c user/crypttest.c \
	    -I$(MUSL_PREFIX)/include -I$(MBEDTLS_PREFIX)/include \
	    -o $(OBJ_DIR)/user/crypttest.muslo
	ld -m elf_i386 -static -Ttext-segment=$(USER_BASE) -e _start -o $@ \
	    $(MUSL_PREFIX)/lib/crt1.o $(MUSL_PREFIX)/lib/crti.o \
	    $(OBJ_DIR)/user/crypttest.muslo \
	    --start-group \
	    $(MBEDTLS_PREFIX)/lib/libmbedtls.a $(MBEDTLS_PREFIX)/lib/libmbedx509.a \
	    $(MBEDTLS_PREFIX)/lib/libmbedcrypto.a $(MUSL_PREFIX)/lib/libc.a \
	    `gcc -m32 -print-libgcc-file-name` --end-group \
	    $(MUSL_PREFIX)/lib/crtn.o

# §M39 stage 3 — ssltest: same mbedTLS static link (SSL/x509/crypto), in-memory
# TLS handshake.  (Own rule so it also pulls the SSL + x509 objects.)
user/ssltest.muslelf: user/ssltest.c $(MUSL_LIBC)
	@mkdir -p $(OBJ_DIR)/user
	gcc $(MUSL_CC_FLAGS) -c user/ssltest.c \
	    -I$(MUSL_PREFIX)/include -I$(MBEDTLS_PREFIX)/include \
	    -o $(OBJ_DIR)/user/ssltest.muslo
	ld -m elf_i386 -static -Ttext-segment=$(USER_BASE) -e _start -o $@ \
	    $(MUSL_PREFIX)/lib/crt1.o $(MUSL_PREFIX)/lib/crti.o \
	    $(OBJ_DIR)/user/ssltest.muslo \
	    --start-group \
	    $(MBEDTLS_PREFIX)/lib/libmbedtls.a $(MBEDTLS_PREFIX)/lib/libmbedx509.a \
	    $(MBEDTLS_PREFIX)/lib/libmbedcrypto.a $(MUSL_PREFIX)/lib/libc.a \
	    `gcc -m32 -print-libgcc-file-name` --end-group \
	    $(MUSL_PREFIX)/lib/crtn.o

$(OBJ_DIR)/user/%_muslblob.o: user/%.muslelf
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

# §M37 — DYNAMICALLY-linked musl programs.  Same compile, but linked as a PIE
# (-pie) against the SHARED libc.so with the musl dynamic linker as PT_INTERP
# (/lib/ld-musl-i386.so.1).  Scrt1.o is the PIC/PIE crt0.  We link libc by name
# (-L…lib -lc) NOT by full path, so DT_NEEDED records "libc.so" (a clean soname
# the on-target ld.so can resolve), not a build path.  The kernel loads the main
# object at the user base and the interpreter clear of it, then jumps to ld.so
# (see proc.c) — ld.so does all relocation + symbol resolution in ring 3.
# Generic: user/<name>.c → user/<name>.dynelf → <name>_dynblob.o
# (symbol _binary_user_<name>_dynelf_start).
MUSL_DYN_CFLAGS := $(MUSL_CC_FLAGS:-static=) -fPIC -fno-stack-protector
ifeq ($(ARCH),x86_64)
# x86_64: the cross-gcc driver links Scrt1/crti/libc.so/libgcc/crtn as a PIE and
# stamps PT_INTERP = $(DOS_LDSO).  DT_NEEDED "libc.so" is recorded by soname.
user/%.dynelf: user/%.c $(MUSL_SYSROOT)/lib/libc.so
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall \
	    -Wl,-dynamic-linker,$(DOS_LDSO) $< -o $@
else
user/%.dynelf: user/%.c $(MUSL_LIBSO)
	@mkdir -p $(OBJ_DIR)/user
	gcc $(MUSL_DYN_CFLAGS) -c user/$*.c -I$(MUSL_PREFIX)/include \
	    -o $(OBJ_DIR)/user/$*.dyno
	ld -m elf_i386 -pie -dynamic-linker /lib/ld-musl-i386.so.1 -e _start -o $@ \
	    $(MUSL_PREFIX)/lib/Scrt1.o $(MUSL_PREFIX)/lib/crti.o \
	    $(OBJ_DIR)/user/$*.dyno \
	    -L$(MUSL_PREFIX)/lib --start-group -lc \
	    `gcc -m32 -print-libgcc-file-name` --end-group \
	    $(MUSL_PREFIX)/lib/crtn.o
endif

$(OBJ_DIR)/user/%_dynblob.o: user/%.dynelf
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

# -----------------------------------------------------------------------------
# §M38 — C++ programs, built with the musl C++ cross-toolchain (g++ 11.2.0).
# Compiled -fPIE and linked -pie so they are ET_DYN, which the §M37 loader
# relocates to the user base (a non-PIE EXEC would land at 0x08048000, inside
# the kernel region).  PT_INTERP = /lib/ld-musl-i386.so.1 (our provisioned
# musl); DT_NEEDED = libstdc++.so.6 + libgcc_s.so.1 + libc.so (+ any app .so),
# which ld.so resolves from /lib (pkg.c provisions them there at boot).
# -----------------------------------------------------------------------------
ifeq ($(ARCH),x86_64)
MUSL_CXX        := $(MUSL_ELF_CXX)
MUSL_CXX_STRIP  := third_party/musl-cross-x86_64/bin/x86_64-linux-musl-strip
MUSL_CXX_SYSLIB := $(MUSL_SYSROOT)/lib
else
MUSL_CXX_DIR    := third_party/musl-cross-i686
MUSL_CXX        := $(MUSL_CXX_DIR)/bin/i686-linux-musl-g++
MUSL_CXX_STRIP  := $(MUSL_CXX_DIR)/bin/i686-linux-musl-strip
MUSL_CXX_SYSLIB := $(MUSL_CXX_DIR)/i686-linux-musl/lib
endif
CXXFLAGS_DOS    := -Os -fPIC

# The C++ shared library that throws (libcpplib.so → /lib).
user/libcpplib.so: user/cpplib.cpp $(MUSL_CXX)
	$(MUSL_CXX) $(CXXFLAGS_DOS) -shared -Wl,-soname,libcpplib.so -o $@ user/cpplib.cpp
	-$(MUSL_CXX_STRIP) $@

# The C++ test program (PIE, links libcpplib by name → DT_NEEDED libcpplib.so).
# PT_INTERP is stamped explicitly to the arch's provisioned musl ld.so.
user/cpptest.cxxelf: user/cpptest.cpp user/libcpplib.so $(MUSL_CXX)
	$(MUSL_CXX) -Os -fPIE -pie -Wl,-dynamic-linker,$(DOS_LDSO) \
	    -o $@ user/cpptest.cpp -Luser -lcpplib
	-$(MUSL_CXX_STRIP) $@

# Stage stripped copies of the runtime .so's with clean names for objcopy
# (→ _binary_user_libstdcxx_so_start / _binary_user_libgccs_so_start).
user/libstdcxx.so: $(MUSL_CXX)
	cp $(MUSL_CXX_SYSLIB)/libstdc++.so.6 $@
	-$(MUSL_CXX_STRIP) $@
user/libgccs.so: $(MUSL_CXX)
	cp $(MUSL_CXX_SYSLIB)/libgcc_s.so.1 $@
	-$(MUSL_CXX_STRIP) $@

$(OBJ_DIR)/user/cpptest_cxxblob.o: user/cpptest.cxxelf
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@
$(OBJ_DIR)/user/libcpplib_blob.o: user/libcpplib.so
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@
$(OBJ_DIR)/user/libstdcxx_blob.o: user/libstdcxx.so
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@
$(OBJ_DIR)/user/libgccs_blob.o: user/libgccs.so
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

# §M37 — the musl dynamic linker == the shared libc.so, embedded so the kernel
# can install it at $(DOS_LDSO) at boot (pkg.c ldso_provision).  We stage a copy
# named `ldmusl.so` so objcopy derives the clean symbol
# _binary_user_ldmusl_so_start (path-based naming) — matching pkg.c's externs.
# The libc.so source is the arch's musl (i386: built musl-i386; x86_64: prebuilt
# musl.cc sysroot).
ifeq ($(ARCH),x86_64)
LDSO_SRC := $(MUSL_SYSROOT)/lib/libc.so
else
LDSO_SRC := $(MUSL_LIBSO)
endif
$(OBJ_DIR)/user/ldmusl_blob.o: $(LDSO_SRC)
	@mkdir -p $(@D)
	cp $(LDSO_SRC) user/ldmusl.so
	objcopy --input-target=binary $(USER_OCARGS) user/ldmusl.so $@
	rm -f user/ldmusl.so

# §M43 — the on-device C compiler: embed the tcc binary + a rootfs archive
# (tcc's own headers → /usr/lib/tcc/include, musl headers → /usr/include, musl
# crt → /lib) that pkg.c unpacks into the VFS at boot, so `tcc hello.c -o hello`
# can compile + link a full libc program ON d-os.
$(OBJ_DIR)/user/dostcc_blob.o: third_party/tinycc-i686/bin/tcc
	@mkdir -p $(@D)
	cp third_party/tinycc-i686/bin/tcc user/dostcc
	objcopy --input-target=binary --output-target=elf32-i386 \
	    --binary-architecture=i386 user/dostcc $@
	rm -f user/dostcc

user/rootfs.bin: third_party/tinycc-i686/bin/tcc $(MUSL_LIBC) user/tcc_hello.c user/hi.c
	python3 scripts/pack-rootfs.py $@ \
	    third_party/tinycc-i686/include:/usr/lib/tcc/include \
	    third_party/tinycc-i686/lib/libtcc1.a:/usr/lib/tcc/libtcc1.a \
	    $(MUSL_PREFIX)/include:/usr/include \
	    $(MUSL_PREFIX)/lib/crt1.o:/lib/crt1.o \
	    $(MUSL_PREFIX)/lib/crti.o:/lib/crti.o \
	    $(MUSL_PREFIX)/lib/crtn.o:/lib/crtn.o \
	    user/tcc_hello.c:/hello.c \
	    user/hi.c:/hi.c

$(OBJ_DIR)/user/rootfs_blob.o: user/rootfs.bin
	@mkdir -p $(@D)
	objcopy --input-target=binary --output-target=elf32-i386 \
	    --binary-architecture=i386 user/rootfs.bin $@

# §M37 stage 5 — a genuinely SEPARATE shared library (libgreet.so) + a program
# that links against it by name.  libgreet.so is embedded as a blob (installed
# at /lib/libgreet.so by pkg.c) so ld.so can resolve the program's DT_NEEDED
# "libgreet.so" via the /lib search path at runtime.
ifeq ($(ARCH),x86_64)
user/libgreet.so: user/libgreet.c $(MUSL_SYSROOT)/lib/libc.so
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -shared -fPIC -Os -Wall -Wl,-soname,libgreet.so \
	    user/libgreet.c -o $@
else
user/libgreet.so: user/libgreet.c $(MUSL_LIBSO)
	@mkdir -p $(OBJ_DIR)/user
	gcc -m32 -fPIC -Os -Wall -c user/libgreet.c -I$(MUSL_PREFIX)/include \
	    -o $(OBJ_DIR)/user/libgreet.o
	ld -m elf_i386 -shared -soname libgreet.so -o $@ \
	    $(OBJ_DIR)/user/libgreet.o \
	    -L$(MUSL_PREFIX)/lib --start-group -lc \
	    `gcc -m32 -print-libgcc-file-name` --end-group
endif

$(OBJ_DIR)/user/libgreet_blob.o: user/libgreet.so
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

# solibtest overrides the generic %.dynelf rule to also link -lgreet (its
# DT_NEEDED then lists libgreet.so + libc.so).
ifeq ($(ARCH),x86_64)
user/solibtest.dynelf: user/solibtest.c user/libgreet.so $(MUSL_SYSROOT)/lib/libc.so
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -Wl,-dynamic-linker,$(DOS_LDSO) \
	    user/solibtest.c -Luser -lgreet -o $@
else
user/solibtest.dynelf: user/solibtest.c user/libgreet.so $(MUSL_LIBSO)
	@mkdir -p $(OBJ_DIR)/user
	gcc $(MUSL_DYN_CFLAGS) -c user/solibtest.c \
	    -I$(MUSL_PREFIX)/include -o $(OBJ_DIR)/user/solibtest.dyno
	ld -m elf_i386 -pie -dynamic-linker /lib/ld-musl-i386.so.1 -e _start -o $@ \
	    $(MUSL_PREFIX)/lib/Scrt1.o $(MUSL_PREFIX)/lib/crti.o \
	    $(OBJ_DIR)/user/solibtest.dyno \
	    -L$(OBJ_DIR)/user -Luser -lgreet \
	    -L$(MUSL_PREFIX)/lib --start-group -lc \
	    `gcc -m32 -print-libgcc-file-name` --end-group \
	    $(MUSL_PREFIX)/lib/crtn.o
endif

# -----------------------------------------------------------------------------
# §M38 support libs (toward NetSurf) — vendored C libraries cross-built against
# musl and installed into the content-addressed store as versioned, swappable
# packages (same principle as the runtime musl; see pkg.c).  zlib first: it is
# the foundational compression dep of freetype/libpng + NetSurf's gzip.
# -----------------------------------------------------------------------------
ZLIB_DIR  := third_party/zlib
ZLIB_SRCS := adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c \
             gzwrite.c infback.c inffast.c inflate.c inftrees.c trees.c \
             uncompr.c zutil.c

.PHONY: zlib
zlib: user/libz.so.1

# libz.so.1 — the shared library (soname libz.so.1), built straight from the
# vendored sources (no configure; zconf.h is generated by copying the .in, whose
# defaults are correct for a musl/Linux target).
user/libz.so.1: $(ZLIB_DIR)/zlib.h
	@[ -f $(ZLIB_DIR)/zconf.h ] || cp $(ZLIB_DIR)/zconf.h.in $(ZLIB_DIR)/zconf.h
	$(MUSL_ELF_CC) -shared -fPIC -Os -DHAVE_HIDDEN -Wl,-soname,libz.so.1 \
	    -o $@ $(addprefix $(ZLIB_DIR)/,$(ZLIB_SRCS))

$(OBJ_DIR)/user/libz_blob.o: user/libz.so.1
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

# ztest — a dyn program linked against zlib by path (records DT_NEEDED libz.so.1
# from its soname); ld.so resolves libz.so.1 from /lib (the store profile view).
user/ztest.dynelf: user/ztest.c user/libz.so.1
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(ZLIB_DIR) \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/ztest.c user/libz.so.1 -o $@

# libpng — the PNG codec, soname libpng16.so.16, depends on zlib (links libz.so.1
# → DT_NEEDED libz.so.1, so its store package's closure pins zlib).
LIBPNG_DIR  := third_party/libpng
LIBPNG_SRCS := png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c pngrio.c \
               pngrtran.c pngrutil.c pngset.c pngtrans.c pngwio.c pngwrite.c \
               pngwtran.c pngwutil.c

.PHONY: libpng
libpng: user/libpng16.so.16

user/libpng16.so.16: $(LIBPNG_DIR)/png.h user/libz.so.1
	@[ -f $(LIBPNG_DIR)/pnglibconf.h ] || cp $(LIBPNG_DIR)/scripts/pnglibconf.h.prebuilt $(LIBPNG_DIR)/pnglibconf.h
	$(MUSL_ELF_CC) -shared -fPIC -Os -I$(LIBPNG_DIR) -I$(ZLIB_DIR) \
	    -Wl,-soname,libpng16.so.16 -o $@ \
	    $(addprefix $(LIBPNG_DIR)/,$(LIBPNG_SRCS)) user/libz.so.1

$(OBJ_DIR)/user/libpng16_blob.o: user/libpng16.so.16
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/pngtest.dynelf: user/pngtest.c user/libpng16.so.16
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(LIBPNG_DIR) -I$(ZLIB_DIR) \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/pngtest.c \
	    user/libpng16.so.16 user/libz.so.1 -o $@

# freetype — the font rasteriser, soname libfreetype.so.6, depends on zlib.
# Built from ~40 amalgamated module sources (slow under emulation → its own
# target, not part of a normal build; the blob guard keys on the output .so).
FT_DIR   := third_party/freetype
FT_BASE  := ftsystem ftinit ftdebug ftbase ftbbox ftbitmap ftglyph ftmm ftpfr \
            ftstroke ftsynth fttype1 ftwinfnt ftgasp ftfstype ftcid ftbdf
FT_MODS  := autofit/autofit bdf/bdf cache/ftcache cff/cff cid/type1cid \
            gzip/ftgzip lzw/ftlzw pcf/pcf pfr/pfr psaux/psaux pshinter/pshinter \
            psnames/psnames raster/raster sfnt/sfnt smooth/smooth \
            truetype/truetype type1/type1 type42/type42 winfonts/winfnt sdf/sdf svg/svg
FT_SRCS  := $(addprefix $(FT_DIR)/src/base/,$(addsuffix .c,$(FT_BASE))) \
            $(addprefix $(FT_DIR)/src/,$(addsuffix .c,$(FT_MODS)))

.PHONY: freetype
freetype: user/libfreetype.so.6

user/libfreetype.so.6: $(FT_DIR)/include/ft2build.h user/libz.so.1
	$(MUSL_ELF_CC) -shared -fPIC -Os -DFT2_BUILD_LIBRARY -I$(FT_DIR)/include \
	    -I$(ZLIB_DIR) -Wl,-soname,libfreetype.so.6 -o $@ $(FT_SRCS) user/libz.so.1

$(OBJ_DIR)/user/libfreetype6_blob.o: user/libfreetype.so.6
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/fttest.dynelf: user/fttest.c user/libfreetype.so.6
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(FT_DIR)/include \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/fttest.c \
	    user/libfreetype.so.6 user/libz.so.1 -o $@

# harfbuzz — text shaping, soname libharfbuzz.so.0.  Built from the single
# amalgamated C++ TU (src/harfbuzz.cc); a C++ .so so its closure pins
# libstdc++.so.6.  Own target (slow C++ compile under emulation).
HB_DIR := third_party/harfbuzz

.PHONY: harfbuzz
harfbuzz: user/libharfbuzz.so.0

user/libharfbuzz.so.0: $(HB_DIR)/src/harfbuzz.cc
	$(MUSL_ELF_CXX) -shared -fPIC -Os -std=c++11 -DHB_NO_MT \
	    -fno-exceptions -fno-rtti -I$(HB_DIR)/src \
	    -Wl,-soname,libharfbuzz.so.0 -o $@ $(HB_DIR)/src/harfbuzz.cc

$(OBJ_DIR)/user/libharfbuzz0_blob.o: user/libharfbuzz.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/hbtest.dynelf: user/hbtest.c user/libharfbuzz.so.0
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(HB_DIR)/src \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/hbtest.c \
	    user/libharfbuzz.so.0 -o $@

# -----------------------------------------------------------------------------
# §M42 — NetSurf's own component libraries (store packages).  Built straight
# from the sources (their netsurf-buildsystem is bypassed — we just compile the
# .c set into a .so), cross-linked against musl + their pinned deps by path.
# -----------------------------------------------------------------------------
# -fcommon: these libs predate gcc 10's -fno-common default and rely on
# tentative-definition merging (e.g. libcss's `_ALIGNED`) — otherwise duplicate
# globals across TUs become "multiple definition" link errors.
NSLIB_CFLAGS := -fPIC -Os -w -DNDEBUG -fcommon

# libwapcaplet — string interning (single TU), no deps.
user/libwapcaplet.so.0: third_party/libwapcaplet/src/libwapcaplet.c
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -Ithird_party/libwapcaplet/include \
	    -Wl,-soname,libwapcaplet.so.0 -o $@ third_party/libwapcaplet/src/libwapcaplet.c

$(OBJ_DIR)/user/libwapcaplet0_blob.o: user/libwapcaplet.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/wctest.dynelf: user/wctest.c user/libwapcaplet.so.0
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -Ithird_party/libwapcaplet/include \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/wctest.c \
	    user/libwapcaplet.so.0 -o $@

# libparserutils — parsing building blocks (input streams, buffers, charset
# codecs), no deps.  The charset alias table (src/charset/aliases.inc) is
# perl-generated from build/Aliases first; then compile the whole src tree.
LPU_DIR := third_party/libparserutils
user/libparserutils.so.0: $(LPU_DIR)/Makefile
	@[ -f $(LPU_DIR)/src/charset/aliases.inc ] || ( cd $(LPU_DIR) && perl build/make-aliases.pl )
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -I$(LPU_DIR)/include -I$(LPU_DIR)/src \
	    -Wl,-soname,libparserutils.so.0 -o $@ $(shell find $(LPU_DIR)/src -name '*.c')

$(OBJ_DIR)/user/libparserutils0_blob.o: user/libparserutils.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/putest.dynelf: user/putest.c user/libparserutils.so.0
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(LPU_DIR)/include \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/putest.c \
	    user/libparserutils.so.0 -o $@

# libhubbub — the HTML5 parser/tokeniser, deps libparserutils.  Two generated
# tables first: entities.inc (perl) + autogenerated-element-type.c (gperf, then
# a sed to make the table static).  The gperf output is #included by
# element-type.c, so it is EXCLUDED from the standalone compile set.
LHB_DIR := third_party/libhubbub
user/libhubbub.so.0: $(LHB_DIR)/Makefile user/libparserutils.so.0
	@[ -f $(LHB_DIR)/src/tokeniser/entities.inc ] || ( cd $(LHB_DIR) && perl build/make-entities.pl )
	@[ -f $(LHB_DIR)/src/treebuilder/autogenerated-element-type.c ] || \
	    ( cd $(LHB_DIR)/src/treebuilder && gperf --output-file=aet.tmp element-type.gperf && \
	      sed -e 's/^\(const struct element_type_map\)/static \1/' aet.tmp > autogenerated-element-type.c && rm -f aet.tmp )
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -I$(LHB_DIR)/include -I$(LHB_DIR)/src \
	    -I$(LPU_DIR)/include -Wl,-soname,libhubbub.so.0 -o $@ \
	    $(shell find $(LHB_DIR)/src -name '*.c' ! -name 'autogenerated-*') user/libparserutils.so.0

$(OBJ_DIR)/user/libhubbub0_blob.o: user/libhubbub.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/hbbtest.dynelf: user/hbbtest.c user/libhubbub.so.0
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(LHB_DIR)/include \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/hbbtest.c \
	    user/libhubbub.so.0 user/libparserutils.so.0 -o $@

# libnsgif — GIF decoder (lzw + gif), no deps.
LNG_DIR := third_party/libnsgif
user/libnsgif.so.0: $(LNG_DIR)/src/gif.c
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -I$(LNG_DIR)/include -I$(LNG_DIR)/src \
	    -Wl,-soname,libnsgif.so.0 -o $@ $(shell find $(LNG_DIR)/src -name '*.c')

$(OBJ_DIR)/user/libnsgif0_blob.o: user/libnsgif.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/gtest.dynelf: user/gtest.c user/libnsgif.so.0
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(LNG_DIR)/include \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/gtest.c \
	    user/libnsgif.so.0 -o $@

# libcss — the CSS parser/selection engine.  Deps libwapcaplet + libparserutils.
# Heavy codegen first (own target, minutes under emulation): a host gen_parser
# tool builds a per-property parser for each line of properties.gen, and a
# python generator emits the select autogenerated_* tables.  Then the whole src
# tree (incl. the generated autogenerated_<prop>.c) compiles.
LWC_DIR  := third_party/libwapcaplet
LCSS_DIR := third_party/libcss
.PHONY: libcss
libcss: user/libcss.so.0
user/libcss.so.0: $(LCSS_DIR)/Makefile user/libwapcaplet.so.0 user/libparserutils.so.0
	cd $(LCSS_DIR) && gcc -o /tmp/gen_parser src/parse/properties/css_property_parser_gen.c && \
	  for p in $$(perl -pe'$$_="" unless /^([^\#][^:]+):/;$$_=$$1 . " "' src/parse/properties/properties.gen); do \
	      /tmp/gen_parser -o src/parse/properties/autogenerated_$$p.c "$$(grep "^$$p:" src/parse/properties/properties.gen)"; \
	  done && python3 src/select/select_generator.py >/dev/null
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -I$(LCSS_DIR)/include -I$(LCSS_DIR)/src \
	    -I$(LWC_DIR)/include -I$(LPU_DIR)/include -Wl,-soname,libcss.so.0 -o $@ \
	    $$(find $(LCSS_DIR)/src -name '*.c') user/libwapcaplet.so.0 user/libparserutils.so.0

$(OBJ_DIR)/user/libcss0_blob.o: user/libcss.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/csstest.dynelf: user/csstest.c user/libcss.so.0
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(LCSS_DIR)/include -I$(LWC_DIR)/include \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/csstest.c \
	    user/libcss.so.0 user/libwapcaplet.so.0 user/libparserutils.so.0 -o $@

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
	# User-space build artifacts live in user/ with arch-agnostic names (their
	# blob symbol names derive from the path), so a stale i386 .dynelf/.so can
	# shadow an x86_64 rebuild (and vice versa).  Wipe them on clean so an
	# ARCH switch always rebuilds them for the right target.
	rm -f user/*.muslelf user/*.dynelf user/*.cxxelf \
	      user/libgreet.so user/libcpplib.so user/libstdcxx.so \
	      user/libgccs.so user/ldmusl.so user/rootfs.bin user/libz.so.1 \
	      user/libpng16.so.16 user/libwapcaplet.so.0 user/libparserutils.so.0 \
	      user/libhubbub.so.0 user/libnsgif.so.0

clean-all:
	rm -rf build
