# M20 — x86_64 (long mode) port — progress tracker

> **Purpose:** This file is the resumption-safe scratchpad for the M20
> milestone.  M20 is large (1500-2500 LOC across multiple subsystems)
> and likely spans multiple Claude sessions.  This file captures
> everything a fresh session needs to pick up cleanly: phase status,
> what was tried, what's broken, exact test commands, and the next
> concrete step.
>
> **Update discipline:** after every commit, flip the phase status,
> append a one-line "what shipped" note, and update §Next concrete
> action.  Delete this file once M20 is done (DOCS.md + PLAN.md hold
> the long-term record).

---

## North-star DoD (from PLAN.md §M20)

- [ ] `make ARCH=x86_64` builds a separate `kernel.bin64` (or
  equivalent under `build/x86_64/`).
- [ ] Boots in `qemu-system-x86_64`.
- [ ] All shell commands work (same DOD as i386).
- [ ] `DOCS.md` gains a "supported architectures" section.
- [ ] `CLAUDE.md` status row flipped to ✅ M20.

i386 baseline (`make` with no ARCH) MUST keep building and booting
unchanged through every phase.

---

## Phase status

| Phase | What | Status | Commit |
|---|---|---|---|
| 1 | Build infra: Makefile ARCH switch, linker-x86_64.ld, grub.cfg, empty hal/x86_64 skeleton that produces a valid (halting) multiboot2 ELF64 | ✅ shipped | (this commit) |
| 2 | boot.s: multiboot2 header + 32→64 long-mode entry + early serial "hello" print + halt | ✅ shipped | (this commit) |
| 3 | hal/x86_64 stubs: hal_arch.c, gdt.c, idt.c, tss.c, task_arch.c, switch.s, isr_stubs.s (compile-only; usermode.s deferred to M20.5) | ✅ shipped | (this commit) |
| 4 | hal/x86_64/vmm.c (4-level paging behind vmm.h API), widen vmm.h to uintptr_t | ✅ shipped | (this commit) |
| 5 | Full kernel_main link + boot; mb2 shim; SMP/syscall stubs; gated APIC init — shell prompt up | ✅ shipped | (this commit) |
| 6 | IDT + PIT IRQ + scheduler + shell prompt up | ✅ folded into Phase 5 | — |
| 7 | SYSCALL/SYSRET | 🔲 deferred to M20.5 | — |
| 8 | Docs: DOCS.md §arch, PLAN.md ✅ flip, CLAUDE.md status, delete M20-PROGRESS.md | 🔲 next | — |

---

## Design decisions (locked in)

1. **Multiboot2** — required for 64-bit (well, technically MB1 can boot a
   32-bit ELF that transitions to long mode, but MB2 is cleaner and
   PLAN.md says MB2).  We keep MB1 for the i386 build.
2. **64-bit ELF binary** with mixed `bits 32`/`bits 64` in boot.s.
   `_start` is 32-bit code; after CR0/CR4/EFER setup it jumps to
   64-bit code.
3. **Identity-map the first 1 GiB** in early page tables.  Higher-half
   relocation (link at `0xFFFFFFFF80000000`) is a Phase 5+ concern.
4. **mcmodel=large** initially (allows linking anywhere).  Switch to
   `mcmodel=kernel` later when we move kernel to the upper canonical
   half.
5. **vmm.h API widens** to `uintptr_t` for virt/phys (Phase 4).  On
   i386 `uintptr_t == uint32_t` so no source change at call sites.
   x86_64 callers can pass full 64-bit addresses.
6. **vmm.c moves to per-arch**: `kernel/hal/x86/vmm.c` (current) +
   `kernel/hal/x86_64/vmm.c` (new).  Remove `kernel/mem/vmm.c`.
7. **All legacy PC drivers** (PIT, 16550 serial, PS/2 keyboard,
   virtio-blk legacy port I/O) compile under both ARCHes.  They use
   `inb`/`outb` which work identically on x86_64.
8. **No SMP under x86_64 in M20.**  Bring the AP path up in M20.5 or
   a follow-up.  M20's DoD is UP boot + shell.
9. **No SYSCALL/SYSRET in M20.**  Keep `int 0x80` for now (works on
   both archs).  SYSCALL is M20.5 or later.

---

## Architecture-relevant file groups

### Stays as-is on both archs
- `kernel/core/` — all files (need uintptr_t hygiene; spot-check)
- `kernel/fs/` — all files
- `kernel/mem/pmm.c`, `kmalloc.c`, `slab.c`
- `kernel/drivers/` — all (port I/O and MMIO same on x86_64)
- `kernel/includes/hal_api.h` — same interface

