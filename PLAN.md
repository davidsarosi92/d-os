# d-os — work plan

> **What this file is:** the forward-looking roadmap for d-os.  Each
> milestone has a status, a design sketch, and explicit "definition of
> done" criteria so a fresh session (human or AI) can resume cleanly.
>
> **What `DOCS.md` is:** the *current state* — every component that
> already exists, with its API and quirks.  Once a milestone here ships,
> its design notes graduate into a section of DOCS.md and the entry here
> shrinks to a one-line "done" pointer.

> **Navigation tip for assistants:** this file is ~960 lines.  CLAUDE.md
> already has the high-level status — come here only to read a specific
> §-section.  Use `Read offset/limit` based on the TOC below; the
> milestone you're working on is usually the only section you need.

## Table of contents

(Approximate line numbers; refresh with
`grep -n '^## §\|^## How\|^## Change' PLAN.md` if it drifts.)

| Section | Purpose | ~line |
|---------|---------|------:|
| North-star design constraints | The 7 rules | 49 |
| Status snapshot | Done + backlog table | 115 |
| §P  | Portability cut (x86 → x64 / ARM) | 171 |
| §S  | Shell as swappable console provider | 244 |
| §SMP | Concurrency readiness on UP | 290 |
| §MEM | Memory at scale (allocator tiers) | 324 |
| §DRV | Linux-inspired, not Linux-bound | 361 |
| §G  | Git hygiene + repo onboarding | 398 |
| §M1 – M11 | Shipped milestones (one-liners) | 438 |
| §M12 | exFAT + multi-FS abstraction | 569 |
| §M13 | Preemptive scheduling | 617 |
| §M14 | Multi-session shell (FB panes) | 643 |
| §M15 | USB host stack + HID keyboard | 676 |
| §M16 | Keyboard layout abstraction | 713 |
| §M17 | Portability cut — extract hal_api.h | 752 |
| §M18 | SMP (APIC, AP boot, per-CPU, locks) | 774 |
| §M19 | Memory at scale (slab, huge pages) | 804 |
| §M20 | x64 (long mode) port | 836 |
| §M21 | ARM (aarch64) port | 867 |
| §M22 | GUI infrastructure (+ Wayland reuse if viable) | 894 |
| §M23 | Audio subsystem | ~1040 |
| §M24 | Network stack (Ethernet → TCP/IP → sockets) | ~1080 |
| How to use this document | Workflow rules | 930 |
| Change log | Plan-doc revision history | 945 |

---

## North-star design constraints

These rules apply to every milestone below and override local
convenience when they conflict.

1. **Modularity (driver registry).**  Every driver class — video,
   input, fs, console, shell, timer source, future ones — uses the same
   linker-section + `MODULE()` registration pattern.  Adding a new
   driver = drop a `.c` file with a registration line; *never* edit
   `kernel_main` to wire it in.

2. **Architecture portability.**  Today x86 (i386).  Tomorrow x64,
   ARM, RISC-V.  Therefore:
   - All arch-specific code lives under `kernel/hal/<arch>/`.
   - Core code (`kernel/core/`, `kernel/mem/`, `kernel/fs/`,
     `kernel/drivers/<class>/` for portable drivers) talks only through
     `kernel/includes/hal_api.h`.
   - x86-only concepts (GDT, IDT, TSS, port I/O, PIC, PIT, multiboot1)
     stay behind that wall.
   - When adding a new HAL primitive, declare it in `hal_api.h` first,
     then implement it in `kernel/hal/x86/`.  Future arches add their
     own implementation; core code is unchanged.

3. **Stable interfaces from day one.**  Define the final API shape
   even when the first implementation is a stub or in-memory.  Don't
   ship "we'll wrap it later" — wrap it now.

4. **Multi-session by design.**  Anything that holds per-user / per-
   shell / per-console state must already be expressed as instance
   data (a struct), even if there is only one instance today.  Avoid
   global singletons that would have to be refactored when a second
   session shows up.

5. **SMP-ready, even on UP.**  Code that *will* race on multi-CPU
   hardware must already be written assuming it might.  That means:
   - locking primitives (`spinlock`, `mutex`) used at every shared-
     state boundary, even when they no-op on a single-CPU build;
   - per-CPU data accessed through a `percpu_get(struct foo)` macro
     even if today there's only one bank;
   - no "I know it's UP today" shortcuts that would need to be hunted
     down later when the second core boots.
   The aim is "going SMP is a programming exercise, not a redesign."

6. **Memory at scale.**  Targeting machines with very large RAM (≥
   tens of GiB) and high allocation throughput.  Implications:
   - keep the bitmap PMM for now, but plan a buddy / size-class
     allocator behind the same `pmm_alloc_frame` interface;
   - `kmalloc` becomes a slab allocator on top of the page allocator;
   - kernel mappings should use 2 MiB / 1 GiB pages where they make
     sense (low TLB pressure for the big identity / direct-map
     region);
   - NUMA support stays out of v1 but the page allocator should not
     bake in a single-pool assumption that fights NUMA later.

7. **Linux-inspired, not Linux-bound.**  Adopt the patterns Linux
   has demonstrated work (driver registry, devfs, procfs, file_ops on
   everything).  Reject the parts that are accidents of history,
   binary-compat baggage, or too heavy for a small kernel
   (`kobject`, full `sysfs` complexity, the Linux scheduler's six
   classes, RCU until needed, capabilities, namespaces, cgroups).
   When in doubt: prefer a clean d-os shape that can grow than a
   carbon-copy.  The doc / commit messages should justify divergences
   instead of pretending they don't exist.

---

## Status snapshot

Everything in DOCS.md §7 marked `[x]` is done.  Below is the active
backlog.

### Done (M1–M7)

| # | Milestone                                  | Section |
|---|--------------------------------------------|---------|
| 1 | kmalloc — heap allocator over VMM          | DOCS §4.10 |
| 2 | Driver registry / module framework         | DOCS §4.0 |
| 3 | Timer (PIT IRQ0) + millisecond tick        | DOCS §4.X |
| 4 | VFS skeleton + ramfs                       | DOCS §4.X |
| 5 | Config store on top of VFS                 | DOCS §4.X |
| 6 | TSS + user-mode jumps (ring 3)             | DOCS §4.X |
| 7 | Process struct + scheduler                 | DOCS §4.X |

### Active backlog — by theme, not strict order

The order below is a *suggestion* based on dependency (what unblocks
what); a session can pick a theme and push on it.

