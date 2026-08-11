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

# -----------------------------------------------------------------------------
# Userland lists shared by the two x86 arches.
#
# WHY THESE ARE SHARED AND NOT COPIED PER ARCH: every build rule below is
# already $(ARCH)-parameterised, so the only thing that ever made x86_64's
# userland smaller than i386's was these lists being written out twice and one
# copy falling behind.  Keeping them in ONE place is what turns "x86_64 has
# parity" from a claim into a property of the build.
#
# Ring-3 programs built against the IN-TREE libc (user/libc.c), whose syscall
# shim covers all three arches.
X86_USER_BLOBS := user/hello_blob.o user/spin_blob.o user/wedge_blob.o \
                  user/args_blob.o user/forktest_blob.o user/forkexec_blob.o \
                  user/pipetest_blob.o user/sigtest_blob.o \
                  user/dnstest_blob.o user/httptest_blob.o \
                  user/threadtest_blob.o user/tlstest_blob.o \
                  user/posixtest_blob.o \
                  user/wlclient_blob.o user/wlapp_blob.o
# NB: linuxhello is deliberately NOT here — it hard-codes the i386 Linux syscall
# numbers and `int 0x80`, which is the whole point of that program (it mimics an
# unmodified 32-bit Linux binary).  The x86_64 equivalent would be a separate
# source using the SYSCALL instruction and the amd64 numbers.

# Coreutils linked against REAL musl.  The list is the modular seam: add a name
# here + a user/<name>.c + a recipe in pkg.c and both arches pick it up.
MUSL_COREUTILS := echo cat ls env sh
MUSL_COREUTIL_BLOBS := $(patsubst %,user/%_muslblob.o,$(MUSL_COREUTILS))
# Other musl-linked programs that are not coreutils (socket/network probes).
# wedgewin = a GUI client that opens a window and then freezes: the automated
# test for "the chrome still works when the app behind it is frozen".
MUSL_PROG_BLOBS := user/muslhello_muslblob.o user/netmusl_muslblob.o \
                   user/wedgewin_muslblob.o user/pthreadtest_muslblob.o \
                   user/epollmusl_muslblob.o
# §M37 dynamic-linking artifacts — the ld.so blob + the dynamically linked tests.
MUSL_DYN_BLOBS  := user/ldmusl_blob.o user/muslhellodyn_dynblob.o \
                   user/libgreet_blob.o user/solibtest_dynblob.o \
                   user/thrdyn_dynblob.o \
                   user/dlopentest_dynblob.o
# §M38 C++ runtime artifacts (cpptest = exceptions across a .so + the .so's).
CXX_RUNTIME_BLOBS := user/cpptest_cxxblob.o user/libcpplib_blob.o \
                     user/libstdcxx_blob.o user/libgccs_blob.o