### Per-arch (different impl under each `hal/<arch>/`)
- `boot.s`, `hal_arch.c`, `gdt.c`, `idt.c`, `tss.c`, `isr_stubs.s`,
  `switch.s`, `usermode.s`, `task_arch.c`, `ap_trampoline.s`,
  `lapic.c`, `ioapic.c`, `smp.c`, `pci.c`, `vmm.c` (post-move),
  `io.c`

### Widens API (Phase 4)
- `kernel/includes/vmm.h` — uint32_t → uintptr_t
- `kernel/mem/vmm.c` → DELETE (moves to hal/x86/vmm.c)

---

## Test commands

### i386 baseline (must keep working)
```sh
make                          # uses ARCH=i386 (default)
qemu-system-i386 -display none -no-reboot -serial file:/tmp/log-i386.log \
    -monitor stdio -m 256M -cdrom build/d-os.iso < /dev/null > /tmp/qemu-i386.out 2>&1 &
# wait ~5s then check log
```

### x86_64 build & boot
```sh
make ARCH=x86_64
qemu-system-x86_64 -display none -no-reboot -serial file:/tmp/log-x64.log \
    -monitor stdio -m 256M -cdrom build/x86_64/d-os.iso < /dev/null > /tmp/qemu-x64.out 2>&1 &
# wait ~5s then check log
```

For SMP (later, M20.5):
```sh
qemu-system-x86_64 -smp 4 ... 
```

---

## Lessons learned (filled in as we ship phases)

**Phase 1:**
- `objcopy --input-target=binary` mints symbol names from the *input
  filename* (with `/` and `.` replaced by `_`).  When we moved the
  build into `build/$(ARCH)/obj/...`, the symbols became
  `_binary_build_i386_obj_kernel_..._start` instead of
  `_binary_kernel_hal_x86_..._start`.  The fix: keep the `.bin` at
  its source-relative path; only the `.o` wrapper goes into the
  per-arch build tree.  See Makefile rule for
  `ap_trampoline_blob.o`.

**Phase 5:**
- Multiboot2 → mb1 shim (`kernel/hal/x86_64/mb2.c`) keeps the rest
  of the kernel unchanged.  Tag types we translate: 4 (basic mem),
  6 (memory map — repacked into mb1's size-prefixed format),
  8 (framebuffer).  Unknown tags are skipped silently.
- Multiboot2 header must explicitly REQUEST the framebuffer via a
  type-5 header tag (`width`, `height`, `depth`).  Without it GRUB
  doesn't deliver a framebuffer tag at runtime and fb_terminal stays
  inert.  The i386 multiboot1 equivalent was `flags |= bit 2` plus
  mode_type/width/height/depth fields in the same header.
- Bridge `x86_64_main_entry` in `kernel/hal/x86_64/main_entry.c` is
  called from boot.s's long_mode_entry.  It does the mb2 translation
  and then calls `kernel_main(MULTIBOOT_BOOTLOADER_MAGIC, mb1_ptr)`,
  so kernel.c stays arch-independent.
- `kernel_main`'s `mb_info` arg widened from `uint32_t` to
  `uintptr_t`; no caller change needed on i386 (uintptr_t = uint32_t).
- SMP/APIC init blocks in kernel.c gated under
  `#if defined(__i386__)` — x86_64 stays on the 8259 PIC for UP IRQs
  (PIT works fine via legacy delivery).  LAPIC/IOAPIC/SMP x86_64
  port lands in M20.5.
- `m20_stubs.c` provides UP-friendly no-op implementations for the
  symbols kernel core/drivers reference unconditionally
  (lapic_*, ioapic_*, smp_*, syscall_dispatch, enter_user_mode_wrap,
  xhci_poll).  Lives in kernel/hal/x86_64/; shrinks as later phases
  add real impls.
- `kprintf` supports only `%x %u %d %s %c %p %%` — no `%l` prefix.
  Passing `%lx` for a `uint64_t` arg corrupts the va_arg sequence
  (the `default:` case echoes "%l" verbatim without consuming an
  arg, so every subsequent format reads the wrong slot).  Workaround:
  cast 64-bit values to uint32_t and use `%x`, or split into hi/lo
  pairs.  Real fix (a printf rewrite) is a post-M20 polish item.

**Phase 4:**
- `vmm.h` API widened to `uintptr_t` for virt/phys/return types
  (vmm_map, vmm_map_4mib, vmm_unmap, vmm_translate,
  vmm_kernel_pd_phys).  i386 build sees uintptr_t == uint32_t, so
  no caller changes were needed; the i386 vmm.c keeps its uint32_t
  internals but casts at the API boundary.
- `kernel/mem/vmm.c` moved to `kernel/hal/x86/vmm.c` — it's
  i386-specific (2-level paging + PSE), so its location now
  reflects that.  Makefile updated.
- x86_64 vmm.c **inherits** boot.s's PML4/PDPT/PD via extern symbols
  rather than rebuilding the kernel page tables from scratch.  This
  avoids the chicken-and-egg of building new tables while
  identity-mapped on the old ones.  vmm_init only logs status.
- `vmm_map_4mib` semantic preservation: on i386 it's literally a
  PSE 4 MiB PDE; on x86_64 it installs TWO adjacent 2 MiB large
  PD entries to keep the 4 MiB contract.  Callers (fb_terminal,
  xhci) don't need source changes.
- x86_64 `rdmsr` cannot use the `=A` GCC asm constraint (which
  means "edx:eax pair as one 64-bit value" — long-mode rdmsr
  zero-extends into rax/rdx, not the eax+edx legacy split).
  Use two separate `=a`/`=d` outputs and combine in C.

**Phase 3:**
- `idt.h`'s `struct int_frame` is necessarily arch-specific (different
  push count, different register names, different segment-save
  semantics).  Solution: `#if defined(__x86_64__)` conditional inside
  the header, with both layouts having the same int_no + err_code
  field names so portable handlers (pit_irq, keyboard_irq) compile
  for both archs.  Register-set fields (eax vs rax) only get touched
  in arch-specific code (syscall.c — and even there only for i386
  until M20.5 lands SYSCALL/SYSRET on x86_64).
