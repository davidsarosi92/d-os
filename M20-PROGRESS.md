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
| 3 | hal/x86_64 stubs: hal_arch.c, gdt.c, idt.c, tss.c, task_arch.c, switch.s, isr_stubs.s, usermode.s (compile-only) | 🔲 pending |  |
| 4 | hal/x86_64/vmm.c (4-level paging behind vmm.h API), widen vmm.h to uintptr_t | 🔲 pending |  |
| 5 | Full kernel_main link + boot; fix driver compile issues on 64-bit | 🔲 pending |  |
| 6 | IDT + PIT IRQ + scheduler + shell prompt up | 🔲 pending |  |
| 7 | SYSCALL/SYSRET (can defer to M20.5) | 🔲 deferable |  |
| 8 | Docs: DOCS.md §arch, PLAN.md ✅ flip, CLAUDE.md status | 🔲 pending |  |

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

Phase 3.  Create the per-arch HAL files under `kernel/hal/x86_64/`.
The goal is to mirror everything in `kernel/hal/x86/` so kernel_main
will link cleanly once Phase 5 lands.  Files to author (all
compile-only at the end of Phase 3 — no link target yet):

1. `hal_arch.c` — implements all of `hal_api.h`.  Most of it is
   line-for-line ports of i386 with 64-bit register names
   (pushfq/popq, rflags vs eflags).
2. `io.c` — copy of i386 io.c; `inb`/`outb` opcodes are identical
   in 64-bit mode, just with REX.W when accessing 64-bit regs (we
   don't).
3. `gdt.c` — 64-bit GDT.  Differences from i386:
   - Code segment uses L=1 (long mode bit, D must be 0).
   - Ring-3 code/data + TSS descriptor (TSS desc is 16 bytes in
     long mode, not 8).
   - No per-segment base/limit handling — long mode ignores them
     except for fs/gs base which we'll set via MSRs in a later
     milestone.
4. `tss.c` — 64-bit TSS: very different layout.  No ESP1/SS1/...
   stack arrays; instead RSP0/RSP1/RSP2 (8 bytes each) + 7 IST
   stacks.  Total size is 104 bytes (vs i386's 104 too,
   coincidentally — but the field layout is different).
5. `idt.c` — 64-bit IDT.  Each gate is 16 bytes (was 8 on i386):
   offset is now 64 bits split across three fields, and there's a
   new IST selector (3 bits) that lets the CPU switch to one of
   TSS.IST[1..7] on entry.
6. `isr_stubs.s` — 64-bit ISRs.  push all 16 GPRs (rax..r15), no
   segment register saving (segments are largely ignored in long
   mode), iretq instead of iret.  The C handler signature widens
   to take a 64-bit context struct.
7. `switch.s` — `context_switch(uint64_t* save_rsp_to, uint64_t
   new_rsp)`.  System V x86_64 ABI: callee-saved are rbx, rbp,
   r12-r15.  Args come in rdi/rsi (not the stack).
8. `usermode.s` — `enter_user_mode_wrap`: iretq instead of iret,
   pushes rsp/rflags/cs/rip + ss (note: 8-byte each, total 40
   bytes pushed).
9. `task_arch.c` — `hal_task_init_stack` builds the initial 64-bit
   frame layout that matches switch.s's pop order.

DoD for Phase 3: `make ARCH=x86_64` compiles each new .c/.s into a
.o file (we can list them in the Makefile and run `make` even
without a link target — `make build/x86_64/obj/.../foo.o` proves
each file is self-consistent).  Nothing actually runs yet.

---

## If a future session opens this

1. Read this whole file top-to-bottom.
2. `git log --oneline -20` to see what shipped.
3. Run i386 baseline test to confirm nothing rotted.
4. Run x86_64 test to confirm current state.
5. Pick up at §Next concrete action.