# §M39 TLS test programs (link against the ported Mbed TLS).
MBEDTLS_PROG_BLOBS := user/crypttest_muslblob.o user/ssltest_muslblob.o \
                      user/httpstest_muslblob.o user/wget_muslblob.o

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
      kernel/hal/x86/fpu.c \
      kernel/hal/x86/vmm.c \
      kernel/hal/x86/fb_present.c \
      kernel/hal/x86/ringtest.c \
      kernel/hal/x86/pci.c \
      kernel/hal/x86/hal_arch.c \
      kernel/hal/x86/task_arch.c \
      kernel/hal/x86/lapic.c \
      kernel/hal/x86/tlb.c \
      kernel/hal/x86/tsc.c \
      kernel/hal/x86/ioapic.c \
      kernel/hal/x86/smp.c \
      kernel/hal/x86/syscall.c \
      kernel/hal/x86/uaccess.c \
      kernel/hal/x86/fork.c \
      kernel/hal/x86/signal.c \
      kernel/hal/x86/linux_abi.c

  ARCH_ASM_SRCS := \
      kernel/hal/x86/boot.s \
      kernel/hal/x86/isr_stubs.s \
      kernel/hal/x86/usermode.s \
      kernel/hal/x86/switch.s

  ARCH_EXTRA_OBJS := kernel/hal/x86/ap_trampoline_blob.o $(X86_USER_BLOBS) \
                     user/linuxhello_blob.o

  # REAL musl-linked programs are embedded ONLY when musl has been built
  # (`make musl`); otherwise the kernel builds without them.  This keeps the
  # default build independent of the (fetched, on-demand) musl toolchain.
  ifneq ($(wildcard third_party/musl-i386/lib/libc.a),)
    ARCH_EXTRA_OBJS += $(MUSL_PROG_BLOBS) $(MUSL_COREUTIL_BLOBS)
  endif

  # §M37: dynamic-linking artifacts need the SHARED musl (libc.so, produced by
  # the same `make musl`).  ldmusl = the dynamic linker itself (embedded so the
  # kernel can install it at /lib/ld-musl-i386.so.1); muslhellodyn = a
  # dynamically-linked test program (PT_INTERP set, PIE main).
  ifneq ($(wildcard third_party/musl-i386/lib/libc.so),)
    ARCH_EXTRA_OBJS += $(MUSL_DYN_BLOBS)
  endif

  # §M39 stage 2+3: crypttest + ssltest link against the ported Mbed TLS.
  # stage 3b: httpstest = real HTTPS over an M24 socket + the CA bundle
  # (third_party/cacert.pem, provisioned to /etc/ssl/cert.pem at boot).
  MBEDTLS_PREFIX := third_party/mbedtls-i686
  ifneq ($(wildcard $(MBEDTLS_PREFIX)/lib/libmbedcrypto.a),)
    ARCH_EXTRA_OBJS += $(MBEDTLS_PROG_BLOBS)
    ifneq ($(wildcard third_party/cacert.pem),)
      ARCH_EXTRA_OBJS += third_party/cacert_blob.o
    endif
  endif

  # §M38: C++ runtime artifacts, present only once the musl C++ toolchain was
  # built (make musl-cross-i686).  cpptest = the DoD (exceptions across a .so);
  # libcpplib/libstdcxx/libgccs are the .so's provisioned into /lib at boot.
  ifneq ($(wildcard third_party/musl-cross-i686/bin/i686-linux-musl-g++),)
    ARCH_EXTRA_OBJS += $(CXX_RUNTIME_BLOBS)
  endif

  # §M40 (i386) — the upstream libwayland client (`make wayland`).
  ifneq ($(wildcard third_party/wayland-i386/lib/libwayland-client.a),)
    ARCH_EXTRA_OBJS += user/wlupstream_muslblob.o
    ifneq ($(wildcard third_party/weston/clients/simple-shm.c),)
      ARCH_EXTRA_OBJS += user/simpleshm_muslblob.o
    endif
  endif
  # §M43: the on-device C compiler (make tcc) — the tcc binary + a rootfs
  # archive (tcc/musl headers + crt) unpacked into the VFS at boot.
  TINYCC_PREFIX := third_party/tinycc-i686
  ifneq ($(wildcard $(TINYCC_PREFIX)/bin/tcc),)
    ARCH_EXTRA_OBJS += user/dostcc_blob.o user/rootfs_blob.o
  endif

  # §M42 (i386) — the NetSurf browser stack, mirroring the x86_64 block below.
  # All guards are shared-path wildcards (source or prebuilt .so), so the same
  # conditions select the i386 builds; build.sh wipes user/*.so on arch switch,
  # so the prebuilt-.so guards only fire once each is rebuilt for i386.  NB:
  # NetSurf does NOT link harfbuzz directly (freetype is built without it), so
  # the slow C++ harfbuzz build is not required here.
  ifneq ($(wildcard third_party/zlib/zlib.h),)
    ARCH_EXTRA_OBJS += user/libz_blob.o
  endif
  ifneq ($(wildcard third_party/libpng/png.h),)
    ARCH_EXTRA_OBJS += user/libpng16_blob.o
  endif
  ifneq ($(wildcard user/libfreetype.so.6),)
    ARCH_EXTRA_OBJS += user/libfreetype6_blob.o
  endif
  ifneq ($(wildcard third_party/libwapcaplet/src/libwapcaplet.c),)
    ARCH_EXTRA_OBJS += user/libwapcaplet0_blob.o
  endif
  ifneq ($(wildcard third_party/libparserutils/Makefile),)
    ARCH_EXTRA_OBJS += user/libparserutils0_blob.o
  endif
  ifneq ($(wildcard third_party/libhubbub/Makefile),)
    ARCH_EXTRA_OBJS += user/libhubbub0_blob.o
  endif
  ifneq ($(wildcard third_party/libnsgif/src/gif.c),)
    ARCH_EXTRA_OBJS += user/libnsgif0_blob.o
  endif
  ifneq ($(wildcard third_party/libnsbmp/src/libnsbmp.c),)
    ARCH_EXTRA_OBJS += user/libnsbmp0_blob.o
  endif
  ifneq ($(wildcard user/libcss.so.0),)
    ARCH_EXTRA_OBJS += user/libcss0_blob.o
  endif
  ifneq ($(wildcard user/libdom.so.0),)
    ARCH_EXTRA_OBJS += user/libdom0_blob.o
  endif
  ifneq ($(wildcard third_party/libnsutils/src/base64.c),)
    ARCH_EXTRA_OBJS += user/libnsutils0_blob.o
  endif
  ifneq ($(wildcard third_party/libnslog/src/core.c),)
    ARCH_EXTRA_OBJS += user/libnslog0_blob.o
  endif
  ifneq ($(wildcard third_party/libnspsl/src/nspsl.c),)
    ARCH_EXTRA_OBJS += user/libnspsl0_blob.o
  endif
  ifneq ($(wildcard third_party/libnsfb/src/libnsfb.c),)
    ARCH_EXTRA_OBJS += user/libnsfb0_blob.o
  endif
  ifneq ($(wildcard user/netsurf.dynelf),)
    ARCH_EXTRA_OBJS += user/netsurf_dynblob.o user/netsurf_res_blob.o
  endif

  # Tier B — in-tree user libc build knobs (i386 reference).
  USER_CFLAGS   := -m32 -ffreestanding -fno-pie -fno-stack-protector \
                   -fno-builtin -nostdlib -Os -Wall -std=c11 -Iuser
  USER_LDEMU    := -m elf_i386
  USER_BASE     := 0x40000000
  USER_OCARGS   := --output-target=elf32-i386 --binary-architecture=i386
  USER_CRT0_SRC   = user/crt0.s
  USER_CRT0_BUILD = nasm -f elf32 $(USER_CRT0_SRC) -o $(OBJ_DIR)/user/crt0.o
  # The canonical PT_INTERP path an i386 musl dynamic binary carries.
  DOS_LDSO      := /lib/ld-musl-i386.so.1
  # Where this arch's musl headers + crt objects live (the §M43 rootfs archive
  # ships them to the on-device compiler).  i386 builds its own musl; x86_64
  # uses the prebuilt cross sysroot — same files, different home.
  MUSL_HDR_DIR  := third_party/musl-i386/include
  MUSL_CRT_DIR  := third_party/musl-i386/lib
  # TinyCC cross-build knobs (see the `tcc` target).
  TCC_CPU       := i386
  TCC_CC        := i686-linux-musl-gcc
  TCC_TOOLCHAIN := third_party/musl-cross-i686

  # §M42 i386 — the musl cross toolchain that builds the store .so's + the
  # NetSurf binary (same one §M38's C++ uses).  Only defined when it's present
  # (make musl-cross-i686); its musl is ABI-compatible with the provisioned
  # /lib/ld-musl-i386.so.1, so binaries built here run on it (as libstdc++ does).
  MUSL_TRIPLE   := i686-linux-musl
  MUSL_AR       := third_party/musl-cross-i686/bin/i686-linux-musl-ar
  MUSL_ELF_CC   := third_party/musl-cross-i686/bin/i686-linux-musl-gcc
  MUSL_ELF_CXX  := third_party/musl-cross-i686/bin/i686-linux-musl-g++
  MUSL_SYSROOT  := third_party/musl-cross-i686/i686-linux-musl

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
      kernel/hal/x86_64/fpu.c \
      kernel/hal/x86_64/vmm.c \
      kernel/hal/x86_64/task_arch.c \
      kernel/hal/x86_64/mb2.c \
      kernel/hal/x86_64/main_entry.c \
      kernel/hal/x86_64/m20_stubs.c \
      kernel/hal/x86_64/smp.c \
      kernel/hal/x86_64/syscall.c \
      kernel/hal/x86_64/uaccess.c \
      kernel/hal/x86_64/linux_abi.c \
      kernel/hal/x86_64/fork.c \
      kernel/hal/x86_64/signal.c \
      kernel/hal/x86/lapic.c \
      kernel/hal/x86/tlb.c \
      kernel/hal/x86/tsc.c \
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

  ARCH_EXTRA_OBJS := kernel/hal/x86_64/ap_trampoline_blob.o $(X86_USER_BLOBS)

  # Tier B — in-tree user libc build knobs (x86_64).  -mno-sse* because the
  # kernel does not init/save FPU/XMM state for user tasks.
  USER_CFLAGS   := -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-sse3 \
                   -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
                   -nostdlib -Os -Wall -std=c11 -Iuser
  USER_LDEMU    := -m elf_x86_64
  USER_BASE     := 0x40000000
  USER_OCARGS   := --output-target=elf64-x86-64 --binary-architecture=i386
  USER_CRT0_SRC   = user/crt0_x86_64.s
  USER_CRT0_BUILD = nasm -f elf64 $(USER_CRT0_SRC) -o $(OBJ_DIR)/user/crt0.o

  # §M36/§M37 (x86_64) — the musl userland for x86_64 comes from the PREBUILT
  # musl.cc cross-toolchain (third_party/musl-cross-x86_64), whose sysroot IS a
  # complete x86_64 musl (static libc.a + shared libc.so=ld.so + crt + a musl
  # libstdc++).  No separate `make musl` needed — the prebuilt sysroot is it.
  MUSL_SYSROOT := third_party/musl-cross-x86_64/x86_64-linux-musl
  MUSL_TRIPLE  := x86_64-linux-musl
  MUSL_AR      := third_party/musl-cross-x86_64/bin/x86_64-linux-musl-ar
  MUSL_ELF_CC  := third_party/musl-cross-x86_64/bin/x86_64-linux-musl-gcc
  MUSL_ELF_CXX := third_party/musl-cross-x86_64/bin/x86_64-linux-musl-g++
  # The canonical PT_INTERP path an x86_64 musl dynamic binary carries; pkg.c
  # provisions the ld.so (== libc.so) there at boot.
  DOS_LDSO     := /lib/ld-musl-x86_64.so.1
  MUSL_HDR_DIR := $(MUSL_SYSROOT)/include
  MUSL_CRT_DIR := $(MUSL_SYSROOT)/lib
  TCC_CPU       := x86_64
  TCC_CC        := x86_64-linux-musl-gcc
  TCC_TOOLCHAIN := third_party/musl-cross-x86_64
  # The SAME musl programs i386 embeds — coreutils + `sh` + the socket probes —
  # plus forktest64, which is x86_64-only (it checks the 64-bit fork/COW path).
  # The %.muslelf rule already has an x86_64 branch driving the cross gcc, so
  # nothing but this list was ever missing.
  ifneq ($(wildcard $(MUSL_SYSROOT)/lib/libc.a),)
    ARCH_EXTRA_OBJS += $(MUSL_PROG_BLOBS) $(MUSL_COREUTIL_BLOBS) \
                       user/forktest64_muslblob.o
  endif
  # §M37 dynamic-linking artifacts (x86_64): the shared libc.so=ld.so blob +
  # dynamically-linked test programs.  The prebuilt sysroot's libc.so IS the
  # dynamic linker, so the same wildcard gate as the static libc.a applies.
  ifneq ($(wildcard $(MUSL_SYSROOT)/lib/libc.so),)
    ARCH_EXTRA_OBJS += $(MUSL_DYN_BLOBS)
  endif
  # §M38 C++ runtime (x86_64) — the prebuilt sysroot ships a musl libstdc++.so.6
  # + libgcc_s.so.1, so cpptest (exceptions across a .so) works once the g++
  # driver exists.  Same artifacts as i386.
  ifneq ($(wildcard $(MUSL_ELF_CXX)),)
    ARCH_EXTRA_OBJS += $(CXX_RUNTIME_BLOBS)
  endif
  # §M39 (x86_64) — Mbed TLS built against the same cross sysroot as the rest of
  # the x86_64 userland (`make mbedtls ARCH=x86_64`), so crypttest/ssltest/
  # httpstest/wget are the same four programs i386 has.
  MBEDTLS_PREFIX := third_party/mbedtls-x86_64
  ifneq ($(wildcard $(MBEDTLS_PREFIX)/lib/libmbedcrypto.a),)
    ARCH_EXTRA_OBJS += $(MBEDTLS_PROG_BLOBS)
    ifneq ($(wildcard third_party/cacert.pem),)
      ARCH_EXTRA_OBJS += third_party/cacert_blob.o
    endif
  endif
  # §M40 (x86_64) — the upstream libwayland client (`make ARCH=x86_64 wayland`).
  ifneq ($(wildcard third_party/wayland-x86_64/lib/libwayland-client.a),)
    ARCH_EXTRA_OBJS += user/wlupstream_muslblob.o
    ifneq ($(wildcard third_party/weston/clients/simple-shm.c),)
      ARCH_EXTRA_OBJS += user/simpleshm_muslblob.o
    endif
  endif
  # §M43 (x86_64) — the on-device C compiler (`make tcc ARCH=x86_64`).
  TINYCC_PREFIX := third_party/tinycc-x86_64
  ifneq ($(wildcard $(TINYCC_PREFIX)/bin/tcc),)
    ARCH_EXTRA_OBJS += user/dostcc_blob.o user/rootfs_blob.o
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
  ifneq ($(wildcard third_party/libnsbmp/src/libnsbmp.c),)
    ARCH_EXTRA_OBJS += user/libnsbmp0_blob.o user/btest_dynblob.o
  endif
  # libcss — CSS engine; heavy codegen (gen_parser host tool + per-property
  # parsers + python select generator) → slow, guarded on the PREBUILT .so
  # (run `make libcss`).
  ifneq ($(wildcard user/libcss.so.0),)
    ARCH_EXTRA_OBJS += user/libcss0_blob.o user/csstest_dynblob.o
  endif
  # libdom — DOM (deps wapcaplet+hubbub+parserutils); 97 TUs → prebuilt-.so guard.
  ifneq ($(wildcard user/libdom.so.0),)
    ARCH_EXTRA_OBJS += user/libdom0_blob.o user/domtest_dynblob.o
  endif
  # §M42 browser-runway libs: the utility + framebuffer-surface deps the NetSurf
  # *binary* needs on top of the parsing/DOM/decoder core above.
  # libnsutils — base64/time/unistd helpers (no deps); nsutest = base64 round-trip.
  ifneq ($(wildcard third_party/libnsutils/src/base64.c),)
    ARCH_EXTRA_OBJS += user/libnsutils0_blob.o user/nsutest_dynblob.o
  endif
  # libnslog — logging + flex/bison filter (no deps); provisioned only (used by
  # NetSurf's utils/log.c NSLOG macros).
  ifneq ($(wildcard third_party/libnslog/src/core.c),)
    ARCH_EXTRA_OBJS += user/libnslog0_blob.o
  endif
  # libnspsl — public-suffix list (pre-generated psl.inc, no deps); provisioned.
  ifneq ($(wildcard third_party/libnspsl/src/nspsl.c),)
    ARCH_EXTRA_OBJS += user/libnspsl0_blob.o
  endif
  # libnsfb — the framebuffer-surface library = the fb frontend's render target.
  # Only the RAM surface is built (sdl/x/vnc/wld need host libs); nsfbtest plots
  # into a RAM surface and reads a pixel back (proves the frontend surface path).
  ifneq ($(wildcard third_party/libnsfb/src/libnsfb.c),)
    ARCH_EXTRA_OBJS += user/libnsfb0_blob.o user/nsfbtest_dynblob.o
  endif
  # §M42 the NetSurf browser binary + its resources — slow (~147 TUs) → guarded
  # on the prebuilt binary (run `make ARCH=x86_64 netsurf`).  Embeds the binary
  # (exec'd by the `netsurf` shell cmd) + a /res archive (unpacked by pkg.c).
  ifneq ($(wildcard user/netsurf.dynelf),)
    ARCH_EXTRA_OBJS += user/netsurf_dynblob.o user/netsurf_res_blob.o
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

  # §A2/A3 — the musl userland for aarch64, provisioned exactly like x86_64's:
  # a PREBUILT musl.cc cross-toolchain whose sysroot IS a complete aarch64 musl
  # (static libc.a + shared libc.so == ld.so + crt).  Fetch it once with
  #     ./scripts/fetch-musl-cross-prebuilt.sh aarch64-linux-musl
  # which was already arch-parametric — a download, not the ~10h from-source
  # gcc build the i386 path once needed.
  #
  # NOTE the two toolchains above are NOT interchangeable: the aarch64-linux-gnu
  # cross gcc builds the KERNEL (freestanding, -mgeneral-regs-only), while this
  # one builds RING-3 binaries against musl.  Using the kernel compiler for
  # userland would produce something with no libc; using this one for the kernel
  # would drag in FP/NEON we do not save on context switch.
  MUSL_SYSROOT := third_party/musl-cross-aarch64/aarch64-linux-musl
  MUSL_TRIPLE  := aarch64-linux-musl
  MUSL_AR      := third_party/musl-cross-aarch64/bin/aarch64-linux-musl-ar
  MUSL_ELF_CC  := third_party/musl-cross-aarch64/bin/aarch64-linux-musl-gcc
  MUSL_ELF_CXX := third_party/musl-cross-aarch64/bin/aarch64-linux-musl-g++
  # The canonical PT_INTERP path an aarch64 musl dynamic binary carries.
  DOS_LDSO     := /lib/ld-musl-aarch64.so.1
  MUSL_HDR_DIR := $(MUSL_SYSROOT)/include
  MUSL_CRT_DIR := $(MUSL_SYSROOT)/lib
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
      kernel/hal/aarch64/fork.c \
      kernel/hal/aarch64/signal.c \
      kernel/hal/aarch64/linux_abi.c \
      kernel/hal/aarch64/syscall.c \
      kernel/hal/aarch64/uaccess.c \
      kernel/hal/aarch64/fpu.c \
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

  # §A1 — the fork/pipe self-tests, now that aarch64 has proc_fork.  These are
  # in-tree-libc programs and the build rules are already arch-parameterised,
  # so adding them here is the whole change.
  ARCH_EXTRA_OBJS := user/hello_blob.o user/spin_blob.o user/wedge_blob.o \
                     user/forktest_blob.o user/pipetest_blob.o \
                     user/sigtest_blob.o user/muslhello_muslblob.o \
                     user/epollmusl_muslblob.o \
                     $(MUSL_COREUTIL_BLOBS)

  # Tier B — in-tree user libc build knobs (aarch64).  Uses the cross toolchain
  # ($(CC)/$(LD)/$(CROSS)objcopy); user base is 4 GiB (above the identity map).
  USER_CFLAGS   := -mgeneral-regs-only -ffreestanding -fno-pie \
                   -fno-stack-protector -fno-builtin -nostdlib -Os -Wall \
                   -std=c11 -Iuser
  USER_LDEMU    :=
  USER_BASE     := 0x100000000
  USER_OCARGS   := --output-target=elf64-littleaarch64 --binary-architecture=aarch64
  USER_OBJCOPY  := $(CROSS)objcopy
  USER_CRT0_SRC   = user/crt0_aarch64.S
  USER_CRT0_BUILD = $(CC) $(USER_CFLAGS) -c $(USER_CRT0_SRC) -o $(OBJ_DIR)/user/crt0.o

