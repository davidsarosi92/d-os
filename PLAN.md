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
| §M20.5 | x64 SMP + APIC + ring-3 (int 0x80) | ~1033 |
| §M18.6 | SMP polish (carry-overs from M18.5) | ~1100 |
| §M19.5 | Memory polish (carry-overs from M19) | ~1150 |
| §M20.6 | x86_64 closure (SYSCALL/SYSRET, USB/blk DMA) | ~1200 |
| §M21 | ARM (aarch64) port | ~1260 |
| §M22 | GUI infrastructure — ✅ shipped (see DOCS §4.13) | ~1405 |
| §M22.2 | GUI modularity — desktop-shell interface, app registry, docs | ~1475 |
| §M22.3 | Desktop polish — task manager, task_kill, minimize, Alt-Tab | ~1545 |
| §M22.4 | Compositor smoothness — cursor race, drag damage, tearing | ~1539 |
| §M22.5 | Desktop apps — editor, BASIC, file manager 2.0, maximize | ~1557 |
| §M22.6 | Tear-free present (page flip) + display scaling | ~1587 |
| §M22.7 | Per-task GUI apps + panel-as-task — ✅ shipped | ~1660 |
| §M23 | Audio subsystem — ✅ stage 1 (i386): AC97 PCM output + tone (DOCS §4.26) | ~1040 |
| §M24 | Network stack (Ethernet → TCP/IP → sockets) — ✅ stages 1–3 (i386): virtio-net + ARP/IPv4/ICMP/UDP/TCP + DNS + ping/wget (DOCS §4.25) | ~1080 |
| §M25 | Userland foundation (Wayland prerequisites) — ✅ stages 1–7 + Tier B tail (concurrent user processes + full-arch libc; DOCS §4.24) | ~1545 |
| §M26 | Wayland server (wire protocol on M22 + M25) | ~1615 |
| §M27 | Process model — init, hierarchy, reaper, kill-tree — ✅ shipped | ~1818 |
| §M28 | System log (klog ring buffer + dmesg) — ✅ shipped | ~1860 |
| Tier A | Blocking primitives — wait-queue + task_wait + blocking IPC — ✅ shipped (DOCS §4.20) | — |
| §M29 | Services / daemons — supervisor + SERVICE() registry + service bus (endpoint/contract/transport) — ✅ shipped (DOCS §4.21) | ~1895 |
| §M30 | Task scheduling — cron service — ✅ shipped (DOCS §4.23) | ~1935 |
| §M31 | Watchdog — heartbeat freeze detection (task / CPU / hw) — ✅ shipped L1+L2 (DOCS §4.22; L3 HW deferred) | ~1960 |
| §M32 | Multi-user — identity, login, file perms, isolation | ~2160 |
| §M33 | Execution domains — where a service runs (kernel / user / isolated); driver placement is the flagship case | ~2265 |
| §M34 | POSIX process & signals — ✅ shipped (i386): fork(COW)/execve/waitpid/pipe/dup2/signals (DOCS §4.27) | — |
| §M35 | Threads & futex — ✅ shipped (i386, UP): clone/futex/thread_create (DOCS §4.28; SMP blocked on per-CPU TSS) | — |
| §M35.5 | Package manager & isolation — content-addressed store (Nix-shaped); gates every port | — |
| §M36 | POSIX syscall breadth + native libc (musl port) | — |
| §M37 | Dynamic linking — ld.so / `.so` / dlopen | — |
| §M38 | C++ runtime + support libs (libc++/unwind, zlib, freetype, ICU, harfbuzz…) | — |
| §M39 | Crypto + entropy + TLS + DNS resolver | — |
| §M40 | Client graphics stack — Wayland client + EGL/GL (Mesa swrast) + Skia | — |
| §M41 | Linux syscall ABI shim — optional binary-compat accelerator | — |
| §M42 | Web browser bring-up — NetSurf → WebKit → Firefox/Chromium (north star) | — |
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
| M18.6 | SMP polish — per-CPU runqueue + load balancer ✅, preempt_count ✅, taskset ✅, cross-CPU IPI ✅, MSI/MSI-X ✅ | Concurrency | §M18.6 |
| M19.5 | Memory polish — HIGHMEM ✅ (x86_64), empty-slab caching ✅, SRAT/NUMA ✅ (parser) | Memory | §M19.5 |
| M20 | x64 (long mode) port (UP)                       | Architecture     | ✅ DOCS §4.X (closed by §M20.5) |
| M20.5 | x64 SMP + APIC + ring-3 (int 0x80) — Phase A/B/C | Architecture | ✅ §M20.5 |
| M20.6 | x86_64 closure — SYSCALL/SYSRET, xHCI + virtio-blk 64-bit DMA | Architecture | §M20.6 |
| M21 | ARM (aarch64 generic / RPi) port                | Architecture     | ✅ Phase A–M — **full x86 parity**: boot + SMP + virtio-blk + exFAT + DTB + framebuffer + EL0 userspace + full shell.c + M22 GUI (virtio-input kbd/mouse, PL031 clock) + **USB (xHCI + HID over PCIe ECAM)** on ARM64 (DOCS §4.17) — §M21 |
| M22 | GUI infrastructure — compositor, windows, mouse, widgets, taskbar, file manager | UX | ✅ DOCS §4.13 |
| M22.2 | GUI modularity — swappable desktop shell + app registry + GUI dev docs | UX | ✅ DOCS §4.14 |
| M22.3 | Desktop polish — task manager, task_kill, term-window close, minimize, Alt-Tab, damage rects | UX | ✅ DOCS §4.13 |
| M22.4 | Compositor smoothness — cursor-damage race, rect-bounded drag, tearing mitigation | UX | ✅ DOCS §4.13 |
| M22.5 | Desktop apps — text editor, BASIC interpreter, file manager 2.0, maximize/restore | UX | ✅ DOCS §4.13 |
| M22.6 | Tear-free present — Bochs-VBE page flip + display-scaling fix | UX | ✅ DOCS §4.13 |
| M22.7 | Per-task GUI apps (each WIN_APP on its own task) + panel-as-task | UX | ✅ DOCS §4.16 |
| M23 | Audio subsystem (AC97 / HDA / I2S)              | Devices          | §M23    |
| M24 | Network stack (NIC → TCP/IP → sockets)          | Networking       | §M24    |
| M25 | Userland foundation — per-process VMM, ELF, fd, unix sockets, mmap | Architecture | ✅ §M25 (stages 1–7) + Tier B (concurrent user processes + full-arch libc, DOCS §4.24) |
| M26 | Wayland server — wire protocol over M22 compositor + M25 substrate | UX | §M26 |
| M27 | Process model — init, parent/child hierarchy, always-on reaper, kill-tree | Concurrency | ✅ DOCS §4.15 |
| M28 | System log — klog ring buffer, severity levels, /proc/kmsg, dmesg | Observability | ✅ DOCS §4.18 |
| Tier A | Blocking primitives — wait-queue (block/wake), task_wait, blocking socket read + poll | Concurrency | ✅ DOCS §4.20 |
| M29 | Services / daemons — SERVICE() registry + supervisor (autostart, restart policy) + service bus (endpoint / contract / transport, location-independent binding) | Architecture | ✅ DOCS §4.21 |
| M30 | Task scheduling — cron service (crontab, timer loop, RTC-driven jobs) | Architecture | ✅ DOCS §4.23 |
| M31 | Watchdog — heartbeat freeze detection (per-task / per-CPU softlockup / hardware) | Reliability | ✅ DOCS §4.22 (L1+L2; L3 HW deferred) |
| M32 | Multi-user — credentials, user DB, login, file ownership/perms, per-user isolation | Security | §M32 |
| M33 | Execution domains — a service's run location (kernel / user / isolated) as a declared capability + config choice; driver placement (fault-tolerant → user-mode isolation) is the flagship case | Reliability | §M33 |

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

## §S — Shell as swappable console provider — **shipped: M14 + §S.1**

The CONSOLE half landed in M14 as `struct vc` (virtual console) —
each shell runs as its own task bound to its own `vc`, `kprintf`
routing via console.c's per-task emit hook.  The PROVIDER half (the
`shell_provider` sketch below) was only finished in **§S.1
(2026-07-04)**, prompted by the M22.2 modularity review: until then
all three spawn sites hard-wired `extern shell_task_entry`.

**§S.1 as built:** `SHELL_PROVIDER(name, entry)` linker-section
registry (shell_provider.h, same pattern as MODULE()/GUI_APP());
the full shell registers as "d-os", `kernel/core/rescue_shell.c`
registers "rescue" (3-command proof-of-swap); boot shell, `pane
split` and GUI terminal windows all resolve through
`shell_provider_active()` = the `shell.provider` config key.
Verified in QEMU: `setconf shell.provider rescue` + `gui` → both
terminal windows run the rescue prompt.  The notes below survive
only as the design-time rationale.

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

## §M18 — SMP support — ✅ shipped

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

## §M18.5 — APs scheduling — ✅ shipped

Shipped 2026-06-28.  See DOCS.md §4.X (SMP) and the M18.5 change-log
entry.  APs went from "online but idle" to "running RUNNABLE tasks
in parallel with BSP."

**What landed:**

- LAPIC timer driver (calibrate / start_periodic / stop) — calibrated
  once on BSP against PIT, count reused on every AP for 100 Hz.
- IDT vector 0x40 for LAPIC timer; 0x41 reserved for cross-CPU
  preempt IPI (placeholder).
- `idt_load()` for per-CPU `lidt` on APs (IDT data shared, IDTR
  per-CPU).
- `task_install_ap_idle()` — each AP joins the runqueue as its own
  idle task in `ap_main`.
- BSP idle task synthesized separately at `task_init` time so
  kernel_main can `task_exit` cleanly after boot.
- Scheduler policy refactor: round-robin among RUNNABLE non-idle
  tasks; idle is a fallback only.
- Boot self-test: two CPU-bound hogs run concurrently; PASS on
  `-smp 2` and `-smp 4`.

**Lessons learned:**

- *IDTR is per-CPU.*  The IDT data structure is shared in memory,
  but each CPU's `lidt` programs its own per-core IDTR register.
  Without per-AP `idt_load`, IRQs land in la-la-land and the AP
  silently triple-faults.
- *BSP MUST have an explicit idle from boot.*  If kernel_main is
  the only thing in the ring and the last non-idle worker on BSP
  dies, BSP halts forever via `task_exit`'s halt loop.  That halts
  PIT IRQ delivery, which freezes `timer_ticks_ms` on every other
  CPU and the whole system deadlocks waiting on the timer.
  Fix: spawn `idle-0` in `task_init` as a separate kernel task
  with `is_idle = 1`, distinct from pid 0.
- *Scheduler must NOT round-robin into idle when a worker is
  RUNNABLE on this CPU.*  Without an explicit "skip idle in normal
  walk + fallback only" policy, the scheduler bounces between a
  hog and idle every quantum, killing throughput.  Fix:
  `pick_next_locked` skips `is_idle` tasks; only the no-work
  fallback path picks idle.

**Still deferred (genuine M19/later work):**

- Per-CPU runqueue + load balancer.  Today's global queue +
  `task_running_elsewhere` walk is O(ncpus) per pick — fine for
  ncpus≤8 but not the long-term shape.
- Per-CPU `preempt_count`.
- Task affinity / `taskset`-style pinning.
- Cross-CPU preempt IPI (vector 0x41 reserved; sender not built).
- MSI/MSI-X (IOAPIC suffices for legacy IRQs; modern PCIe wants
  MSI for direct-to-CPU delivery).

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

## §M20 — x64 (long mode) port — ✅ shipped (UP only)

Shipped, see **DOCS.md §4.X "Supported architectures"** for the
as-built shape — multi-arch build matrix (`make ARCH=i386|x86_64`),
`kernel/hal/x86_64/` tree, multiboot2 + 32→64 long-mode entry,
4-level paging behind a `uintptr_t`-widened `vmm.h`, mb2→mb1 tag
translator that keeps `pmm.c` / `fb_terminal.c` / `mboot_print_*`
unchanged, shell prompt running on `qemu-system-x86_64 -m 256M`.

**SMP, USB, block, ring-3 deferred to §M20.5.**  x86_64 stays on
the 8259 PIC for UP IRQ delivery and uses `int 0x80` (currently
stubbed) for the eventual syscall path.

## §M20.5 — x86_64 SMP + APIC + ring-3 — ✅ shipped (Phase A+B+C)

**Shipped 2026-06-29 in three phases.**  x86_64 reached parity with
i386 for SMP + APIC + ring-3.  See DOCS.md change-log entries
"2026-06-29 — M20.5 Phase A / Phase B / Phase C" for the as-built
details.  Highlights:

- **Phase A:** LAPIC + IOAPIC compile for x86_64 (`kernel/hal/x86/
  lapic.c` + `ioapic.c` shared across archs).  `kernel.c` arch-gates
  removed.  `kprintf` length modifiers (`%l`, `%ll`, `%z`) +
  uintptr_t-wide `%p`.
- **Phase B:** x86_64 SMP AP bring-up.  New `kernel/hal/x86_64/
  ap_trampoline.s` (16→32→64-bit chain with inline trampoline GDT;
  far-rets into the kernel GDT for the final CS reload) + new
  `kernel/hal/x86_64/smp.c`.  `-smp 4` brings up all 4 CPUs.
- **Phase C:** x86_64 ring-3 via `int 0x80`.  New `kernel/hal/x86_64/
  usermode.s` (5-quadword iretq frame + SYS_EXIT teleport) + new
  `kernel/hal/x86_64/syscall.c` (rax/rbx-field dispatcher).  Moved
  the old `kernel/core/syscall.c` to `kernel/hal/x86/syscall.c` —
  this closes one of the M17 deferred items.

`m20_stubs.c` shrank from 9 symbols at M20-ship to just `xhci_poll`.

**SYSCALL/SYSRET instruction path NOT shipped in this milestone.**
The SYSRET selector-arithmetic convention (user CS =
STAR[63:48] + 16, user SS = STAR[63:48] + 8) doesn't fit our
current GDT slot layout (user CS at 0x18, user DS at 0x20 — no
STAR[63:48] satisfies both).  Adding it requires either a GDT
reorg (touching i386 + x86_64 + usermode.s + trampoline) or
duplicate SYSRET-compatible descriptors after the TSS.  Tracked
as a polish item, not blocking — `int 0x80` covers every current
ring-3 need on both archs.

**Lessons learned (filed in source comments and DOCS.md change-log):**
- `lapic.c` / `ioapic.c` are arch-family-shared, not "x86-only" —
  pure MMIO + MSR (`rdmsr`/`wrmsr`/`pause` encode identically in
  32-bit and 64-bit mode).  They keep living under `kernel/hal/x86/`
  for historical reasons but participate in both arch builds.
- A self-contained trampoline GDT is the right shape for an
  x86_64 AP bring-up trampoline: `lgdt` in 16-bit real mode reads
  m16:24, but the long-mode kernel GDT pointer is m16:64.  The
  trampoline carries its own 32+64-bit code/data descriptors,
  transitions to 64-bit, then `lgdt`s the kernel GDT and far-rets
  to reload CS atomically.
- On x86_64 long mode, EVERY level of the 4-level page-table walk
  checks the US bit, not just the leaf PT entry.  boot.s built
  PML4[0] / PDPT[0] / PD[i] with US=0; the first user mapping
  under that subtree #PFs with err=5 (P+U set) because PML4[0]'s
  US=0 is the binding constraint.  Fix: walk_to_pt OR's US into
  existing intermediate entries when the caller's flags request
  it.  Permissions can only widen, never tighten — safe under any
  caller mix.

**Polish carried forward — filed as their own milestones below:**
- §M18.6 — SMP polish (per-CPU runqueue, preempt_count, taskset,
  cross-CPU IPI sender, MSI/MSI-X).
- §M19.5 — Memory polish (HIGHMEM, empty-slab caching, SRAT/NUMA).
- §M20.6 — x86_64 closure (SYSCALL/SYSRET instruction path; xHCI
  + virtio-blk 64-bit DMA audit).

None of these blocks M21+ — they're orthogonal to picking up the
next big milestone.  But they're tracked work, not "someday" items.

---

## §M18.6 — SMP polish (carry-overs from M18.5)

**Status: 5/5 sub-items shipped 2026-06-29..30.**
(.1 per-CPU runqueue + load balancer, .2 per-CPU preempt_count,
.3 taskset, .4 cross-CPU IPI sender, .5 MSI/MSI-X discovery +
vector allocator).  See DOCS.md change-log entries for the two
polish rounds.

**Why now:** The M18.5 ship-now / fix-later list collected real
work that the scheduler will eventually need.  None of it blocks
shell-level functionality (M18.5 already gets RUNNABLE tasks onto
APs and scheduled in parallel), but the current design has
known-quadratic costs and missing capabilities that will hurt
once ncpus grows past ~8 or once real userspace lands.

**Design — outline.**

### §M18.6.1 — Per-CPU runqueue + load balancer

**Status quo:** one global `task_table[]` walked by every CPU on
every schedule-pick.  `task_running_elsewhere` is an O(ncpus) scan
to avoid double-running.  Each pick is therefore O(ntasks + ncpus)
under the global runqueue lock — a lock that every preempt-tick
contends.

**Design:**
- One `struct runqueue { struct spinlock lock; struct task* head; ... }`
  per CPU, embedded in `struct percpu`.
- `task_spawn` enqueues to the current CPU's runqueue (or a
  caller-specified one if affinity is set).
- `schedule()` looks at this_cpu's local rq only; idle fallback
  if it's empty.
- Periodic load-balance pass (every N ticks on tick handler) steals
  tasks from the heaviest queue to the lightest.  Cheap heuristic:
  count of non-idle RUNNABLE entries per rq.

**Files:** `kernel/core/task.c`, `kernel/includes/percpu.h`.

### §M18.6.2 — Per-CPU `preempt_count`

**Status quo:** today we toggle preempt via a global `preempt_off`
flag.  Disabling on one CPU "globally" gates preemption for ALL
CPUs, which is incorrect on SMP.

**Design:**
- `struct percpu` gains `int preempt_count`.
- `preempt_disable()` increments the current CPU's count;
  `preempt_enable()` decrements.  Schedule check fires only when
  count drops to 0.
- IRQ entry increments to gate nested rescheduling; IRQ exit
  decrements + checks for a pending reschedule.

**Files:** `kernel/core/task.c`, `kernel/core/lock.c`, the IRQ
entry stubs.

### §M18.6.3 — Task affinity / `taskset`

**Design:** add `cpuset_t mask` to `struct task`.  scheduler picks
only tasks whose mask includes this_cpu_id.  Shell command
`taskset <pid> <cpuid>` (or hex mask) sets it.

**Files:** `kernel/core/task.c`, `kernel/core/shell.c`.

### §M18.6.4 — Cross-CPU preempt IPI sender

**Status quo:** vector 0x41 is reserved in IDT setup on both
archs but no code ever sends an IPI to it.  Means we can't force
a remote CPU to re-evaluate its runqueue (it'll only do so on the
next local tick — up to 10 ms latency).

**Design:** `smp_send_reschedule(int cpu)` writes LAPIC ICR with
delivery-mode=Fixed, vector=0x41, destination=that CPU's APIC id.
The 0x41 handler (already wired) just calls `schedule_check()`.
Use in: `task_set_runnable` when target task's home CPU isn't
this_cpu.

**Files:** `kernel/hal/x86/lapic.c` (new `lapic_send_ipi` helper),
`kernel/core/task.c`.

### §M18.6.5 — MSI / MSI-X

**Status quo:** IOAPIC routes legacy IRQs (PIT, PS/2, virtio-blk
INTx).  Modern PCIe devices want MSI for direct-to-CPU delivery
without the IOAPIC pin bottleneck.  Required if we ever add a
high-rate device (NIC, NVMe).

**Design:** scan PCI capabilities for MSI (0x05) or MSI-X (0x11)
capability; allocate a free IDT vector; program MSI address (=
LAPIC base | apic_id) + data (= vector) in the device's
capability registers.  Provide `pci_alloc_msi(dev, handler)`.

**Files:** `kernel/hal/x86/pci.c`, `kernel/hal/x86/lapic.c`.

**Definition of done (whole §M18.6):**
- 16-CPU `qemu -smp 16` boots cleanly; load-balance test (many
  CPU-bound tasks) shows roughly equal distribution.
- `taskset 5 0x2` pins task 5 to CPU 1, observable on `procfs`
  `last_cpu` field.
- A virtio-blk driver patch that uses MSI shows IRQ delivery
  going to the configured CPU.

---

## §M19.5 — Memory polish (carry-overs from M19)

**Status: 3/3 sub-items shipped 2026-06-29..30.**
- **.1 HIGHMEM (x86_64 path):** `hal_extend_identity_map` installs 1 GiB
  PDPT pages to cover all RAM up to `BUDDY_MAX_FRAMES` (4 GiB).  PMM
  managed up to 4 GiB on x86_64.  **i386 kmap deferred** — the i386
  HAL impl returns the existing 256 MiB cap as a no-op; an honest
  multi-GiB i386 path needs kmap-style temp mappings (substantial new
  code, not blocking on QEMU).
- **.2 empty-slab caching:** per-cache LIFO of up to 4 empty slabs.
- **.3 SRAT parsing:** per-CPU NUMA-node lookup wired in via percpu;
  PMM still has single zone set, per-node zones deferred to when
  there's a real NUMA test board.

**Why now:** M19 shipped a per-zone buddy + slab + per-CPU
magazines, but three items were filed as "later if needed."  All
become required once we move from "single board, few GiB" to
real hardware.

### §M19.5.1 — HIGHMEM zone population + kmap