| #   | Milestone                                       | Theme            | Section |
|-----|-------------------------------------------------|------------------|---------|
| G   | Git hygiene: init, .gitignore, README, license  | Repo             | §G      |
| M8  | Driver lifecycle scaffold (`driver_ops`)        | Driver framework | ✅ DOCS §4.0 |
| M9  | `devfs` — drivers as files under `/dev`         | Driver framework | ✅ DOCS §4.X |
| M10 | `procfs` — kernel state as files under `/proc`  | Driver framework | ✅ DOCS §4.X |
| M11 | Block layer + first block driver (virtio-blk)   | Storage          | ✅ DOCS §4.X |
| M12 | exFAT (with multi-FS abstraction for FAT/NTFS)  | Storage          | ✅ DOCS §4.X |
| M13 | Preemptive scheduling (timer IRQ → schedule)    | Concurrency      | ✅ DOCS §4.X |
| M14 | Multi-session shell with FB pane splitting      | UX               | ✅ DOCS §4.X |
| M15 | USB host stack + USB HID keyboard              | Input            | ✅ DOCS §4.X |
| M16 | Keyboard layout abstraction (US, HU, DE, …)     | Input            | ✅ DOCS §4.X |
| M17 | Portability cut — extract `hal_api.h`           | Architecture     | ✅ DOCS §4.X (partial — see notes) |
| M18 | SMP support — APIC, AP boot, per-CPU, locking   | Concurrency      | ✅ DOCS §4.X |
| M19 | Memory at scale — slab, huge pages, near-NUMA   | Memory           | ✅ DOCS §4.8, §4.10 |
| M20 | x64 (long mode) port                            | Architecture     | §M20    |
| M21 | ARM (aarch64 generic / RPi) port                | Architecture     | §M21    |
| M22 | GUI infrastructure (+ Wayland reuse if viable)  | UX               | §M22    |
| M23 | Audio subsystem (AC97 / HDA / I2S)              | Devices          | §M23    |
| M24 | Network stack (NIC → TCP/IP → sockets)          | Networking       | §M24    |

### Cross-cutting constraints

These are not milestones — they are rules that touch every milestone
above.  Each has a section below describing what to watch for.

| ID  | Constraint                                  | Section |
|-----|---------------------------------------------|---------|
| §P  | Portability — no x86 leak into core         | §P      |
| §S  | Shell as a swappable console provider       | §S      |
| §SMP| Lock + per-CPU discipline even on UP        | §SMP    |
| §MEM| Allocator interfaces sized for big systems  | §MEM    |
| §DRV| Linux-inspired but not Linux-bound          | §DRV    |

---

## §P — Portability cut

**Goal:** no `kernel/core/`, `kernel/mem/`, `kernel/fs/`, or
`kernel/drivers/<class>/` file directly references x86 instructions,
ports, descriptor tables, or multiboot fields.  All such access goes
through `hal_api.h`.

**First-round target arches:**
- **x86 (i386)** — current, working.  Stays the reference port.
- **x86_64 (long mode)** — second port; same family but different
  page table format (4-level), different boot path (UEFI / multiboot2),
  different syscall ABI (`syscall`/`sysret` vs. `int 0x80`).
- **aarch64 (ARMv8)** — third port; very different MMU (granule
  tables, ASIDs), interrupt controller (GIC vs. APIC), bootloader
  (UEFI on most boards, U-Boot on others), no port I/O at all
  (everything is MMIO).

The `hal_api.h` interface must be expressive enough that none of
these need core-code branches.  Where one arch has a feature another
doesn't (e.g. x86 port I/O), it stays in that arch's HAL only and is
called only from drivers that are themselves arch-gated.

### What `hal_api.h` should expose

A first cut, to refine as milestones land:

```
/* CPU control */
void     hal_cpu_halt(void);            /* hlt / wfi */
void     hal_cpu_pause(void);           /* pause / yield */
void     hal_intr_enable(void);         /* sti / cpsie */
void     hal_intr_disable(void);        /* cli / cpsid */
uint32_t hal_intr_save(void);           /* save+disable, returns prior state */
void     hal_intr_restore(uint32_t);

/* Interrupt routing — abstract over PIC / GIC / APIC */
typedef void (*hal_irq_fn)(void* ctx);
int      hal_irq_install(int irq, hal_irq_fn fn, void* ctx);
void     hal_irq_eoi(int irq);

/* MMU — abstract over 2-level / 4-level / ARM tables */
int      hal_map(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags);
int      hal_unmap(uint64_t virt, uint64_t size);

/* Time — abstract over PIT / HPET / ARM generic timer */
uint64_t hal_ticks_ms(void);

/* Memory probing — abstract over multiboot / device tree */
int      hal_meminfo(struct hal_mem_region* out, int max);
```

### Existing code that must move behind the wall

- `inb` / `outb` / `outw` are x86-only.  Stay declared in `hal.h` (which
  becomes the i386 internal HAL header), but any *core* consumer must
  switch to a portable abstraction.  Drivers that are inherently legacy
  PC (8042 keyboard, 8259 PIC, PIT) keep using port I/O directly because
  they will only ever exist on PC-compatible builds.
- `kernel_main` signature `(uint32_t magic, uint32_t info)` is multiboot1-
  specific.  When porting, the arch entry stub will normalize whatever
  the bootloader hands it into a generic `struct boot_info*` that
  `kernel_main(struct boot_info*)` consumes.

### Refactor plan
1. After M2 lands the registry framework, add `kernel/includes/hal_api.h`.
2. Move declarations one at a time, fixing core callers as we go.
3. The x86 `kernel/hal/x86/` files become impls of the API.
4. CI gate (informal): grep for `inb\|outb\|cli\|sti\|__asm__` outside
   `kernel/hal/<arch>/` should only match driver code that is opted-in
   to PC-only builds.

---

## §S — Shell as swappable console provider — **shipped with M14**

The console abstraction landed in M14 as `struct vc` (virtual
console) — a leaf in the FB split tree.  Each shell runs as its own
task bound to its own `vc`; `kprintf` routing happens through
console.c's per-task emit hook installed by vc_init.  See DOCS.md
§4.X (Virtual consoles / pane split) for the as-built shape.  The
notes below survive only as the design-time rationale.

**Goal:** the shell is not a special kernel function but a registered
"console provider" that runs against an abstract `console` instance.
Multiple consoles can coexist — different regions of the framebuffer,
different shells, etc.  Even with one CPU and no scheduler today, the
data structures support N sessions.

### Abstractions to introduce

```
struct console {
    /* output side: where bytes the shell prints go */
    void (*putchar)(struct console*, char);
    void (*clear)  (struct console*);
    /* input side: where characters come from (NULL = no input) */
    int  (*getchar_nowait)(struct console*);
    /* per-instance state: cursor, fg/bg, viewport rect on FB, ... */
    void* state;
};

struct shell_provider {
    const char* name;
    void (*run)(struct console*);    /* never returns; one instance per call */
};
```

A console can be:
- the whole framebuffer (current default)
- a horizontal/vertical split rectangle on the FB
- a serial line
- a future "virtual console" backed by a scrollback buffer

The current `terminal_*` API stays for in-kernel diagnostic prints
(boot log, panic), but user-facing prompts/input flow through the
console abstraction so multiple shells don't fight over the screen.

### When does this land?

Stub the structs in M2 (when the registry framework is built).  Wire
one console + one shell instance through them then.  Splitting into
multiple instances waits until M7 (when the scheduler can give each
shell its own context).  M14 is the actual implementation milestone.

---

## §SMP — concurrency readiness

**Goal:** code we add today must boot identically on a UP build but be
*correct* on SMP, with zero hidden races to track down later.