else
  $(error Unsupported ARCH "$(ARCH)" — supported: i386, x86_64, aarch64)
endif

# -----------------------------------------------------------------------------
# §M40 — the Mesa software-GL runtime + the EGL triangle.
#
# ARCH-AGNOSTIC ON PURPOSE.  This block used to sit inside the x86_64 branch,
# which made "Mesa is x86_64-only" a property of the BUILD rather than of the
# code — every path below is already parameterised by $(ARCH), so i386 was
# excluded by where the lines lived and nothing else.  (The same duplication is
# what kept the whole userland i386-only until §4.39.)  It is guarded on the
# built artifact, so an arch without a Mesa tree simply skips it; build one
# with `ARCH=<arch> ./scripts/build-mesa.sh`.
# -----------------------------------------------------------------------------
ifneq ($(wildcard third_party/mesa-$(ARCH)/lib/dri/swrast_dri.so),)
  ARCH_EXTRA_OBJS += user/libEGL_so_blob.o user/libGLESv2_so_blob.o \
                     user/libglapi_so_blob.o user/libexpat_so_blob.o \
                     user/libdrm_so_blob.o user/swrast_dri_so_blob.o \
                     user/mesaz_so_blob.o user/libwlclient_so_blob.o \
                     user/egltri_dynblob.o
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
    kernel/core/uaccess.c \
      kernel/core/crash.c \
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
    kernel/gui/dosgui.c \
    kernel/gui/widget.c \
    kernel/gui/wayland.c \
    kernel/gui/wl_keymap.c \
    kernel/gui/w_editor.c \
    kernel/gui/clipboard.c \
    kernel/gui/shell_vista.c \
    kernel/gui/shell_bare.c \
    kernel/gui/apps/fileman.c \
    kernel/gui/apps/about.c \
    kernel/gui/apps/newshell.c \
    kernel/gui/apps/hello.c \
    kernel/gui/apps/taskman.c \
    kernel/gui/apps/crashapp.c \
    kernel/gui/apps/editor.c \
    kernel/gui/apps/basic.c \
    kernel/gui/apps/netsurf_app.c \
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
    kernel/drivers/watchdog/ib700.c \
    kernel/drivers/null/null.c \
    kernel/drivers/block/virtio_blk.c \
    kernel/drivers/net/virtio_net.c \
    kernel/core/net.c \
    kernel/core/futex.c \
    kernel/core/workqueue.c \
    kernel/core/ktime.c \
    kernel/core/ktimer.c \
    kernel/core/timerfd.c \
    kernel/core/itimer.c \
    kernel/core/epoll.c \
    kernel/core/abi_engine.c \
    kernel/core/abi_linux.c \
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
    kernel/core/uaccess.c \
      kernel/core/crash.c \
    kernel/core/pkg.c \
    kernel/core/futex.c \
    kernel/core/workqueue.c \
    kernel/core/ktime.c \
    kernel/core/ktimer.c \
    kernel/core/timerfd.c \
    kernel/core/itimer.c \
    kernel/core/epoll.c \
    kernel/core/abi_engine.c \
    kernel/core/abi_linux.c \
    kernel/core/net.c \
    kernel/core/audio.c \
    kernel/core/random.c \
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
    kernel/gui/dosgui.c \
    kernel/gui/widget.c \
    kernel/gui/wayland.c \
    kernel/gui/wl_keymap.c \
    kernel/gui/w_editor.c \
    kernel/gui/clipboard.c \
    kernel/gui/shell_vista.c \
    kernel/gui/shell_bare.c \
    kernel/gui/apps/fileman.c \
    kernel/gui/apps/about.c \
    kernel/gui/apps/newshell.c \
    kernel/gui/apps/hello.c \
    kernel/gui/apps/taskman.c \
    kernel/gui/apps/crashapp.c \
    kernel/gui/apps/editor.c \
    kernel/gui/apps/basic.c \
    kernel/gui/apps/netsurf_app.c \
    kernel/mem/pmm.c \
    kernel/mem/slab.c \
    kernel/mem/kmalloc.c \
    kernel/fs/vfs.c \
    kernel/fs/ramfs.c \
    kernel/fs/procfs.c \
    kernel/fs/devfs.c \
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
    kernel/core/uaccess.c \
      kernel/core/crash.c \
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
    kernel/gui/dosgui.c \
    kernel/gui/widget.c \
    kernel/gui/wayland.c \
    kernel/gui/wl_keymap.c \
    kernel/gui/w_editor.c \
    kernel/gui/clipboard.c \
    kernel/gui/shell_vista.c \
    kernel/gui/shell_bare.c \
    kernel/gui/apps/fileman.c \
    kernel/gui/apps/about.c \
    kernel/gui/apps/newshell.c \
    kernel/gui/apps/hello.c \
    kernel/gui/apps/taskman.c \
    kernel/gui/apps/crashapp.c \
    kernel/gui/apps/editor.c \
    kernel/gui/apps/basic.c \
    kernel/gui/apps/netsurf_app.c \
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
    kernel/drivers/watchdog/ib700.c \
    kernel/drivers/null/null.c \
    kernel/drivers/block/virtio_blk.c \
    kernel/drivers/net/virtio_net.c \
    kernel/core/net.c \
    kernel/drivers/audio/ac97.c \
    kernel/core/audio.c \
    kernel/core/futex.c \
    kernel/core/workqueue.c \
    kernel/core/ktime.c \
    kernel/core/ktimer.c \
    kernel/core/timerfd.c \
    kernel/core/itimer.c \
    kernel/core/epoll.c \
    kernel/core/abi_engine.c \
    kernel/core/abi_linux.c \
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
	# MUSL_VER=1.2.5: pin the toolchain's musl to match the runtime musl the
	# kernel provisions (make musl → third_party/musl-i386, 1.2.5) so an i386
	# musl-cross build and the /lib ld.so are the SAME version — the §M42
	# browser stack builds and runs on one musl (was a 1.2.3/1.2.5 split).
	$(MAKE) -C /tmp/mcm TARGET=$(1) OUTPUT=/tmp/mcm/out MUSL_VER=1.2.5 install
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
# Mbed TLS (§M39 stage 2) — build the vendored crypto/TLS library for the musl
# userland of the CURRENT ARCH.  Pure C, so the only per-arch difference is which
# compiler + headers are used:
#   i386   — host gcc -m32 with our own musl headers (same path as the coreutils)
#   x86_64 — the prebuilt musl.cc cross gcc, which carries its own sysroot
# Built on the container-local fs (the PSA driver-wrapper generation writes many
# files; keep it off the slow mount).  The image must have
# python3-jsonschema/jinja2 (Dockerfile, §M39) for the PSA wrapper generation.
# Produces third_party/mbedtls-<arch>/{lib,include}.
#     docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build make mbedtls
#     ... ARCH=x86_64 make mbedtls
# -----------------------------------------------------------------------------
MBEDTLS_DIR    := third_party/mbedtls
ifeq ($(ARCH),x86_64)
  MBEDTLS_CC     := $(CURDIR)/$(MUSL_ELF_CC)
  MBEDTLS_CFLAGS := -Os -fno-stack-protector -w