**Status quo:** `ZONE_HIGHMEM` is reserved in pmm.c but
unpopulated.  All physical memory above the kernel's identity
map sits unused.  On i386 this caps usable RAM at 1 GiB (size of
identity map); on x86_64 at 1 GiB (boot-time identity map size).

**Design:** populate ZONE_HIGHMEM with pfns above the identity
range.  For free-list link storage (which today lives INSIDE the
free page), allocate a kmap-style temporary mapping per access.
On x86_64 the natural choice is to extend the identity map to
cover all of RAM at boot — long mode has plenty of virtual
address space.  On i386 the kmap dance is unavoidable.

**Files:** `kernel/mem/pmm.c`, `kernel/hal/<arch>/vmm.c`.

### §M19.5.2 — Empty-slab caching

**Status quo:** every slab page becomes empty → returned to the
buddy.  Bursty allocators get hit by the round-trip.

**Design:** each `slab_cache` keeps up to N empty slabs in a per-
cache LIFO; only release excess to the buddy.  N tuned per-cache
(small objects benefit more from caching since their slab is more
likely to refill).

**Files:** `kernel/mem/slab.c`.

### §M19.5.3 — ACPI SRAT parsing → per-NUMA-node zones

**Status quo:** PMM has one set of zones (DMA / NORMAL / HIGHMEM)
shared across the whole system.  On a multi-socket NUMA machine,
this means cross-socket memory traffic.

**Design:** parse SRAT (System Resource Affinity Table) from ACPI;
build `pmm_node[]` with per-node zones.  `pmm_alloc_frame()`
takes a node hint (default: this_cpu's home node).  Slab too,
ideally, but that's a deeper refactor.

**Files:** `kernel/acpi/acpi.c` (SRAT walker), `kernel/mem/pmm.c`
(per-node zones), `kernel/core/percpu.h` (cpu → node map).

**Definition of done (whole §M19.5):**
- `meminfo` shows HIGHMEM with actual frames managed (non-zero).
- `slabinfo` reports cached-empty-slab count per cache.
- On a `-numa node,nodeid=0/1` QEMU launch, `procfs` exposes
  per-node free-frame counts.

---

## §M20.6 — x86_64 closure (SYSCALL/SYSRET, USB+block DMA)

**Why now:** three concrete x86_64-specific limitations carried
over from M20.5.  None blocks shell parity (which §M20.5 already
delivered via `int 0x80` ring-3 and APIC + SMP), but they each
prevent a real-world workload from running on x86_64.

### §M20.6.1 — SYSCALL/SYSRET instruction path

**Status quo:** ring 3 reaches the kernel via `int 0x80` only.
Modern x86_64 userspace prefers the `syscall` instruction (lower
latency, no IDT round-trip).  Linux phased out int 0x80 from
glibc decades ago.

**Design:**
- **GDT reorganization.**  SYSRET (64-bit) demands a specific
  layout: starting from selector S = STAR[63:48], the CPU loads
  user CS = (S + 16) | 3, user SS = (S + 8) | 3.  Our current
  GDT slots are 0 null / 1 kernel-CS64 / 2 kernel-DS / 3
  user-CS64 / 4 user-DS / 5+6 TSS.  No value of S satisfies both
  user CS at slot 3 and user DS at slot 4.  Two options:
  a) Reorder the GDT to Linux's pattern (null / kernel-CS32 unused
     / kernel-CS64 / kernel-DS / user-CS32 unused / user-DS /
     user-CS64 / TSS).  Touches gdt.c, gdt.h, usermode.s,
     ap_trampoline.s, isr_stubs.s.  Most invasive but cleanest.
  b) Duplicate SYSRET-compatible user descriptors AFTER the TSS
     (slots 7+8).  No churn on existing selectors; SYSRET reads
     a parallel set.  Cheaper but uglier (two definitions of
     "user CS").
  Recommend (a) for long-term cleanliness; bundle with M20.6 ship.
- **MSR setup.**  At `hal_arch_early_init` (after gdt_init):
  - IA32_EFER (0xC0000080): set bit 0 (SCE).
  - IA32_STAR (0xC0000081): kernel base in [47:32], user base in [63:48].
  - IA32_LSTAR (0xC0000082): physical address of the syscall entry stub.
  - IA32_FMASK (0xC0000084): RFLAGS bits to clear on entry (typically IF, TF).
  - IA32_KERNEL_GS_BASE (0xC0000102): per-CPU struct pointer (so swapgs gets us to per-CPU data).
- **Entry stub** (new `kernel/hal/x86_64/syscall_entry.s`):
  - swapgs (kernel GS active)
  - save user rsp via gs:offset, load kernel rsp
  - push rcx (= user RIP) + r11 (= user RFLAGS) onto kernel stack
  - construct an int_frame compatible with the int 0x80 path
  - call syscall_dispatch
  - restore
  - swapgs back
  - sysretq
- **Ringtest update:** add a SYSCALL variant alongside the int 0x80
  variant.  Hand-coded bytes: `0F 05` = `syscall`.

**Files:** `kernel/hal/x86_64/gdt.{c,h}`, `kernel/hal/x86_64/syscall_entry.s`
(new), `kernel/hal/x86_64/hal_arch.c` (MSR init), `kernel/core/shell.c`
(ringtest variant), possibly `kernel/hal/x86_64/usermode.s` (selector
constants change if GDT reorg).

### §M20.6.2 — xHCI 64-bit DMA audit

**Status quo:** `kernel/drivers/usb/xhci.c` is compiled out of
the x86_64 build.  The driver was written assuming all DMA
buffers and ring pointers fit in 32 bits.  Once the kernel runs
with >4 GiB RAM and the buddy allocator hands out a frame above
4 GiB, the driver would silently write a truncated address to
xHCI's MMIO registers.

**Design:**
- Audit every `(uint32_t)` cast on a buffer/ring pointer in xhci.c.
- Switch to `uint64_t` where the spec says 64-bit (most xHCI
  registers ARE 64-bit, accessed as two 32-bit halves on i386 but
  natively on x86_64).
- For allocations that MUST be in low memory (no good reason today
  but some xHCI controllers have a 32-bit DMA mask): use a new
  `pmm_alloc_frame_zone(ZONE_DMA)` to force <4 GiB.
- Add a sentinel: if any DMA pointer above the controller's
  declared DMA mask is passed in, panic loudly.

**Files:** `kernel/drivers/usb/xhci.c`, `kernel/mem/pmm.c` (zone-
hint API).

### §M20.6.3 — virtio-blk + exFAT 64-bit DMA audit

**Status quo:** same as xHCI but for `kernel/drivers/block/virtio_blk.c`.
virtio rings and descriptor tables use 64-bit phys addrs in the
spec; the i386 driver squeezes everything into 32-bit.

**Design:** parallel to §M20.6.2.  Audit casts, widen, use
ZONE_DMA hint where needed.  exFAT itself is fs-layer code — it
doesn't see DMA directly — but verify no shortcut casts.

**Files:** `kernel/drivers/block/virtio_blk.c`, `kernel/fs/exfat.c`.

**Definition of done (whole §M20.6):**
- `qemu-system-x86_64 -m 8G` boots; `meminfo` reports the full
  range as managed.
- ringtest's SYSCALL variant prints "hello from ring 3!" and
  returns — same DoD as the int 0x80 variant on i386.
- `qemu-system-x86_64 -drive if=virtio,...` + `qemu-system-x86_64
  -device qemu-xhci ...`: serial log shows the drivers register
  and a smoke test (block dd, USB enumeration) runs.

---

---

## §M21 — ARM (aarch64) port

**Why now:** the third arch.  ARM is fundamentally different (no
port I/O, GIC instead of APIC, exception levels instead of rings)
so it's the real torture test of HAL portability.

**Phased like the x86_64 port was (M20 → M20.6)** — a full boot-to-
shell bring-up of a novel arch is not one landing.  Phase breakdown:

| Phase | Scope | Status |
|-------|-------|--------|
| **A** | Toolchain + build + boot (EL2→EL1) + PL011 UART + exception vectors + MMU identity map | ✅ **shipped** (2026-07-07, DOCS §4.17) |
| **B** | GICv2 distributor/CPU-IF + ARM generic timer (per-CPU tick, replaces PIT) + IRQ install API | ✅ **shipped** (2026-07-07, DOCS §4.17) |
| **C** | Context switch + `hal_task_init_stack` + full HAL (hal_arch.c) + PMM/kmalloc on the aarch64 map + serial console sink + preemptive scheduler | ✅ **shipped** (2026-07-07, DOCS §4.17) |
| **D** | Interactive serial shell (UART RX + REPL, on the scheduler) + VFS + ramfs — an interactive shell with a real filesystem on ARM64 | ✅ **shipped** (2026-07-07, DOCS §4.17) |
| **E** | SMP via PSCI — secondary cores join the stock per-CPU runqueue + load balancer; two tasks run on two cores in parallel | ✅ **shipped** (2026-07-07, DOCS §4.17) |
| **F** | virtio-MMIO block device (modern transport) registered as /dev/vda with the stock block layer; write→read self-test + `blk` shell command | ✅ **shipped** (2026-07-07, DOCS §4.17) |
| **G** | exFAT on /dev/vda (stock block_cache.c + exfat.c) mounted at /mnt — persistent storage; files written from the shell survive a reboot | ✅ **shipped** (2026-07-07, DOCS §4.17) |
| **H** | Device-tree (FDT/DTB) parsing — the kernel discovers RAM size + CPU count from the DTB and sizes the PMM to the actual `-m`, with a fallback | ✅ **shipped** (2026-07-07, DOCS §4.17) |
| **I** | virtio-gpu framebuffer — the SAME portable `fb_terminal.c` renders the boot log + interactive shell graphically (fb_present backend abstraction; x86 DISPI flip hoisted out) | ✅ **shipped** (2026-07-09, DOCS §4.17) |
| **L** | EL0 userspace substrate (M25 prerequisite) — per-process VMM (`vmm.c`) + EL0 entry + SVC syscall (`usermode.S`/`syscall.c`); a user program runs at EL0 and services SYS_PRINT/SYS_EXIT.  Brings ARM to the x86 M6/M20.5 baseline → all 3 arches M25-ready | ✅ **shipped** (2026-07-10, DOCS §4.17) |
| **J / K** | The *same* full `shell.c` on a virtual console + the M22 GUI (compositor + taskbar + PL031 clock + windows), driven by virtio-input keyboard/mouse over the virtio-gpu framebuffer.  Portability shims (`arch_ringtest`, PSCI reboot/shutdown, `pl031_rtc`, `fb_present_flush`, `virtio_input`) + a scheduler idle-loop IRQ-enable fix.  **M22 arch parity.** | ✅ **shipped** (2026-07-10, DOCS §4.17) |
| **M** | USB (M15 arch port) — new PCIe-ECAM layer (`pci.c`: config via MMIO + BAR assignment, no firmware) → the stock xhci.c + usb_hid.c link + run (MMIO, polled from the timer ISR); USB HID keyboard drives the shell.  **Full x86 parity.** | ✅ **shipped** (2026-07-10, DOCS §4.17) |

### Phase A — ✅ shipped (2026-07-07, DOCS §4.17)

Boots on `qemu-system-aarch64 -M virt -cpu cortex-a72 -nographic
-kernel build/aarch64/kernel.bin`.  As-built:
- **Raw-ELF boot, no GRUB / no multiboot** — QEMU's `virt` loader
  reads the PT_LOAD segments and jumps to `_start` (linker-aarch64.ld
  links at 0x40080000, just above the `virt` RAM base 0x40000000).
- **boot.S**: reads `CurrentEL`; if we woke in EL2 (`virtualization=on`)
  it configures `HCR_EL2.RW`, the EL1 timer access (`CNTHCTL_EL2`,
  `CNTVOFF_EL2`), a sane MMU-off `SCTLR_EL1`, and `eret`s down to EL1h;
  then sets SP, zeroes `.bss`, and calls `aarch64_main_entry(dtb)`.
- **PL011 UART** (`uart.c`) at MMIO 0x09000000 — the dependency-free
  early console (ARM analogue of the x86 boot.s inline COM1 print).
- **Exception vectors** (`vectors.S` + `exceptions.c`): the fixed
  16-entry, 2 KiB-aligned EL1 table into `VBAR_EL1`; each slot saves a
  272-byte trapframe and calls a C dispatcher (SYNC/SError → ESR/FAR
  dump + halt; IRQ → weak hook for Phase B).
- **MMU** (`mmu.c`): 4 KiB granule, 39-bit VA, a single level-1 table
  using 1 GiB *block* descriptors — index 0 = Device-nGnRnE (peripheral
  window: UART + GIC), indices 1..3 = Normal WB inner-shareable RAM.
  MAIR/TCR/TTBR0 set, then `SCTLR_EL1.{M,C,I}` flip the MMU + caches on.
- **Build**: separate `Dockerfile.aarch64` (the aarch64 cross toolchain
  `Conflicts:` gcc-multilib, so it can't share the x86 image);
  Makefile `ARCH=aarch64` branch (GNU-as `.S` via the cross gcc, no
  nasm); `scripts/{build,run_qemu}.sh` grew an aarch64 path.

**Verified on serial:** boots at EL1, installs VBAR_EL1, enables the
MMU, and a post-MMU Normal-cached RAM read-back returns the sentinel
(proves the identity map + cache attributes are correct).

**Lesson learned:** `exceptions.c` + `exceptions.S` both compile to
`exceptions.o` under the mirror-path object tree → the second silently
overwrote the first and `vector_table` went undefined at link.  The
assembler half is `vectors.S`.  (No header deps here, but the same
"same basename, different ext" footgun as any C/asm pair.)

### Phase B — ✅ shipped (2026-07-07, DOCS §4.17)

The interrupt controller + periodic tick — the ARM equivalent of the
x86 IOAPIC + PIT/LAPIC-timer.  As-built:
- **GICv2** (`gic.c`): distributor (GICD @0x08000000) + CPU interface
  (GICC @0x08010000).  `gic_init` enables the CPU-IF with an all-pass
  priority mask + the distributor; `gic_enable_irq(intid)` unmasks one
  line (priority + CPU-0 target for SPIs); `gic_register_handler` binds
  a C handler.  The strong `aarch64_irq_dispatch` overrides the Phase-A
  weak stub and runs the ack→dispatch→EOI handshake (GICC_IAR →
  handler → GICC_EOIR).  This is the ARM half of the "IRQ install API".
- **Generic timer** (`timer.c`): the non-secure EL1 physical timer
  (CNTP_*), whose `virt` interrupt is PPI 14 → GIC INTID 30 (EL1 access
  was granted in boot.S via CNTHCTL_EL2).  `timer_init(hz)` arms
  CNTP_TVAL for one interval + enables CNTP_CTL; the ISR re-arms TVAL +
  bumps a monotonic `tick_count` (no auto-reload register on ARM — the
  standard rearm-per-IRQ pattern).  Exposes `timer_ticks()` /
  `timer_ticks_ms()` / `timer_raw_count()` (CNTPCT, the TSC analogue)
  for Phase C's scheduler quantum.
- **IRQ unmask**: `msr daifclr, #2` (the `sti` analogue) — boot.S left
  DAIF fully masked after the EL2→EL1 eret.
- **run_qemu.sh** pins `-M virt,gic-version=2` so the hard-coded GIC
  MMIO layout always matches (newer QEMU may default the board to v3).

**Verified on serial:** GIC init + timer arm, then 1 s / 2 s / 3 s tick
milestones (0x64 / 0xc8 / 0x12c) and a PASS after 300 periodic IRQs —
the full path GIC delivery → EL1 IRQ vector → dispatcher → timer ISR →
EOI, repeatedly, with no fault.

### Phase C — ✅ shipped (2026-07-07, DOCS §4.17)

The kernel's heart on ARM — preemptive multitasking + the memory manager.
Rather than porting the heavily x86-coupled shared `kernel_main`
(multiboot/ACPI/LAPIC/PIT) up front, aarch64 runs its OWN bring-up in
`main_entry.c` and calls the *portable* core subsystems directly.  As-built:
- **Context switch** (`switch.S`): saves/restores the 12 AAPCS64 callee-saved
  regs (x19–x30) across a stack swap; `ret` branches to the restored LR.
  `task_arch.c` synthesises a brand-new task's frame (LR = `task_trampoline`,
  x19 = entry) — the ARM analogue of the x86 ebx/rbx trick.
- **Full HAL** (`hal_arch.c`): `hal_intr_{enable,disable,save,restore}` via
  PSTATE.DAIF (`msr daifset/clr, #2` = cli/sti), `hal_cpu_{halt,pause,idle}`
  (wfi/yield), `hal_arch_early_init` (= exceptions + MMU),
  `hal_extend_identity_map`, and a `hal_syscall_exit_to_kernel` placeholder.
- **Memory** — the stock `pmm.c` + `slab.c` + `kmalloc.c` link and run
  unchanged.  Two enablers: `BUDDY_MAX_FRAMES` bumped to the 4 GiB cap for
  aarch64 (RAM sits at pfn 0x40000, past the old 1 GiB cap), and `stubs.c`
  synthesises a `struct mboot_info` + AVAILABLE mmap entry for the `virt`
  RAM (0x4000_0000, 256 MiB) so the mmap-walking pmm needs no ARM awareness.
- **Serial console sink** (`stubs.c`): registers the PL011 as a `console_sink`
  so `kprintf()` reaches the serial log.
- **Scheduler**: the stock `task.c` + `percpu.c` + `lock.c` link with a
  handful of UP stubs (`lapic_id`→0, `acpi_*`→1-CPU, `smp_*`→no-op; percpu
  stays in not-ready/CPU-0 mode).  The timer ISR calls `schedule_request`;
  the GIC IRQ-exit calls `schedule_check` → `schedule()` → `context_switch`.
- **Freestanding libc** (`lib.c`): `mem{set,cpy,move,cmp}` (gcc emits calls to
  these on ARM) + a `__getauxval` stub (libgcc's LSE-atomics init needs it);
  built with `-mno-outline-atomics` + `-fno-tree-loop-distribute-patterns`.

**Verified on serial:** pmm reports 253 MiB free RAM managed at 0x4000_0000;
kmalloc self-test reuses a freed slot (heap in RAM); pid 0 installed; then two
never-yielding hog tasks BOTH make ~equal progress (hogA≈501M, hogB≈509M) —
proving the timer IRQ preempts and the context switch is correct — PASS, no
fault.

### Phase D — ✅ shipped (2026-07-07, DOCS §4.17)

An interactive shell with a real filesystem on ARM64.  The x86 `shell.c`
reads from a framebuffer-backed VC and its command set is welded to
subsystems still x86-only on ARM (the GUI/VC, ring-3 usermode, vmm.c, the
block/USB drivers) — reaching it verbatim is gated on those ports (Phase E+).
So Phase D brings up a genuine REPL over the UART instead:
- **UART RX** (`uart.c` `uart_early_getchar`): non-blocking PL011 receive; the
  shell polls it and `task_yield()`s while idle (the timer keeps preempting
  underneath, so no CPU is hogged).  (A PL011 RX IRQ would let it block
  instead of poll — deferred; polling is simple and correct.)
- **Serial shell** (`serial_shell.c`): a REPL that runs as an ordinary
  scheduler task and drives the PORTABLE services already up — `help`, `echo`,
  `meminfo`/`free` (PMM stats), `uptime`, `ps` (task_for_each), and the ramfs:
  `ls`, `cat`, `mkdir`, `write`, `rm`, plus `clear`.
- **VFS + ramfs**: the stock `vfs.c` + `ramfs.c` (+ `block.c` for vfs's
  symbol closure, `module.c` for the registry) link unchanged; `vfs_init()` +
  `module_init_all()` register + mount ramfs at `/`.

**Verified on serial (scripted REPL):** `ls /` shows the ramfs skeleton
(mnt/ proc/ tmp/ dev/ etc/); `mkdir /foo` → `write /foo/a.txt hello-from-arm64`
→ `ls /foo` shows `a.txt` → `cat` returns the content → `rm` → `ls` empty
again; `ps` lists the shell (current) + idle + kernel; `meminfo` = 253 MiB
free; no fault.

### Phase E — ✅ shipped (2026-07-07, DOCS §4.17)

SMP on the third arch — the real torture test of the "SMP-ready on UP"
abstraction: the STOCK per-CPU runqueue + load balancer + percpu.c table
(the same core the x86 SMP port drives) now run secondary cores on ARM via a
completely different mechanism.  As-built:
- **PSCI** (`smp.c`): no INIT-SIPI-SIPI / no low-memory trampoline — a
  `PSCI_CPU_ON` HVC (QEMU's emulated PSCI, HVC conduit at EL1) releases each
  secondary vCPU at a physical entry with the MMU off.
- **Secondary trampoline** (`smp_entry.S`): sets an MMU-off SCTLR, derives the
  core index from MPIDR.Aff0, loads its private stack from `ap_sp[]`, and calls
  `smp_secondary_main`.
- **Per-CPU bring-up** (`smp_secondary_main`): turns the MMU on FIRST (so the
  core is cache-coherent with the others before any lock), sets VBAR, brings up
  its GIC CPU interface (`gic_cpu_init` — the banked GICC + PPIs are per-CPU),
  `percpu_init_ap` + `task_install_ap_idle`, then arms its OWN generic timer so
  its tick drives local preemption.
- **Enablers**: `mmu.c` split into build-once + `mmu_enable_this_cpu`; `gic.c`
  split out `gic_cpu_init`; `smp.c` provides the percpu.c topology hooks
  (`lapic_id` = MPIDR.Aff0, linear ACPI topology) so the stock percpu apic_id→
  index map works; cross-CPU kick is a GIC SGI (`smp_send_reschedule`).

**Verified on serial:** `percpu: 2 CPUs known`; `secondary CPU 1 online`;
`SMP — 2 CPU(s) online`; then two never-yielding hog tasks run on **CPU1 and
CPU0** (`parallelism PASS`) — genuine parallel execution across cores driven by
the load balancer.  Configurable via `AARCH64_MAX_CPUS` + `-smp` (shipped at 2).

### Phase F — ✅ shipped (2026-07-07, DOCS §4.17)

The ARM proof of "every device is MMIO": a real disk on `/dev/vda` over the
virtio-MMIO transport.  The existing `virtio_blk.c` speaks virtio over PCI
(port I/O) — meaningless on ARM — so a fresh, self-contained driver:
- **`virtio_mmio_blk.c`**: scans QEMU `virt`'s 32 virtio-MMIO slots
  (0x0a00_0000, stride 0x200) for a block device, runs the modern
  (version-2) init handshake (reset → ACK → DRIVER → feature-OK →
  DRIVER_OK), sets up one split virtqueue (desc/avail/used programmed via
  the Desc/Driver/Device Low/High registers), and does POLLED synchronous
  512-byte sector read/write (3-descriptor requests: header + data +
  status).  Registers with the STOCK block layer (`blk_register`) as
  `/dev/vda`, so nothing else needs to know the transport is MMIO.
- **`main_entry.c`**: a write→read round-trip self-test on a scratch sector.
- **`serial_shell.c`**: a `blk [lba]` command that hexdumps a sector.
- **Run**: `-global virtio-mmio.force-legacy=false` (QEMU `virt` defaults its
  virtio-mmio slots to legacy/version-1; we want modern) + `-drive ...
  -device virtio-blk-device` (wired into run_qemu.sh, disk optional).

**Verified on serial:** `/dev/vda ready (8192 sectors, 4 MiB)`; the
write→read self-test PASSes on sector 100; `blk 0` from the shell hexdumps
the on-disk bytes (`...D-OS-ARM64-DISK-SECTOR0-HELLO`) — real DMA read/write
end-to-end.

### Phase G — ✅ shipped (2026-07-07, DOCS §4.17)

Persistent storage on ARM64: a real exFAT filesystem on the virtio-blk disk.
The stock `block_cache.c` + `exfat.c` are arch-independent (exfat.c even
carries its own `memcpy_`/`memset_`, no RTC/port-I/O), so they link + run
unchanged — the payoff of keeping the fs layer portable.
- **`main_entry.c`**: after the block device is up, `bcache_init()` then
  `vfs_mount("exfat", "/mnt", "vda")`.
- The serial shell's existing `ls`/`cat`/`write`/`rm` (which go through the
  VFS) now operate on real disk under `/mnt`.
- **Test**: an exFAT image is `mkfs.exfat`'d in the x86 build image (which
  carries exfatprogs) and attached as the virtio-blk disk.  (No `-boot d`
  gotcha here — the ARM `-kernel` path is not BIOS-based, so the disk's boot
  signature is irrelevant.)