### Standing rules
- Every shared mutable state gets a lock.  Lock APIs:
  ```
  struct spinlock { ... };
  void  spin_init(struct spinlock*);
  void  spin_lock(struct spinlock*);          /* irq-safe variant: ..._irqsave */
  void  spin_unlock(struct spinlock*);
  /* mutex with sleep — only meaningful once we have multiple tasks */
  struct mutex { ... };
  ```
  On UP they degrade to cli/sti pair (irqsave) or to a counter increment.
- No "I'll add the lock when SMP comes" — the lock goes in at first
  write, even if it never contends.
- `current` task pointer becomes per-CPU — accessed through `this_cpu()`
  which today returns CPU 0 always.
- `kmalloc` and `pmm_alloc_frame` already need to grow per-CPU caches
  (see §MEM) — don't lock the whole heap on the hot path.
- Memory barriers: introduce `smp_mb()`, `smp_rmb()`, `smp_wmb()`
  macros that today expand to `__sync_synchronize()` / nothing.

### What lands when
- Locking primitives ship with M13 (preemption — that's when races
  first matter).
- Per-CPU machinery ships with M18 (real SMP boot).
- Until M18 the per-CPU index is hardcoded 0; the macros / APIs are
  already in place.

---

## §MEM — memory at scale

**Goal:** the allocator interfaces we expose now must scale to many-
GiB systems with high allocation pressure, even if the first
implementations are simple.

### Layered model
```
+-----------------------+
|  callers              |  kmalloc / kcalloc / kfree (small allocs)
|                       |  page_alloc / page_free  (4 KiB / huge pages)
+-----------------------+
|  slab allocator       |  size-class pools, per-CPU magazines (M19)
+-----------------------+
|  page allocator       |  buddy or size-class on top of PMM (M19 expand)
+-----------------------+
|  PMM                  |  bitmap today; future: per-zone (DMA / NORMAL /
|                       |  HIGHMEM / per-NUMA-node) buddy allocator
+-----------------------+
|  hardware memory map  |  multiboot today, ACPI SRAT later for NUMA
+-----------------------+
```

### Standing rules
- `kmalloc`'s public signature does not change as we swap the inner
  algorithm.  Today block free-list; M19 → slab.
- Page allocations go through `page_alloc(order)` not `pmm_alloc_frame`
  for anything bigger than a frame.  PMM gains a buddy in M19.
- 2 MiB / 1 GiB pages used wherever the kernel maps a contiguous
  region (kernel text/data, direct map, frame buffer).  That's a VMM
  upgrade, scheduled with M19.
- A single `struct page` (or equivalent) is reserved for tracking
  every physical frame eventually.  Don't introduce one yet — it's
  expensive — but do not ship code that *prevents* its introduction.

---

## §DRV — Linux-inspired, not Linux-bound

**Goal:** capture the design value of Linux's driver patterns without
inheriting its weight.

### What we adopt
- **`MODULE()` registry** ← `module_init` / linker-section trick.
- **`devfs`** ← Linux's character/block device files.
- **`procfs`** ← Linux's `/proc` for kernel introspection.
- **`file_operations`-shaped per-driver ops** so the same VFS handle
  can talk to a regular file, a device, or a synthetic file.
- **A device tree** — eventually — to express parent/child (USB hub
  → keyboard, PCI → AHCI controller → disk).

### What we don't
- `kobject` / `kref` infrastructure — too heavy.  Plain refcounts
  where needed.
- Full sysfs — `/proc` covers our needs and is simpler.
- Capabilities, namespaces, cgroups — way out of scope.
- The Linux scheduler's class hierarchy — we'll have one or two
  scheduling classes, no more.
- RCU — until we hit a read-mostly bottleneck, the answer is
  rwlocks.

### How to evaluate a Linux pattern before adopting it
Ask in this order:
1. What concrete problem does this pattern solve?
2. Do we have that problem today, or in the next 2 milestones?
3. Is there a simpler shape that solves *our* version of the problem?
4. If we adopt the Linux pattern, what assumptions does it bake in
   that we'd regret?

If the pattern survives all four, adopt it explicitly.  Document the
divergence from Linux in DOCS.md so future-us knows it was deliberate.

---

## §G — Git hygiene + repo onboarding

**Goal:** make d-os a respectable open-source-ready repo: a clean
`git init`, a useful `.gitignore`, a README that introduces the
project, a license, and (eventually) CI.

### Subtasks

- **`.gitignore`** at the project root, covering at minimum:
  - `build/` (entire build output)
  - `iso/` (the dynamic ISO staging area; the source `boot/grub/` stays)
  - `*.o`, `*.bin`, `*.iso`, `*.elf`, `*.map`
  - `.DS_Store`, editor swap files
  - `/tmp/` artifacts that find their way in
  - Docker build cache locations if any are local

- **README.md** at the root:
  - one-paragraph elevator pitch
  - quickstart: `./scripts/build.sh && ./scripts/run_qemu.sh`
  - link to DOCS.md and PLAN.md
  - architecture note (i386 today, x64/ARM coming)
  - license + contributor note

- **LICENSE** — pick one.  MIT or Apache-2.0 are both fine; MIT is
  shorter.  (Decision deferred — flag for the user.)

- **First commit** — clean tree (no build artifacts), descriptive
  message, tag as `v0.0.1` or similar so the next session has an
  anchor.

- **Eventually**: GitHub repo, CI that runs the docker build on every
  push, maybe a smoke-test script that boots qemu and checks for the
  banner string.

**Caveat:** never `git push` without explicit confirmation.  The
`.gitignore`, README, LICENSE can be drafted any time; remote work
waits for the user to say go.

---

## §M1 — kmalloc ✅

Shipped 2026-04-25.  See DOCS.md §4.10.  4 MiB heap at 0xD0000000,
K&R block free-list, alloc/free/reuse self-test passes.

---

## §M2 — Driver registry / module framework ✅

Shipped 2026-04-25.  See DOCS.md §4.0.  Single `MODULE()` macro,
linker-section based registry, `console_sink` interface with
mutually-exclusive `screen` category, four drivers migrated.  Added
`lsmod` and `lsconsole` shell commands.

**Lesson learned:** struct alignment must match iterator stride — used
`aligned(4)` not `aligned(8)` for `struct module_def` (12 bytes on
i386).  Mismatch causes silent unaligned reads → page fault.  Codified
as a comment in `module.h` for future arch ports.

---

## §M3 — Timer (PIT IRQ0) + ms tick ✅

Shipped 2026-04-25.  See DOCS.md §4.X.  PIT@1 kHz, `timer_ticks_ms` /
`timer_msleep`, `uptime` shell command, libgcc linked for 64-bit math.

---

## §M4 — VFS skeleton + ramfs ✅

Shipped 2026-04-26.  See DOCS.md §4.X.  Path resolution, mount
registry, open/read/write/readdir, ramfs as first impl, mounted at /.
Shell commands: ls, cat, mkdir, touch, write.

---

## §M5 — Config store on VFS ✅

