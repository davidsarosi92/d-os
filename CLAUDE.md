# d-os — orientation for Claude

> **Purpose of this file:** give the assistant enough state to start
> working without re-reading every doc.  Keep it tight — it loads
> automatically into every session.  Update only when status moves
> milestones or when a hard convention changes.

## What this is

Hobby / teaching i386 OS kernel.  Boots from GRUB → installs its own
GDT / IDT / paging → talks to a 1024×768 framebuffer with an embedded
8×8 bitmap font → IRQ-driven PS/2 keyboard + xHCI USB host with
boot-HID keyboards → ramfs + devfs + procfs mounted at
`/`, `/dev`, `/proc` → preemptive round-robin scheduler with ring-3
syscalls via `int 0x80` → virtio-blk block device exposed as
`/dev/vda` → exFAT mountable as `/mnt` → screen split into multiple
shell panes (Alt-N to focus, `pane split h|v` to split).

## Status (update when a milestone ships)

✅ **M1 – M20 + M18.5** shipped.  Highlights so far: VFS + ramfs +
exFAT on virtio-blk, devfs + procfs, preemptive scheduler with
lock-handoff, multi-pane shell, xHCI USB + HID, keyboard layouts,
HAL cut (`hal_api.h`), SMP on i386 (APIC + IOAPIC + INIT/SIPI APs
+ per-CPU current + real cmpxchg spinlocks), memory at scale (per-
zone buddy PMM + slab + per-CPU magazines), APs scheduling (LAPIC
timer per-CPU, per-CPU idle tasks, scheduler idle-fallback policy),
**x86_64 (long mode) UP port — multiboot2 + 32→64 entry, 4-level
paging, 64-bit GDT/IDT/TSS/ISR stubs, vmm.h API widened to
uintptr_t, full kernel_main runs to the same shell prompt as i386;
SMP + SYSCALL/SYSRET on x86_64 deferred to M20.5**.

🔲 **Next options** (pick one):

- **M20.5** — x86_64 SMP + APIC + SYSCALL/SYSRET (closes the
  x86_64 port).
- **M21** — aarch64 port.  Third arch, real torture test of HAL
  portability (no port I/O, GIC instead of APIC, EL1/EL0 instead
  of rings).
- **M22** — GUI infrastructure (compositor + windows; Wayland-reuse
  evaluation phase per §M22).
- **M23** — Audio (AC97 → HDA → I2S).
- **M24** — Network (NIC → IP/UDP/TCP → sockets).
- **Polish backlog** (smaller, can interleave): per-CPU runqueue +
  load balancer, per-CPU `preempt_count`, task affinity / taskset,
  cross-CPU preempt IPI (vector 0x41 reserved), MSI/MSI-X,
  printf rewrite to support %l (M20 lesson: blocked us briefly).

🔲 **PLAN extensions (placeholders, design only):**
- §M22 — GUI includes a Wayland-reuse evaluation phase
  (libwayland-server port vs. custom protocol).
- §M23 — Audio subsystem (AC97 → HDA → I2S).
- §M24 — Network stack (virtio-net → IP/UDP/TCP → sockets).

## Hard conventions (do NOT deviate without asking)

1. **Heavy English comments in code.**  Conversation with the user is
   in **Hungarian** — reply in Hungarian.
2. **Drivers self-register** via `MODULE()` (legacy) or `DRIVER()`
   (probe/init/shutdown lifecycle) — never edit `kernel_main` to wire
   a driver in.
3. **Arch portability:** everything x86-specific lives under
   `kernel/hal/x86/`.  Core code (`kernel/core/`, `kernel/mem/`,
   `kernel/fs/`, portable drivers) must NOT do `__asm__`, port I/O,
   reference descriptor tables, or assume page-table layout.  Target
   arches: x86 (now), x86_64, aarch64.
4. **SMP-ready on UP:** lock + per-CPU APIs in place even when no-op
   today.  Don't ship code that would have to be hunted down later
   for a second core.