**Verified on serial:** `exfat: mounted dev=vda clusters=7680 ...`; from the
shell, `write /mnt/hello.txt hi-from-arm-exfat` → `ls /mnt` shows `hello.txt`
→ `cat` returns the content; and — the key proof — on a FRESH boot with the
same disk, `cat /mnt/hello.txt` still returns it: the write persisted to the
exFAT volume across a reboot.  Full chain end-to-end: virtio-MMIO → block
cache → exFAT → VFS → shell.

### Phase H — ✅ shipped (2026-07-07, DOCS §4.17)

The kernel discovers the machine instead of hard-coding it.  On ARM there is
no BIOS/ACPI enumeration — firmware hands over a **device tree** (FDT/DTB).
- **`dtb.c`**: a minimal big-endian FDT parser — walks the structure block for
  the `/memory` node's `reg` (base + size) and counts `/cpus/cpu@*` nodes.
- **Finding the blob**: QEMU's direct-ELF `-kernel` entry passes no x0 pointer
  and places no DTB in RAM, so the run script loads one at a fixed address
  (`-device loader,addr=0x48000000`); `fdt_find` checks x0, then that address,
  then scans low RAM.  Generated per machine config via `-machine dumpdtb`.
- **Payoff**: `aarch64_boot_meminfo_init` (stubs.c) now sizes the PMM map to
  the DTB-discovered RAM window instead of the baked-in 256 MiB, with a clean
  fallback to the default when no DTB is present.

**Verified on serial:** with `-m 512M -smp 4` + the loaded DTB,
`dtb: found @ 0x48000000 — RAM 512 MiB @ 0x40000000, 4 CPU(s)` and the PMM
comes up with **509 MiB free** (vs. the 253 MiB the hard-coded 256 MiB gave);
without a DTB, `dtb: no device tree found (using built-in defaults)` and the
PMM falls back to 253 MiB — the kernel adapts to the actual machine.

### Phase I — ✅ shipped (2026-07-09, DOCS §4.17)

The framebuffer on the third arch, running the *same* portable console x86 uses.
QEMU `virt` has no VGA/Bochs-VBE and no linear-VRAM BAR — the display is a
virtio-gpu device on a virtio-MMIO slot, and it is a COMMAND device (guest RAM
buffer → host resource backing → scanout → per-update transfer+flush), not a
plain framebuffer.  As-built:
- **`fb_present.h` backend cut** — the one x86-only part of `fb_terminal.c` (the
  Bochs-VBE port I/O + the vmm identity map) moved behind a two-call interface:
  `fb_present_map(phys,size)` (x86: 4 MiB PSE map; ARM: no-op, RAM already
  mapped) and `fb_present_flush(x,y,w,h)` (x86: no-op, the linear FB *is* the
  scanout; ARM: virtio-gpu transfer+flush).  The M22.6 double-buffer page flip
  (`fb_flip_init`/`fb_flip_to`) moved verbatim from `fb_terminal.c` to
  `kernel/hal/x86/fb_present.c`; gui.c is unchanged.  `fb_terminal.c` is now
  arch-portable and self-flushes each render primitive's dirty rect.
- **`virtio_gpu.c`** — modern virtio-MMIO handshake (reused from Phase F) +
  control virtqueue; RESOURCE_CREATE_2D → ATTACH_BACKING (a contiguous ~4 MiB
  `pmm_alloc_contiguous` framebuffer) → SET_SCANOUT for a 1280×800 B8G8R8X8
  display; then `fb_term_init_direct()` hands the buffer to the console.
- **`main_entry.c`** brings the GPU up right after kmalloc, so most of the boot
  log renders graphically (and still to the serial log).

**Verified (QEMU screendump, `-device virtio-gpu-device`):** the boot log
renders at 1280×800 (160×100 grid) and `help`/`ls /`/`meminfo` show crisp output
on the framebuffer; the i386 GUI compositor page-flip (now via the moved
`fb_present.c`) is regression-free.  **Lesson learned:** on SMP the serial-shell
banner interleaved character-by-character with pid 0's hand-off line on the
shared console — harmless byte-mixing on serial, but it corrupts the shared
cursor on the framebuffer.  Fixed by printing the hand-off line *before*
spawning the shell (pid 0 then only idles); the general fix — console output
serialization — is deferred to when a second concurrent FB writer actually
needs it (Phase J's VC panes).

### Phase L — ✅ shipped (2026-07-10, DOCS §4.17)

EL0 userspace on the third arch — the prerequisite that makes **M25 startable on
all three architectures**.  x86 has had ring 3 + `int 0x80` since M6/M20.5;
this brings aarch64 to the same baseline.  As-built:
- **`vmm.c`** — per-process TTBR0 address spaces: `aarch64_vmm_create` allocates a
  private L1 table and copies the kernel's low-4-GiB identity blocks into it (so
  the kernel + peripherals stay mapped in every space, as on x86); page-granular
  `aarch64_vmm_map_user` (EL0-accessible, AP=01 + PXN, UXN cleared only for code)
  at VA ≥ 4 GiB; `aarch64_vmm_switch` (load TTBR0 + `tlbi`).
- **`usermode.S` + `syscall.c`** — `aarch64_enter_user` `eret`s to EL0 (SP_EL0 +
  ELR + SPSR); a `svc #0` traps to the EL0 sync vector, `exceptions.c` decodes
  ESR.EC==0x15 and dispatches (x8=number, x0..x5=args; shared `syscall.h`);
  SYS_EXIT teleports back via `aarch64_user_exit`.  No TSS analogue needed — the
  CPU auto-selects SP_EL1 on the EL0→EL1 exception.

**Verified (serial):** `usertest: dropping to EL0 …` → `hello from EL0 (aarch64
userspace)!` (printed by the EL0 program via `svc`) → `…back at EL1 (SYS_EXIT
teleport OK)`.  i386 + x86_64 `ringtest` re-verified identical.  **Lesson
learned:** user VA must clear the kernel's 1 GiB identity blocks — placing user
pages at ≥ 4 GiB (L1 index ≥ 4) keeps `aarch64_vmm_map_user` from ever trying to
split a kernel *block* descriptor into a table.

**Remaining DoD (Phase J / K — NOT M25 prerequisites):** the *same* framebuffer
`shell.c` (VC panes + input routing; today ARM uses `serial_shell.c` over the
UART) and the GUI compositor.  These are ARM ports of M22/M15, independent of the
userland (M25) line, and can follow at any time.

---

## §M22 — GUI infrastructure (compositor + windows) — ✅ shipped

Shipped 2026-07-03.  See **DOCS.md §4.13** for the as-built shape:
gfx primitives + surfaces (`kernel/gui/gfx.c`), compositor + window
manager + terminal windows (`kernel/gui/gui.c`), PS/2 aux mouse
driver (IRQ12), `gui` shell command.  Works on i386 AND x86_64.

**Wayland evaluation outcome (the mandated sub-phase, 2026-07-03):**
libwayland-server / upstream clients hard-depend on a POSIX substrate
d-os lacks (per-process address spaces, fd table, unix sockets with
fd passing, mmap, ELF loader, libc) — far beyond the ≤ 50% overhead
rule.  Per the fallback clause we shipped a custom in-kernel protocol
with Wayland-shaped objects (surface + damage/commit + seat focus
model); the wire protocol is §M26, its prerequisites are §M25.

**DOD status:** two windows each with its own shell ✅ · mouse cursor
+ click-to-focus ✅ · drag-resize ✅ (wireframe + realloc on release) ·
DOCS GUI chapter ✅ · Wayland eval recorded ✅ · **stage 6 widget
toolkit ✅ (M22.1, 2026-07-04)** — label/button/listview/textinput,
IRQ→task event dispatch, first client = the file manager.

**M22.1 follow-up (shipped 2026-07-04, see DOCS §4.13):** Vista-shaped
taskbar (Start menu, per-window buttons, CMOS-RTC clock), APP window
kind with close button, content-preserving resize (terminal char
backing store), file manager (browse/MkDir/Touch/Del/View —
`vfs_unlink` + ramfs unlink landed for Del), 1280×800 FB.

**Lessons learned:**

- *Reuse the VC, not the shell.*  One `emit` hook on `struct vc`
  (`vc_create_offscreen`) let windows host completely stock shell
  tasks — shell.c did not change at all.  The alternative (a parallel
  "window terminal" type) would have duplicated the input-ring +
  binding logic.
- *Hidden panes must be actively muted.*  After the compositor takes
  the screen, background pane shells still print (prompt redraws,
  `loop` tasks).  Without `vc_screen_suppress` they scribble straight
  over the windows — the GUI cannot only *own* the screen, it must
  also *revoke* it.
- *Never kmalloc/kfree in the pointer IRQ.*  Live resize reallocs
  surfaces; doing that in IRQ context races the compositor's blit
  (use-after-free).  Wireframe resize + realloc-on-release on the
  compositor task is both safer and the classic UX.
- *8042 ACK ordering.*  Device ACKs (0xFA) must be eaten synchronously
  before `irq_install(12)` — 0xFA passes the packet sync check
  (bit 3 set) and would shift every packet by one byte.

**Deferred follow-ups:** → collected into §M22.2 (modularity + docs)
and §M22.3 (desktop polish + task manager); USB HID mouse and the
TrueType-ish font layer remain free-floating follow-ups.

---

## §M22.2 — GUI modularity: desktop-shell interface + app registry + docs — ✅ shipped

Shipped 2026-07-04.  See **DOCS.md §4.13 + §4.14** for the as-built
shape: `GUI_APP()` + `DESKTOP_SHELL()` linker-section registries
(gui_app.h / desktop.h), gui.c reduced to compositor + WM core,
`shell_vista.c` (default chrome) + `shell_bare.c` (swap proof, chosen
via `setconf gui.shell bare`), apps under kernel/gui/apps/ (fileman,
about, newshell, hello), `launch [app]` shell command, gui_internal.h
WM services with an explicit IRQ-vs-task calling convention, and the
DOCS §4.14 GUI development guide.

**All DoD items met:** registry-launched fileman (no app symbols in
gui.c) ✅ · bare shell boots via config key ✅ · hello sample appears
in the menu with zero core changes ✅ · docs chapter ✅.

**Lessons learned:**
- *The IRQ/task split must be part of the interface.*  Handing shells
  raw callbacks without the `*_locked` naming convention +
  `gui_queue_*` indirection would invite chrome code to call app
  launches (kmalloc, VFS) from the mouse IRQ.  Encoding the contract
  in gui_internal.h's names makes the wrong thing look wrong.
- *Chrome clicks need "first refusal" routing.*  The shell's click
  callback runs before window hit-testing and consumes chrome
  clicks; menu-open-but-clicked-elsewhere closes the menu and lets
  the click fall through — matching real desktop behaviour.

## §M22.3 — Desktop polish: task manager + window lifecycle — ✅ shipped

Shipped 2026-07-04.  See **DOCS.md §4.13 + change log** for the
as-built shape: task_kill / task_should_stop / task_reap (cooperative
kthread_stop contract), per-task cpu_ms, `kill` + `ps` CPUMS, Task
Manager app (tick-driven refresh, End task), minimize + close on all
windows (terminal close = kill→reap→vc_destroy state machine),
Windows-style taskbar buttons, Alt-Tab (raw-keycode hook), dirty-rect
composition with `gui stats` counters (typing runs ~20:1
partial:full).

**Lessons learned:**
- *No forced kill without user processes.*  Our spinlocks don't
  disable preemption, so a task interrupted at an arbitrary point may
  hold a lock — killing it there deadlocks the compositor.  The
  honest contract is Linux's kthread rule: kill lands at voluntary
  yield points, CPU-bound workers poll task_should_stop().  Forced
  termination becomes possible with ring-3 processes (§M25).
- *Alt-Tab must demote the top VISIBLE window.*  Minimized windows
  park at the top of the z-order; rotating the raw top stalls the
  cycle without changing focus.
- *Partial compose is only cheap if EVERY 1 Hz source is partial.*
  The clock initially requested full frames — one full recompose per
  second dwarfed the typing savings.  Damaging just the chrome strip
  fixed the ratio.
- *Terminal close is a retried state machine, not a blocking wait.*
  The compositor polls kill→DEAD→reap across its loop passes; a
  blocking wait would freeze rendering for a tick (or forever, if
  the shell never yields).

**Deferred:** widget containers (vbox/hbox — the task manager's
manual layout stayed readable without them).

## §M22.4 — Compositor smoothness: cursor race, drag damage, tearing

**Shipped 2026-07-04, see DOCS.md §4.13.**  Cursor-damage race fixed
with compositor-side cursor bookkeeping (compose() unions the
last-drawn + fresh cursor rects itself; IRQ glide = bare need_frame
wake); DRAG_MOVE damages old∪new rect per motion (drag stays
partial-frame dominated, 52:5 in the scripted test); tearing then
noted as std-VGA-inherent — **that call was wrong, see §M22.6**: the
Bochs-VBE DISPI Y_OFFSET pan register IS a present boundary, so a
page flip fixes it without virtio-gpu; program close
propagates to the Task Manager within one frame (task_set_change_hook
→ immediate on_tick) and taskman opportunistically reaps DEAD tasks
not bound to a VC (vc_task_bound).

**Lesson learned:** compose() snapshots the damage rect BEFORE the WM
state — any IRQ-supplied rect describing "where a moving thing was"
can be stale by snapshot time.  Damage for compositor-owned artifacts
(the cursor sprite) must be derived from what the compositor actually
drew last frame, not from what the IRQ saw.

## §M22.5 — Desktop apps: editor, BASIC, file manager 2.0, maximize

**Shipped 2026-07-04, see DOCS.md §4.13.**  All seven stages landed:
nav keys end-to-end (PS/2 E0 cluster → HID usages → widget keycode
events), multiline editor widget (selection + clipboard + viewport),
kernel clipboard, Editor app (open/save/Ctrl+S), Tiny-BASIC
(interpreter core + REPL window via gui_window_create_task + `run`
command), file manager 2.0 (path bar, columns+sorting, Ren/Copy/
recursive-Del, GUI_APP_ASSOC extension associations, vfs_rename/
vfs_copy/vfs_unlink_recursive), maximize/restore.  The definition-of-
done story runs scripted in QEMU: Editor → save to /mnt (exFAT) →
fileman keyboard nav + Enter → BASIC window LOADs + RUNs → maximize/
restore → rename/copy/delete on ramfs.

**Lessons learned:**
- *Growing a linker-section registry struct invalidates every stale
  .o that embeds it.*  gui_app_def gained two fields; without header
  dependencies the old app objects kept the 8-byte layout while new
  ones used 16 — the section walk then miscounted (5 apps instead
  of 7).  Symptom to remember: section size not divisible by the new
  sizeof, mixed entry sizes in `objdump -t`.  `make clean` after any
  shared-struct change (CLAUDE.md pitfall, now with teeth).
- *A formatted disk image carries a boot signature.*  SeaBIOS then
  prefers the disk over the CD and hangs in the empty exFAT boot
  sector — QEMU test invocations with an attached formatted image
  need `-boot d`.
- *Copy-to-self is a truncation footgun.*  vfs_copy opens dst with
  TRUNC; if src == dst that empties the source before the first
  read.  Guard: probe dst without TRUNC and compare inodes.

## §M22.6 — Tear-free presentation (page flip) + display scaling — ✅ shipped

**Shipped 2026-07-04, see DOCS.md §4.13.**  Triggered by the user
report "the picture still wiggles on mouse move, are we synced?".  The
investigation separated two conflated symptoms:

1. **Host-side scaling shimmer (the visible one).**  `run_qemu.sh` ran
   `-display cocoa,zoom-to-fit=on`, which bilinearly rescales the
   1280×800 guest onto a non-integer Retina window.  Every small screen
   update re-presents the whole scaled frame, and the interpolation
   nudges static edges ±1 px → a continuous shimmer that *tracks mouse
   motion* and reads exactly like tearing.  It is NOT the compositor: a
   pointer glide only re-blits the ~14×20 cursor rect; the rest of VRAM
   is byte-identical, so static edges physically cannot move on screen.
   Fix: `zoom-to-fit=off` (crisp 1:1).  Comment in the script points at
   raising the guest resolution if a bigger-yet-crisp window is wanted
   (never re-enable non-integer zoom).

2. **Real compositor tearing.**  The final present was a direct blit
   into the LIVE scanout buffer.  Fixed with a hardware page flip on
   the Bochs-VBE device (QEMU `-vga std`; DISPI ID 0xB0Cx): reserve a
   second VRAM frame (DISPI VIRT_HEIGHT = 2×H, ~4 MiB extra, fits the
   16 MiB default), compose into the hidden buffer, then pan the
   scanout origin (DISPI Y_OFFSET) in one register write.  No vblank
   IRQ is needed — QEMU only ever scans out a fully-composed buffer.
   Buffer age is 2 (ping-pong), so each present copies `dirty_N ∪
   prev_dmg` from `backsurf` (kept a complete correct frame by the
   damage-rect optimisation).  Graceful fallback to the single-buffer
   direct blit on non-Bochs displays (`fb_flip_init` fails).  API:
   `fb_flip_init` / `fb_flip_to` in fb_terminal.c.

**Same-session follow-ups (all shipped, both archs):**

3. **1920×1200 desktop.**  Multiboot header raised to 1920×1200×32
   (both `boot.s`).  Forced two infra fixes: (a) VRAM — the double
   buffer is ~18.4 MiB, over std-VGA's 16 MiB default, so the run
   script uses `-vga none -device VGA,vgamem_mb=32` (`-global
   VGA.vgamem_mb=` is ignored — wrong device name); (b) heap — a
   9.2 MiB full-screen surface exceeded `BUDDY_MAX_ORDER` 10 (4 MiB
   max contiguous), so it went to 12 (16 MiB).  Also `-m 256M`.

4. **Terminal windows auto-close when their hosted task dies.**  A
   shell killed via Task Manager / CLI `kill` / by returning from its
   entry used to leave its dead, un-typeable window on screen.  The
   compositor now flags such a WIN_TERM window for its normal close
   teardown as soon as the hosted task hits TASK_DEAD — reaping it,
   so it also drops off the Task Manager.  Keyed on *actual death*,
   not the kill request, so a task flagged-but-still-running keeps its
   window (the "stop instruction vs stopped" distinction the request
   called out).

**Verified:** i386 + x86_64 both log "gui: page-flip present enabled"
at 1920×1200; 80-event mouse-move stress composed+flipped with no
fault; `kill <windowed-shell-pid>` produced "gui: window '…'
auto-closing (hosted pid N died)" and a clean teardown.