else
  MBEDTLS_CC     := gcc -m32
  MBEDTLS_CFLAGS := -I$(CURDIR)/$(MUSL_PREFIX)/include -Os -fno-stack-protector -w
endif

mbedtls:
	@test -f $(MBEDTLS_DIR)/Makefile || { \
	  echo "Mbed TLS missing — run ./scripts/fetch-mbedtls.sh first"; exit 1; }
	rm -rf /tmp/mb && cp -a $(MBEDTLS_DIR) /tmp/mb
	$(MAKE) -C /tmp/mb/library CC='$(MBEDTLS_CC)' CFLAGS='$(MBEDTLS_CFLAGS)' \
	    libmbedcrypto.a libmbedx509.a libmbedtls.a
	rm -rf $(MBEDTLS_PREFIX)
	mkdir -p $(MBEDTLS_PREFIX)/lib
	cp /tmp/mb/library/lib*.a $(MBEDTLS_PREFIX)/lib/
	cp -a /tmp/mb/include $(MBEDTLS_PREFIX)/include
	rm -rf /tmp/mb
	@echo "Mbed TLS $(ARCH) libs → $(MBEDTLS_PREFIX)/lib/"

# -----------------------------------------------------------------------------
# §M40 — UPSTREAM libwayland-client, cross-built for this arch's musl.
#
# §M26 built d-os's own Wayland SERVER and a hand-written mini client library
# (user/libwl).  This is the other half: the REAL libwayland-client, so an
# unmodified Wayland application links against the same library it would on
# Linux.  Nothing here is forked — the vendored tree is pristine.
#
# THREE PIECES, and only the first is unusual:
#
#   wayland-scanner  runs on the HOST (apt's, see the Dockerfile) and turns the
#                    protocol XML into C.  Nothing generated is committed; it is
#                    regenerated into $(WL_GEN) on every build.
#   libffi           libwayland dispatches an incoming event by building an
#                    argument list at runtime and calling the listener through
#                    ffi_call.  There is no way around it short of forking the
#                    library, which is what this milestone exists NOT to do.
#   the library      four C files + the generated protocol table, compiled with
#                    the same musl cross toolchain as the rest of the userland.
#
# Produces third_party/wayland-<arch>/{lib,include}.
#     docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build \
#         make ARCH=x86_64 wayland
# -----------------------------------------------------------------------------
WL_DIR     := third_party/wayland
FFI_DIR    := third_party/libffi
WL_PREFIX  := third_party/wayland-$(ARCH)
FFI_PREFIX := third_party/libffi-$(ARCH)
WL_GEN     := $(OBJ_DIR)/wlgen
XDG_XML    := /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml

wayland: $(WL_PREFIX)/lib/libwayland-client.a

# libffi first — the client library links against it.
$(FFI_PREFIX)/lib/libffi.a:
	@test -f $(FFI_DIR)/configure || { \
	  echo "libffi missing — run ./scripts/fetch-wayland.sh first"; exit 1; }
	@test -x $(MUSL_ELF_CC) || { \
	  echo "musl toolchain for $(ARCH) missing ($(MUSL_ELF_CC))"; exit 1; }
	rm -rf /tmp/ffi && cp -a $(FFI_DIR) /tmp/ffi
	cd /tmp/ffi && CC=$(CURDIR)/$(MUSL_ELF_CC) ./configure \
	    --host=$(MUSL_TRIPLE) --prefix=$(CURDIR)/$(FFI_PREFIX) \
	    --disable-shared --enable-static --disable-docs --with-pic >/dev/null
	$(MAKE) -C /tmp/ffi -j4 >/dev/null
	$(MAKE) -C /tmp/ffi install >/dev/null
	rm -rf /tmp/ffi
	@echo "libffi ($(ARCH)) → $(FFI_PREFIX)/lib/libffi.a"