5. **Stable interfaces from day one.**  Define the final API shape
   even when the first implementation is a stub.  Don't ship "we'll
   wrap it later."
6. **Linux-inspired, not Linux-bound.**  Adopt the patterns that
   solve a concrete problem we have; reject what's accidental
   history (`kobject`, sysfs, RCU until needed, namespaces, cgroups).

## Where to read (on demand, not eagerly)

- **DOCS.md** — current state, per-component reference.  Has a TOC at
  the top — use `Read` with `offset`/`limit` to land in a specific
  section.
- **PLAN.md** — roadmap + design sketches for upcoming milestones.
  Same TOC pattern.
- **README.md** — public-facing intro.
- Source: arch-independent under `kernel/{core,mem,fs,drivers}/`;
  x86 specifics under `kernel/hal/x86/`.

## Build / run

```sh
./scripts/build.sh                    # default ARCH=i386 → build/i386/d-os.iso
./scripts/run_qemu.sh                 # i386 GUI window, NO disk attached

ARCH=x86_64 ./scripts/build.sh        # → build/x86_64/d-os.iso
ARCH=x86_64 ./scripts/run_qemu.sh     # x86_64 in qemu-system-x86_64
```

`make clean` wipes the current ARCH only; `make clean-all` wipes all
builds.  No header dependencies (yet) — after editing a shared
header (e.g., `hal_api.h`, `vmm.h`, `idt.h`), run `make clean
ARCH=<arch>` to force a rebuild.

For block-layer / future-fs testing, the disk image must be attached
manually (the script intentionally doesn't add `-drive`):

```sh
dd if=/dev/zero of=build/test.img bs=1M count=4   # once
qemu-system-i386 -cdrom build/i386/d-os.iso \
    -drive if=virtio,file=build/test.img,format=raw
```

For headless / automated testing (capture serial log):

```sh
qemu-system-i386 -display none -no-reboot \
    -serial file:/tmp/serial.log -monitor stdio \
    -m 256M -cdrom build/i386/d-os.iso \
    -drive if=virtio,file=build/test.img,format=raw
```

Block / USB drivers are i386-only today; x86_64 boots without them
(virtio-blk + xhci need a 64-bit DMA-path revisit — M20.5+).

## Session etiquette

- Use **TodoWrite** for any multi-step work (every milestone qualifies).
- When a milestone ships:
  - Add a component section to **DOCS.md** (under `## 4. Components`).
  - Add a change-log entry to DOCS.md (`## 8. Change log`).
  - Flip the PLAN.md status table row to ✅ and condense the design
    section to a one-paragraph "Shipped, see DOCS.md §…" pointer.
- **Boot-test in QEMU** before claiming done.  For most milestones a
  sendkey-driven script + `-serial file:` capture is enough; for the
  framebuffer text path, `pmemsave 0xb8000` + a small Python script
  renders the cells to ASCII.
- **Pitfalls hit during bring-up** go into BOTH the source comment
  (so future readers see why) AND the PLAN.md milestone as a "Lesson
  learned" note (so the design-time intuition is preserved).

## Where things live (when you're modifying)

| Concern                              | File                                  |
|--------------------------------------|---------------------------------------|
| Boot order, new milestone wiring     | `kernel/core/kernel.c`                |
| Adding a shell command               | `kernel/core/shell.c`                 |
| Adding a new .c to the build         | `Makefile` (C_SRCS or ASM_SRCS)       |
| New linker section                   | `linker.ld`                           |
| New driver class                     | `kernel/includes/<class>.h` + impl    |
| x86 arch primitives                  | `kernel/hal/x86/`                     |

## Memory pointers (cross-project user preferences)

These live under `~/.claude/.../memory/` and are auto-indexed in
MEMORY.md:
- `feedback_dos_style.md` — heavy English comments, Hungarian
  conversation, keep DOCS.md current.
- `project_dos_arch_goals.md` — modular driver registry, multi-arch
  HAL, multi-session shell, PLAN.md is the roadmap.