**Lesson learned:** "no vblank" ≠ "no present boundary".  A pan/flip
register (DISPI Y_OFFSET, or a virtio-gpu flush) gives tear-free
presentation even without a scanout-completion interrupt, because the
device reads a whole consistent buffer between your composes.  M22.4's
"not fixable on std-VGA" was too quick — the fix was a $0 register
write, not a new display device.  Second lesson: when a user reports a
visual artifact, rule out the *host* display path (scaling, filtering,
compositor of the emulator's own window) before blaming the guest —
here the dominant symptom was entirely host-side.

## §M22.7 — Per-task GUI apps + panel-as-task

**Why:** the compositor used to run every WIN_APP's callbacks (widget
hit-test, key/mouse handlers, ~1 Hz tick, redraw) on ITS OWN task.  Two
costs: apps were not processes (invisible in the Task Manager, not
independently killable), and a slow/blocking app handler froze the
WHOLE GUI (cursor, other windows — everything).  Making each app its own
task fixes both and is the natural stepping stone to the Wayland client
model (M26): the compositor becomes a pure surface-compositor + input
router, and every UI surface is drawn by its own task.

### Stage A — per-task apps — ✅ shipped (2026-07-05, DOCS §4.16)

Each WIN_APP window is driven by a dedicated **app-host task**
(`app_host_main`).  Mechanism:
- `task_spawn_arg()` (task.c) hands the host its app's open fn via
  `start_arg`; the host runs it (creating the window + widgets ON the
  host task) then services the window(s) it owns.
- Input: the compositor no longer touches widgets — it routes each event
  into the window's per-window queue (`win->aq`); the host does the
  hit-test + widget dispatch + `app_redraw` off the compositor.  The
  compositor still composites `win->surf` under `win->lock` (unchanged).
- `on_tick`/`on_layout` become `tick_pending`/`layout_pending` flags the
  compositor sets and the host acts on.
- Teardown (two-actor dance): on want_close the host runs on_close +
  frees widgets + sets `host_released`; the compositor then disposes the
  window struct and reaps the host (reap_owned, so init keeps off it; a
  no-window singleton host is caught by a sweep).  An externally-killed
  host is detected (host_task DEAD) and the compositor does the cleanup.