- `tss.h` API widened to `uintptr_t` for the pointer-shaped values
  (tss_set_kernel_stack, tss_get_addr) so the same signature works
  on both 32- and 64-bit kernels.  No external callers needed
  changes since uintptr_t == uint32_t on i386.
- The x86_64 TSS descriptor is **16 bytes** (2 GDT slots) because it
  holds a 64-bit base — see kernel/hal/x86_64/gdt.c set_tss_entry.
  Skipping the high half writes garbage into the next slot, which
  the CPU sees as a malformed system descriptor and #GPs on `ltr`.
- Long-mode interrupt entry always pushes 5 quadwords (ss, rsp,
  rflags, cs, rip) — no "only on privilege change" asymmetry like
  i386.  Our isr_stubs.s relies on that uniform shape to compute
  16-byte stack alignment before `call isr_handler`.
- `Makefile` has no header dependencies, so changing a .h does NOT
  trigger a .c rebuild.  Run `make clean ARCH=<arch>` after editing
  shared headers.  Quality-of-life fix (auto-generated .d files via
  `gcc -MMD`) deferred to a post-M20 polish commit.

**Phase 2:**
- The Intel-prescribed long-mode entry sequence (Vol 3A §9.8.5) is
  PAE → CR3 → EFER.LME → CR0.PG → far-jmp.  Skipping the far-jmp
  leaves you in long-mode-compatibility submode (32-bit code with
  64-bit paging) which silently miscomputes near-everything because
  REX prefixes aren't recognised.  Always close the transition with
  a far-jmp through a 64-bit code segment.
- The 32-bit `lgdt` operand is 6 bytes (16-bit limit + 32-bit base);
  the 64-bit `lgdt` operand is 10 bytes (16+64).  We load the
  6-byte form in 32-bit mode just before the far-jmp; the CPU
  zero-extends the base when transitioning, so the same GDTR
  pointer remains valid in long mode as long as `gdt64` lives in
  the lower 4 GiB (which it does, being .rodata in the
  1-MiB-loaded kernel).
- For long-mode code descriptors the L bit (bit 53 of the
  descriptor) MUST be 1 and the D bit MUST be 0.  Setting both
  triggers #GP at the far-jmp.  Data descriptors are mostly
  ignored in long mode but ss should still load a non-null
  selector — some instructions check ss.base regardless.
- QEMU's COM1 accepts polled out-without-configuration, so we can
  emit serial bytes from the very first long-mode instruction
  without an init sequence.  Real hardware would need the M2
  serial driver's full LCR/FCR/MCR setup.

---

## Next concrete action

Phase 8 (docs + final commit).
- DOCS.md gains §arch / "Supported architectures" section pointing
  out i386 (full) and x86_64 (UP-only, SMP/SYSCALL deferred to M20.5).
  Change-log entry for M20.
- PLAN.md M20 row flipped to ✅, condensed to "Shipped, see DOCS.md
  §arch" pointer.
- CLAUDE.md status snapshot bumped to "M1–M20 + M18.5", with M20.5
  / M21 / M22 / M23 / M24 left as next options.
- Delete this M20-PROGRESS.md file — DOCS.md + PLAN.md hold the
  long-term record from here.

---

## If a future session opens this

1. Read this whole file top-to-bottom.
2. `git log --oneline -20` to see what shipped.
3. Run i386 baseline test to confirm nothing rotted.
4. Run x86_64 test to confirm current state.
5. Pick up at §Next concrete action.