$(WL_PREFIX)/lib/libwayland-client.a: $(FFI_PREFIX)/lib/libffi.a
	@test -d $(WL_DIR)/src || { \
	  echo "wayland missing — run ./scripts/fetch-wayland.sh first"; exit 1; }
	@command -v wayland-scanner >/dev/null || { \
	  echo "wayland-scanner missing — rebuild the docker image"; exit 1; }
	# Build in a COPY: wayland's sources do `#include "../config.h"`, which meson
	# normally generates at the tree root.  Writing it into the vendored tree
	# would stop that tree being pristine, so the whole build happens in /tmp.
	rm -rf /tmp/wl && cp -a $(WL_DIR) /tmp/wl
	printf '%s\n' \
	  '/* Generated by the d-os build (meson would normally emit this). */' \
	  '#define HAVE_ACCEPT4 1' \
	  '/* musl has a working MSG_CMSG_CLOEXEC and no BSD sys/ucred.h. */' \
	  > /tmp/wl/config.h
	rm -rf $(WL_GEN) && mkdir -p $(WL_GEN)
	# Protocol -> C.  `private-code` emits the interface TABLES (the symbols the
	# library and every client share); `client-header` emits the typed proxy
	# wrappers an application actually calls.
	# PUBLIC code for the core protocol: `private-code` hides the interface
	# symbols (wl_surface_interface & co), which is right for a program that
	# links the tables privately and wrong for a SHARED library that has to
	# export them to its users — real libwayland-client.so exports them too.
	wayland-scanner public-code    /tmp/wl/protocol/wayland.xml $(WL_GEN)/wayland-protocol.c
	wayland-scanner client-header  /tmp/wl/protocol/wayland.xml $(WL_GEN)/wayland-client-protocol.h
	wayland-scanner private-code   $(XDG_XML) $(WL_GEN)/xdg-shell-protocol.c
	wayland-scanner client-header  $(XDG_XML) $(WL_GEN)/xdg-shell-client-protocol.h
	# wayland-version.h is normally produced by meson from the .in template.
	sed -e 's/@WAYLAND_VERSION_MAJOR@/1/' -e 's/@WAYLAND_VERSION_MINOR@/22/' \
	    -e 's/@WAYLAND_VERSION_MICRO@/0/' -e 's/@WAYLAND_VERSION@/1.22.0/' \
	    /tmp/wl/src/wayland-version.h.in > $(WL_GEN)/wayland-version.h
	# Mesa's wayland platform links against wayland-SERVER too (for its wl_drm
	# glue), so the server half has to exist even though d-os is its own
	# compositor and never runs libwayland-server.  Same sources, one more
	# scanner pass for the server-side protocol header.
	wayland-scanner server-header /tmp/wl/protocol/wayland.xml $(WL_GEN)/wayland-server-protocol.h
	for f in connection wayland-client wayland-os wayland-util; do \
	    $(MUSL_ELF_CC) -c -Os -fPIC -std=gnu99 \
	        -I/tmp/wl/src -I$(CURDIR)/$(WL_GEN) -I$(CURDIR)/$(FFI_PREFIX)/include \
	        /tmp/wl/src/$$f.c -o $(WL_GEN)/$$f.o || exit 1; \
	done
	$(MUSL_ELF_CC) -c -Os -fPIC -std=gnu99 \
	    -I/tmp/wl/src -I$(WL_GEN) $(WL_GEN)/wayland-protocol.c \
	    -o $(WL_GEN)/wayland-protocol.o
	$(MUSL_ELF_CC) -c -Os -fPIC -std=gnu99 \
	    -I/tmp/wl/src -I$(WL_GEN) $(WL_GEN)/xdg-shell-protocol.c \
	    -o $(WL_GEN)/xdg-shell-protocol.o
	rm -rf $(WL_PREFIX) && mkdir -p $(WL_PREFIX)/lib $(WL_PREFIX)/include
	# libwayland-egl lives in the WAYLAND tree, not Mesa's (it moved years ago).
	# An EGL application calls wl_egl_window_create(); libEGL reads the resulting
	# struct through the wayland-egl-backend ABI.
	$(MUSL_ELF_CC) -c -Os -fPIC -std=gnu99 -I/tmp/wl -I/tmp/wl/src \
	    -I$(CURDIR)/$(WL_GEN) /tmp/wl/egl/wayland-egl.c -o $(WL_GEN)/wayland-egl.o
	for f in wayland-server event-loop wayland-shm; do \
	    $(MUSL_ELF_CC) -c -Os -fPIC -std=gnu99 \
	        -I/tmp/wl -I/tmp/wl/src -I$(CURDIR)/$(WL_GEN) \
	        -I$(CURDIR)/$(FFI_PREFIX)/include \
	        /tmp/wl/src/$$f.c -o $(WL_GEN)/$$f.o || exit 1; \
	done
	# libwayland-client must be SHARED.  Mesa links it into libEGL.so, and the
	# application links it too — with a static archive each ends up with its OWN
	# copy of libwayland's object tables, so an event demarshalled by one refers
	# to proxies registered in the other's table.  That crashes in
	# wl_connection_demarshal with a null interface, which is exactly the fault
	# this cost an afternoon to find.  One shared instance, one table.
	$(MUSL_ELF_CC) -shared -Wl,-soname,libwayland-client.so.0 \
	    -o $(WL_PREFIX)/lib/libwayland-client.so.0 \
	    $(WL_GEN)/connection.o $(WL_GEN)/wayland-client.o $(WL_GEN)/wayland-os.o \
	    $(WL_GEN)/wayland-util.o $(WL_GEN)/wayland-protocol.o \
	    $(FFI_PREFIX)/lib/libffi.a
	ln -sf libwayland-client.so.0 $(WL_PREFIX)/lib/libwayland-client.so
	# xdg-shell is a wayland-protocols EXTENSION, not part of libwayland: every
	# client links its tables itself, so it stays private and out of the .so.
	$(MUSL_AR) rcs $(WL_PREFIX)/lib/libxdg-shell.a $(WL_GEN)/xdg-shell-protocol.o
	$(MUSL_AR) rcs $(WL_PREFIX)/lib/libwayland-egl.a $(WL_GEN)/wayland-egl.o
	$(MUSL_AR) rcs $(WL_PREFIX)/lib/libwayland-server.a \
	    $(WL_GEN)/wayland-server.o $(WL_GEN)/event-loop.o $(WL_GEN)/wayland-shm.o \
	    $(WL_GEN)/connection.o $(WL_GEN)/wayland-os.o $(WL_GEN)/wayland-util.o \
	    $(WL_GEN)/wayland-protocol.o
	$(MUSL_AR) rcs $(WL_PREFIX)/lib/libwayland-client.a \
	    $(WL_GEN)/connection.o $(WL_GEN)/wayland-client.o $(WL_GEN)/wayland-os.o \
	    $(WL_GEN)/wayland-util.o $(WL_GEN)/wayland-protocol.o \
	    $(WL_GEN)/xdg-shell-protocol.o
	cp $(WL_DIR)/src/wayland-client.h $(WL_DIR)/src/wayland-client-core.h \
	   $(WL_DIR)/src/wayland-util.h $(WL_PREFIX)/include/
	rm -rf /tmp/wl
	cp $(WL_GEN)/wayland-client-protocol.h $(WL_GEN)/xdg-shell-client-protocol.h \
	   $(WL_GEN)/wayland-version.h $(WL_PREFIX)/include/
	# pkg-config metadata.  Mesa (and any meson-built consumer) discovers
	# wayland through pkg-config, not by guessing paths, so a hand-built library
	# without a .pc is invisible to it however correct the .a is.
	#
	# The .pc names the SHARED library by absolute path, not `-lwayland-client`.
	# The static archive still exists beside it for the static clients that link
	# it explicitly, and with both present a plain -l left Mesa linking its own
	# private copy of libwayland into libEGL.so — two object tables in one
	# process, and a null-interface crash in wl_connection_demarshal.
	cp $(WL_DIR)/egl/wayland-egl-backend.h $(WL_DIR)/egl/wayland-egl.h \
	   $(WL_DIR)/egl/wayland-egl-core.h $(WL_PREFIX)/include/
	mkdir -p $(WL_PREFIX)/lib/pkgconfig
	printf '%s\n' \
	  'prefix=$(CURDIR)/$(WL_PREFIX)' 'libdir=$${prefix}/lib' \
	  'includedir=$${prefix}/include' '' \
	  'Name: Wayland Client' 'Description: Wayland client side library' \
	  'Version: 1.22.0' 'Cflags: -I$${includedir}' \
	  'Libs: $${libdir}/libwayland-client.so.0' \
	  > $(WL_PREFIX)/lib/pkgconfig/wayland-client.pc
	printf '%s\n' \
	  'prefix=$(CURDIR)/$(WL_PREFIX)' 'includedir=$${prefix}/include' '' \
	  'Name: Wayland EGL backend' \
	  'Description: Interface between EGL and the Wayland client library' \
	  'Version: 3' 'Cflags: -I$${includedir}' \
	  > $(WL_PREFIX)/lib/pkgconfig/wayland-egl-backend.pc
	cp $(WL_DIR)/src/wayland-server.h $(WL_DIR)/src/wayland-server-core.h \
	   $(WL_PREFIX)/include/
	cp $(WL_GEN)/wayland-server-protocol.h $(WL_PREFIX)/include/
	printf '%s\n' \
	  'prefix=$(CURDIR)/$(WL_PREFIX)' 'libdir=$${prefix}/lib' \
	  'includedir=$${prefix}/include' '' \
	  'Name: Wayland Server' 'Description: Wayland server side library' \
	  'Version: 1.22.0' 'Cflags: -I$${includedir}' \
	  'Libs: -L$${libdir} -lwayland-server -L$(CURDIR)/$(FFI_PREFIX)/lib -lffi' \
	  > $(WL_PREFIX)/lib/pkgconfig/wayland-server.pc
	rm -rf /tmp/wl
	@echo "libwayland-client ($(ARCH)) → $(WL_PREFIX)/lib/"

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
	@test -x $(TCC_TOOLCHAIN)/bin/$(TCC_CC) || { \
	  echo "musl toolchain for $(ARCH) missing ($(TCC_TOOLCHAIN)/bin/$(TCC_CC))"; exit 1; }
	cp $(TCC_TOOLCHAIN)/$(subst -gcc,,$(TCC_CC))/lib/libc.so $(DOS_LDSO)
	# Clean musl headers at /usr/include so tcc (built here, then RUN here to
	# compile libtcc1.a) sees a musl world — the host's glibc /usr/include
	# otherwise conflicts.  (The musl-cross gcc is unaffected: own sysroot.)
	rm -rf /usr/include && cp -a $(CURDIR)/$(MUSL_HDR_DIR) /usr/include
	rm -rf /tmp/tcc && cp -a $(TINYCC_DIR) /tmp/tcc
	cd /tmp/tcc && PATH=$(CURDIR)/$(TCC_TOOLCHAIN)/bin:$$PATH \
	    CC=$(TCC_CC) ./configure --cpu=$(TCC_CPU) --config-musl \
	      --elfinterp=$(DOS_LDSO) --crtprefix=/lib --libpaths="{B}:/lib" \
	      --sysincludepaths="{B}/include:/usr/include" --prefix=/usr \
	      --extra-cflags="-fPIE" --extra-ldflags="-pie" --config-pie \
	      --config-bcheck=no --config-backtrace=no
	cd /tmp/tcc && PATH=$(CURDIR)/$(TCC_TOOLCHAIN)/bin:$$PATH $(MAKE) tcc libtcc1.a
	rm -rf $(TINYCC_PREFIX) && mkdir -p $(TINYCC_PREFIX)/bin $(TINYCC_PREFIX)/lib
	cp /tmp/tcc/tcc $(TINYCC_PREFIX)/bin/tcc
	cp /tmp/tcc/libtcc1.a $(TINYCC_PREFIX)/lib/libtcc1.a
	cp -a /tmp/tcc/include $(TINYCC_PREFIX)/include
	rm -rf /tmp/tcc
	@echo "tcc (on-device C compiler, $(ARCH)) → $(TINYCC_PREFIX)/bin/tcc"

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

user/hello_$(ARCH).elf: user/libc.c user/hello.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c  -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/hello.c -o $(OBJ_DIR)/user/hello.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/hello.o $(OBJ_DIR)/user/libc.o


user/spin_$(ARCH).elf: user/libc.c user/spin.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/spin.c -o $(OBJ_DIR)/user/spin.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/spin.o $(OBJ_DIR)/user/libc.o