Shipped 2026-04-26.  See DOCS.md §4.X.  Defaults + file overlay,
`config_get/set/save/load`, shell commands `config/getconf/setconf/saveconf`.
First consumer: shell prompt comes from `shell.prompt` config key.

---

## §M6 — TSS + ring 3 user-mode ✅

Shipped 2026-04-26.  See DOCS.md §4.X.  GDT extended (user CS/DS +
TSS), TSS with esp0 set, `enter_user_mode_wrap` does iret to ring 3,
int 0x80 with DPL=3 routes through `syscall_dispatch`, SYS_PRINT +
SYS_EXIT working end-to-end via `ringtest` shell command.

**Lesson learned:** NASM macros expand `%1` literally into label
names, so `ISR_NOERR 0x80` produces `isr0x80` not `isr128`.  Use
decimal in the asm source and mention it in the comment so future
ports don't repeat the mistake.

---

## §M7 — Process struct + scheduler ✅

Shipped 2026-04-26.  See DOCS.md §4.X.  Cooperative round-robin in a
circular run-queue, `task_spawn` / `task_yield` / `task_exit` /
`task_list`, asm `context_switch` saves callee-saved regs + ESP.
Demo: `spawn` creates a ticker that prints `[tick N]` in parallel with
the shell, exits cleanly after 6 iterations.

**Open follow-ups (now their own milestones below):**
- Preemptive scheduling → §M13.
- Multi-session shell → §M14.
- Per-task VMM space → folded into §M11/§M19.
- DEAD task stack reclamation — small janitor task; lands with §M13.

---

## §M8 — Driver lifecycle scaffold (`driver_ops`) ✅

Shipped 2026-05-02.  See DOCS.md §4.0.  `DRIVER(name, class, ops,
ctx)` macro + linker section, `probe → init → shutdown` lifecycle,
parallel state tracking, `lsdrv` shell command.  First user:
`kernel/drivers/null/null.c` (placeholder for /dev/null in M9).
Existing MODULE() entries left in place — migration deferred to when
each driver gains a real reason to switch.

---

## §M9 — `devfs` ✅

Shipped 2026-05-02.  See DOCS.md §4.X.  Synthetic files under /dev
(devtmpfs-style, not a separate fs_type).  Built-ins null + zero;
driver-registered com1 + keyboard.  Pre-init queue + flush.  /dev/fb0
deferred until ioctl design lands (M22 territory).

---

## §M10 — `procfs` ✅

Shipped 2026-05-02.  See DOCS.md §4.X.  8 built-in read-only nodes
(version, uptime, meminfo, modules, drivers, console, tasks, config),
lazy content generation, growing-string writer.  Added iterators on
console / task / config so procfs nodes render without poking internals.

**Deferred:** the "shell commands shell out to cat /proc/..." part of
the original definition of done.  The procfs path works alongside the
legacy direct prints; refactoring shell commands to read from /proc
is a follow-up — both backends produce equivalent output today, so
no urgency.

---

## §M11 — Block layer + virtio-blk ✅

Shipped 2026-05-12.  See DOCS.md §4.X.  Abstract `block_device` +
PCI enumeration + legacy virtio-blk driver, exposed as `/dev/vda`.
`blktest` shell command verifies round-trip; disk image persistence
confirmed via `xxd` after QEMU exit.

**Pitfalls codified for future drivers:**
- Legacy virtio QUEUE_SIZE is read-only — the device's qsize wins,
  not ours.
- Descriptor addresses are physical; heap-backed virtual addresses
  must go through `vmm_translate` first.
- Single-page DMA buffers only until per-page descriptor chaining
  lands.

**Test setup workflow (until automated):**
1. Create disk image: `dd if=/dev/zero of=build/test.img bs=1M count=4`
2. Run QEMU manually with `-drive if=virtio,file=build/test.img,format=raw`
   (the existing `scripts/run_qemu.sh` doesn't attach it).

---

## §M12 — exFAT (✅ shipped) + multi-FS plan for FAT32 / FAT / NTFS later

**Shipped 2026-06-27.** See DOCS.md §4.X "Filesystem layer" (VFS
refactor), §4.X "Block cache", §4.X "exFAT".  Round-trip self-test
in `kernel_main` proves create + write + persistence across reboot;
Linux `fsck.exfat -y` reports the image clean.

**Family roadmap (still to do — future milestones, NOT M12).**

| Filesystem | Notes                                              | Order |
|------------|----------------------------------------------------|-------|
| exFAT      | ✅ done                                            | 1     |
| FAT32      | trivially smaller cousin once exFAT works          | 2     |
| FAT16/12   | same fs, different cluster width                   | 3     |
| NTFS       | read-only first; write needs significant work      | 4     |

Adding FAT32 should be a few hundred lines now that the VFS shape is
right: cluster allocator + dir entry parser, no UNICODE complications,
no SetChecksum.  FAT16 / FAT12 are width adjustments to FAT32.

**M12 follow-ups deferred to later milestones (write them up here when
they get scheduled):**
- `mkdir` / `unlink` / `rmdir` on exFAT.
- exFAT filenames >30 chars and non-ASCII (proper UTF-16 + Up-case
  Table).
- ActiveFat / VolumeDirty bit management in the VolumeFlags field.
- Block cache improvements: multi-sector buffers, per-device cache,
  read-ahead, real LRU list instead of O(N) victim scan.

**Lessons learned during bring-up (codified in source comments):**
- SeaBIOS prefers HDD over CD-ROM if both are attached.  An exFAT
  raw image has no MBR, so without `-boot d` the boot stalls before
  the kernel even starts and the serial log is empty — easy to
  mistake for a kernel crash.
- A non-root mountpoint already carries a placeholder inode from
  ramfs's bootstrap; `vfs_mount` now detaches it before handing the
  dentry to the fs (the previous inode leaks for now — fine, ramfs
  bootstrap inodes carry no payload, and `umount` is a future
  milestone).
- bcache buffers must be physically contiguous because the virtio-blk
  DMA path can't gather across page boundaries; using one PMM frame
  per slot guarantees this trivially.

---

## §M13 — Preemptive scheduling — **shipped**

Shipped 2026-06-27.  See DOCS.md §4.X (Tasks + scheduler) and the
2026-06-27 change-log entry for the as-built design.  One-line
summary: PIT IRQ → `schedule_request()` sets a deferred flag → the
IDT's `isr_handler` calls `schedule_check()` after `pic_eoi`, which
context-switches to the next RUNNABLE task from IRQ context.  Lock
primitives (`spinlock_t` UP-stub, `preempt_count`) ship in
`kernel/core/lock.c` ready for §M18 SMP.  Boot self-test proves a
tight-loop hog no longer freezes the kernel thread; `loop` shell
command is the interactive equivalent.

**Lessons learned (read before touching the scheduler):**

- *EOI before reschedule, every time.*  If `pit_irq` context-switches
  before `pic_eoi` runs, the PIC keeps IRQ0 in-service forever and
  timer ticks stop arriving — the system appears alive but never
  preempts again.  Deferred-flag pattern (`need_resched`) is the
  fix and stays in place.
- *Brand-new tasks inherit IF=0.*  A task that reaches the CPU via
  `context_switch`'s `ret` for the first time arrives in the state
  the outgoing path left it: cli'd.  Without an explicit `sti` in
  the trampoline, the new task can never receive an interrupt — so
  it can't be preempted, and (because most blocking primitives need
  IRQs) often can't even cooperatively yield.