- `window_alloc` now claims its slot under `state_lock` (hosts create
  windows concurrently); the `launch` command + taskbar both go through
  `gui_queue_launch` → the compositor spawns the host (never call the
  open fn on the caller's task).

**Verified (i386 + x86_64):** About / Task Manager / File Manager each
come up as `app:<name>` tasks and render (Task Manager list showed 641
white text pixels — the tick ran on the host; File Manager showed its
VFS directory listing); the X button closes a window (host cleanup +
reap, "app window '…' closed" log); no fault.  Apps now appear in the
Task Manager as real tasks.

### Stage B — panel / desktop shell as its own task — ✅ shipped (2026-07-05)

The desktop shell (taskbar/launcher/clock) now runs on its own
**`desktop` task** and renders into a full-screen **`panelsurf`** at
screen coordinates — so shell_vista's draw/click/motion/second_tick
code is *unchanged*, it just runs on the panel task.  The compositor
composites only the OPAQUE parts of panelsurf on top of the windows:
the taskbar strip (always) and the launcher popup rect (while open), so
the rest never occludes.  The shell publishes its popup extent via
`gui_panel_set_popup`; the compositor uses it both to composite the
popup and to route clicks (`in_panel_region`).  Panel-region input goes
to a `pevq` the desktop task drains, running shell->click/motion under
state_lock (their old IRQ-held contract).  The clock's `second_tick`
(RTC I/O) and all chrome redraws happen on the desktop task; the
compositor no longer calls the shell at all.

**Verified (i386 + x86_64):** `gui: desktop shell up on pid 8`; the
taskbar renders (gradient + Start text); the Start button opens the
launcher (popup composited — menu-bg pixels present); clicking a menu
item closes the popup AND launches the app as its own `app:<name>`
host; no fault.  End-state reached: **the compositor is now a pure
surface-compositor + input router; every UI surface (windows, apps,
panel) is drawn by its own task** — the M26 Wayland shape, with the
internal API instead of the wire protocol.

**Lessons learned:**
- *A popup that overlays windows can't live in a bottom-strip surface.*
  The Start menu pops up above the taskbar over the work area, so the
  panel surface is full-screen and the compositor composites explicit
  opaque rects (taskbar + published popup) rather than a fixed strip —
  the rest of the surface is never blitted, so it doesn't occlude.
- *Moving IRQ-contract callbacks to a task means the task must honour
  the contract.*  shell->click/motion assumed the WM lock was held (old
  IRQ path); the desktop task acquires state_lock around them, so the
  shell code didn't change.
- *`bare` shell regressed cosmetically* (bottom_reserve=0 → nothing
  composited from its panel → its hint line is invisible).  Acceptable
  for a rescue shell; a shell with real chrome must reserve a strip.

### Post-ship refinements (2026-07-05)

- **Latency.**  Every task loop `hal_cpu_idle()`d (halt a full tick)
  each pass, so with the extra always-runnable tasks (desktop +
  app-hosts) the compositor's turn came around only every N ticks —
  the lag reported with the menu / Task Manager open.  Fix: halt ONLY
  when idle (`if (need_frame) compose(); else hal_cpu_idle();`), in the
  compositor, desktop, and app-host loops.  Plus vista_motion no longer
  full-recomposes per hover (`gui_panel_dirty` = chrome-only): measured
  2 full frames over 50 menu-hover motions vs one full each before.
  (A proper block/wake primitive would beat polling+hlt entirely — a
  candidate for the M27 scheduler line later.)
- **Parentage.**  App launches moved from the compositor to the desktop
  task, so a launched app is a child of the **desktop/session**, not
  the display server (`ps`: `app:File Manager` under `desktop`).  The
  display server owning the apps was the wrong shape — apps belong to
  the session/launcher (the Wayland/X model).
- **Panel memory.**  `panelsurf` dropped from a full-screen 9.2 MiB
  surface to just the bottom strip (taskbar reserve + `PANEL_POPUP_MAX`)
  — screen-addressed via an offset `px` + clip, so the shell code stays
  screen-coordinate.  ~5 MiB saved.
- **Damage rect LIST (cursor-hitch fix).**  Damage was a single bounding
  box, so a Task Manager refresh in one corner + the cursor in another
  merged into a huge diagonal blit every refresh → the cursor stuttered.
  Now damage is a LIST of ≤16 disjoint rects; `compose()` snapshots the
  WM state once and paints + presents each rect separately, and the page
  flip replays a per-rect `prev_dmg` list.  Measured with TM + cursor:
  ~630 KB/frame vs the old union's ~2.4–5.3 MB — hitch gone.  Verified
  (renders correctly; drag leaves no trail).
- **Precise structural damage + listview-only refresh (the two
  follow-ups).**  A window click used to `gui_damage_all()` (full 9 MB
  frame) for the focus/z change; now `gui_mouse` damages only the two
  affected windows (old focus un-highlights, clicked window raises +
  highlights) — geometry changes (resize apply, maximize) still take the
  full path.  And the Task Manager repaints only its listview rect via
  the new `gui_window_request_redraw_rect` (its CPU-ms column ticks every
  second; the title/buttons/status don't), not the whole window.
  Verified on both archs: raising a covered window renders correctly (its
  focused title now paints over the overlap); the TM list updates with
  the chrome intact; no fault.
- **Session vs detached shells.**  A GUI-launched terminal used to
  orphan to init (the transient launcher app-host created the WIN_TERM
  then exited).  New `task_spawn_under(name, entry, ppid)` parents the
  shell explicitly: **"New Shell"** = SESSION (child of `desktop`, dies
  with a `kill_tree(desktop)`), **"Detached Shell"** = child of init
  (outlives the session; window persists while the compositor runs — the
  nohup/tmux-detach idea in a GUI).  gui.c tracks `desktop_pid`.  The two
  modes map straight onto M27's `task_spawn` vs `task_spawn_detached`
  primitives.  Verified: `ps` shows the session shell under `desktop`,
  the detached one under `init`.
- **GUI session root + clean-desktop start.**  `gui_start` now spawns
  the `desktop` task FIRST and parents the compositor + the (formerly
  auto-started) shells UNDER it via `task_spawn_under`, so the whole GUI
  is one session subtree (`boot-shell → desktop → {compositor,
  windows/apps}`) instead of scattered under the boot shell — a
  `kill_tree(desktop)` closes the session.  And the two starter shells
  are gone: the GUI boots as a bare desktop (wallpaper + taskbar, 0
  windows; focus NULL until one opens) and the user launches terminals
  from Start ("New Shell" / "Detached Shell").  (Still under the boot
  shell: the desktop itself — the boot shell is the launcher.  A
  dedicated login/session-manager root is §M32 territory.)

## §M23 — Audio subsystem — ✅ stage 1 shipped (AC97 PCM output, i386)

**Status (2026-07-11): AC97 PCM output SHIPPED on i386 — see DOCS.md §4.26.**
`audio_dev` registry (block/net-shaped) + an AC97 codec driver (BDL bus-master
DMA, 48 kHz 16-bit stereo out) + a square-wave tone generator.  Shell
`lsaudio`/`beep`/`tone`.  Boot-tested via QEMU's `-audiodev wav` backend: a
440 Hz beep captured as a clean ±8000 square wave (~444 Hz by zero-crossing) —
the tone → audio_dev → AC97 DMA → backend path verified end-to-end.  **Still
open** (design below is the roadmap): a `play <path>` WAV-file player (stage 4
— tone is the smoke test proving the path), `/dev/dsp` (stage 3), mixer /
multi-stream / resampling, PCM input, Intel HDA, IRQ completion, x86_64/aarch64.

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
- ✅ Audible PCM on QEMU's audio backend.  (Shipped as `beep`/`tone` — a
  square wave DMA'd through AC97, captured + verified in the `-audiodev wav`
  output; the `play /test.wav` file player is the remaining stage 4.)
- ✅ `lsaudio` lists registered audio devices.
- ✅ DOCS.md §4.26 "Audio" chapter.

**Out of scope:** mixer / multiple streams / resampling, MIDI,
synthesis, surround, ALSA-compat layer.

---

## §M24 — Network stack (NIC → TCP/IP → sockets) — ✅ stages 1–3 shipped (i386)

**Status (2026-07-11): §M24.1–.3 + stage 6 (sockets) SHIPPED on i386 — see
DOCS.md §4.25.**  virtio-net driver + `net_device` registry + Ethernet/ARP/
IPv4/ICMP/UDP/TCP + a DNS stub resolver, all arch-independent in
`kernel/core/net.c`.  **Plus the BSD socket syscall API to userland** (stage 6):
`FD_NETSOCK` + `struct netsock` back `socket`/`bind`/`connect`/`sendto`/
`recvfrom` (syscalls 22–26) — UDP + a single-connection TCP.  Boot-tested
end-to-end through QEMU SLIRP: `nettest` (kernel: ICMP 3/3, DNS, TCP 200 OK)
*and from ring 3* `dnstest` (UDP-socket DNS → example.com) + `httptest`
(UDP DNS + TCP socket → `HTTP/1.1 200 OK`, 829 B).  Shell: `lsnic`/`ping`/`arp`/
`nslookup`/`wget`/`nettest`/`dnstest`/`httptest`.  RX is polled from the calling
task (no IRQ/lock yet).  **Still open** (design below is the roadmap): a `struct
sockaddr` layer + multiple concurrent TCP conns + TX segmentation; IRQ-driven RX
+ a `netd` task; TCP retransmit/congestion + a server role; DHCP (stage 7);
IPv6; `/proc/net`; x86_64/aarch64 ports.

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
- ✅ §M24.1 — `lsnic` shows the virtio-net device; `ping <host>` works from
  the shell.  (Shipped: ARP + ICMP echo to the SLIRP gateway.)
- ✅ §M24.2 — UDP works.  (Shipped as a DNS resolver + `nslookup` rather than
  `nc -u`: a UDP datagram round-trip to the SLIRP DNS proxy, name resolved.)
- ✅ §M24.3 — `wget http://host/path` returns a response over TCP.  (Shipped:
  handshake + HTTP/1.0 GET + `HTTP/1.1 200 OK` from example.com.)

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

## §M25 — Userland foundation (Wayland prerequisites)

**Why:** the M22 Wayland evaluation (2026-07-03) concluded that the
real cost of Wayland compatibility is not the wire protocol (~3 KLOC
of marshalling) but the missing POSIX substrate underneath it: d-os
today has kernel threads sharing one page directory, a 2-entry
syscall table (SYS_PRINT / SYS_EXIT), and no fd concept at all.
This milestone builds that substrate.  It is worth doing regardless
of Wayland — it is what turns d-os tasks into real user processes.

**Also the unlock for the M29/M33 service model.**  The service bus's
non-local transports (`IPC` / `SharedMemory`, §M29) and the `USER` /
`ISOLATED` execution domains (§M33) are *defined now but reserved* —
they are real only once this milestone's per-process address spaces +
fd passing + shared memory exist.  M25 is therefore the gate that turns
"a service can be configured to run in its own isolated process" from
design into a working config flip.  Design M25's fd/IPC/shm APIs with
that consumer in mind (they are what the bus transports bind to).

**Prerequisites — ✅ ready on all three arches (2026-07-10).**  The
arch substrate M25 builds on is now present and verified uniformly:
each of i386 / x86_64 / aarch64 can enter user mode (ring 3 / EL0),
service a syscall (`int 0x80` / `svc`), and map EL0-accessible user
pages (i386/x86_64 `vmm_map(…, VMM_USER)`; aarch64 `vmm.c`
`aarch64_vmm_map_user` + per-process `aarch64_vmm_create`).  See the
M25-readiness matrix in DOCS.md §4.17.  So stage 1 below can start on
any arch.  (Older deferred items — §M20.6.1 SYSCALL/SYSRET, §M19.5.1
i386 kmap — are optimisations, NOT M25 blockers, and stay deferred.)

**North-star decision — two privilege levels only, forever (2026-07-10).**
d-os uses exactly **ring 0 + ring 3** (EL1 + EL0 on ARM); rings 1 and 2
are deliberately never used.  The reasoning, so it is not re-litigated:
(a) x86 *paging* is binary — the page U/S bit only distinguishes
supervisor (rings 0/1/2) from user (ring 3), so a "ring 1 driver" has
*full kernel memory access* and gains **no memory isolation**, which is
the entire point of userland; (b) x86_64 long mode + `SYSCALL`/`SYSRET`
are built around CPL 0/3 and made rings 1/2 vestigial (the one real user,
32-bit Xen's ring-1 guest kernel, was dropped for exactly this reason);
(c) aarch64 has no rings at all and no intermediate EL for drivers, so a
ring-1/2 design would be non-portable (violates convention #3).  **The
principle:** the security model's axis is *address spaces + capabilities*,
NOT the count of CPU privilege levels — every arch usefully offers two
(kernel/user), and every richer trust tier (isolated drivers, sandboxes)
is built in *software* on top of address spaces, never by consuming more
rings.  So the M33 "intermediate tier" is a ring-3 process with a
restricted capability set (I/O-bitmap port grants, syscall filtering,
IOMMU-bounded DMA) — not a middle ring.  This milestone builds exactly
that 0/3 + per-process-address-space substrate, uniform across arches.

**Design — staged subsystems.**

1. **Per-process address spaces — ✅ shipped (2026-07-10, all 3 arches).**
   A portable `vmm_space` handle (vmm.h): `vmm_space_create/destroy/map/
   unmap/pd_phys/switch` + `vmm_user_base()` + `VMM_EXEC`.  `struct task`
   gained `mm` (NULL = kernel thread, shared kernel table); the scheduler
   calls `vmm_space_switch(next->mm)` before `context_switch`, reloading
   CR3/TTBR0 **only when it changes** (kernel-thread → kernel-thread stays
   free — no TLB flush).  A new space snapshots the kernel's top-level
   table so the kernel stays mapped after a switch, then owns a private
   user region.  Self-test `mmtest` (shell): create a space, map a user
   page carrying a sentinel, switch to it + read it back, then confirm the
   mapping is invisible in the kernel table — **PASS on i386, x86_64,
   aarch64** (`read 0xc0ffee42 → PASS; kernel translate(UVA)=0 → PASS`),
   no boot regression, SMP self-test still green.
   **Lessons:** (a) *x86_64 needs a private PDPT, not just a PML4 copy* —
   the whole kernel lives under PML4[0], so a bare PML4 copy would share
   the user region too; the space gets its own PDPT under PML4[0] (kernel
   PD subtrees shared by pointer, user PDPT[1] private) or isolation
   silently fails (the mmtest `translate=0` check catches it).  (b) *The
   CR3/TTBR0 reload MUST be skipped when unchanged* — doing it every switch
   would TLB-flush on every kernel-thread hop (esp. aarch64's `tlbi
   vmalle1`), a severe perf regression.  (c) *User-VA base is arch-specific*
   (i386/x86_64 = 1 GiB, aarch64 = 4 GiB above its identity map), hence
   `vmm_user_base()`.  (d) Stage-1 limitation: a kernel mapping *added*
   after a space is created won't propagate into it — fine today (all
   kernel high-mappings are boot-time); the fix (shared kernel PT pages /
   generation counter) is deferred.
2. **ELF loader** — load a static ELF from the VFS (ramfs or
   exFAT), map segments into a fresh vmm_space, enter at ring 3.
   - **Stage 2a — ✅ shipped (2026-07-10, all 3 arches): the loader.**
     Portable `kernel/core/elf.c` (`elf_load(space, image, len, &entry)`):
     understands BOTH ELF classes at runtime (ELFCLASS32 for i386,
     ELFCLASS64 for x86_64 / aarch64 — decoded into width-normalised
     header views so one map-loop serves both, no arch #ifdef); for each
     PT_LOAD it allocates frames, copies the file image, zero-fills the
     BSS tail, and maps the pages into the space with the segment's R/W/X
     (via `VMM_EXEC`).  Static executables only — no interp/dynamic/reloc
     (M25 scope).  Self-test `elftest` (shell): synthesise a native-class
     ELF with one PT_LOAD carrying a known payload at `vmm_user_base()`,
     `elf_load` it, switch into the space and confirm the segment bytes +
     entry landed correctly AND the mapping is private to the space —
     **PASS on i386 (ELF32), x86_64 + aarch64 (ELF64)**.
   - **Stage 2b — ✅ shipped (2026-07-10, all 3 arches): run a loaded ELF.**
     Portable `kernel/core/proc.c` `proc_exec_elf(image, len)`: create a
     space, `elf_load` it, map a user stack (1 MiB above the image base),
     bind the space to the calling task (`task->mm = s`, so the scheduler
     maintains CR3/TTBR0 across any preemption), switch to it, and drop to
     ring 3 / EL0 at `e_entry` via the existing `enter_user_mode_wrap` —
     returning on SYS_EXIT, then unbinding + destroying the space.  Two
     arch seams: `enter_user_mode_wrap` (x86 had it; aarch64 got a wrapper
     onto `aarch64_enter_user`) and `arch_user_hello(buf, cap, base)` —
     the per-arch hello payload (x86 the i386 SYS_PRINT/SYS_EXIT encoding
     with an absolute msg ptr; aarch64 the PIC `user_stub`).  Shell
     `userrun` builds a hello ELF and execs it: **i386, x86_64 and aarch64
     all print the greeting from ring 3 / EL0 and return rc=0**, program
     loaded-from-ELF-image, isolated in its own space (vs the older
     `ringtest`, which hand-pokes code into the shared kernel map).
     Excursion model — the hello is short enough that no tick lands
     mid-user; fully independent, long-running, preemptible user processes
     (per-task TSS.esp0, robust IRQ-from-user) come with the scheduling
     work in stage 3+.
3. **Per-process fd table — ✅ shipped (2026-07-10, all 3 arches).**
   `struct task` gained `fds[32]`; syscalls `write/read/open/close/lseek`
   (portable handlers in `kernel/core/usyscall.c`, each arch dispatcher
   just extracts args); fds 0/1/2 are the implicit console.  Shell `fdtest`
   + `userrun` (now `write(1,…)` from ring 3): PASS ×3.
4. **mmap + shared memory — ✅ shipped (2026-07-10, all 3 arches).**
   A generic **`struct ofile`** (fd.h/fd.c: VFS file / shm / socket, refcounted)
   replaced the raw `struct file*` in the fd table; `memfd` shm objects +
   `mmap` (anonymous and shm-backed).  A **`VMM_SHARED`** PTE bit (x86 bit 10
   / aarch64 sw-bit 55) marks BORROWED frames so `vmm_space_destroy` drops
   the mapping without freeing the owner's frames (no double-free).  Shell
   `shmtest` (one memfd mapped at two VAs shares one frame set): PASS ×3.
5. **Unix domain sockets with fd passing — ✅ shipped (2026-07-10, all 3
   arches).**  `kernel/core/usock.c`: connected `socketpair` (each endpoint a
   receive ring + a passed-fd queue) + `send`/`recv` with SCM_RIGHTS — a
   sender queues a fresh `ofile` reference on the peer, the receiver installs
   it as a new fd.  Shell `socktest`: byte stream both ways + a memfd passed
   over the socket → mapped on the far side → the sentinel reads back (one
   shm object reached via a travelled descriptor — the wl_shm handover):
   PASS ×3.
6. **Readiness API — ✅ shipped (2026-07-10, all 3 arches).**  `poll(2)`
   (`struct pollfd` + POLLIN/POLLOUT); non-blocking readiness snapshot
   (socket readable iff buffered bytes, writable iff peer + space; VFS always
   ready).  Shell `polltest` (not-ready → send → ready → drain → not-ready):
   PASS ×3.  True *sleep-until-ready* blocking waits on the concurrent-
   process scheduler (deferred, see note below).
7. **Minimal libc — ✅ shipped i386 (2026-07-10); x86_64/aarch64 port
   pending.**  In-tree `user/` libc (`crt0.s` + `libc.c`: `int 0x80` syscall
   wrappers, string/mem, `malloc` over `mmap`, `printf`/`puts`) + a real
   compiled-C `hello.c`, linked static at 0x40000000 and embedded as a blob
   the kernel loads via `proc_exec_elf`.  Shell `libctest`: the compiled-C
   program runs in ring 3 and prints via `printf`, uses `malloc`+`memcpy`,
   returns rc=0.  The libc C is arch-neutral; the x86_64/aarch64 port needs
   only a per-arch crt0 + user link + blob rule (the command links on all
   arches via weak symbols and reports "not built" where absent).

**Deferred (M25 tail → later):** the *synchronous-excursion* model runs one
user program at a time on the calling task.  Fully independent, long-running,
preemptible, concurrently-scheduled user *processes* — per-task TSS.esp0 /
SP_EL1, a user-task trampoline, SYS_EXIT→task_exit, and blocking syscalls
(read/poll that sleep on a wait queue) — are the remaining substrate, plus
the x86_64/aarch64 libc port.  The APIs above are all in their final shape,
so that work slots in under them without reshaping the ABI.

**Definition of done:**
- A static ELF binary loaded from disk runs in ring 3 in its own
  address space and prints via `write(1, …)`.
- Two user processes: A creates a shared-memory fd, passes it to B
  over a unix socket; B mmaps it and reads what A wrote.
- A poll/epoll-style wait unblocks on socket readability.
- DOCS.md gains a "Userland" chapter.

**Out of scope:** fork/exec fidelity (spawn-style API is fine),
signals, dynamic linking, user-space threads, job control.

---

## §M26 — Wayland server implementation

**Why:** with the M22 compositor speaking a Wayland-shaped internal
object model (surface + buffer + attach/damage/commit + seat) and
the M25 substrate providing unix sockets + fd passing + mmap, wire
compatibility becomes the thin remaining layer — exactly the
sequencing the M22 evaluation recommended.

**Depends on:** §M22 (compositor internals), §M25 (userland
substrate).

**Design — staged.**

1. **Port-vs-reimplement decision** — re-run the libwayland-server
   port assessment against M25's actual API surface (epoll shim,
   socket semantics).  If the port fights our libc, write the
   marshalling in-tree; the wire format is small and stable.
2. **Core globals:** `wl_display`, `wl_registry`, `wl_compositor`,
   `wl_shm`, `wl_seat` (keyboard + pointer), and `xdg_shell`
   (`xdg_wm_base` / `xdg_surface` / `xdg_toplevel`).
3. **Compositor bridge:** `wl_surface` attach/damage/commit maps
   1:1 onto the M22 internal surface API — no compositor rewrite.
4. **Keymap delivery:** `wl_keyboard` sends the keymap as an fd;
   generate an xkb-format blob from our M16 layout tables (or ship
   a fixed per-layout blob first).
5. **Client path:** first an in-tree static test client speaking
   raw wire bytes; then upstream `weston-simple-shm`
   cross-compiled statically as the stretch target.

**Definition of done:**
- An in-tree Wayland client connects over a unix socket, creates a
  wl_shm buffer, and its window appears composited on screen.
- Keyboard + pointer input reaches the focused client via wl_seat.
- Stretch: unmodified `weston-simple-shm` (static build) runs.
- DOCS.md "GUI" chapter gains a Wayland-protocol section.

**Out of scope:** DMA-BUF, explicit sync, colour management,
subsurfaces beyond the minimum xdg_shell needs, XWayland.

---

## §M27 — Process model: init, hierarchy, reaper, kill-tree — ✅ shipped

**Shipped 2026-07-04, see DOCS.md §4.15.**  `struct task` gained
`ppid` + `exit_code` + `reap_owned`; parent = the spawner (pid 0 very
early).  An always-on **init task** (the first thing kernel_main
spawns) is the *universal reaper*: it sweeps DEAD tasks that are not
`reap_owned` at ~100 Hz (the compositor idle pattern), closing the old
"exited task leaks as DEAD unless the Task Manager is open" gap.
`task_kill_tree()` cooperatively kills a pid + all descendants
(fixpoint subtree collection under master_lock, flagged after
release); the GUI window close uses it so a shell window takes anything
it spawned down with it.  On reap a task's surviving children
re-parent to init (never a dangling ppid).  Visibility: `ps` and
`/proc/tasks` grew a PPID column; the Task Manager renders a real
process **tree** (children indented under parents).  pid 0 (the boot
"swapper") and init are guarded against reaping.

**Verified (i386 + x86_64):** init reaps a leaked boot self-test task;
`ps` shows correct ppid; a `spawn`ed child under a GUI shell is taken
down + re-parented + reaped when its window is closed
("auto-closing … pid N", "init: reaped 'ticker' (…ppid→init…)"); no
fault; pid 0 survives.

**Lessons learned:**
- *The reaper eagerly ate pid 0.*  kernel_main task_exit()s after boot,
  so pid 0 goes DEAD — and a universal reaper will happily reap it
  (memory-safe: its stack is the un-owned boot stack).  Alarming in
  the log and against the "swapper is permanent" convention, so pid 0
  (and init itself) are explicitly skipped.
- *Reap ownership must be explicit, not GUI-coupled.*  Core task.c
  must not call into the GUI to ask "is this task window-bound?"  A
  plain `reap_owned` flag on the task decouples it: the GUI sets it on
  its window shells and keeps reaping them; init skips them.  This
  replaced the taskman's old `vc_task_bound()` reap gate.
- *Death goes down, notification goes up.*  kill-tree propagates
  termination to descendants; a child dying does NOT kill its parent —
  the parent is meant to be NOTIFIED and apply policy.  That upward
  half (wait/supervision + freeze watchdog) is deferred to §M29 and a
  new §M31; see those.

---

## §M28 — System log (klog ring buffer + dmesg) — ✅ shipped

**Shipped 2026-07-10, see DOCS.md §4.18.**  `kernel/core/klog.c` — a
static 512-record ring (usable from the first boot kprintf, no heap):
monotonic seq + boot-relative ms timestamp + printk severity
(EMERG…DEBUG) + source tag + message.  `printf.c`'s `emit()` tees every
byte into `klog_feed_char`, which line-assembles and commits on `\n`, so
all existing `kprintf` output is captured automatically (INFO/"kernel")
with zero call-site churn; `klog(level, tag, fmt, …)` is the structured
entry point (formats through the shared `kvprintf`, so it still hits the
console).  Read paths: `dmesg [-l <level>]` (severity-filtered, rendered
`[  sec.mmm] LEVEL tag: msg`, via `console_*` so it doesn't re-log
itself) + a `/proc/kmsg` procfs node.  Verified on i386 + x86_64.

**Lesson learned — the `va_list` array-type trap.**  Factoring a
`kvprintf(fmt, va_list)` core out of `kprintf` corrupted *all* formatted
output on x86_64 while i386 was fine.  On the x86_64 SysV ABI `va_list` is
an *array* type, so a `va_list` **parameter** decays to a pointer and
`&ap` becomes a pointer-to-pointer — the wrong type for the
`va_list*`-taking `fetch_*` helpers; i386's scalar `va_list` hid it.  Fix:
`va_copy` into a genuine local array and format off that.  Rule: a helper
that forwards a `va_list` by pointer must own a real `va_list` local (via
`va_copy`), never `&`-a-`va_list`-parameter.

**Deferred follow-ups (out of scope, as planned):** CMOS-RTC absolute
wall-clock stamping (v1 is monotonic-since-boot), persistence to
`/var/log/messages` on exFAT, journald-style binary records, log
rotation, remote syslog, rate-limiting.

---

## §M29 — Services / daemons: supervisor + SERVICE() registry + service bus

> ✅ **SHIPPED (2026-07-10) — see DOCS.md §4.21.**  Supervisor (SERVICE()
> registry + `task_wait`-driven restart with crash-loop backoff + config gate
> + `service` command + `/proc/services`) AND service bus (endpoint /
> contract@version / transport, strict binding + opt-in `BUS_ADAPTER` gated by
> `bus.allow-adaptation` + `/proc/bus`) both landed on i386 / x86_64 / aarch64,
> exactly as designed below.  `bustest` + the heartbeat/crasher/Greeter
> demonstrators verify it.  The non-local (IPC/SharedMemory) transports remain
> reserved for M25 as planned.  Design retained below for rationale.

**Two halves.**  (a) A **supervisor** (systemd-lite / SMF-lite) — the
lifecycle answer to child death (stages 1–4 below).  (b) A **service
bus** — the *discovery + binding* answer to "how do services find and
call each other" (endpoint / contract / transport, the subsection after
the supervisor).  Both follow the established registry pattern
(`DRIVER()`, `GUI_APP()`, `SHELL_PROVIDER()`), so this is idiomatic d-os,
not a new subsystem style.  The bus is what turns the supervisor from a
"process babysitter" into a **service broker**: it doesn't just keep a
service alive, it publishes the service's endpoint and wires callers to
it over the right transport — which is what makes §M33's execution
domains (where a service runs) a pure config decision instead of a code
decision.

**Why now:** gives the OS a "systemd-lite / SMF-lite" — long-lived
supervised workloads with a real lifecycle.

**This is the "upward" answer to child death.**  M27 propagates
termination *downward* (kill-tree) but deliberately does NOT let a
child's death kill its parent.  The established convention for "what
happens up the tree" is a **supervisor** (Erlang/OTP supervision trees,
systemd, runit, s6, daemontools): the parent is *notified* of a child's
exit and applies a *restart policy* — it does not simply die too.  M29
is exactly that supervisor.  The clean primitive it wants from M27 is a
`task_wait(pid, &code)` (M27 shipped the pieces — exit_code + the change
hook — but a blocking wait was left for here, since the supervisor is
its first real user).

**Depends on:** §M27 (parentage, exit codes, kill-tree, detached spawn
for the supervised children; the supervisor uses task_spawn_detached so
services aren't tied to whoever ran `service start`), §M28 (log there).

### Supervisor — the lifecycle half

**Design — staged.**

1. **`SERVICE()` linker section** — `struct service { name, entry,
   autostart, restart }` where `restart ∈ {no, on-failure, always}`.
   Same self-registration story as drivers; no `kernel_main` edits.
2. **Supervisor task** — could be init itself or a child of it.  At
   boot it starts every `autostart` service, records the child pid,
   and on the `task_set_change_hook` notices a service task went DEAD
   and restarts it per policy (with a simple backoff so a crash-loop
   does not spin a core).
3. **Control surface** — `service list|start|stop|restart|status
   <name>`; `/proc/services` (name, state, pid, restarts); enable /
   disable via `/etc/d-os.conf` keys (or an `/etc/services.d`).
4. **First services** — trivial demonstrators (a heartbeat logger, a
   procfs stats sampler) proving autostart + restart-on-crash; cron
   (M30) becomes the first *real* service.

### Service bus — the discovery + binding half

**Why:** today a caller reaches a subsystem by *hard-linking* to its
symbols (call `net_send()` directly).  That couples the caller to *this*
implementation, in *this* address space.  The bus replaces the hard link
with a **named, versioned, transport-abstracted binding** — the QNX
resource-manager / Fuchsia FIDL / Android-Binder shape, sized down for a
teaching OS.  The three concepts (deliberately mirroring how `hal_api.h`
already versions an interface):

- **Endpoint** — a name in a flat namespace (`net.default`, `net.eth0`,
  `block.vda`).  *Discovery*: a caller resolves an endpoint to a
  binding; it never names an implementation or an address space.  This
  is the same idea as the `shell.provider` / `gui.shell` config keys,
  generalised into a runtime registry.
- **Contract** — a *versioned interface* identified by `(name,
  version)`, e.g. `NetworkDevice v1`.  Concretely a versioned
  struct-of-function-pointers (exactly `hal_api.h`'s shape).  **No IDL /
  codegen** — hand-written C interface structs stay readable and are the
  right altitude for this OS.
- **Transport** — *how* a binding is invoked.  `LocalCall` (direct
  function call, same address space — the only real one until §M25);
  `SharedMemory` and `IPC` are defined now but reserved (they need
  §M25's per-process spaces + fd passing).

**Binding resolution.**  A caller asks the broker for
`(endpoint, contract@version)`; the broker finds the provider, checks
the transport is valid for the provider's **execution domain** (§M33 —
a KERNEL-domain provider can serve `LocalCall`; a USER-domain provider
needs `IPC`/`SharedMemory`), and returns a handle the caller invokes
transport-agnostically.  This is where location-independence *comes
from*: the caller's code is identical whether the service is in-kernel
or in a ring-3 process — only the transport differs, and the broker
picks it.

**Contract-versioning policy — decided (2026-07-10).**  Strict and
adapter-shim are *not* competing options; they live in different layers,
so we take both:

- The broker is **always strict on the wire**: it binds only an *exact*
  `contract@version` match.  Deterministic, debug-friendly, no silent
  adaptation.
- **Compatibility is an opt-in mechanism, not a policy branch:** an
  `ADAPTER(from = NetworkDevice v1, to = NetworkDevice v2)` registry
  entry (same self-registration story as everything else).  When a
  strict bind for `v1` misses but a `v2` provider exists, the broker —
  *iff* the `allow-adaptation` config bit is set — inserts the registered
  shim, which synthesises a `v1` endpoint over the `v2` provider.
- **"Backward-compatible" is then just a special case:** a provider that
  implements several versions registers as its *own* multi-version
  adapter.  No separate policy code path.
- **Boilerplate only where it's real:** a shim is written only for the
  version pair that actually needs bridging — not paid by every service.
  The config knob is a single `allow-adaptation` bit (off = pure strict,
  deterministic; on = registered bridges live), *not* a global
  Strict-vs-Backward toggle.

**Marshalling discipline — the crux, enforce it from day one.**
`LocalCall` passes a pointer and calls a function; `IPC`/`SharedMemory`
must *serialise* arguments.  A contract that is designed for pointer
passing **cannot** later move to a non-local transport without breaking.
So — per convention #5 — contracts are designed **as if marshalled even
while only `LocalCall` exists**: arguments are handles + copied/shared
buffers, never freely-shared raw kernel pointers.  Get this wrong and a
`v1` contract is stuck in-kernel forever; get it right and moving a
service to a USER domain (§M33) is a config flip, not a rewrite.

**Design — staged.**

5. **Registry + resolver (LocalCall only).**  `SERVICE()` grows
   `endpoint` + `contract` (name+version); a `bus_bind(endpoint,
   contract)` resolver returns a `LocalCall` handle; `/proc/bus` lists
   endpoints (name, contract\@ver, domain, transport, provider pid).
   Arch-independent, buildable *now* — the immediate win is services
   finding each other by endpoint instead of hard-linking.
6. **Contract discipline + adapters.**  Define the first real contracts
   (`NetworkDevice`, `BlockDevice`) marshalling-shaped; add the
   `ADAPTER()` section + `allow-adaptation` resolution path.
7. **Non-local transports (design now, land with §M25).**  The `IPC` /
   `SharedMemory` transport backends — reserved interface today, real
   once §M25 ships unix sockets + fd passing + shared memory.  This is
   the same waist §M33's driver-runtime API needs; build it once, share
   it.

**Definition of done:**
- *Supervisor:* an `always`-restart service killed by hand comes back on
  its own, visible in `dmesg` + `/proc/services`; `service list`
  reflects live state; disabling in config keeps it down across boots.
- *Bus:* a caller binds `net.default` / `NetworkDevice v1` and calls it
  with no compile-time link to the provider; `/proc/bus` shows the
  endpoint, its contract version, domain, and transport.
- *Versioning:* with only a `v2` provider registered, a strict `v1` bind
  fails cleanly with `allow-adaptation` off, and succeeds via the
  registered `v1→v2` shim with it on.
- DOCS.md gains a "Services & the service bus" chapter.

**Out of scope:** dependency ordering / socket activation, resource
limits (cgroup-style), user/permission separation (no userland yet); the
non-local (`IPC`/`SharedMemory`) transports are *designed* here but only
land with §M25; remote/networked endpoints (a service on another host
over §M24) are the logical extreme of location-independence but stay an
explicit non-goal — do not let them pull the transport abstraction into
premature generality.

---

## §M30 — Task scheduling: cron service

> ✅ **SHIPPED (2026-07-10) — see DOCS.md §4.23.**  cron is itself an M29
> service (autostart, restart=always); a `CRON_JOB()` registry + interval
> schedules (registered default / `/etc/crontab` / config) + run-once-no-
> backfill; `crontab -l` / `cron reload` + `/proc/cron`.  A `tick-log` demo job
> (every 5s) fires + logs on i386 / x86_64 / aarch64.  Design retained below.

**Why now:** the natural capstone of the cluster — a scheduler for
*work over time*, and the first genuinely useful service.  Small once
M27–M29 exist, because cron is literally a service that spawns
(and owns → reaps) child tasks on a schedule.

**Depends on:** §M29 (cron is a service), §M27 (it parents/reaps its
jobs), §M28 (it logs runs).  Time source already exists (CMOS RTC +
`timer_ticks_ms`).

**Design — staged.**

1. **Crontab** — `/etc/crontab` parsed into a table of
   `{schedule, command}`.  Start with interval schedules
   (`every N s/min`) plus a minimal cron-field form; wall-clock
   alignment via the RTC.
2. **cron service** — a timer loop that, each tick, spawns the due
   jobs as its own children and logs start/exit (+ exit code) to
   klog.  Missed-tick policy: run-once-on-catch-up, not backfill.
3. **Job as task** — a job is a kernel task (or, post-M25, a spawned
   program); cron reaps it and records the result.
4. **Control** — `crontab -l` to list, `cron status` for the next
   due times; reload on `/etc/crontab` change.

**Definition of done:**
- A `every 5s` job fires on schedule and its runs appear in `dmesg`.
- cron shows up under `service list` and survives a restart.
- DOCS.md "Services" chapter gains a cron section.

**Out of scope:** at/batch one-shots (could be a thin follow-up),
per-user crontabs, timezone handling beyond the RTC's wall clock,
persistence of last-run state across reboot.

---

## §M31 — Watchdog: heartbeat-based freeze detection

> ✅ **SHIPPED L1+L2 (2026-07-10) — see DOCS.md §4.22.**  Per-task heartbeat
> (`watchdog_register`/`watchdog_kick` → missed-deadline detect + kill-tree +
> M29-supervisor restart) and per-CPU softlockup (a `percpu.ticks` counter the
> timer bumps; a stalled CPU is warned) both landed on i386 / x86_64 / aarch64
> with `/proc/watchdog` + a `wdtest` self-test.  Layer 3 (hardware watchdog
> timer — i6300esb / SP805) is deferred: it needs a per-platform device driver.
> Design retained below.

**Why:** M27 handles a task that *dies*; this handles a task that is
alive but *wedged* (an infinite loop that never yields, a deadlock, a
livelock).  Death and freeze are different failure modes and need
different machinery — you cannot reap what has not exited.  "Is a
program frozen?" is genuinely a *global* problem with three layers.

**Design — three layers, small.**

1. **Per-task heartbeat (the systemd `WatchdogSec=` model).**  A
   supervised task (M29 service, or any opt-in worker) periodically
   "pets" its watchdog — `watchdog_kick()` — which stamps a last-seen
   time.  A watchdog sweep (init, or a dedicated task) flags any task
   that missed its deadline as hung and applies policy: log, then
   kill_tree + restart (M29).  Opt-in: a task that never registers is
   never watched (a legitimately long compute is not a freeze).
2. **Per-CPU softlockup detector.**  A low-frequency check that each
   CPU is still taking timer ticks / making scheduler progress (a
   per-CPU "still alive" counter the tick bumps; a peer notices if it
   stops).  Catches a core wedged in an IRQ storm or a spinlock
   deadlock — the thing a per-task heartbeat can't see because the
   watchdog sweep itself may be starved.
3. **Hardware watchdog (last resort).**  Arm an emulated/real watchdog
   timer (QEMU `-watchdog`), pet it from a healthy path; if the whole
   box wedges, it resets.  The only recovery when software is too dead
   to help itself.

**The hard truth (cooperative-kill model).**  We can *detect* a freeze
at any layer, but we cannot always safely *force-kill* a wedged kernel
thread — it may hold a spinlock (the very reason M22.3 made kill
cooperative).  So layer 1's "kill + restart" only works if the frozen
task reaches a yield/poll point.  A truly wedged kthread that never
yields is only recoverable by layers 2–3 (or a reboot).  This gets
clean once **§M25** gives real user processes: a frozen *user* process
can be force-killed at any instruction and its address space + fds
reclaimed by the kernel, because its failure can't corrupt kernel
state.  So: heartbeat + restart for services now (M31), genuine
force-kill of frozen tasks later (M25 userland).

**Definition of done:**
- A service that stops petting its watchdog is detected + restarted;
  the event shows in `dmesg` (M28) and `/proc/services` (M29).
- A wedged CPU is reported (softlockup warning) rather than silently
  hanging the box.

**Out of scope:** NMI-based hardlockup detection, lockdep-style
deadlock prediction, per-task CPU-time rlimits.

**Depends on:** §M28 (log the warnings), §M29 (restart policy +
`/proc/services`); the hardware layer is independent.

---

## §M32 — Multi-user: identity, login, file permissions, isolation

**Why:** turn d-os from a single-operator machine into a real
multi-user system — several users on it at once, each with their own
identity, home, and processes, unable to read or kill each other's
work.  This is the security spine the OS has lacked.

**The hard dependency — §M25.**  *Real* isolation needs per-process
address spaces: today every task is a ring-0 kernel thread sharing the
kernel's address space, so any thread can read any memory and "users"
could only ever be advisory.  Enforcement (one user can't touch
another's memory) lands only once §M25 (userland foundation:
per-process VMM, ring-3 processes, ELF loader, fd table) exists.
Identity + the user DB + file ownership can land earlier as advisory
metadata and gain teeth when M25 arrives.  Also builds on §M27
(process hierarchy → sessions) and the VFS.

**Design — staged.**

1. **Credentials on tasks.**  `struct cred { uid, gid, groups[]; }` on
   `struct task`, inherited across spawn (a child gets its parent's
   creds).  uid 0 = root.  `getuid`/`setuid`-style accessors; privilege
   *drop* on login.  Cheap; the identity half can precede M25.
2. **User database.**  A `/etc/passwd`-shaped text store (name, uid,
   gid, home, shell, password hash) + `/etc/group`.  Text files, not a
   binary blob (same anti-registry stance as §M-registry).  A
   `user_lookup(name)` / `user_by_uid(uid)` API.
3. **Authentication + login.**  A `login` flow: read username +
   password, verify against the hash, then establish a **session** with
   that user's creds — cwd = home, `$USER`, and the user's shell,
   under a session-leader task (ties into the §M22.7 "GUI session"
   idea: one session per logged-in user).  A password *hash* placeholder
   with an explicit "NOT production crypto until a real primitive lands"
   caveat.
4. **File ownership + permissions.**  VFS inodes gain `owner_uid` /
   `owner_gid` / `mode` (rwx user/group/other); `vfs_open` / `unlink` /
   `rename` / `mkdir` check them against the caller's creds.  ramfs
   stores them; procfs synthesises (a `/proc/<pid>` is owned by the
   task's user); devfs nodes get sane defaults; exFAT (no Unix perms on
   disk) maps to a mount-wide default owner.  `chmod` / `chown`
   commands, gated on ownership / root.
5. **Privilege gating.**  Privileged operations — mount, reboot/shutdown,
   killing another user's process, writing another user's files, binding
   system resources — require uid 0 (start root-vs-not; a Linux-style
   capability set is the later refinement, not pure root).  `task_kill`
   / `task_kill_tree` reject a target owned by a different non-root user.
6. **Per-user isolation (the teeth — needs M25).**  Each user process in
   its own address space, so cross-user memory reads are impossible.
   procfs lists all pids but hides another user's cmdline/fds; a user
   sees + signals only their own processes (root sees all).  Optional:
   per-user resource caps.
7. **Simultaneous sessions.**  Several users logged in at once — GUI
   sessions and/or shell panes, each with its own creds + home +
   process subtree (session-leader per user, owned by that uid).  A
   `ps` USER column; `whoami` / `id` / `su` / `login` commands.

**Definition of done:**
- Two users log in (different panes or GUI sessions); each gets a shell
  running as their uid with their home; `ps` shows the USER column.
- User A cannot read user B's `0600` file; root can.  `chmod`/`chown`
  enforced.
- A non-root user cannot reboot or kill root's / another user's process.
- **(Post-M25)** user A's process cannot read user B's address space.
- DOCS.md gains a "Users & permissions" chapter.

**Out of scope (initially):** POSIX ACLs beyond rwx, PAM-style pluggable
auth, network identity (NIS/LDAP), SELinux/AppArmor-style mandatory
access control, disk quotas, real password KDF (scrypt/argon2) until a
crypto primitive exists, namespaces/containers.

**Depends on:** §M25 (hard, for real isolation), §M27 (sessions), VFS;
the GUI multi-session piece leans on the §M22.7 "GUI session" model.

---

## §M33 — Execution domains: where a service runs (kernel / user / isolated)

**The generalisation.**  Don't hard-code "kernel vs user" as a binary
baked into each subsystem.  Instead make the **execution domain** a
first-class, *declared* property of a service (§M29), chosen by config:

- `DOMAIN_KERNEL` — ring 0 / EL1, shared kernel address space; the
  bus's `LocalCall` transport works directly; monolithic, zero IPC cost.
- `DOMAIN_USER` — ring 3 / EL0, its own address space; needs a non-local
  transport (`IPC` / `SharedMemory`); real memory isolation.
- `DOMAIN_ISOLATED` — a USER domain plus a restricted capability set
  (granted ports / MMIO / IRQs only); the sandboxed extreme.

**Domain = declared capability, config *chooses* — not arbitrary.**  A
service declares which domains it *can* run in
(`.domains = DOMAIN_KERNEL | DOMAIN_USER`, default KERNEL-only) — that's
a capability of the code.  Config then picks *among the declared set*;
the broker (§M29) resolves it at bind time and selects a transport valid
for the chosen domain.  So domain and transport are coupled: **choosing a
domain constrains the valid transports** (KERNEL → LocalCall; USER /
ISOLATED → IPC / SharedMemory).  This is the config-driven, user-tunable
"where does it run" the discussion asked for — a deployment decision, not
a code decision, made honest by the capability declaration.

**Honesty gate — no advisory isolation that pretends to be real.**  Today
*every* task is a ring-0 kthread in one shared address space, so only
`DOMAIN_KERNEL` + `LocalCall` is *actually* real.  `DOMAIN_USER` /
`DOMAIN_ISOLATED` are **defined now but reserved**, and become real only
once §M25 (per-process VMM, ring-3 processes, fd table, IPC) ships —
exactly like §M32's "advisory until M25" stance.  The domain field
accepts `KERNEL` today; `USER` / `ISOLATED` are refused (loudly) until
the substrate exists, so we never ship isolation theatre.

**The flagship case — driver placement (a driver is a service).**  Every
driver runs today in ring 0 in the single shared address space (the
`DRIVER()` registry links them in at boot).  A buggy driver can corrupt
any memory, and a fault panics the whole system — the Windows 9x / VxD
failure mode.  Applying execution domains to drivers gives them *fault
tolerance* and, on top of §M25, *real isolation*: a driver can crash and
be **restarted without taking the system down**, and selected drivers can
be moved into their own ring-3 process.  The knob is **per-driver** and
config-driven (a desktop keeps drivers in KERNEL for speed; a server
profile moves them to USER), applied at restart — a **hybrid kernel**
(NT / XNU-shaped), not a wholesale micro-vs-monolith flip.  The staged
plan below is written in driver terms because drivers are the first and
hardest domain-switchable service; the same machinery serves any §M29
service.

**The key architectural idea — one narrow waist, two backends.**  A
driver is written against a *driver-runtime API* (`drv_port_out`,
`drv_mmio_map`, `drv_irq_wait`, `drv_dma_alloc`, `drv_send_to_client`)
instead of calling `outb` / `kmalloc` / `register_irq` directly.  That API
has two implementations, chosen per driver:
- **in-kernel backend** — direct calls (`outb`, plain function call);
  zero IPC overhead; monolithic.
- **user-mode backend** — IO-bitmap port grants, VMM-mapped MMIO, the IRQ
  forwarded to a "wait-for-IRQ" syscall, IPC messages to clients;
  isolated; microkernel-shaped.

The *same* driver source runs either way — the NetBSD rump-kernel model.
The API must be IPC-/capability-shaped **from day one** even while only the
in-kernel backend exists (convention #5), or the second backend will not
fit later.

**This waist *is* §M29's transport abstraction — build it once.**  "One
API, an in-kernel backend and a user-mode backend" is exactly the bus's
"one Contract, invoked over `LocalCall` or over `IPC`/`SharedMemory`."
The driver-runtime API is a Contract; its two backends are two
Transports; the per-driver domain flag is the config choice the broker
resolves.  Don't design a second, parallel marshalling boundary for
drivers — the same marshalling discipline (handles + copied/shared
buffers, no free pointers) and the same `IPC`/`SharedMemory` transport
backends serve both.  M33 is the *policy + capability + recovery* layer
on top of the M29 *binding* layer.

**Design — staged (climb, don't jump).**

1. **Tier 0 — fault-tolerant in-kernel hosting (no §M25 needed).**  Wrap
   driver entry points (init, IRQ handler) so a fault (#PF/#GP/#DE) whose
   faulting IP lies in a driver traps to a per-driver recovery path
   instead of a global panic: mark the driver DEAD, run its existing
   `DRIVER()` `shutdown`, and let the supervisor (§M29) restart it per
   policy; the watchdog (§M31) catches *hung* (non-faulting) drivers into
   the same restart path.  **Honest limit:** this is *not* memory
   isolation — a wild write has already happened before the trap; it
   converts the common trap-style faults + hangs from panic into restart,
   covering a large fraction of crash modes cheaply, and fits the
   monolithic philosophy (drivers stay ring 0).
2. **The runtime-API waist (design at §M25 time).**  Define the
   driver-runtime API in its final IPC-shaped form; implement only the
   in-kernel backend first.  Add a per-driver capability flag to the
   registry: `.domains = DOMAIN_KERNEL | DOMAIN_USER` (default
   KERNEL-only) — the same declared-capability field the intro defines.
   **Boot-critical** drivers (console / framebuffer, timer, interrupt
   controller, boot storage) are pinned DOMAIN_KERNEL — they come up
   *before* the process / IPC substrate exists (chicken-and-egg) and
   never appear in the toggle list.
3. **Tier 1 — user-mode isolation for non-DMA drivers (needs §M25).**
   Implement the user-mode backend and move a first *non-DMA* driver (PS/2
   keyboard or serial) into a ring-3 process, proving the same source runs
   both ways (rump-style demo).  Full memory isolation + real restart, no
   IOMMU required (no DMA to constrain).
4. **Domain list + config surface.**  Config keys mirror the existing
   pattern (`gui.shell`, `shell.provider`): `driver.profile =
   desktop|server` plus per-driver `driver.<name>.domain =
   kernel|user|isolated` overrides.  A `/proc/drivers` live view (name,
   domain, transport, pid if user-mode, isolation = full/advisory/none,
   restart count) and a `driver list | set <name> kernel|user` command.
   **Restart-to-apply** is the v1
   semantics — changing where a driver runs re-plumbs its bring-up, and
   live re-placement is the hard live-update problem; `driver set` writes
   config and reports "restart required".  (Live re-placement falls out
   for free later, once Tier 2's teardown + re-init + client-reconnect
   machinery exists.)
5. **IOMMU driver (VT-d / AMD-Vi) — its own, boundable piece.**  A
   DMA-capable driver moved to user-mode is *not* isolated without an
   IOMMU: the device can still DMA over kernel memory (kernel-bypass /
   DPDK is the proof that userspace ≠ isolated).  An IOMMU driver
   constrains device DMA to the driver's granted regions.  Until it
   exists, toggling a `.needs_dma` driver to user-mode is `ISOLATION:
   ADVISORY(!)` — allowed but loudly flagged, or refused under a strict
   profile.
6. **Tier 2 — DMA-driver isolation (needs §M25 + IOMMU) — the north
   star.**  virtio-blk / xHCI (later NIC / GPU) in ring 3 with
   IOMMU-constrained DMA + the full recovery discipline: clean resource
   teardown (MMIO unmap, IRQ release, DMA free, port revoke), device
   re-init from scratch, and **client reconnection** — the block layer /
   input subsystem must tolerate a driver vanishing and returning
   (idempotent / replayable requests).  This client-reconnect interface
   discipline is the genuinely hard, pervasive part.

**Definition of done:**
- Tier 0: a driver made to fault on command is restarted by the
  supervisor instead of panicking; visible in `dmesg` + the
  `/proc/drivers` restart count.
- Tier 1: `driver set ps2kbd user` + restart brings the keyboard up in a
  ring-3 process (`/proc/drivers` shows domain=user, a pid,
  isolation=full); killing that process restarts it, the keyboard keeps
  working, and the kernel does not fault.
- The same driver source, unchanged, runs in both domains.
- DOCS.md gains an "Execution domains / driver isolation" chapter.

**Out of scope (initially):** live (no-restart) re-placement; Tier 2
without an IOMMU; GPU / NIC isolation (arrive with their own drivers);
driver-to-driver dependency ordering across the boundary; per-driver
resource quotas.

**Depends on:** §M27 (lifecycle + kill-tree + change-hook — shipped) for
Tier 0; §M29 for the *binding* substrate this builds on — the service bus
(endpoint / contract / transport) + the domain-declaration field + the
broker that resolves a domain to a transport, plus the supervisor for the
restart half; §M25 (ring 3 + per-process address spaces + IPC) for the
user-mode backend / non-local transports (Tier 1+); §M31 (watchdog) for
the detect-hang half; an IOMMU driver for *safe* Tier 2.  **Philosophy
note:** Tier 0–1 fit the Linux-inspired monolith (CLAUDE.md #6); Tier 2
(user-mode DMA drivers) leans microkernel — a deliberate identity choice,
hence gated behind the IOMMU and treated as a north star, not a default.

---

## Userland maturation (§M34–§M42) — a real POSIX platform

**The goal is the platform, not the browser.**  Read this cluster as
"grow d-os into a real POSIX userland," *not* "build toward a browser."
Every milestone here — a process/signal model, threads, a full C library,
a package manager, a dynamic linker, a C++ runtime + support libraries,
TLS + DNS, a client graphics stack — is **independently necessary and
independently valuable**: each one unblocks a whole class of software
(shells, build tools, servers, native d-os apps, language runtimes), not
just one program.  The objective is simply *to have these capabilities*.
A browser (§M42) is included as the **proof / possible end-product**: if
all the pieces exist, running one becomes possible — a welcome *validation
that the platform is complete*, and a bonus, **not the driver of the
work**.  Nothing here is justified by "the browser needs it"; each stands
on its own.

**Why the browser is a good completeness test.**  It is the single
heaviest POSIX consumer we know of — it exercises the process model,
threads, the full libc, dynamic linking, the C++ runtime, TLS, and the
graphics stack all at once.  So "a browser can run" is a convenient
*shorthand for* "the userland is genuinely complete," which is why §M42
stays on the list as a validation target rather than being dropped.

**Honesty note on that test.**  A native Firefox/Chromium port is a
multi-year effort (each assumes the Linux syscall ABI + tens of millions
of lines of C++), so §M42 is staged around a *realistic* first browser
(NetSurf: own layout engine, framebuffer target, minimal deps — the path
SerenityOS/ToaruOS took), with WPE-WebKit as the mid target and
Firefox/Chromium as an acknowledged **north star**, not a scheduled
deliverable.  §M41 (Linux ABI shim) is the pragmatic accelerator — and is
itself broadly useful (run prebuilt Linux tooling), independent of any
browser.  Primary arch target is **x86_64** (then aarch64); i386 is out
of scope for the heavier ports (address-space + no upstream support).

**The porting discipline gate — §M35.5.**  Before pulling in *any*
foreign code (musl §M36 onward), a **package manager + isolation
substrate** must exist, or the ports pollute the system, breed version
conflicts, and rot.  §M35.5 is that gate: a **content-addressed store**
(Nix/Guix-shaped, *not* dpkg/apt) where every port lives in an immutable,
hash-named, per-version path with an explicit pinned dependency closure —
so the system stays uncluttered, apps depend on exactly their declared
deps (no global `/lib` soup), and multiple versions coexist.  **Every
milestone from §M36 on installs into the store, never the global FS.**
The runtime-isolation half is co-designed with §M37 (RPATH to exact
store paths = the loader-level isolation) and §M33/§M32 (capability- +
user-scoped FS view per app).

**Critical path (each `→` = hard dependency):**

```
§M25 userland ─► §M34 process/signals ─► §M35 threads/futex ─► §M35.5 pkg/store ─► §M36 POSIX libc
                                                                                        │
                          ┌──────────────────────────────────────────────────────────────┤
                          ▼                                                               ▼
                   §M37 dynamic link ─► §M38 C++/support libs ─► §M40 client GFX ─► §M42 browser
                                                                     ▲                  ▲
§M24 network ─► §M39 crypto/TLS/DNS ─────────────────────────────────┼──────────────────┤
§M26 Wayland ────────────────────────────────────────────────────────┘                  │
§M23 audio (soft, media only) ───────────────────────────────────────────────────────────┤
§M41 Linux ABI shim (optional; can substitute for parts of M36–M38 by emulation) ─────────┘
```

(§M35.5 gates every porting milestone — §M36–§M42 all install into its
store.)  Each milestone below restates its own **Depends on** line so it
is self-contained when read in isolation.

---

## §M34 — POSIX process & signals layer — ✅ shipped (i386)

> ✅ **SHIPPED (2026-07-11, i386) — see DOCS.md §4.27.**  All slices done +
> boot-tested: SysV initial stack (argc/argv/envp/auxv); **copy-on-write fork**
> (`vmm_space_clone` + `vmm_cow_fault` on the #PF path + `enter_user_mode_regs`);
> `waitpid` (Tier-A wait-queue); `execve` loading `/bin/*` from the VFS
> (`bin_install`); `pipe`+`dup2` (usock ring); **signals** (sigaction/kill/raise,
> return-to-user delivery + `__sig_trampoline`→SYS_SIGRETURN, default-terminate).
> Syscalls 14–21; shell `runargs`/`forktest`/`forkexec`/`pipetest`/`sigtest`.
> **Still open** (design below is the roadmap): EINTR / sigprocmask; user #PF →
> SIGSEGV (a user fault still panics); `vfork`/`posix_spawn`; job control /
> sessions / controlling tty; x86_64/aarch64 (the fork/signal register-restore is
> i386 asm).  **The net socket syscall API (§M24 stage 6) also shipped
> 2026-07-11** — ring-3 UDP+TCP sockets (see §M24).  **Next per the agreed
> sequencing:** §M35 threads → §M35.5 pkg → §M36 libc → …; §M26 Wayland still
> deferred until POSIX + libc exist.

**Why:** the single largest gap between today's userland (§M25) and any
real POSIX program.  Browsers — Chromium especially, with its
multi-process sandbox — assume `fork`/`exec`, argv/env, `waitpid`,
pipes, and signals.  Today `enter_user_mode` is one-way with no argv,
and there is no signal infrastructure at all.  This layer is a
**general POSIX abstraction** — it unblocks shells, build tools, and
essentially every future port, not just the browser.

**Design — staged.**
1. **`execve(path, argv, envp)`** — replace the current image in a task's
   address space, build a System V initial stack (argc / argv / envp /
   **auxv** — `AT_PHDR`, `AT_ENTRY`, `AT_PAGESZ`, `AT_RANDOM`, …), enter
   at the ELF entry.  Extends §M25's `proc_exec_elf` from "hello excursion"
   to a real program launch.
2. **`fork` / `vfork`** — duplicate the calling process: clone the
   `vmm_space` **copy-on-write** (a new `VMM_COW` PTE bit + a #PF/permission
   fault handler that copies the page and drops COW), dup the fd table
   (shared `ofile` refs), copy creds/cwd.  COW is the hard part and the
   reason this is post-§M25 (needs per-process address spaces + a fault
   path).  `posix_spawn` offered as the cheaper primary API; `fork` for
   compatibility.
3. **`waitpid` / exit status** — expose §M27's reaper to userland: a
   parent blocks (Tier A wait-queue) on a child's death and reads
   `WIFEXITED`/`WEXITSTATUS`/`WIFSIGNALED`.
4. **Pipes + fd plumbing** — `pipe`/`pipe2`, `dup`/`dup2`/`dup3`,
   `O_CLOEXEC`, `fcntl(F_GETFD/F_SETFD/F_GETFL/F_SETFL)`.  Pipes reuse the
   §M25 `ofile` ring machinery.
5. **Signals** — `struct sigaction`, `kill`/`tgkill`/`raise`,
   `sigprocmask`, delivery on return-to-user (per-task pending mask +
   handler trampoline pushing a `ucontext`, `sigreturn`), default actions
   (term/core/ignore/stop), `SIGSEGV`/`SIGCHLD`/`SIGPIPE`/`SIGINT`.
   Arch seam: the return-to-user path (already per-arch) grows a
   "deliver pending signal" hook.
6. **Sessions / process groups / job control** — `setsid`, `setpgid`,
   controlling terminal, `SIGINT`/`SIGTSTP` from the console, foreground
   pgrp — so a real shell (bash) and Ctrl-C work.
7. **Device nodes programs assume** — `/dev/null`, `/dev/zero`,
   `/dev/full`, `/dev/tty` (§M39 adds `/dev/urandom`).

**Definition of done:**
- A ring-3 program `fork`s, the child `execve`s `/bin/echo hi` with argv,
  the parent `waitpid`s and reads exit code 0.
- A pipeline `a | b` runs: `a`'s stdout is `b`'s stdin via `pipe`+`dup2`.
- Ctrl-C sends `SIGINT` to the foreground pgrp; a handler catches it.
- DOCS.md gains a "POSIX process model" chapter.

**Out of scope:** `clone` thread flags (→ §M35), real-time signals depth,
`ptrace`, namespaces/cgroups, `io_uring`.

**Depends on:** §M25 (per-process address spaces, ELF loader, fd table,
`ofile`), §M27 (init + reaper + hierarchy + kill-tree), Tier A
(blocking wait-queue for `waitpid`).  COW needs a new fault-handler path
on each arch.

---

## §M35 — Threads & futex — ✅ shipped (i386, UP)

> ✅ **SHIPPED (2026-07-11, i386) — see DOCS.md §4.28.**  `proc_clone`
> (SYS_CLONE) creates a thread that SHARES the creator's address space
> (`task->mm_shared` stops the reap from freeing it) + dups the fd table,
> entering ring 3 at a given entry/stack; `futex` (SYS_FUTEX, `futex.c`):
> FUTEX_WAIT parks iff `*uaddr==val` (lost-wakeup-free over the Tier-A
> wait-queue) / FUTEX_WAKE, hashed by physical address; libc `thread_create`/
> `thread_join`/`futex` + a 3-state Drepper mutex in `threadtest`.  **UP-tested:
> 4 threads × 5000 shared-counter increments = 20000/20000 PASS.**  **Known
> pre-existing gap:** ring-3 tasks don't run on APs (a single global TSS in
> `tss.c` + no per-CPU `LTR`), so `threadtest` AND `procspawn` hang on `-smp 2`
> — the fix is **per-CPU TSS + per-AP LTR** (a self-contained SMP-userland infra
> change; the threads/futex code itself is SMP-safe).  **Still open:** that
> per-CPU TSS fix; TLS (`__thread`/`set_thread_area`); PI/robust futexes;
> `gettid`; per-thread signal masks; x86_64/aarch64.

**Why:** browsers are massively multi-threaded (compositor, network, GC,
worker pools); today there are **no user-space threads at all**.  Also a
prerequisite for a real libc's `pthread`/TLS and for `std::thread`.

**Design.**
1. **`clone`-style thread creation** — a thread = a task sharing its
   parent's `vmm_space` + fd table (`CLONE_VM|CLONE_FS|CLONE_FILES|
   CLONE_THREAD`) but with its own user stack + kernel stack + TID.
   Reuses the SMP scheduler already in place (threads land on per-CPU
   runqueues, load-balanced) — the kernel side is largely present; the
   new work is the *shared-address-space task* semantics + thread-group
   exit (`exit_group` kills all threads).
2. **Thread-local storage** — set the arch TLS base per thread:
   `arch_prctl(ARCH_SET_FS)` (x86_64), `set_thread_area`/`GDT` entry
   (i386), `TPIDR_EL0` (aarch64); a `__tls` block laid out per the ELF
   TLS ABI (`.tdata`/`.tbss`, initialised at thread start).
3. **`futex`** — the one syscall every modern threading library needs:
   `FUTEX_WAIT`/`FUTEX_WAKE` (+ `_BITSET`, `_REQUEUE`, `PRIVATE`) over a
   hashed wait-queue keyed by physical address, built on Tier A's
   block/wake.  This is what mutexes/condvars/`std::atomic` waits sit on.
4. **Thread-group signal + exit semantics** — signals target a thread
   group; `exit`/`exit_group` distinction; `gettid` vs `getpid`.

**Definition of done:**
- A program spawns 4 threads that increment a shared counter under a
  futex-backed mutex to the correct total; runs correctly on SMP (≥2 CPUs).
- TLS: each thread reads its own `__thread` variable.
- DOCS.md "Threads & futex" chapter.

**Out of scope:** priority inheritance / PI-futexes, robust-list depth,
NPTL cancellation edge cases, per-thread scheduling policies beyond the
current scheduler.

**Depends on:** §M25 (address spaces), §M34 (process model, exit/signal
semantics threads extend), Tier A (block/wake under futex), the existing
SMP per-CPU scheduler.  Per-arch TLS-base seam.

---

## §M35.5 — Package manager & isolation (the substrate for every port)

**Why — a gate, not an afterthought.**  Everything from §M36 on brings in
*foreign* code (musl, then dozens of libraries, then a browser).  Without
a discipline enforced *before* the first port, the system fills with
untracked files, breeds version conflicts ("dependency hell"), and
becomes impossible to clean or reproduce.  The three stated requirements —
**isolation**, **no clutter**, **minimal version coupling** — are exactly
what a content-addressed store solves, so this milestone must land before
§M36.  It is also generally valuable: the same store manages *native*
d-os software, not only ports.

**Linux-inspired, not Linux-bound (convention #6).**  We **reject**
dpkg/rpm/apt — their model is a *mutable global `/usr` + `/lib`*, which is
the direct cause of version conflicts, "cannot safely remove X", and
scriptlets mutating the system as root.  That is accidental history.  We
**adopt the content-addressed store** (Nix / Guix) because it solves the
three requirements structurally rather than by convention.

**Design.**
1. **Content-addressed store** — `/store/<hash>-<name>-<version>/`,
   **immutable** after build.  The hash covers the build inputs (source +
   recipe + the store paths of its dependencies).  Consequence: **many
   versions/variants coexist** with zero conflict; nothing ever writes
   into a shared global `/lib` or `/bin`.  → satisfies "no clutter" +
   "multiple versions."
2. **Explicit, pinned dependency closure** — each package declares its
   dependencies by **exact store path**; there is no ambient global search
   path.  A package's runtime closure *is* its declared graph, pinned by
   hash.  → satisfies "don't depend on other versions more than
   necessary": the coupling is exactly what you wrote down, and it is
   reproducible.
3. **Two-level isolation** (where it binds to d-os primitives):
   - **Load-time:** RPATH baked to exact store paths → a binary resolves
     *only* its declared dependencies, no global `/lib` soup.  This is the
     isolation mechanism of §M37 (dynamic linker) — **co-design the two**.
   - **Run-time:** each app runs in its own §M25 address space with a
     §M33-capability- and §M32-user-scoped **FS view** — it sees its own
     store closure + its data directory, not the whole system
     ("container-lite" over the VFS: a bind/overlay-style restricted mount
     namespace, no full container runtime).
4. **Hermetic builds** — a builder runs in a **sandboxed execution domain
   (§M33)**: no network except a pinned-hash fetch phase, only the declared
   inputs visible, a fixed environment → **reproducible** and
   host-contamination-free.  This stops "garbage" leaking in from the
   build side.
5. **Profiles + garbage collection** — "installed" = a **symlink forest /
   generated view** selecting which store paths appear on `PATH` / in the
   library search.  Uninstall = drop from the profile; unreferenced store
   paths are reclaimed by a **GC** (mark from the live profiles/roots).
   Old generations are kept for **rollback**.  → the system never silently
   accumulates cruft.
6. **Text recipes, not binary metadata** — a package is a **text recipe**
   (name, version, source URL + hash, dependency list, build steps) — the
   same anti-binary-blob stance as §M-registry.  A `pkg
   build/install/remove/gc/list/why` command; store metadata browsable via
   procfs.
7. **Bootstrap** — a seed toolchain (cross-built musl + compiler, brought
   in once) breaks the chicken-and-egg; from there everything is built
   *in* the store.  (Later: signed packages once §M39 crypto exists;
   binary substitution/cache is a further follow-up.)

**Implementation sketch (concrete shapes).**

*On-disk layout* (the store is the source of truth; everything else is a
view over it):

```
/store/<hash>-<name>-<version>/         immutable, read-only after seal
                              bin/  lib/  include/  share/
/store/.meta/<hash>.recipe              the exact text recipe that built it
/store/.meta/<hash>.closure             newline list of dep store paths (pinned graph)
/etc/pkg/recipes/<name>.recipe          source recipes (text, version-controlled)
/etc/pkg/profiles/<name>/               symlink forest → the active PATH/lib view
/etc/pkg/profiles/<name>.gen/<N>/       numbered generations (rollback)
/var/pkg/roots/                         GC roots: live profiles + running-process pins
```

*Store-path hash* = `H(recipe-text ‖ source-content-hash ‖ each-dep's-store-hash)`.
Deterministic → identical inputs yield the identical path (reproducibility
+ safe coexistence); changing any input forks a new path, so old consumers
are untouched.

*Recipe format* (text, declarative — the anti-blob stance):

```
name     zlib
version  1.3.1
source   https://zlib.net/zlib-1.3.1.tar.gz
sha256   9855b6d802d7fe5b7bd5b196a2271655...
deps     musl
build    ./configure --prefix=$OUT
         make
         make install
```

`$OUT` = the assigned store path (known before the build); `deps` resolve
to store paths and are the *only* things visible in the build sandbox.

*`pkg` command surface* (a shell command first; later an §M29 service so
installs/GC can run supervised):

| command | effect |
|---------|--------|
| `pkg build <recipe>` | resolve deps → fetch+verify source → hermetic build → **seal** store path (make ro) |
| `pkg install <name> [-p profile]` | build if absent, add symlink to profile, bump generation |
| `pkg remove <name>` | drop from profile → new generation (store path survives until GC) |
| `pkg rollback [-p profile]` | point the profile at the previous generation |
| `pkg gc` | mark from roots (profiles + running pins), sweep unreferenced store paths |
| `pkg why <name>` / `pkg closure <name>` | print the pinned dependency closure (introspection) |
| `pkg list [-p profile]` | what each profile currently exposes |

*RPATH isolation (co-design with §M37).*  At seal time, patch every ELF's
`DT_RUNPATH` to the **exact** store `lib/` paths of its declared deps.
Then `ld.so` (§M37) never consults a global `/lib`; each binary loads
precisely its closure.  No `LD_LIBRARY_PATH`, no version soup.  (Seed
toolchain: the cross-linker sets it; in-store builds: a `pkg`-side
patch step.)

*Build sandbox (a §M33 execution domain).*  A builder is a child process
(§M34) run in an isolated domain: FS view = only the deps' store paths + a
fresh `$OUT` + a private `/tmp` (a restricted mount view over the VFS); **no
network capability** except the dedicated content-verified fetch step; a
fixed environment (`PATH` = deps only, stable `TZ`/locale, no host
leakage).  On success `$OUT` is sealed read-only and hashed; on failure it
is discarded — the live system is never touched mid-build.

*Runtime app isolation.*  Launching an app = spawn (§M34) into a §M25
address space whose FS view is scoped (§M33 capability + §M32 user) to its
store closure + a per-app data dir — it cannot see other store paths or
other users' data.  "Container-lite": a restricted mount view, not a full
container runtime.

*Garbage collection.*  Roots = every still-referenced profile generation +
every running process's pinned closure (a process pins its closure for its
lifetime).  Mark-sweep over `/store`; unreferenced paths deleted.
Immutability makes this safe — nothing ever mutates a store path in place,
so a path is either wholly live or wholly dead.

*Bootstrap.*  Import a prebuilt **seed** (cross-built musl + a C/C++
toolchain) into the store by hash, once — the single clearly-marked
non-reproducible step (Guix's "bootstrap seed" model).  Everything after is
built in-store from recipes.

*Procfs introspection.*  `/proc/pkg/store` (paths + sizes),
`/proc/pkg/profiles/<p>` (current generation + contents) — store state
inspectable the Unix way, no binary registry.

*Staging within the milestone (build order):*
1. Store layout + recipe parser + `pkg build` (sandbox can start loose,
   tighten later).
2. Profiles + `install`/`remove` + generations + `rollback`.
3. RPATH sealing (lands with §M37) → real load-time isolation.
4. §M33 build sandbox + §M25/§M32/§M33 runtime FS-view → real isolation.
5. GC + procfs + `why`/`closure`.

**Definition of done:**
- Two versions of a library coexist in the store; two apps each link their
  own version by RPATH and both run.
- `pkg install` then `pkg remove` + `pkg gc` leaves **zero residue**
  outside the store; nothing was written to a global `/lib`/`/bin`.
- At runtime an app can reach only its dependency closure + its data
  directory, not the rest of the FS.
- Rebuilding a package from the same pinned inputs yields the **same store
  hash** (reproducible).
- DOCS.md gains a "Package manager & isolation" chapter.

**Out of scope (initially):** a binary substituter / cache server,
distributed builds, a full Nix-style pure-functional language (a simpler
declarative recipe format suffices), cross-store trust/signing until §M39
crypto lands, full container/namespace runtime (only the FS-view slice
needed for app isolation).

**Depends on:** §M34 (process model — run builders/installers as child
processes), VFS + a writable FS for the store (ramfs/exFAT); **co-designed
with §M37** (dynamic-linker RPATH is the load-time isolation mechanism);
leans on §M33 (execution domains / capabilities for the build + run
sandbox) and §M32 (per-user profiles + FS-view scoping).  **Gates
§M36–§M42** — every milestone that ports foreign code installs into this
store, never the global filesystem.

---

## §M36 — POSIX syscall breadth + native libc (musl port)

**Why:** the in-tree libc is ~120 lines (`write/read/open/mmap/malloc/
printf`).  A browser (and its build tools) needs a full libc and the
several-hundred-syscall surface it sits on.  Porting **musl** (small,
clean, static-friendly, permissive licence) as the native libc is the
target — it defines exactly which syscalls must exist.

**Design.**
1. **Syscall surface expansion** — bring the table from §M25's handful to
   the musl-required set: `stat`/`fstat`/`lstat`/`fstatat`, `getdents64`,
   `mprotect`/`madvise`/`brk`/`mremap`, `clock_gettime`/`clock_nanosleep`/
   `gettimeofday`/`nanosleep`, `readv`/`writev`/`pread`/`pwrite`,
   `getcwd`/`chdir`/`mkdir`/`unlink`/`rename`/`symlink`/`readlink`,
   `epoll_create1`/`epoll_ctl`/`epoll_wait`, `eventfd2`, `timerfd`,
   `uname`, `sysinfo`, `getrandom` (→ §M39), `fcntl`, `poll`/`ppoll`
   (§M25 has `poll`), the AF_INET/AF_UNIX `socket`/`bind`/`connect`/
   `accept`/`listen` family (AF_INET via §M24), plus `sysconf` inputs.
   Each is a portable handler in `usyscall.c`; arch dispatchers only
   marshal args.
2. **`errno` discipline** — negative-return convention from the kernel,
   `errno` set in the libc wrapper (musl already does this — the kernel
   just needs consistent `-E*` returns).
3. **musl integration** — cross-compile musl against d-os's syscall
   numbers (a d-os `arch/` under musl, or a thin Linux-number alias if
   §M41 lands first), replacing `user/libc.c`.  Keep the tiny in-tree
   libc for the self-test programs.
4. **A `/bin` + `/lib`** convention on ramfs/exFAT so programs and (later)
   shared objects have a home; a minimal coreutils (`sh`, `ls`, `cat`,
   `echo`, `env`) as the first musl-linked programs.

**Definition of done:**
- A musl-linked `sh` runs interactively in ring 3, forks/execs coreutils,
  pipes work, exit codes propagate.
- `stat`/`getdents` back a real `ls -l`; `clock_gettime` returns monotonic
  + realtime.
- DOCS.md "libc & syscall surface" chapter with the supported-syscall list.

**Out of scope:** glibc-specific extensions, NSS plugins, iconv beyond
UTF-8, full locale database (`C`/`C.UTF-8` only until §M38's ICU),
`io_uring`, `inotify`.

**Depends on:** §M35.5 (the store — musl is the *first* port and installs
into it, establishing the pattern), §M34 (process model — musl assumes
fork/exec/signals), §M35 (threads — musl's pthread), §M24 (AF_INET
syscalls), §M25 (fd/mmap substrate).

---

## §M37 — Dynamic linking (ld.so / `.so` / dlopen)

**Why:** browsers and their libraries ship as shared objects; static
linking a whole browser is often infeasible (size, `dlopen` plugins,
GL driver loading).  Today the ELF loader (§M25) handles *static*
executables only — no interpreter, no runtime relocations.

**Design.**
1. **PIE / PIC executables** — load `ET_DYN` main objects at a base,
   apply `R_*_RELATIVE` relocations.
2. **`PT_INTERP` handling in `execve`** — when present, map the requested
   dynamic linker and hand it control with the correct auxv (`AT_PHDR`/
   `AT_PHNUM`/`AT_BASE`/`AT_ENTRY`); musl's `ld-musl` is the interpreter.
3. **Shared objects** — parse `PT_DYNAMIC`, `DT_NEEDED` search
   (`/lib`, `DT_RPATH`/`RUNPATH`, `LD_LIBRARY_PATH`), symbol resolution
   (`.dynsym`/`.hash`/`.gnu.hash`), `GLOB_DAT`/`JMP_SLOT` relocations,
   lazy vs `BIND_NOW` (start with `BIND_NOW` — simpler), `DT_INIT_ARRAY`
   ordering.
4. **TLS relocations** — the general-dynamic/local-dynamic TLS model
   (`__tls_get_addr`, `DTPMOD`/`DTPOFF`/`TPOFF`) so `__thread` works
   across shared objects (ties to §M35 TLS).
5. **`dlopen`/`dlsym`/`dlclose`** on top.

**Definition of done:**
- A dynamically-linked `hello` (`ld-musl` interp, `libc.so`) runs.
- A program `dlopen`s a `.so` and calls a symbol from it.
- `__thread` variables resolve correctly in a shared library on a thread.
- DOCS.md "Dynamic linking" chapter.

**Out of scope:** symbol interposition/`LD_PRELOAD` subtleties, lazy PLT
(defer to `BIND_NOW`), `STB_GNU_UNIQUE`, prelink, `ifunc` beyond a basic
resolver.

**Depends on:** §M36 (musl + the syscall surface `ld.so` uses: `mmap`/
`mprotect`/`open`/`read`), §M35 (TLS model), §M25 (ELF loader to extend).

---

## §M38 — C++ runtime + support libraries

**Why:** browsers are C++; and even NetSurf/WebKit pull a stack of C
libraries.  This milestone ports the runtime + the "everybody needs
these" libraries so higher milestones (and any future C++/graphics app)
have them.

**Design — port, in dependency order:**
1. **C++ runtime** — `libc++` + `libc++abi` + `libunwind` (LLVM, matches
   musl cleanly), or `libstdc++` + `libgcc_s`.  Needs working **DWARF
   exception unwinding** (`.eh_frame` + `_Unwind_*`), RTTI, thread-safe
   statics (`__cxa_guard_*` → futex), `__cxa_atexit`.  This is the item
   that most exercises §M37 (unwinding across shared objects) + §M35
   (thread-safe init).
2. **Compression / image** — `zlib`, `libpng`, `libjpeg-turbo`,
   `brotli` (HTTP content-encoding).
3. **Text / fonts** — `freetype` (glyph rasterisation) + `fontconfig`
   (font discovery; needs a `/usr/share/fonts` + a couple of TTFs) +
   `harfbuzz` (shaping) + **ICU** (Unicode segmentation/normalisation —
   large, but browsers hard-depend on it).
4. **2D primitives** — `pixman`, and `cairo`/`Skia`'s software path
   (Skia bundled with the browser; cairo for NetSurf/GTK targets).
5. **Parsing / misc** — `expat`/`libxml2`, `sqlite` (browser storage),
   `nghttp2` (HTTP/2, over §M39 TLS).

**Definition of done:**
- A C++ program that throws + catches across a `.so` boundary runs
  correctly (unwinding works).
- A test renders a UTF-8 string with freetype+harfbuzz to a bitmap.
- `zlib`/`png`/`jpeg`/`sqlite` self-tests pass in ring 3.
- DOCS.md "C++ runtime & support libraries" chapter listing ported libs +
  versions.

**Out of scope:** GTK/Qt full toolkits (only what NetSurf/WebKit's chosen
frontend needs), OpenMP, Fortran runtime, the browser itself (→ §M42).

**Depends on:** §M36 (libc), §M37 (dynamic linking — these ship as `.so`s
and unwinding crosses them), §M35 (thread-safe statics).

---

## §M39 — Crypto, entropy, TLS, and DNS

**Why:** no modern site loads over plain HTTP — **HTTPS is mandatory**,
and HTTPS needs a TLS stack, which needs entropy.  §M24 gives raw
TCP; this makes it usable.  Also a general capability (SSH, package
signing, `/etc/shadow` KDF for §M32 all want it).

**Design.**
1. **Entropy** — a kernel CSPRNG seeded from hardware (`RDRAND`/`RDSEED`
   on x86, `RNDR` on aarch64 where present) + timing/IRQ jitter; exposed
   as `/dev/urandom`, `/dev/random`, and the `getrandom` syscall + auxv
   `AT_RANDOM`.  (This is the honest gate — §M32 noted "NOT production
   crypto until a real primitive lands"; this milestone is that primitive.)
2. **Crypto library** — port **mbedTLS** (small, self-contained — good
   first target) and/or **BoringSSL** (what Chromium expects).  Provides
   AEAD/ECC/RSA/hashing.
3. **TLS integration** — TLS 1.2/1.3 client over §M24 sockets; a CA trust
   store at `/etc/ssl/certs` (bundle Mozilla's CA set); certificate +
   hostname verification.
4. **DNS resolver** — `getaddrinfo`/`getnameinfo` (in libc/musl) over a
   UDP/TCP stub resolver; `/etc/resolv.conf` populated by the §M24 DHCP
   client; `/etc/hosts`.

**Definition of done:**
- `getrandom` + `/dev/urandom` return non-repeating, well-distributed
  bytes; `AT_RANDOM` populated per exec.
- `wget https://<host>/` fetches a page over verified TLS 1.3 (cert +
  hostname checked against the CA store).
- `getaddrinfo("example.com")` resolves via DNS.
- DOCS.md "Crypto, entropy & TLS" chapter.

**Out of scope:** a hardware TRNG driver beyond `RDRAND`/`RNDR`, TLS
*server* role, QUIC/HTTP-3 (later), FIPS modes, smartcard/PKCS#11.

**Depends on:** §M24 (TCP/UDP sockets + DHCP for `resolv.conf`), §M36
(libc — `getaddrinfo`, and mbedTLS/BoringSSL link against it), §M37 (they
ship as `.so`).

---

## §M40 — Client graphics stack (Wayland client + GL + Skia)

**Why:** a browser does not draw to the framebuffer directly — it talks
to a **display server** and renders through a GL/2D stack.  §M26 provides
the Wayland *server*; this milestone provides the *client* side plus the
rendering path the browser plugs into.

**Design.**
1. **Wayland client** — `libwayland-client` over the §M25 unix-socket +
   fd-passing + mmap substrate (the same primitives §M26's server uses);
   `xkbcommon` for keymap handling; `wayland-protocols` (xdg-shell) so a
   real client's surface/seat/keyboard/pointer wiring works.
2. **Software GL** — **Mesa's software rasteriser** (`llvmpipe`/`swrast`)
   exposing EGL + GLES2/GL3, running purely on the CPU (no GPU driver
   needed — the pragmatic path; hardware GL is a much later, per-GPU
   effort).  EGL platform = Wayland.
3. **Skia software backend** — Chromium/Flutter-style rendering; Skia is
   bundled with the browser but needs EGL/GL or its CPU raster backend
   wired to a Wayland buffer.
4. **Frontend toolkit (target-dependent)** — NetSurf's own framebuffer/
   Wayland frontend, or WPE-WebKit's `WPEBackend` (designed for exactly
   this minimal EGL-on-Wayland embedded case) — chosen in §M42.

**Definition of done:**
- A `weston-terminal`-class Wayland client (or `wpe`'s test app) runs
  against the §M26 server: draws, takes keyboard + pointer input.
- An EGL+GLES2 program clears + draws a triangle via `llvmpipe`, presented
  through a Wayland buffer.
- DOCS.md "Client graphics stack" chapter.

**Out of scope:** hardware GPU acceleration (per-GPU drivers — a north
star of its own), Vulkan, X11/XWayland, DMA-BUF zero-copy (software
buffers via shm are fine to start).

**Depends on:** §M26 (Wayland server — the thing the client talks to),
§M36 (libc), §M37 (Mesa/Wayland ship as `.so`), §M38 (C++ for
Skia/Mesa + pixman/freetype).  Soft: §M23 (audio) for `<video>`/WebRTC.

---

## §M41 — Linux syscall ABI shim (optional binary-compat accelerator)

**Why:** the pragmatic alternative to porting every library.  Rather than
recompiling the whole browser + its deps against d-os, implement enough
of the **Linux** syscall ABI (numbers + struct layouts + semantics) that
*unmodified* Linux ELF binaries run — the FreeBSD-Linuxulator / WSL1
model.  This can substitute for large parts of §M36–§M38's "port it"
work by *emulation* instead, and is broadly useful (run prebuilt Linux
tooling).  Marked **optional** because it is a strategy choice, not a
strict dependency of §M42 — either "native musl ports" (§M36–M38) *or*
"Linux ABI + prebuilt binaries" (this) can feed the browser.

**Design.**
1. **A per-process "Linux personality"** — a flag on `execve` (from ELF
   `EI_OSABI` / an `.note.ABI-tag`, or a launcher) selecting the Linux
   syscall dispatch table.
2. **Syscall translation** — map Linux x86_64/aarch64 syscall numbers to
   d-os primitives; translate struct layouts (`struct stat`, `iovec`,
   `sigaction`, `termios`, `epoll_event`, `sockaddr`) between Linux and
   native shapes.  Reuses §M34–M36 mechanisms underneath — the shim is a
   *translation* layer, not a second kernel.
3. **`/proc` + `/sys` shims** — the subset Linux programs actually read
   (`/proc/self/maps`, `/proc/cpuinfo`, `/proc/self/auxv`, `/sys/...`
   device probes) synthesised from d-os state.
4. **vDSO** — a Linux-shaped vDSO for `clock_gettime`/`getcpu` fast paths
   many binaries expect via auxv `AT_SYSINFO_EHDR`.

**Definition of done:**
- An unmodified prebuilt Linux `busybox` (static, then dynamic with a
  Linux `ld-musl`/`ld-linux`) runs under the personality: `ls`, `cat`,
  `sh` work.
- A prebuilt Linux `curl https://…` works end-to-end (exercises the shim
  + §M24 + §M39).
- DOCS.md "Linux ABI compatibility" chapter documenting covered syscalls +
  known gaps.

**Out of scope:** 100 % Linux ABI (only the browser-relevant subset),
`io_uring`/`bpf`/`seccomp` deep fidelity, cgroup/namespace emulation,
running Linux *kernel* modules.

**Depends on:** §M34 (process/signals — the shim maps onto them), §M35
(threads/futex — Linux `clone`/`futex` semantics), §M36 (the native
syscall surface it translates to), §M37 (to run dynamic Linux binaries).

---

## §M42 — Web browser bring-up (validation target, not the goal)

**Why:** *not* a goal in itself — the **completeness proof** for §M34–§M41.
A browser is the heaviest POSIX consumer we know of, so getting one to
render a real page demonstrates, in one shot, that the process model,
threads, libc, package store, dynamic linker, C++ runtime, TLS and
graphics stack are all genuinely done.  It is included as that
validation + a welcome bonus, **not as the objective driving the earlier
milestones** — each of those stands on its own and would be built anyway.

**Design — staged by browser, easiest first (the honest ordering):**
1. **Tier 1 — NetSurf.**  Own compact layout engine, C, minimal deps,
   a **framebuffer / Wayland frontend**, no GPU/JS-heavy requirement
   (its JS is optional/limited).  The realistic first "it renders a web
   page" — the SerenityOS/ToaruOS-class achievement.  Needs §M36 libc +
   §M38 (freetype/png/jpeg/curl) + §M39 (TLS) + a frontend (§M40 or raw
   framebuffer).
2. **Tier 2 — WPE-WebKit.**  A real, standards-compliant engine
   (WebKit) with a full JS engine (JavaScriptCore), explicitly designed
   for embedded EGL-on-Wayland with a minimal backend (`WPEBackend-fdo`).
   Needs the full §M38 stack + §M40 (EGL/GL + Wayland client) + §M35
   threads.  This is "a modern site mostly works."
3. **North star — Firefox / Chromium.**  Multi-process sandbox
   architecture: hard-depends on §M34 (`fork`/`exec` + the sandbox's
   `seccomp`-style filtering), §M35 (heavy threading), §M39 (TLS/crypto),
   §M40 (GL), the complete §M38 support stack, and realistically §M41
   (the build/runtime assumes so much Linux ABI that emulation is easier
   than a full native port).  Acknowledged as a **multi-year north
   star**, not a scheduled deliverable — documented so the ambition and
   its true cost are both explicit.

**Definition of done (staged):**
- Tier 1: NetSurf loads `https://example.com` over TLS and renders the
  page (text + images + layout) into a window on the §M22/§M26 desktop;
  links are clickable.
- Tier 2: WPE-WebKit renders a JS-driven page; input works.
- North star: documented feasibility + gap analysis; not required to ship.
- DOCS.md "Web browser" chapter.

**Out of scope (per tier):** GPU-accelerated compositing (software GL is
the baseline), WebRTC/full media (needs §M23 audio + codecs), extensions,
DRM/EME, the Chromium sandbox's full Linux-namespace isolation.

**Depends on:** §M36 + §M38 + §M39 (all tiers); §M40 + §M26 (graphical
frontend, Tier 2+); §M34 + §M35 (Tier 2 threads, Tier 3 multi-process);
§M41 (pragmatically, Tier 3); §M23 (soft — media only).  In short: the
capstone of the entire cluster.

---

## §M-registry (parked) — hierarchical config store

**Status:** intentionally NOT scheduled.  A Windows-style registry
(monolithic, opaque, corruption-prone) is exactly the "accidental
history" the project rejects (see CLAUDE.md #6).  The Unix answer to
the same need already exists here: `/etc` text configs + the config
subsystem + procfs.  If a concrete need for *hierarchical,
runtime-tunable, persisted* settings appears, the direction is a
sysfs-style tunables tree (procfs write-handlers + save-to-`/etc`),
not a binary registry.  Revisit only with a specific use case.

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

- **2026-07-11** — **§M35 shipped (threads + futex, i386, UP).**  `proc_clone`
  (SYS_CLONE) = a task sharing the creator's address space (`mm_shared`) + fds,
  at a ring-3 entry/stack; `futex` (SYS_FUTEX) FUTEX_WAIT/WAKE over hashed
  Tier-A wait-queues (lost-wakeup-free); libc `thread_create`/`thread_join` +
  a 3-state Drepper mutex.  UP-tested: 4 threads × 5000 increments = 20000/20000.
  **Discovered a pre-existing gap:** ring-3 tasks don't run on APs (single global
  TSS + no per-CPU LTR) — threadtest AND procspawn hang on `-smp 2`; fix =
  per-CPU TSS (self-contained SMP-userland infra, unblocks all ring-3-on-AP).
  Next: per-CPU TSS → TLS → §M35.5 pkg.  See DOCS.md §4.28.
- **2026-07-11** — **§M24 stage 6 shipped: BSD socket API to userland (i386).**
  Ring-3 networking over the in-kernel stack: `FD_NETSOCK` ofile + `struct
  netsock` back `socket`/`bind`/`connect`/`sendto`/`recvfrom` (syscalls 22–26).
  UDP via a per-socket datagram RX ring on net.c's port bindings; TCP via
  `net_tcp_connect`/`send`/`recv`/`close` (read/write on the fd, one connection
  at a time).  Host-order IPv4 + port ints (no sockaddr yet).  Boot-tested from
  ring 3: `dnstest` (UDP-socket DNS → example.com) + `httptest` (UDP DNS + TCP
  socket → `HTTP/1.1 200 OK`, 829 B).  The §M39 TLS bridge (swap TCP for TLS →
  HTTPS).  Next: §M35 threads.  See DOCS.md §4.25.
- **2026-07-11** — **§M34 shipped (POSIX process model, i386).**  The classic
  Unix process API on the M25 userland (DOCS §4.27): SysV initial stack
  (argc/argv/envp/auxv); **copy-on-write fork** (`vmm_space_clone` shares
  writable pages read-only+COW ref-counted, `vmm_cow_fault` resolves writes on
  the #PF path, `enter_user_mode_regs` resumes the child at the fork point with
  eax=0); `waitpid` (Tier-A wait-queue); `execve` loading `/bin/*` from the VFS
  (`bin_install` populates `/bin`); `pipe`+`dup2` (usock ring); **signals**
  (sigaction/kill/raise, return-to-user delivery with a user-stack signal frame
  + `__sig_trampoline`→SYS_SIGRETURN, default-terminate on INT/TERM/KILL/SEGV).
  Syscalls 14–21; shell runargs/forktest/forkexec/pipetest/sigtest, all
  boot-tested (forktest proves COW isolation).  Fork/signal register-restore is
  i386 asm → i386-only for now.  Open: EINTR, sigprocmask, user #PF→SIGSEGV,
  vfork/posix_spawn, job control, x86_64/aarch64.  Next: net socket syscall API
  → §M35 threads.  See DOCS.md §4.27.
- **2026-07-11** — **§M23 stage 1 shipped (audio, i386).**  An `audio_dev`
  registry (block/net-shaped) + an AC97 codec driver (PCI 0x8086:0x2415, NAM
  mixer + NABM bus-master, PCM output via a Buffer Descriptor List over a
  128 KB PMM DMA buffer) + a portable square-wave tone generator.  Shell
  `lsaudio`/`beep`/`tone`.  Boot-tested via QEMU `-audiodev wav`: a 440 Hz beep
  captured as a clean ±8000 square wave (~444 Hz).  Open: `play <path>` WAV
  player (stage 4), `/dev/dsp`, mixer/multi-stream, input, Intel HDA, IRQ
  completion, x86_64/aarch64.  See DOCS.md §4.26.
- **2026-07-11** — **§M24 stages 1–3 shipped (network stack, i386).**  A
  from-scratch IPv4 stack: virtio-net driver (legacy PCI, two queues +
  pre-posted RX buffers) + a `net_device` registry mirroring the block layer +
  the arch-independent stack in `kernel/core/net.c` (Ethernet → ARP → IPv4 →
  ICMP → UDP → TCP) + a DNS stub resolver.  Shell `lsnic`/`ping`/`arp`/
  `nslookup`/`wget`/`nettest`.  Boot-tested through QEMU SLIRP to the real
  internet: ICMP ping 3/3, DNS resolves example.com, TCP fetches `HTTP/1.1 200
  OK`.  RX polled from the calling task (no IRQ/lock yet); TCP is client-only,
  no retransmit/congestion (safe on the lossless SLIRP link).  Still open: the
  socket *syscall* API to userland (stage 6), IRQ RX + `netd`, TCP timers +
  server role, DHCP (stage 7), IPv6, x86_64/aarch64.  See DOCS.md §4.25.
- **2026-07-11** — **Reframed the §M34–§M42 cluster + fleshed out §M35.5.**
  (1) Renamed "the browser cluster" → **"Userland maturation — a real
  POSIX platform."**  The goal is the *platform capabilities* (each
  milestone independently necessary + valuable — shells, build tools,
  servers, native apps, runtimes); §M42 (browser) recast as the
  **completeness proof / bonus**, explicitly *not* the driver of the
  earlier work.  §M42's "Why" rewritten accordingly (validation target,
  not the goal).  (2) Added an **implementation sketch** to §M35.5:
  on-disk store layout, store-path hash formula, text recipe format, the
  `pkg build/install/remove/rollback/gc/why/list` command surface, RPATH
  sealing (co-design with §M37), the §M33 build sandbox, runtime FS-view
  isolation, mark-sweep GC, bootstrap seed, procfs introspection, and a
  5-step staging order.  No code changed.
- **2026-07-11** — Added **§M35.5 (package manager & isolation)** as a
  hard gate *before* the porting milestones, per the requirement: isolate
  ports, keep the system uncluttered, minimise version coupling.  Answer =
  a **content-addressed store** (Nix/Guix-shaped, explicitly *not*
  dpkg/apt's mutable-global-`/usr` model — convention #6): immutable
  `/store/<hash>-name-version/` paths, pinned dependency closures (many
  versions coexist, no global `/lib` soup), hermetic sandboxed builds
  (§M33 domain), symlink-forest profiles + GC (no cruft, rollback), text
  recipes (anti-blob), two-level isolation (§M37 RPATH at load time +
  §M25/§M33/§M32 FS-view at run time).  Gates §M36–§M42 — every port
  installs into the store, never the global FS.  Updated the cluster
  critical path (`…§M35 → §M35.5 → §M36 …`).  No code changed.
- **2026-07-11** — Added the **browser cluster (§M34–§M42)**, design only,
  answering "after §M24 (network), what is still missing to run
  Firefox/WebKit/Chromium?".  The finding: a browser assumes a whole
  POSIX userland, of which §M24 is ~10 %.  New milestones, each useful on
  its own and with dependencies marked: §M34 POSIX process & signals
  (fork/execve-argv/waitpid/pipes/job-control/signals — the general POSIX
  abstraction layer, needed far beyond the browser), §M35 threads & futex
  (clone/TLS/pthreads/futex on the existing SMP scheduler), §M36 POSIX
  syscall breadth + native libc (musl port), §M37 dynamic linking
  (ld.so/`.so`/dlopen), §M38 C++ runtime + support libs (libc++/unwind,
  zlib, freetype, ICU, harfbuzz, Skia/pixman, sqlite…), §M39 crypto +
  entropy + TLS + DNS (`/dev/urandom`+getrandom, mbedTLS/BoringSSL, CA
  store, getaddrinfo), §M40 client graphics stack (libwayland-client +
  xkbcommon + Mesa `llvmpipe` EGL/GLES + Skia software), §M41 optional
  Linux syscall ABI shim (Linuxulator/WSL1-style binary compat — the
  pragmatic accelerator that can substitute emulation for parts of the
  native ports), and §M42 the browser capstone staged NetSurf (realistic
  first) → WPE-WebKit → Firefox/Chromium (multi-year north star).
  Critical path documented (`§M25→M34→M35→M36→{M37→M38}→M40→M42`, with
  §M24→M39 and §M26→M40 side-branches, §M23 soft for media).  Target arch
  x86_64 (then aarch64); i386 out of scope for a modern browser.  No code
  changed.
- **2026-07-06** — Added §M33 (switchable driver placement): a
  driver-runtime API "narrow waist" with two backends (in-kernel
  direct-call / user-mode IPC), so the *same* driver source runs either
  linked in ring 0 or isolated in a ring-3 process (NetBSD rump-kernel
  model) — a **hybrid kernel**, per-driver, config-driven
  (`driver.<name>.placement`), applied at restart.  Staged: Tier 0
  (fault-tolerant in-kernel hosting, no §M25) → Tier 1 (non-DMA driver in
  user-mode, needs §M25) → IOMMU driver → Tier 2 (DMA-driver isolation,
  the north star).  Captures the "userspace ≠ automatically isolated"
  (kernel-bypass / DMA) and DMA/IOMMU-wall reasoning.  Depends on §M25
  (user-mode backend) + §M29/§M31 (detect + restart); Tier 0 leans only
  on shipped §M27.  No code changed.
- **2026-07-05** — Added §M32 (multi-user): credentials on tasks, a
  `/etc/passwd`-style user DB, login/sessions, file ownership +
  rwx permissions in the VFS, privilege gating (root vs not), and
  per-user process isolation.  Hard-depends on §M25 for *real*
  isolation (ring-0 kthreads share one address space today, so users
  would be advisory until per-process VMM lands); identity + user DB +
  file ownership can precede it.  Leans on §M27 (sessions) + the
  §M22.7 "GUI session" idea (one session per logged-in user).
- **2026-07-04** — §M27 shipped (process model): `struct task` gained
  ppid/exit_code/reap_owned; an always-on **init** task universally
  reaps DEAD non-owned tasks (closes the zombie-leak gap);
  `task_kill_tree()` takes a subtree down cooperatively (GUI window
  close uses it); orphans re-parent to init on reap; ps / /proc/tasks
  grew a PPID column and the Task Manager shows a process tree; pid 0 +
  init are reap-guarded.  Also added `task_spawn_detached()` (parent =
  init, so a spawn can be independent of its caller — the daemon
  pattern, and the substrate for M29 services).  Verified i386 +
  x86_64.  Design follow-ups this surfaced: the *upward* half —
  supervision/wait on child death (§M29) and freeze detection via a
  heartbeat watchdog (new §M31).
- **2026-07-04** — Added the workload-management cluster (design only,
  placeholders): §M27 process model (init/pid 1 + parent-child
  hierarchy + always-on reaper + kill-tree — also closes the current
  "DEAD tasks leak with no Task Manager open" gap), §M28 system log
  (klog ring buffer + levels + dmesg), §M29 services/daemons
  (SERVICE() registry + supervisor with restart policy), §M30 task
  scheduling (cron as the first real service).  Dependency order is
  M27 → M28 → M29 → M30; all independent of the M23/M24/M25 line and a
  good pre-M25 foundation.  A Windows-style registry was explicitly
  parked (§M-registry) as "accidental history" — the /etc + procfs
  model already covers the need.
- **2026-07-04** — §M22.6 shipped (tear-free present + display
  scaling): diagnosed the "picture wiggles on mouse move" report as
  two things — host-side `zoom-to-fit=on` bilinear rescale shimmer
  (fixed 1:1) plus real compositor tearing (direct blit into the live
  scanout).  Killed the tearing with a Bochs-VBE hardware page flip
  (DISPI VIRT_HEIGHT double buffer + Y_OFFSET pan; buffer-age-2
  `dirty_N ∪ prev_dmg` copy from backsurf; graceful fallback).
  Verified on i386 + x86_64.  Corrected M22.4's "not fixable on
  std-VGA" note.  Same session: 1920×1200 desktop (VGA vgamem 32,
  BUDDY_MAX_ORDER 10→12, -m 256M) and terminal-window auto-close on
  hosted-task death (flagged at TASK_DEAD → reused close teardown →
  also leaves the Task Manager list).
- **2026-07-04** — Added §M22.5 (desktop apps): text editor (multiline
  editor widget + clipboard + navigation keys as prerequisites),
  Tiny-BASIC interpreter (kthread contract; interpreter over ring-0
  codegen by design), file manager 2.0 (vfs_rename/copy/recursive
  delete, columns, sorting, file-type association), window
  maximize/restore.  DoD is one connected story: write BASIC in the
  editor, run from the file manager, output in a window.
- **2026-07-04** — Added §M22.4 (compositor smoothness): diagnosed
  the drag "swimming" + cursor ghosting — (1) compose() snapshots
  damage before the cursor position, so fast pointer motion clips
  the cursor out of its own frame; (2) DRAG_MOVE raises full-screen
  damage per motion event; (3) no vblank on QEMU std-VGA.  Fix plan
  recorded, not yet implemented.
- **2026-07-04** — §M22.3 shipped: task manager + cooperative
  task_kill (kthread contract) + cpu_ms accounting + terminal-window
  close (vc_destroy) + minimize + Alt-Tab + dirty-rect composition.
  Section condensed; lessons learned recorded.
- **2026-07-04** — §S.1 shipped: SHELL_PROVIDER() registry closes the
  provider half of §S (the M14 checkmark had overstated it) — boot /
  pane / GUI-window shells resolve via the `shell.provider` config
  key; rescue_shell.c is the swap proof.
- **2026-07-04** — §M22.2 shipped: GUI_APP() + DESKTOP_SHELL()
  registries, shell_vista/shell_bare, apps under kernel/gui/apps/,
  `launch` command, DOCS §4.14 dev guide.  Section condensed.
- **2026-07-04** — Modularity review of the GUI: gfx/widget/fileman
  are clean layers, but the desktop chrome + app launching are welded
  into gui.c (hardcoded menu enum, extern fileman_open) — violates
  north-star #2/#5.  Added §M22.2 (swappable desktop-shell interface
  + GUI_APP registry + GUI dev docs) and §M22.3 (desktop polish:
  task manager + task_kill + per-task CPU accounting, terminal-window
  close + vc_destroy, minimize, Alt-Tab, damage rects — the M22.1
  deferred list promoted to a milestone).  No code changed.
- **2026-07-04** — §M22 stage 6 closed (M22.1): widget toolkit,
  Vista-shaped taskbar (Start menu / window buttons / RTC clock),
  file manager, content-preserving resize, `vfs_unlink`, 1280×800.
  See DOCS.md §4.13 + change log.
- **2026-07-03** — §M22 shipped (compositor, window manager, terminal
  windows, PS/2 mouse; widget toolkit deferred).  Section condensed
  to a pointer at DOCS.md §4.13 + lessons learned.
- **2026-07-03** — Wayland path split out.  M22's Wayland-reuse
  evaluation ran: the wire protocol is cheap, but libwayland-server
  and any upstream client hard-depend on a POSIX substrate d-os
  lacks (per-process address spaces, fd table, unix sockets with
  fd passing, mmap, ELF loader, libc).  Decision per §M22's rule:
  M22 ships a custom in-kernel protocol with Wayland-shaped objects
  (surface + buffer + commit + seat).  Added §M25 (userland
  foundation = the prerequisites) and §M26 (Wayland server proper,
  on top of M22 + M25).  No code changed.
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