# §M46 — wedge: a forever-spinning ring-3 program (frozen-app stand-in) to test
# force-kill.  Same build shape as spin.
user/wedge_$(ARCH).elf: user/libc.c user/wedge.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/wedge.c -o $(OBJ_DIR)/user/wedge.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/wedge.o $(OBJ_DIR)/user/libc.o


user/args_$(ARCH).elf: user/libc.c user/args.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/args.c -o $(OBJ_DIR)/user/args.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/args.o $(OBJ_DIR)/user/libc.o


user/forktest_$(ARCH).elf: user/libc.c user/forktest.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/forktest.c -o $(OBJ_DIR)/user/forktest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/forktest.o $(OBJ_DIR)/user/libc.o


user/forkexec_$(ARCH).elf: user/libc.c user/forkexec.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/forkexec.c -o $(OBJ_DIR)/user/forkexec.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/forkexec.o $(OBJ_DIR)/user/libc.o


user/pipetest_$(ARCH).elf: user/libc.c user/pipetest.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/pipetest.c -o $(OBJ_DIR)/user/pipetest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/pipetest.o $(OBJ_DIR)/user/libc.o


user/sigtest_$(ARCH).elf: user/libc.c user/sigtest.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c    -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/sigtest.c -o $(OBJ_DIR)/user/sigtest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/sigtest.o $(OBJ_DIR)/user/libc.o


user/dnstest_$(ARCH).elf: user/libc.c user/dnstest.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c    -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/dnstest.c -o $(OBJ_DIR)/user/dnstest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/dnstest.o $(OBJ_DIR)/user/libc.o


user/httptest_$(ARCH).elf: user/libc.c user/httptest.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c     -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/httptest.c -o $(OBJ_DIR)/user/httptest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/httptest.o $(OBJ_DIR)/user/libc.o


user/threadtest_$(ARCH).elf: user/libc.c user/threadtest.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c       -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/threadtest.c -o $(OBJ_DIR)/user/threadtest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/threadtest.o $(OBJ_DIR)/user/libc.o


user/tlstest_$(ARCH).elf: user/libc.c user/tlstest.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c    -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/tlstest.c -o $(OBJ_DIR)/user/tlstest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/tlstest.o $(OBJ_DIR)/user/libc.o


user/posixtest_$(ARCH).elf: user/libc.c user/posixtest.c user/libc.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(USER_CRT0_BUILD)
	$(CC) $(USER_CFLAGS) -c user/libc.c      -o $(OBJ_DIR)/user/libc.o
	$(CC) $(USER_CFLAGS) -c user/posixtest.c -o $(OBJ_DIR)/user/posixtest.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/crt0.o $(OBJ_DIR)/user/posixtest.o $(OBJ_DIR)/user/libc.o


# Standalone Linux-ABI test program — NO d-os crt0/libc (entry = _start), uses
# Linux syscall numbers directly.  Run under the Linux personality.
user/linuxhello_$(ARCH).elf: user/linuxhello.c $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(CC) $(USER_CFLAGS) -c user/linuxhello.c -o $(OBJ_DIR)/user/linuxhello.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ $(OBJ_DIR)/user/linuxhello.o


# wlclient — a freestanding NATIVE-ABI ring-3 Wayland client (int 0x80 with d-os
# syscall numbers, no libc); speaks the Wayland wire protocol over fd 3.
user/wlclient_$(ARCH).elf: user/wlclient.c $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(CC) $(USER_CFLAGS) -c user/wlclient.c -o $(OBJ_DIR)/user/wlclient.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ $(OBJ_DIR)/user/wlclient.o


# wlapp — a ring-3 app that speaks Wayland via the libwl client library.
user/wlapp_$(ARCH).elf: user/wlapp.c user/libwl.c user/libwl.h $(USER_CRT0_SRC)
	@mkdir -p $(OBJ_DIR)/user
	$(CC) $(USER_CFLAGS) -c user/wlapp.c -o $(OBJ_DIR)/user/wlapp.o
	$(CC) $(USER_CFLAGS) -c user/libwl.c -o $(OBJ_DIR)/user/libwl.o
	$(LD) $(USER_LDEMU) -N -Ttext $(USER_BASE) -e _start -o $@ \
	    $(OBJ_DIR)/user/wlapp.o $(OBJ_DIR)/user/libwl.o

# -----------------------------------------------------------------------------
# One rule embeds EVERY in-tree ring-3 program.
#
# The intermediate ELF carries the arch in its FILENAME (user/<name>_<arch>.elf)
# so an ARCH flip can never link a stale foreign-arch image.  objcopy mints its
# symbols from that filename, which used to leak the arch into the SYMBOL name
# too (`_binary_user_forktest_i386_elf_start`) — and since the kernel spelled
# those names out, every program was silently i386-only: the blob linked fine on
# x86_64 and the shell still reported "not embedded for this arch".  That is the
# actual reason x86_64's userland lagged behind, not any missing kernel support.
#
# --redefine-sym strips the arch back out, so the kernel refers to "the embedded
# forktest program" and the build decides which architecture that is.  Explicit
# rules (the .so's, ld.so, rootfs, netsurf resources) still win over this pattern
# for their own targets, which is why they need no exclusion here.
# -----------------------------------------------------------------------------
$(OBJ_DIR)/user/%_blob.o: user/%_$(ARCH).elf
	@mkdir -p $(@D)
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) \
	    --redefine-sym _binary_user_$*_$(ARCH)_elf_start=_binary_user_$*_elf_start \
	    --redefine-sym _binary_user_$*_$(ARCH)_elf_end=_binary_user_$*_elf_end \
	    --redefine-sym _binary_user_$*_$(ARCH)_elf_size=_binary_user_$*_elf_size \
	    $< $@


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
# §A3 — the condition is "does this arch have a musl cross-SYSROOT", not "is
# this x86_64".  It used to name the arch, which is exactly the shape §M47.5
# found and fixed elsewhere: a per-arch list that one arch quietly falls off.
# aarch64 now sets MUSL_SYSROOT too, so it takes this branch unchanged.
ifneq ($(MUSL_SYSROOT),)
# A prebuilt musl.cc cross-gcc driver links crt1/crti/libc.a/libgcc/crtn
# itself; -static -no-pie + -Wl,-Ttext-segment relocates the whole image (ELF
# headers included) to the d-os user base — same trick the i386 rule uses.
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

# §M40 — weston-simple-shm: an UNMODIFIED upstream Wayland application.
#
# Not a d-os test program.  clients/simple-shm.c is compiled exactly as it sits
# in the weston tree; everything below is what weston's own build system would
# provide — the generated protocol code, its shared/ helper, and a config.h.
#
# config.h normally comes from meson.  HAVE_MEMFD_CREATE is defined because d-os
# implements it (§M40 stage 2), which sends os_create_anonymous_file down the
# memfd path instead of hunting for a writable XDG_RUNTIME_DIR — d-os has no
# such directory, so this is the path that can actually work.
WESTON_DIR := third_party/weston
FS_XML     := /usr/share/wayland-protocols/unstable/fullscreen-shell/fullscreen-shell-unstable-v1.xml

user/simpleshm.muslelf: $(WESTON_DIR)/clients/simple-shm.c \
                        $(WL_PREFIX)/lib/libwayland-client.a
	@mkdir -p $(WL_GEN)
	wayland-scanner private-code  $(FS_XML) $(WL_GEN)/fullscreen-shell-protocol.c
	wayland-scanner client-header $(FS_XML) $(WL_GEN)/fullscreen-shell-unstable-v1-client-protocol.h
	printf '%s\n' '#define HAVE_MEMFD_CREATE 1' > $(WL_GEN)/config.h
	# NB: -I$(WESTON_DIR)/shared is deliberately ABSENT.  weston has its own
	# shared/signal.h, and putting that directory on the include path makes
	# simple-shm.c's `#include <signal.h>` pick up weston's server-side header
	# instead of the C library's.  The sources include it as
	# "shared/os-compatibility.h", so -I$(WESTON_DIR) is what they actually need.
	$(MUSL_ELF_CC) -static -no-pie -Os -w -std=gnu99 -D_GNU_SOURCE \
	    -I$(WL_GEN) -I$(WL_PREFIX)/include \
	    -I$(WESTON_DIR) -I$(WESTON_DIR)/include \
	    -Wl,-Ttext-segment=$(USER_BASE) \
	    $(WESTON_DIR)/clients/simple-shm.c \
	    $(WESTON_DIR)/shared/os-compatibility.c \
	    $(WL_GEN)/fullscreen-shell-protocol.c \
	    -o $@ $(WL_PREFIX)/lib/libwayland-client.a $(FFI_PREFIX)/lib/libffi.a

# §M40 — egltri: the EGL + GLES2 triangle, the milestone's remaining DoD.
#
# DYNAMICALLY linked, unlike every other §M40 client: Mesa's libEGL and
# libGLESv2 are shared objects and libEGL dlopen()s the DRI driver on top of
# that, so there is no static option.  This is the §M37 path (PT_INTERP + our
# ld.so), the same one NetSurf uses.
MESA_PREFIX := third_party/mesa-$(ARCH)
DRM_PREFIX  := third_party/libdrm-$(ARCH)

# The Mesa runtime, embedded so pkg.c can lay it into d-os's /lib at boot.
# Each is copied to an arch-agnostic user/ path first, because objcopy derives
# the blob's symbol name from the input path (the same convention every other
# .so here follows).
define MESA_SO_BLOB
$(OBJ_DIR)/user/$(2)_blob.o: $(3)
	@mkdir -p $$(@D)
	cp $(3) user/$(2)
	objcopy --input-target=binary $$(USER_OCARGS) user/$(2) $$@
	rm -f user/$(2)