- *No runqueue spinlock needed on UP.*  An earlier design used a
  `runqueue_lock` plus a Linux-style "next task unlocks for prev"
  handoff across `context_switch`.  That works, but it's subtle:
  the brand-new task case needs an explicit `unlock` in the
  trampoline that mirrors the schedule() unlock the task would
  have done if it had ever been there.  UP doesn't need any of
  that — cli/sti around runqueue mutation is both correct and
  short.  The `spinlock_t` API still exists (for SMP-ready
  subsystems) but the scheduler doesn't use it.

**Deferred follow-ups (not blockers for M14):**

- `kill <pid>` shell command to reap the `loop` hog without rebooting.
- A janitor task at idle that frees DEAD tasks' `kstack_base`.
- Per-task time accounting (CPU ms used) in `/proc/tasks`.
- `setconf scheduler.quantum_ms` runtime tunable.

---

## §M14 — Multi-session shell with FB pane splitting

**Shipped 2026-06-27.**  See DOCS.md §4.X (Virtual consoles / pane
split) and the 2026-06-27 change-log entry.  One-line summary: a
binary split tree of `vc_node`s partitions the FB into ≤9 panes;
each leaf owns a `struct vc` (rect + cursor + SPSC input ring + bound
shell task).  `pane split horizontal|vertical` mutates the tree in
place under preempt_disable; Alt-N focuses the Nth VC; per-task
`out_console` + console.c hook routes each shell's `kprintf` to its
own pane.  Verified with 3 concurrent shells in QEMU (H-split + V-split
in the bottom).

**Lessons learned (read before touching VC code):**

- *Bind the VC BEFORE the task runs.*  The natural code shape is
  `t = task_spawn(...); task_set_out_console(t, vc);`.  But task_spawn
  inserts into the runqueue immediately, and a PIT preemption can
  schedule the new task between those two lines.  The task's first
  kprintf then runs with `out_console == NULL` and either disappears
  or wanders to the wrong pane.  Fix: bracket the spawn + bind in
  `preempt_disable` / `preempt_enable`.
- *Split mutates the node in place; never swap pointers.*  The leaf
  being split has a parent that references it by pointer.  If we
  alloc'd a new SPLIT node and tried to re-point the parent, that's
  fine — but if the leaf is the ROOT, "the parent" is the global
  `root` pointer.  Simpler: keep the address stable, convert
  kind/contents in place.  Two extra leaf nodes are allocated for the
  children; the original node is repurposed.
- *Per-task routing must coexist with serial.*  Don't replace
  console_putchar's broadcast loop with the per-task hook — keep both.
  Serial sinks must continue to receive everything for the debug log;
  the per-task hook is an ADDITIONAL delivery, not a substitute.
  Once vc_init deactivates the legacy fb sink, the broadcast naturally
  narrows to serial-only on the FB side.

**Deferred follow-ups (not blockers for M15):**

- `pane kill <id>` — terminate the shell task, free vc_node, reflow
  the tree, free freed VC slot for reuse.
- Scrollback buffer per VC so split doesn't lose content.
- Visible focus indicator (colored border, dimmed background on
  unfocused panes, or a status bar).
- Per-VC `shell.prompt` (today the config is global).
- Resize a split point (today every split is 50/50).
- Move focus with keyboard arrows (Alt-↑/↓/←/→ navigates the tree).

---

## §M15 — USB host stack + USB HID keyboard — **shipped**

Shipped 2026-06-27.  See DOCS.md §4.X (USB host stack) and the
2026-06-27 change-log entry.  One-line summary: PCI-discovered xHCI
controller with DCBAA + Command Ring + Event Ring + per-EP Transfer
Rings; root-port enumeration → Enable Slot → Address Device → Get
Descriptors → Set Configuration → Configure Endpoint; single
Interrupt-IN endpoint feeds an HID boot-keyboard class driver whose
8-byte reports get diffed and routed through `vc_kbd_push` like PS/2.
Event Ring drained from PIT IRQ every 10 ms (no MSI/MSI-X yet).
Verified with `-device qemu-xhci -device usb-kbd` in QEMU.

**Lessons learned (read before touching xhci.c):**

- *Producer Cycle State + Link TRB at end of ring.*  The HC and SW
  share the ring by toggling the cycle bit on every wrap.  Forgetting
  to (a) flip your local `cycle` field on wrap and (b) overwrite the
  Link TRB's cycle bit with the OLD value so the HC still processes
  it lands you with a ring the HC silently stops consuming.  Drove
  most of the bring-up debugging time.
- *ERDP write needs the Event Handler Busy bit (1<<3) set on every
  write.*  Spec calls this RW1C-style: writing 1 clears the bit and
  re-arms event delivery; not writing it stalls the next event.
- *Setup TRB packs the 8-byte setup packet as Immediate Data (IDT bit
  set in control).*  The data lives in params, not in a separate DMA
  buffer.  Trying to point at a buffer makes the HC respond with
  Setup TRB Error.
- *Slot Context Root Hub Port Number is 1-based even though PORTSC is
  0-indexed in our register table.*  Off-by-one means Address Device
  succeeds but later transfers stall because the HC routes to a
  non-existent port.
- *We poll the Event Ring from the PIT IRQ.*  That handler currently
  also drives preemption and 1 kHz time-keeping — keep `xhci_poll`
  cheap (it returns instantly when no events are pending) or budget
  for MSI-X delivery before piling on more endpoints.

**Deferred follow-ups (not blockers for M16):**

- Hubs — recursive enumeration, hub class driver, port plug/unplug.
- Multiple simultaneous devices — slot table, per-device state.
- MSI/MSI-X — proper IRQ delivery from xHCI instead of timer polling.
  Required when bulk-mass-storage starts pushing real bandwidth.
- Bulk + isochronous transfers — preconditions for USB MSC, audio,
  cameras.  TRB types are already understood; just need the
  endpoint-management API.
- Full HID report-descriptor parser — needed for non-boot devices
  (mice, gamepads, presenters).  Big chunk of code (HID item parser);
  worth deferring until a real device needs it.
- 64-byte device contexts (CSZ=1) — required by some hardware though
  not qemu-xhci.
- `keyboard.layout`-style config to pick between PS/2 and USB as the
  active input source; today both push to vc_kbd_push so dupes happen
  on platforms where both are present.

---

## §M16 — Keyboard layout abstraction — **shipped**