endef
$(eval $(call MESA_SO_BLOB,,libEGL_so,$(MESA_PREFIX)/lib/libEGL.so.1.0.0))
$(eval $(call MESA_SO_BLOB,,libGLESv2_so,$(MESA_PREFIX)/lib/libGLESv2.so.2.0.0))
$(eval $(call MESA_SO_BLOB,,libglapi_so,$(MESA_PREFIX)/lib/libglapi.so.0.0.0))
$(eval $(call MESA_SO_BLOB,,libexpat_so,$(MESA_PREFIX)/lib/libexpat.so.1.6.7))
$(eval $(call MESA_SO_BLOB,,libdrm_so,$(DRM_PREFIX)/lib/libdrm.so.2.4.0))
$(eval $(call MESA_SO_BLOB,,swrast_dri_so,$(MESA_PREFIX)/lib/dri/swrast_dri.so))
# Mesa's own zlib subproject build: the DRI driver has a DT_NEEDED on plain
# `libz.so`, which is a different soname from the `libz.so.1` the NetSurf stack
# installs, so it needs its own copy rather than a symlink to that one.
$(eval $(call MESA_SO_BLOB,,mesaz_so,$(MESA_PREFIX)/lib/libz.so))
# The shared libwayland-client both libEGL and the application link against.
$(eval $(call MESA_SO_BLOB,,libwlclient_so,$(WL_PREFIX)/lib/libwayland-client.so.0))

user/egltri.dynelf: user/egltri.c $(WL_PREFIX)/lib/libwayland-client.a \
                    $(MESA_PREFIX)/lib/libEGL.so
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -Wl,-dynamic-linker,$(DOS_LDSO) \
	    -I$(WL_PREFIX)/include -I$(MESA_PREFIX)/include \
	    user/egltri.c -o $@ \
	    -L$(MESA_PREFIX)/lib -lEGL -lGLESv2 -lglapi -lexpat \
	    -L$(CURDIR)/third_party/libdrm-$(ARCH)/lib -ldrm \
	    $(WL_PREFIX)/lib/libwayland-egl.a $(WL_PREFIX)/lib/libxdg-shell.a \
	    -L$(WL_PREFIX)/lib -lwayland-client

# §M40 — the upstream-libwayland client.  Its own rule (not the generic
# %.muslelf) because it links libwayland-client + libffi and needs the generated
# protocol headers.  Guarded by the arch's built library, like every other port.
user/wlupstream.muslelf: user/wlupstream.c $(WL_PREFIX)/lib/libwayland-client.a
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -static -no-pie -Os -Wall -I$(WL_PREFIX)/include \
	    -Wl,-Ttext-segment=$(USER_BASE) $< -o $@ \
	    $(WL_PREFIX)/lib/libwayland-client.a $(FFI_PREFIX)/lib/libffi.a

# §M39 — the four Mbed TLS programs.  Each overrides the generic %.muslelf rule
# to ALSO compile with Mbed TLS's headers and link its three static libs:
#   crypttest  (stage 2)  — the crypto primitives
#   ssltest    (stage 3)  — an in-memory TLS handshake
#   httpstest  (stage 3b) — REAL HTTPS: the BIO rides a live M24 socket and the
#                           trust store is the provisioned CA bundle
#   wget                  — the HTTP/HTTPS download front-end (URL from argv)
# They were four copies of one recipe; that duplication is exactly why the
# x86_64 side never grew them.  One canned recipe per arch now serves all four,
# so adding a fifth TLS program is one name in MBEDTLS_PROGS.
MBEDTLS_PROGS := crypttest ssltest httpstest wget

ifeq ($(ARCH),x86_64)
# The cross gcc driver supplies its own crt/libc/libgcc and its own sysroot
# headers, so the whole link is one command (same shape as the generic x86_64
# %.muslelf rule).  -Ttext-segment moves the ELF headers too, putting the whole
# image at the d-os user base.
define MBEDTLS_PROG_BUILD
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -static -no-pie -Os -Wall -I$(MBEDTLS_PREFIX)/include \
	    -Wl,-Ttext-segment=$(USER_BASE) $< -o $@ \
	    $(MBEDTLS_PREFIX)/lib/libmbedtls.a \
	    $(MBEDTLS_PREFIX)/lib/libmbedx509.a \
	    $(MBEDTLS_PREFIX)/lib/libmbedcrypto.a
endef
MUSL_LIBC_DEP := $(MUSL_SYSROOT)/lib/libc.a
else
define MBEDTLS_PROG_BUILD
	@mkdir -p $(OBJ_DIR)/user
	gcc $(MUSL_CC_FLAGS) -c $< \
	    -I$(MUSL_PREFIX)/include -I$(MBEDTLS_PREFIX)/include \
	    -o $(OBJ_DIR)/user/$*.muslo
	ld -m elf_i386 -static -Ttext-segment=$(USER_BASE) -e _start -o $@ \
	    $(MUSL_PREFIX)/lib/crt1.o $(MUSL_PREFIX)/lib/crti.o \
	    $(OBJ_DIR)/user/$*.muslo \
	    --start-group \
	    $(MBEDTLS_PREFIX)/lib/libmbedtls.a $(MBEDTLS_PREFIX)/lib/libmbedx509.a \
	    $(MBEDTLS_PREFIX)/lib/libmbedcrypto.a $(MUSL_PREFIX)/lib/libc.a \
	    `gcc -m32 -print-libgcc-file-name` --end-group \
	    $(MUSL_PREFIX)/lib/crtn.o
endef
MUSL_LIBC_DEP := $(MUSL_LIBC)
endif

# Static pattern rule: an EXPLICIT rule for these four targets, so it wins over
# the generic user/%.muslelf pattern while still providing $* to the recipe.
$(patsubst %,user/%.muslelf,$(MBEDTLS_PROGS)): user/%.muslelf: user/%.c $(MUSL_LIBC_DEP)
	$(MBEDTLS_PROG_BUILD)

# The CA trust bundle, embedded verbatim.  objcopy derives the symbol from the
# input path: _binary_third_party_cacert_pem_{start,end}.  Provisioned into the
# VFS at /etc/ssl/cert.pem during boot (kernel.c).
$(OBJ_DIR)/third_party/cacert_blob.o: third_party/cacert.pem
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) third_party/cacert.pem $@

$(OBJ_DIR)/user/%_muslblob.o: user/%.muslelf
	@mkdir -p $(@D)
	# $(USER_OBJCOPY), not bare `objcopy`: the aarch64 container carries only the
	# cross binutils, so the unqualified name is simply absent there.  The
	# in-tree-libc blob rule above already knew this; this one did not, because
	# no arch had ever needed both a musl blob and a cross objcopy at once.
	$(USER_OBJCOPY) --input-target=binary $(USER_OCARGS) $< $@

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
$(OBJ_DIR)/user/dostcc_blob.o: $(TINYCC_PREFIX)/bin/tcc
	@mkdir -p $(@D)
	cp $(TINYCC_PREFIX)/bin/tcc user/dostcc
	objcopy --input-target=binary $(USER_OCARGS) user/dostcc $@
	rm -f user/dostcc

user/rootfs.bin: $(TINYCC_PREFIX)/bin/tcc user/tcc_hello.c user/hi.c
	python3 scripts/pack-rootfs.py $@ \
	    $(TINYCC_PREFIX)/include:/usr/lib/tcc/include \
	    $(TINYCC_PREFIX)/lib/libtcc1.a:/usr/lib/tcc/libtcc1.a \
	    $(MUSL_HDR_DIR):/usr/include \
	    $(MUSL_CRT_DIR)/crt1.o:/lib/crt1.o \
	    $(MUSL_CRT_DIR)/crti.o:/lib/crti.o \
	    $(MUSL_CRT_DIR)/crtn.o:/lib/crtn.o \
	    user/tcc_hello.c:/hello.c \
	    user/hi.c:/hi.c

$(OBJ_DIR)/user/rootfs_blob.o: user/rootfs.bin
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) user/rootfs.bin $@

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

# libnsbmp — BMP/ICO decoder, no deps (completes the NetSurf image-decoder set).
LNB_DIR := third_party/libnsbmp
user/libnsbmp.so.0: $(LNB_DIR)/src/libnsbmp.c
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -I$(LNB_DIR)/include -I$(LNB_DIR)/src \
	    -Wl,-soname,libnsbmp.so.0 -o $@ $(shell find $(LNB_DIR)/src -name '*.c')

$(OBJ_DIR)/user/libnsbmp0_blob.o: user/libnsbmp.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/btest.dynelf: user/btest.c user/libnsbmp.so.0
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(LNB_DIR)/include \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/btest.c \
	    user/libnsbmp.so.0 -o $@

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

# libdom — the DOM; deps libwapcaplet + libhubbub + libparserutils.  Compiles
# src/ + the hubbub binding (bindings/hubbub, HTML→DOM).  97 TUs → own target.
LDOM_DIR := third_party/libdom
.PHONY: libdom
libdom: user/libdom.so.0
user/libdom.so.0: $(LDOM_DIR)/Makefile user/libwapcaplet.so.0 user/libhubbub.so.0 user/libparserutils.so.0
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -I$(LDOM_DIR)/include -I$(LDOM_DIR)/src \
	    -I$(LHB_DIR)/include -I$(LWC_DIR)/include -I$(LPU_DIR)/include \
	    -I$(LDOM_DIR)/bindings -Wl,-soname,libdom.so.0 -o $@ \
	    $$(find $(LDOM_DIR)/src $(LDOM_DIR)/bindings/hubbub -name '*.c') \
	    user/libwapcaplet.so.0 user/libhubbub.so.0 user/libparserutils.so.0