Shipped 2026-06-27.  See DOCS.md §4.X (Keyboard layouts) and the
2026-06-27 change-log entry.  One-line summary: input drivers emit
(universal keycode = USB HID Usage, modifier-mask = HID layout);
`keymap_translate` resolves it via the active `struct kbd_layout`
which is selected by `keyboard.layout` (config default) or
`setlayout <name>` at runtime.  Layouts: `us` (single source of
truth, replaces old hardcoded tables in ps2/usb drivers) and `hu`
(Magyar QWERTZ).  Verified end-to-end: `setlayout hu` + `echo yz`
→ `zy`.

**Lessons learned:**

- *Pick the universal keycode well.*  Using USB HID Usage IDs as the
  canonical form means the USB driver does no translation at all,
  and the PS/2 driver only carries one small sc1 → HID table.  The
  alternative (a custom KEY_* enum) would have doubled the table
  count for zero benefit.
- *PS/2 modifier tracking needs the 0xE0 state machine.*  RAlt
  (= AltGr) arrives as the 2-byte 0xE0 0x38 sequence.  Without a
  one-shot `e0_pending` flag, the second byte gets misinterpreted as
  a regular scancode (LAlt's 0x38) and the AltGr column never
  activates.
- *LAlt vs RAlt MATTERS.*  LAlt is the policy modifier (intercepted
  for VC pane-switch in the input driver, before keymap_translate).
  RAlt is the layout modifier (feeds the AltGr column).  Both
  drivers must distinguish them.

**Deferred follow-ups (not blockers for M17):**

- Extended font (CP437 magyar / ISO-8859-2 / UTF-8) so the magyar
  accented vowels (á, é, í, ó, ú, ö, ő, ü, ű) can actually render.
  Their slots in `hu_base[]` / `hu_shift[]` are 0 today.
- More layouts: DE, FR, UK, DVORAK.  Once the font's there, these
  are pure data adds.
- Compose / dead-key sequences (`´` + `e` → `é`).  Needs a per-VC
  modal state in the keymap layer.
- Per-VC layout selection — `keyboard.layout` is global today.
- Caps Lock toggle (currently ignored; layouts only honor Shift).

---

## §M17 — Portability cut — extract `hal_api.h` — **shipped (partial)**

Shipped 2026-06-27 as a phased cut.  See DOCS.md §4.X (HAL —
arch-independent interface) and the 2026-06-27 change-log entry.

**What landed:**
- `kernel/includes/hal_api.h` — CPU control (halt/pause/idle),
  interrupt-flag manipulation (enable/disable/save/restore), arch
  bring-up (`hal_arch_early_init`), task stack setup
  (`hal_task_init_stack`), syscall epilogue helper
  (`hal_syscall_exit_to_kernel`).
- x86 implementation: `kernel/hal/x86/hal_arch.c` +
  `kernel/hal/x86/task_arch.c`.
- `kernel/core/task.c`, `lock.c`, `vc.c`, `kernel.c`, `syscall.c`
  migrated — no direct `__asm__`, no `gdt.h`/`idt.h`/`tss.h`
  includes remain.
- `struct task.esp` widened from `uint32_t` to `uintptr_t` so x64
  and aarch64 plug in without source change.
- Legacy PC drivers (`pit`, `ps2`) kept their port I/O (PC-only by
  definition) but switched their `sti; hlt` idle to the atomic
  `hal_cpu_idle`.

**What was deliberately deferred** (= the partial in the table):

- `kernel/mem/vmm.c` still pokes CR0/CR3/CR4/invlpg directly.  The
  clean fix is `hal_map(virt, phys, size, flags)` /
  `hal_unmap(virt, size)` in the HAL — but that abstraction is best
  designed at the same time the x64 (4-level) and aarch64 (granule)
  page tables land.  Bundling it into the x64 port milestone
  (§M20) avoids inventing an API shape blindly.
- `kernel/core/syscall.c` still includes `idt.h` for the x86-
  specific `struct int_frame`.  Splitting the dispatcher into a
  portable arg-marshalling layer plus an arch-specific
  frame-unpack is straightforward but its own follow-up — every
  arch has a different syscall ABI (`int 0x80` vs `syscall`/
  `sysret` vs `svc`), and most of the syscall code IS the
  arch-specific frame work.
- `kernel_main(struct boot_info*)` normalization — today still
  takes `(uint32_t magic, uint32_t info)`.  Naturally cleaned up
  with multiboot2 / EFI handoff in x64 port.

**Lessons learned:**

- *`sti; hlt` is an atomic CPU-guaranteed pair.*  Intel SDM Vol 2:
  `sti` blocks IRQ recognition for exactly one instruction
  boundary, so the immediately-following `hlt` begins before any
  pending IRQ can fire.  That gates the "check ring, then sleep"
  pattern against the IRQ-posted-between-the-two race.  Splitting
  into `hal_intr_enable()` + `hal_cpu_halt()` was the obvious
  first instinct and it would silently break under load — hence
  `hal_cpu_idle()` as its own primitive.
- *Carry HAL types through every layer.*  Widening `task->esp` to
  `uintptr_t` meant the `context_switch` extern declaration needed
  matching, and the cast in `task_spawn` disappeared.  Get the
  type right once and the conversions evaporate.

---

## §M18 — SMP support — **shipped (BSP+APs online, full parallel exec deferred)**

Shipped 2026-06-28.  See DOCS.md §4.X (SMP) and the 2026-06-28
change-log entry.  Single-CPU UP became a multiprocessor: ACPI MADT
parsed, LAPIC + IOAPIC up, 8259 disabled, real cmpxchg-spinlocks,
per-CPU `current` task + percpu table, AP bring-up via INIT+SIPI+SIPI,
all 4 cores online on `-smp 4`.  `lscpu` lists them.

**Lessons learned:**

- *AP trampoline must be flat-binary.*  ELF + `org` doesn't get you
  position-resolved labels at the physical address you copy the
  blob to.  Assemble with `nasm -f bin` and link via
  `objcopy --input-target=binary`.  The symbol names embed the
  input path verbatim (`/` → `_`), so don't `cd` before `objcopy`
  or the extern symbol names on the C side won't match.
- *`percpu_init_bsp` must NOT zero existing slot state.*  task_init
  runs earlier in boot and stamps slot 0's `current` with pid 0;
  blindly memsetting the slot to zero leaves the scheduler with
  prev=NULL.  The system silently never preempts.
- *Lock-handoff for brand-new tasks.*  A scheduler that holds a
  runqueue lock across `context_switch` deadlocks if the new task
  is brand-new (no schedule frame on its stack to release the lock
  on the way out).  Solution: the arch trampoline calls
  `task_finish_first_switch` (which drops the lock) before sti'ing.

**Definition of done (status):**
- ✅ ACPI MADT parsed, `lscpu` lists every present core with online state.
- ✅ All ACPI-listed APs reach online state on `-smp N`.
- ⚠ "Two CPU-bound tasks pinned to different cores actually run in
   parallel" — APs DO boot and idle, but per-CPU scheduler hooks for
   cross-CPU preemption + task pinning are M18.5 work (see follow-ups).

**Deferred follow-ups (M18.5):**

- *Cross-CPU preemption IRQ source.*  APs currently have no
  scheduler IRQ (PIT delivers only to BSP via the IOAPIC).  Two
  paths: (a) LAPIC timer per-CPU (most scalable; one-shot or
  periodic mode), (b) BSP-driven IPI broadcast every quantum.  (a)
  is the long-term answer; (b) ships in one afternoon.
- *Per-CPU runqueue + load balancer.*  Today's global runqueue +
  `task_running_elsewhere` walk is O(ncpus) per pick; per-CPU rqs
  + a periodic balancer eliminate cross-CPU lock pressure.  Land
  with §M19 (slab allocator) since both benefit from the same
  per-CPU machinery.
- *Per-CPU `preempt_count`.*  Today's plain global is wrong on SMP
  the moment more than one CPU wants to ban preemption locally.
- *Task affinity / `taskset`.*  Needed for the canonical "pin two
  CPU-bound tasks, observe parallel speedup" demo.
- *MSI/MSI-X.*  IOAPIC routing is enough for legacy IRQs; modern
  PCIe devices want MSI / MSI-X to deliver directly to a chosen
  CPU's LAPIC vector.  Needed for high-bandwidth I/O.

---

## §M19 — Memory at scale (slab, huge pages, zoned PMM) — ✅ shipped

**Shipped, see DOCS.md §4.8 (buddy PMM) and §4.10 (slab + page_alloc).**

Highlights of what landed:
- **Per-zone binary buddy** in `kernel/mem/pmm.c`.  Free lists
  threaded through the free pages themselves (no external link
  array); single 1-byte side table (`page_state[]`) encodes head-
  of-free-block / allocated / nonexistent.  Public API:
  `page_alloc(order, zone_hint) / page_free(addr, order)`, with
  legacy `pmm_alloc_*` wrapping it.
- **Zones:** `ZONE_DMA` (pfn<4096), `ZONE_NORMAL`, `ZONE_HIGHMEM`
  (slot reserved, not yet populated).  Coalesce refuses to cross
  a zone boundary.
- **Slab allocator** in `kernel/mem/slab.c` with 8 size-class caches
  (16..2048).  Each cache has per-CPU magazines (MAG_CAPACITY = 32,
  MAG_BATCH = 16) — fast path is IRQ-off + push/pop on the local
  array, no spinlock.  Cache identification on free is by slab page
  magic (no per-object header).
- **kmalloc** rewired: ≤ 2048 B → slab, > 2048 B → page_alloc,
  big-alloc order recorded in a side table for kfree dispatch.
  Returns 8-byte aligned for slab objects, 4 KiB aligned for big
  allocations (page_alloc returns frame addresses).
- **Huge pages for kernel direct map**: i386's existing 4 MiB PSE
  identity map (from M5) satisfies the DoD — no VMM change needed.
  Recorded in DOCS so it's not later "missed."
- **New shell commands:** `slabinfo`, `buddyinfo`.  Updated
  `meminfo` shows per-zone PMM summary.
- **Microbench at boot:** 10000 × {alloc(64) + free} round-trips
  in 0–9 ms (varies with SMP overhead under TCG).

**Lessons learned (kept here so the design-time intuition survives):**
1. `big_alloc_order[]` must be explicitly filled with `0xFF` at
   init — 0x00 is a valid order (= one frame), so relying on
   `.bss` zero-fill would misidentify every untouched frame as a
   1-page big allocation on `kfree`.
2. Buddy coalesce must refuse cross-zone merges.  A pfn in DMA
   (< 4096) has a "buddy" address in NORMAL for some orders; if
   you don't check zone membership you can corrupt the lists.
3. Free-list link inside the page only works while every frame is
   inside the kernel's direct map.  When HIGHMEM lands, the
   link-store needs a kmap-style temporary mapping.
4. IRQ-off (not just spinlock) is required for per-CPU magazine
   access — an IRQ handler that allocates would race the
   magazine with itself otherwise.  Per-CPU index is stable
   across the IRQ-off window because migration is gated by IF.

**Definition of done (all met):**
- ✅ `pmm_alloc_frame` returns from a buddy free list, not a
   linear bitmap scan.
- ✅ Kernel direct map uses huge pages (4 MiB PSE on i386).
- ✅ `kmalloc` microbench in place; baseline recorded.
- ✅ DOCS.md §4.8 + §4.10 rewritten.

**Deferred to follow-ups (not blocking M19):**
- HIGHMEM zone population + kmap-style temporary mappings.
- Empty-slab caching (today we release every empty slab back to
  the buddy immediately — fine, but could reduce thrash if a cache
  has bursty traffic).
- ACPI SRAT parsing → per-NUMA-node zones.

---

## §M20 — x64 (long mode) port

**Why now:** the second arch is the test of §M17.  Going x86 → x64
shares enough that we shouldn't rip up the design, but enough must
change (4-level page tables, syscall instruction, REX prefixes, RIP-
relative addressing in ABI) that the HAL boundary really gets
exercised.

**Design.**

- **Boot:** multiboot2 (since multiboot1 doesn't support 64-bit).
  Or eventual UEFI bootloader.
- **Long mode entry:** 32-bit stub sets up CR0.PE, page tables, then
  jumps to 64-bit code.
- **Page tables:** 4-level (PML4 → PDPT → PD → PT).  HAL's
  `hal_map` already takes `uint64_t`.
- **GDT / IDT:** 64-bit gate descriptors.  TSS layout differs.
- **Calling convention:** System V x86_64 ABI (args in registers).
  Asm helpers (context_switch, enter_user_mode_wrap) get a 64-bit
  twin.
- **Syscall mechanism:** `syscall`/`sysret` instructions instead of
  `int 0x80`.  IDT vector 0x80 stays as a fallback.

**Definition of done:**
- `make ARCH=x86_64` builds a separate kernel.bin64.
- Boots in QEMU `qemu-system-x86_64`.
- All shell commands work.
- DOCS.md gains a "supported architectures" section.

---

## §M21 — ARM (aarch64) port

**Why now:** the third arch.  ARM is fundamentally different (no
port I/O, GIC instead of APIC, exception levels instead of rings)
so it's the real torture test of HAL portability.

**Design — outline only; details when we get closer.**

- **Boot:** UEFI on most aarch64 boards, U-Boot on Raspberry Pi.
  Both can hand off to a kernel via a generic protocol.
- **Exception levels:** EL1 = kernel, EL0 = user.  Maps onto our
  ring 0 / ring 3 abstraction without too much pain.
- **MMU:** 4-level page tables with granule (4 KiB) and ASIDs.
- **Interrupt controller:** GICv3 (modern) or GICv2 (older).  IRQ
  install API stays.
- **Timer:** ARM generic timer (system counter + per-CPU timer
  interrupt).  Replaces PIT.
- **No port I/O at all** — every device is MMIO.  Drivers that
  currently use `inb`/`outb` need MMIO equivalents.

**Definition of done:**
- Boots on QEMU `-machine virt -cpu cortex-a72` (or RPi simulation).
- Same shell, same commands.
- DOCS.md gains arch-specific notes.

---

## §M22 — GUI infrastructure (compositor + windows)

**Why now:** beyond text, into a windowed UI.  The framebuffer +
font work from M6 is the seed; this milestone grows it into a real
display server.

**Design — staged subsystems.**

1. **Drawing primitives:** rect, line, blit, alpha-blend.
2. **Surface abstraction:** off-screen framebuffer that can be
   composed onto the main display.
3. **Compositor:** maintains a list of windows, recomposes on
   damage.
4. **Window manager:** position, focus, decorations.
5. **Event system:** keyboard / mouse routed to focused window.
6. **Widget toolkit:** buttons, labels, text input, lists.
7. **Display server protocol** — IPC for client apps to ask the
   compositor to draw, similar to Wayland's design (Linux divergence:
   we'd skip X11's complexity entirely).

**Wayland reuse — investigate, adopt if it doesn't pull focus.**

A custom protocol is straightforward, but Wayland's wire format is
small, well-documented, and has a mature client ecosystem (GTK,
weston-utils, Qt, Firefox).  Spend an explicit sub-phase at the
START of M22 evaluating:

- *Surface of wayland-protocols we'd actually need:* wl_compositor,
  wl_shm, xdg_shell, wl_seat (keyboard + pointer).  That's a small
  subset; we don't need the colour-management / DMA-BUF / explicit
  sync extensions.
- *Implementation cost vs. custom protocol:* libwayland-server is
  ~3 KLOC of C; reimplementing the marshalling is the bulk.  Could
  port libwayland-server itself (it's clean C, no Linux-specific
  syscalls beyond epoll which we can shim) or write a minimal subset
  in-tree.
- *Client testing path:* one upstream `weston-terminal` running
  unchanged against our compositor is a much sharper DOD than any
  in-tree window.

**Decision rule:** if the evaluation shows we can ship a minimal
Wayland-compatible compositor in roughly the same effort as the
custom protocol (estimate ≤ 50% overhead), do Wayland.  If it
balloons, ship a custom protocol that uses the same shapes
(surface + buffer + commit) so a Wayland port stays achievable
later.  In either case, document the call in the M22 change-log.

**Pre-requisites.**

- USB or PS/2 mouse driver.
- Better font rendering — likely a TrueType / FreeType-shaped layer
  for resolution-independent text.
- IPC primitives — shared memory between processes (depends on
  per-task VMM space, §M11/§M19 dependency).

**Definition of done:**
- Two windows on screen, each with its own shell.
- Mouse moves a cursor, clicks focus windows.
- Drag-resize works.
- DOCS.md gains a "GUI" chapter.
- Wayland evaluation outcome recorded.

---

## §M23 — Audio subsystem

**Why now:** after GUI infrastructure (M22), sound is the natural
follow-up for "the OS feels alive."  Decoupled enough from the rest
that it can also land earlier if a driver project pulls it in.

**Design — staged.**

1. **Audio core (`kernel/audio/`)** — `struct audio_dev` registry
   shaped like `block_device`: device name, rate caps, format caps,
   start/stop/write callbacks.  One opaque PCM buffer interface.
2. **First HC driver:** pick by emulator availability:
   - **AC97** (QEMU `-device AC97`) — simplest, well-documented,
     16-bit stereo at 48 kHz.  Recommended first cut.
   - **Intel HDA** (QEMU `-device intel-hda -device hda-output`) —
     modern but heavier (codec discovery, stream descriptors); right
     long-term choice.
   - **Virtio-sound** — pretty, but not on every emulator.
3. **`/dev/dsp`-style char device** for raw PCM writes; mixer
   abstraction (volume per stream) deferred to a follow-up.
4. **WAV player shell command** (`play <path>`) as the smoke test.

**Definition of done:**
- `play /test.wav` produces audible PCM on QEMU's audio backend.
- `lsaudio` lists registered audio devices.
- DOCS.md gains a "Audio" chapter.

**Out of scope:** mixer / multiple streams / resampling, MIDI,
synthesis, surround, ALSA-compat layer.

---

## §M24 — Network stack (NIC → TCP/IP → sockets)

**Why now:** after SMP and (probably) the x64 port, when the kernel
can usefully share state across cores and a real network workload
has the headroom to make sense.

**Design — staged subsystems, each with its own sub-milestone:**

1. **NIC driver** — first cut: virtio-net (QEMU's standard) for the
   same reasons virtio-blk was first for storage: simple ring-based
   interface, well-documented, deterministic.  Eventual second
   driver: Intel e1000 / e1000e for real hardware.
2. **`struct net_device` registry** mirroring `block_device` — name,
   MAC, MTU, send/recv callbacks, statistics.
3. **Link layer:** Ethernet frame parse/build, ARP cache.
4. **Network layer:** IPv4 routing table + ICMP echo; IPv6 deferred
   to a follow-up milestone.
5. **Transport layer:** UDP first (stateless, easy), then TCP
   (sliding window, retransmit, congestion control — the bulk of
   the work).
6. **Socket API** in the kernel: `sys_socket / bind / connect /
   send / recv / close`.  Linux-shaped.
7. **DHCP client + DNS resolver** as user-mode tools once the socket
   API is up.

**Definition of done (staged):**
- §M24.1 — `lsnic` shows the virtio-net device; `ping <host>`
  works from the shell (uses raw socket internally).
- §M24.2 — `nc -u <host> <port>` (UDP) works.
- §M24.3 — `wget http://host/path` returns a response over TCP.

**Out of scope of this milestone (later work):**
- IPv6, multicast, IPsec.
- Bridge / VLAN / bonding device classes.
- Performance: zero-copy RX, GRO/GSO.
- Firewall / netfilter framework.

**Linux divergence:** we won't ship the full `iproute2` toolchain.
Configuration via `setconf net.eth0.ip4 = ...` + `/proc/net/*`-style
diagnostic files is enough; netlink/socket-config protocols are out
of scope.

---

## How to use this document

- **Start of every session:** open `PLAN.md`, find the first non-✅
  milestone, read its design section, get to work.
- **End of every session:** if a milestone advanced, update its section
  with the new state (e.g. "in progress: M4 — file_ops defined,
  ramfs read works, write pending").  If a milestone shipped, condense
  it to a one-line pointer to DOCS.md and bump the next milestone
  status.
- **When the design changes mid-flight:** edit the design section in
  place.  Don't keep stale plans around — the doc is meant to be the
  current truth, not a history.

---

## Change log

- **2026-04-27** — Roadmap expanded.  Added 15 new milestones
  (§G + §M8–§M22) covering driver lifecycle, devfs, procfs, block
  layer, exFAT (+future FAT/NTFS), preemptive scheduling, multi-
  session shell, USB stack, keyboard layouts, portability cut, SMP,
  memory at scale, x64 / ARM ports, GUI.  Added three cross-cutting
  sections: §SMP, §MEM, §DRV.  North-star constraints expanded to 7
  rules (added: SMP-ready, memory-at-scale, Linux-inspired-not-bound).
  No code changed.
- **2026-04-25** — Plan created.  All seven milestones outlined,
  portability and modular-shell constraints captured as cross-cutting
  sections.