$(OBJ_DIR)/user/libdom0_blob.o: user/libdom.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/domtest.dynelf: user/domtest.c user/libdom.so.0
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(LDOM_DIR)/include -I$(LWC_DIR)/include \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/domtest.c \
	    user/libdom.so.0 user/libhubbub.so.0 user/libwapcaplet.so.0 \
	    user/libparserutils.so.0 -o $@

# -----------------------------------------------------------------------------
# §M42 browser-runway libs — libnsutils / libnslog / libnspsl / libnsfb.
# These sit between the NetSurf core (parsers/DOM/decoders, above) and the
# actual browser binary: utility helpers, logging, the public-suffix list, and
# the framebuffer *surface* the fb frontend renders into.  Same "bypass their
# buildsystem, just compile the .c set into a .so" pattern as above.
# -----------------------------------------------------------------------------

# libnsutils — base64 / time / unistd helpers, no deps.
LNU_DIR := third_party/libnsutils
user/libnsutils.so.0: $(LNU_DIR)/src/base64.c
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -I$(LNU_DIR)/include \
	    -Wl,-soname,libnsutils.so.0 -o $@ $(shell find $(LNU_DIR)/src -name '*.c')

$(OBJ_DIR)/user/libnsutils0_blob.o: user/libnsutils.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/nsutest.dynelf: user/nsutest.c user/libnsutils.so.0
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(LNU_DIR)/include \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/nsutest.c \
	    user/libnsutils.so.0 -o $@

# libnslog — logging with a flex/bison filter language, no deps.  Generate the
# parser (bison 3.x → braced api.prefix) + lexer (flex, wrapped by filter-lexer.c)
# into src/ first, then compile core.c + filter.c + the two generated TUs.
LNL_DIR := third_party/libnslog
user/libnslog.so.0: $(LNL_DIR)/src/core.c
	@[ -f $(LNL_DIR)/src/filter-parser.c ] || ( cd $(LNL_DIR)/src && \
	    bison -d -t --define=api.prefix={filter_} \
	        --output=filter-parser.c --defines=filter-parser.h filter-parser.y )
	@[ -f $(LNL_DIR)/src/filter-lexer.c ] || ( cd $(LNL_DIR)/src && \
	    flex --outfile=filter-lexer.inc --header-file=filter-lexer.h filter-lexer.l && \
	    printf '#ifndef __clang_analyzer__\n#include "filter-lexer.inc"\n#endif\n' > filter-lexer.c )
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -I$(LNL_DIR)/include -I$(LNL_DIR)/src \
	    -Wl,-soname,libnslog.so.0 -o $@ \
	    $(LNL_DIR)/src/core.c $(LNL_DIR)/src/filter.c \
	    $(LNL_DIR)/src/filter-parser.c $(LNL_DIR)/src/filter-lexer.c

$(OBJ_DIR)/user/libnslog0_blob.o: user/libnslog.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

# libnspsl — public-suffix list lookup; psl.inc ships pre-generated, no deps.
LNP_DIR := third_party/libnspsl
user/libnspsl.so.0: $(LNP_DIR)/src/nspsl.c
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -I$(LNP_DIR)/include -I$(LNP_DIR)/src \
	    -Wl,-soname,libnspsl.so.0 -o $@ $(LNP_DIR)/src/nspsl.c

$(OBJ_DIR)/user/libnspsl0_blob.o: user/libnspsl.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

# libnsfb — framebuffer surface + plotters.  The source list mirrors the lib's
# own per-dir Makefiles: the plot/*-common.c files are #include TEMPLATES (a
# concrete bpp TU includes them with macros set), NOT standalone TUs, and only
# the default plotters/surface are built.  The RAM surface renders into a
# caller-supplied buffer → exactly what we blit into a gui_window later.
# Surfaces self-register via a __constructor__ (runs under musl ld.so).
LNFB_DIR := third_party/libnsfb
LNFB_SRCS := \
    $(LNFB_DIR)/src/libnsfb.c $(LNFB_DIR)/src/dump.c \
    $(LNFB_DIR)/src/cursor.c $(LNFB_DIR)/src/palette.c \
    $(LNFB_DIR)/src/surface/surface.c $(LNFB_DIR)/src/surface/ram.c \
    user/netsurf/libnsfb_dos.c \
    $(LNFB_DIR)/src/plot/api.c $(LNFB_DIR)/src/plot/util.c \
    $(LNFB_DIR)/src/plot/generic.c $(LNFB_DIR)/src/plot/32bpp-xrgb8888.c \
    $(LNFB_DIR)/src/plot/32bpp-xbgr8888.c $(LNFB_DIR)/src/plot/16bpp.c \
    $(LNFB_DIR)/src/plot/8bpp.c
user/libnsfb.so.0: $(LNFB_DIR)/src/libnsfb.c user/netsurf/libnsfb_dos.c
	$(MUSL_ELF_CC) -shared $(NSLIB_CFLAGS) -I$(LNFB_DIR)/include -I$(LNFB_DIR)/src \
	    -Wl,-soname,libnsfb.so.0 -o $@ $(LNFB_SRCS)

$(OBJ_DIR)/user/libnsfb0_blob.o: user/libnsfb.so.0
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

user/nsfbtest.dynelf: user/nsfbtest.c user/libnsfb.so.0
	@mkdir -p $(OBJ_DIR)/user
	$(MUSL_ELF_CC) -fPIC -pie -Os -Wall -I$(LNFB_DIR)/include \
	    -Wl,-dynamic-linker,$(DOS_LDSO) user/nsfbtest.c \
	    user/libnsfb.so.0 -o $@

# §M42 — the NetSurf browser BINARY.  scripts/build-netsurf.sh compiles the
# curated core+fb TU set and links a musl dynamic PIE against the store .so's
# (see that script for the whole story).  Slow (~146 TUs under emulation) →
# its own PHONY target, guarded on the prebuilt binary like libcss/libdom.
# Needs the NetSurf source (scripts/fetch-netsurf-libs.sh netsurf) + all the
# store libs' .so + the DejaVu TTFs staged under the fb res/fonts dir.
.PHONY: netsurf
netsurf: user/netsurf.dynelf
# Prerequisites MUST list every .so scripts/build-netsurf.sh links against (its
# full DT_NEEDED closure) — otherwise `make netsurf` on a clean tree fails to
# find libs that only `make iso`'s blob targets would otherwise have built (this
# bit the cross-arch rebuild: `ld: cannot find user/libnsgif.so.0`).
user/netsurf.dynelf: user/libcss.so.0 user/libdom.so.0 user/libhubbub.so.0 \
                     user/libwapcaplet.so.0 user/libparserutils.so.0 \
                     user/libnsutils.so.0 user/libnslog.so.0 user/libnspsl.so.0 \
                     user/libnsgif.so.0 user/libnsbmp.so.0 user/libnsfb.so.0 \
                     user/libpng16.so.16 user/libz.so.1 user/libfreetype.so.6 \
                     user/netsurf/dos_image_data.c user/netsurf/fetch_dos.c
	NS_CC=/src/$(MUSL_ELF_CC) NS_LDSO=$(DOS_LDSO) \
	    NS_MBEDTLS=/src/$(MBEDTLS_PREFIX) sh scripts/build-netsurf.sh

# The browser binary itself, embedded as a blob so the `netsurf` shell command
# execs it in ring 3 under the linux-abi personality (like wget) — it is a
# dynamic PIE, so ld.so loads its DT_NEEDED store .so's from /lib at run time.
$(OBJ_DIR)/user/netsurf_dynblob.o: user/netsurf.dynelf
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

# Its runtime resources: the fb frontend's res tree (css / Messages / welcome.html
# / icons …) at /res + the DejaVu TTFs at /res/fonts.  Packed into a flat archive
# that pkg.c unpacks into the VFS at boot (same format as the §M43 tcc rootfs).
user/netsurf_res.bin: user/netsurf.dynelf scripts/pack-rootfs.py
	@mkdir -p build/netsurf
	rm -f build/netsurf/Messages          # split-messages.pl opens O_EXCL
	perl third_party/netsurf/tools/split-messages.pl -l en -p any -f messages \
	    -o build/netsurf/Messages -i third_party/netsurf/resources/FatMessages
	python3 scripts/pack-rootfs.py $@ \
	    third_party/netsurf/frontends/framebuffer/res:/res \
	    build/netsurf/Messages:/res/Messages \
	    user/netsurf/fonts:/res/fonts

$(OBJ_DIR)/user/netsurf_res_blob.o: user/netsurf_res.bin
	@mkdir -p $(@D)
	objcopy --input-target=binary $(USER_OCARGS) $< $@

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
	# NOTE: scripts/build.sh keeps a PER-ARCH copy in build/.userartifacts/,
	# which this does NOT touch (it lives outside build/$(ARCH)) — so the next
	# build restores them instead of re-compiling the NetSurf/freetype stack.
	# `make clean-all` drops the cache too.
	rm -f user/*.muslelf user/*.dynelf user/*.cxxelf \
	      user/libgreet.so user/libcpplib.so user/libstdcxx.so \
	      user/libgccs.so user/ldmusl.so user/rootfs.bin user/libz.so.1 \
	      user/libpng16.so.16 user/libwapcaplet.so.0 user/libparserutils.so.0 \
	      user/libhubbub.so.0 user/libnsgif.so.0

clean-all:
	rm -rf build
