# d-os — developer documentation

A living document. Every milestone updates this file before being declared
complete. If something here contradicts the code, the code is authoritative and
the doc needs fixing.

> **Navigation tip for assistants:** this file is ~850 lines.  Don't
> read it all to "orient" — use the TOC below with `Read offset/limit`
> to land in the relevant component.  CLAUDE.md has the high-level
> state; come here for component details.

## Table of contents

(Approximate line numbers; refresh with `grep -n '^##\|^###' DOCS.md`
when sections are added.)

| § | Section | ~line |
|---|---------|------:|
| 1 | Project layout | 51 |
| 2 | Boot flow | 115 |
| 3 | Memory layout | 132 |
| 4 | Components | 154 |
| 4.0 | Module framework + console registry | 156 |
| 4.1 | Terminal drivers (FB + VGA) | 222 |
| 4.2 | Keyboard (PS/2 IRQ-driven) | 245 |
| 4.3 | Shell | 258 |
| 4.4 | HAL x86 (io.c) | 294 |
| 4.5 | GDT | 304 |
| 4.6 | IDT + PIC + IRQ dispatch | 324 |
| 4.7 | Multiboot info | 348 |
| 4.8 | Physical Memory Manager (M19: buddy + zones) | 378 |
| 4.9 | Virtual Memory Manager | ~440 |
| 4.X | Tasks + scheduler (M13: preemption) | 422 |
| 4.X | SMP — APIC, AP boot, per-CPU, real spinlocks (M18) | ~492 |
| 4.X | HAL — arch-independent interface (M17) | ~580 |
| 4.X | Keyboard layouts (M16) | ~665 |
| 4.X | USB host stack — xHCI + HID (M15) | ~750 |
| 4.X | Virtual consoles / pane split (M14) | ~840 |
| 4.X | Ring 3 / user mode | ~925 |
| 4.X | Supported architectures — i386 + x86_64 (M20) | ~1365 |
| 4.X | Block layer + virtio-blk (M11) | 490 |
| 4.X | procfs | 542 |
| 4.X | devfs | 573 |
| 4.X | Configuration store | 596 |
| 4.X | Filesystem layer (VFS + ramfs) | 612 |
| 4.X | Block cache (M12) | ~700 |
| 4.X | exFAT (M12) | ~730 |
| 4.X | Timer (PIT) | 646 |
| 4.10 | Kernel heap (M19: slab + per-CPU mag + page_alloc) | ~1255 |
| 4.11 | Serial debug (COM1) | 687 |
| 4.12 | ACPI (shutdown) | 701 |
| 4.13 | GUI — compositor, WM, widgets, apps (M22 – M22.5) | ~1532 |
| 4.14 | GUI development — apps, desktop shells (M22.2+) | ~1752 |
| 5 | Build & run | 722 |
| 6 | Compiler flags | 735 |
| 7 | Roadmap | 751 |
| 8 | Change log | 766 |

---

## 1. Project layout

```
d-os/
├── Dockerfile                 # Ubuntu 22.04 + cross-tools (amd64 forced)
├── Makefile                   # build glue: compile, link, iso (ARCH=i386|x86_64)
├── linker-i386.ld             # i386 link script (ELF32, ENTRY=_start, load at 1 MiB)
├── linker-x86_64.ld           # x86_64 link script (ELF64, same load addr)
├── boot/grub/grub.cfg         # GRUB menu entry — i386 multiboot1
├── boot/grub/grub-x86_64.cfg  # GRUB menu entry — x86_64 multiboot2
├── scripts/
│   ├── build.sh               # docker build + make iso
│   └── run_qemu.sh            # prefers host qemu, falls back to docker
└── kernel/
    ├── core/                  # architecture-independent kernel logic
    │   ├── kernel.c           # kernel_main() entry point
    │   ├── shell.c            # interactive REPL
    │   ├── printf.c           # kprintf() — minimal formatter
    │   ├── multiboot.c        # multiboot info validation + mmap walker
    │   ├── module.c           # MODULE() registry + init iteration
    │   ├── driver.c           # DRIVER() registry (probe/init/shutdown)
    │   ├── block.c            # block_device registry (vda, sda, ...)
    │   ├── console.c          # output sink registry (broadcast)
    │   ├── config.c           # key/value store, persisted via VFS
    │   ├── syscall.c          # int 0x80 dispatcher
    │   └── task.c             # kernel-task scheduler (cooperative)
    ├── drivers/               # hardware drivers
    │   ├── terminal/
    │   │   ├── terminal.c             # runtime dispatcher over backends
    │   │   ├── fb_terminal.c          # linear framebuffer + 8x8 font
    │   │   └── vga_terminal.c         # legacy VGA text fallback
    │   ├── keyboard/ps2_keyboard.c    # PS/2 IRQ-driven input
    │   ├── serial/serial.c            # COM1 debug output
    │   ├── timer/pit.c                # 8254 PIT @ 1000 Hz
    │   ├── null/null.c                # /dev/null + /dev/zero
    │   └── block/virtio_blk.c         # virtio-blk (legacy I/O port transport)
    ├── acpi/                  # ACPI table walker + soft-off
    │   └── acpi.c
    ├── mem/                   # memory management
    │   ├── pmm.c              # physical memory manager (bitmap)
    │   ├── vmm.c              # virtual memory manager, page tables
    │   └── kmalloc.c          # kernel heap allocator (block free-list)
    ├── fs/                    # filesystems
    │   ├── vfs.c              # VFS core: registry, mount, path walk
    │   ├── ramfs.c            # in-memory filesystem
    │   ├── devfs.c            # /dev synthetic files for drivers
    │   └── procfs.c           # /proc synthetic files for kernel state
    ├── hal/                   # arch-specific primitives
    │   └── x86/               # i386 implementation
    │       ├── boot.s         # multiboot header + _start stub
    │       ├── gdt.c          # Global Descriptor Table (kernel + user + TSS)
    │       ├── idt.c          # IDT build + PIC remap + C dispatcher
    │       ├── isr_stubs.s    # 48 per-vector asm stubs + 0x80 syscall stub
    │       ├── tss.c          # Task State Segment (esp0 for ring transitions)
    │       ├── usermode.s     # enter_user_mode_wrap (iret to ring 3 + return)
    │       ├── switch.s       # context_switch (kernel-task swap)
    │       ├── pci.c          # PCI config-space access via 0xCF8/0xCFC
    │       └── io.c           # inb/outb/outw/inw/inl/outl, shutdown, reboot
    └── includes/              # public headers for every module above
```

The `kernel/hal/arm/` and `kernel/hal/x64/` directories exist as placeholders
for eventual other architectures; they are empty today.

---

## 2. Boot flow

1. **GRUB** — BIOS loads GRUB from the ISO. GRUB finds the multiboot1 header
   in `kernel.bin` (signature `0x1BADB002`), loads the kernel at physical
   address `1 MiB`, enters 32-bit protected mode with paging off and a flat
   GDT, then jumps to `_start`.
2. **`_start`** (`kernel/hal/x86/boot.s`) — sets up a 16 KiB stack in `.bss`,
   pushes `ebx` (multiboot info pointer) and `eax` (multiboot magic
   `0x2BADB002`) as arguments, calls `kernel_main`.
3. **`kernel_main`** (`kernel/core/kernel.c`) — initializes the terminal,
   installs our own GDT (replacing GRUB's), builds the IDT and remaps the
   PIC (`idt_init`), registers the keyboard IRQ (`keyboard_init`), prints
   the banner, runs `acpi_init()` to discover ACPI tables, `sti`'s to
   unmask CPU interrupts, then enters `shell_run()`.

---

## 3. Memory layout

```
Physical                 Purpose
0x00000000 – 0x000003FF  real-mode IVT (untouched; we're in pmode)
0x00000400 – 0x000004FF  BIOS data area; [0x40E] = EBDA segment (ACPI RSDP search)
0x0009FC00 – 0x0009FFFF  Extended BIOS Data Area (EBDA); ACPI RSDP may live here
0x000A0000 – 0x000BFFFF  VGA framebuffer (text mode uses 0xB8000)
0x000C0000 – 0x000DFFFF  Video ROM / option ROMs
0x000E0000 – 0x000FFFFF  System BIOS; ACPI RSDP may also live here
0x00100000 – kernel_end  kernel image (linker puts .multiboot first, then .text, ...)
kernel_end  – ...        free physical memory managed by the PMM
```

After `vmm_init` paging is enabled.  The first 256 MiB of the virtual
address space is identity-mapped via 4 MiB PSE PDEs (virt == phys), so
every pointer valid before paging is still valid after.  The VMM can
install finer-grained 4 KiB mappings at virtual addresses ≥ 256 MiB by
allocating a new page table from the PMM.

---

## 4. Components

### 4.0 Module framework + console registry

Two pieces of infrastructure replace the old hand-written init in
`kernel_main`:

**Two-tier driver framework — `MODULE()` (legacy) + `DRIVER()` (new)**

`MODULE()` exists from M2 and works for monolithic init.  `DRIVER()`
adds a richer lifecycle — probe (cheap presence check) → init (do
the work) → shutdown (clean stop) — and class metadata so future
devfs/procfs can iterate without per-class plumbing.  Both registries
coexist; existing drivers stay on MODULE() until there's a reason
to migrate.

**DRIVER() registry (`kernel/core/driver.c`, `kernel/includes/driver.h`)**
- `struct driver { name, class, ops, ctx }` — 16 bytes on i386,
  `aligned(4)` matches `sizeof` so iteration stride is correct.
- `struct driver_ops { probe, init, shutdown }` — any may be NULL
  (NULL probe = always present, NULL shutdown = no cleanup).
- `DRIVER(name, class, ops_ptr, ctx_ptr)` macro drops the entry into
  the `drivers` linker section.
- `driver_init_all()` (called from kernel_main after module_init_all
  + kmalloc) walks the section, runs probe → init, tracks per-driver
  state in a parallel `kcalloc`'d byte array.
- `driver_list()` ↔ `lsdrv` shell command shows the registry.
- First user: `kernel/drivers/null/null.c` — placeholder for
  `/dev/null` once devfs (M9) lands.

**Module registry (`kernel/core/module.c`, `kernel/includes/module.h`)**
- Each driver registers a `struct module_def {name, class, init}` via
  the `MODULE("name", "class", init_fn)` macro at file scope.
- The macro places the def into a `modules` linker section.
- `linker.ld` exposes `__start_modules` / `__stop_modules`.
- `module_init_all()` (called from `kernel_main` once after the heap
  is up) walks the array and invokes every init function.
- **Critical detail:** entries use `aligned(4)`, NOT `aligned(8)`.
  `sizeof(struct module_def) == 12` on 32-bit; an 8-byte alignment
  would round each entry to 16 bytes in the section while iteration
  walks with stride 12 → unaligned reads → page fault.  Keep them
  matched.
- Init order is link order (i.e. order in `Makefile` C_SRCS).
- `lsmod` shell command lists registered modules with their class.
- Adding a new driver requires NO change to `kernel_main`.

**Console sink registry (`kernel/core/console.c`, `kernel/includes/console.h`)**
- `struct console_sink {name, category, putchar, clear, active, next}`
  forms an intrusive linked list of registered output sinks.
- Drivers call `console_sink_register(&my_sink)` from their module
  init; flip `active` based on probe results.
- `console_putchar(c)` (called from `kprintf`) broadcasts each byte to
  every active sink — that's how a single kprintf reaches both the
  framebuffer AND the serial debug port.
- `category` enables mutually-exclusive sinks: VGA's module init
  checks `console_sink_any_active("screen")` before activating, so it
  yields to FB if FB came up first.
- **M14 per-task routing:** `console_set_per_task_emit(fn)` installs
  an opaque hook called from `console_putchar` with the running task's
  `out_console` pointer.  vc_init wires this to `vc_putchar` so each
  shell task's output lands inside its own pane; the legacy fb sink is
  deactivated by `fb_sink_disable()` at the same time so kprintf no
  longer wanders across pane boundaries.  Serial sinks stay active for
  the full debug log.
- `lsconsole` shell command prints the current sink table.

**Existing driver classes registered today:**

| Class       | Modules                                |
|-------------|----------------------------------------|
| `console`   | com1-serial, vesa-fb, vga-text         |
| `input`     | ps2-keyboard                           |



### 4.1 Terminal drivers (`kernel/drivers/terminal/`)
Two drivers, both registered as `console` modules and both providing
`screen`-category console sinks (mutually exclusive — only one is
active at a time, FB preferred):

**Framebuffer backend (`fb_terminal.c`):**
- On init, reads `framebuffer_*` from the cached multiboot info.
- Maps the physical FB window with one or more 4 MiB PSE mappings via
  `vmm_map_4mib` (so no page tables are spent on it).
- Renders an embedded 8×8 bitmap font (derived from the public-domain
  IBM PC CGA ROM font) for ASCII 0x20..0x7E.  `0x7F` is a solid block
  used as the fallback glyph for out-of-range bytes.
- Tracks `(cur_row, cur_col)` in character cells.  1024×768 ÷ 8×8
  gives a **128×96** grid — about 5× the rows of classic 80×25.
- `\n`, `\r`, `\b` handled inside `fb_term_putchar`; scrolling copies
  (fb_height − glyph_h) pixel rows up and clears the new last band.
- Colors: `FG = 0xE0E0E0`, `BG = 0x101828` (packed 0xAARRGGBB).

**VGA backend (`vga_terminal.c`):**
- Same structure as the original VGA driver — 80×25 cells at 0xB8000.
- Only reached if the multiboot loader ignored our video request; under
  QEMU + GRUB this path is currently dead.

### 4.2 Keyboard (`kernel/drivers/keyboard/ps2_keyboard.c`)
- **Hardware:** Intel 8042 PS/2 controller. Data port `0x60`, status port
  `0x64`. Bit 0 of status = output buffer full.
- **Mode:** IRQ-driven. On init the driver drains any stale byte the
  controller may have queued, then registers `keyboard_irq` on IRQ line 1
  (vector 33 after the PIC remap).  Decoding (scancode set 1, US layout,
  left+right shift) runs inside the ISR, which pushes ASCII into a 64-byte
  ring buffer.
- **`keyboard_getchar`:** consumer side of the ring.  When the buffer is
  empty it `sti; hlt`s, letting the CPU sleep until the next IRQ wakes it.
- **Limitations:** no Ctrl / Alt / CapsLock / key-repeat handling, extended
  `0xE0`-prefixed codes (arrows etc.) are still dropped.

### 4.3 Shell (`kernel/core/shell.c`)
- **Loop:** write prompt, read a line, dispatch.
- **Line editor:** 128-byte buffer, backspace-aware (updates buffer and
  screen together).
- **Built-in commands:**

| command           | action                                         |
|-------------------|------------------------------------------------|
| `help`            | list commands                                  |
| `clear`           | clear screen                                   |
| `about`           | banner                                         |
| `echo <text>`     | print the arg                                  |
| `meminfo`         | dump mmap + PMM stats + paging state + heap    |
| `lsmod`           | list registered driver modules                 |
| `lsdrv`           | list DRIVER() registry with probe/init state   |
| `lsconsole`       | list registered console sinks + active state   |
| `uptime`          | h:mm:ss.mmm since boot, fed by PIT 1 kHz tick  |
| `ls [path]`       | list directory entries                         |
| `cat <path>`      | print file contents                            |
| `touch <path>`    | create empty file                              |
| `mkdir <path>`    | create directory                               |
| `write <path> <text>` | write `text` to file (created if needed)   |
| `config`          | dump the entire config cache                   |
| `getconf <key>`   | print one config value                         |
| `setconf <key> <value>` | set or replace a config value            |
| `saveconf`        | persist config to /etc/d-os.conf               |
| `ringtest`        | M6 demo: drop to ring 3, syscall, return       |
| `lsblk`           | list registered block devices                  |
| `blktest`         | M11 demo: write pattern to /dev/vda sector 1,  |
|                   | read back, verify                              |
| `ps`              | list tasks with pid + state                    |
| `spawn`           | create a kernel ticker task (M7 demo)          |
| `yield`           | manually yield CPU to next runnable task       |
| `shutdown`        | ACPI S5 soft-off (or QEMU/Bochs fallback)      |
| `reboot`          | 8042 CPU reset pulse (+ ICH 0xCF9 fallback)    |

### 4.4 HAL x86 (`kernel/hal/x86/io.c`)
- **Port I/O:** `inb`, `outb`, `outw`. Thin `__asm__ volatile` wrappers.
- **`hal_shutdown()`:** try ACPI first (`acpi_shutdown()`); if that
  returns (tables missing or `_S5_` unparseable), fall back to known
  emulator ports: QEMU `0x604`/`0xB004` write `0x2000`, Bochs `0x8900`
  writes `"Shutdown"`. Last resort: `cli; hlt` forever.
- **`hal_reboot()`:** wait for 8042 input buffer empty, then write `0xFE`
  to port `0x64` to pulse the CPU reset line. Fallback: `outb 0xCF9, 0x06`
  (ICH fast reset). On truly dead hardware: `cli; hlt`.

### 4.5 GDT (`kernel/hal/x86/gdt.c`)
- **Purpose:** replace GRUB's inherited GDT with one we own and can
  extend later (user segments, TSS).
- **Current entries:**

| Index | Selector | Base | Limit | Access | Flags | Meaning         |
|------:|---------:|-----:|------:|-------:|------:|-----------------|
|   0   |   0x00   |   0  |   0   |  0x00  |  0x0  | mandatory null  |
|   1   |   0x08   |   0  | 4 GiB |  0x9A  |  0xC  | kernel code, ring 0 |
|   2   |   0x10   |   0  | 4 GiB |  0x92  |  0xC  | kernel data, ring 0 |

- **Flags nibble `0xC`:** G=1 (4 KiB granularity), D/B=1 (32-bit), L=0, AVL=0.
- **Access bytes:** P=1, DPL=0, S=1, Type=`1010` for code / `0010` for data.
- **Load sequence:** `lgdt` the pointer, reload DS/ES/FS/GS/SS with the new
  data selector, then far-jump to a local label with the new code selector
  to reload CS.
- **Planned extensions (future milestones):** user code / user data
  descriptors (DPL=3), a TSS for ring-3 → ring-0 transitions and kernel
  stack swaps on interrupt entry.

### 4.6 IDT + PIC + IRQ dispatch (`kernel/hal/x86/idt.c`, `isr_stubs.s`)
- **IDT:** 256 gate descriptors.  Vectors 0..47 point at per-vector asm
  stubs in `isr_stubs.s`; every remaining vector has P=0 so an unexpected
  interrupt raises a #NP we can see rather than silently running a stale
  address.
- **Gate format:** `0x8E` = present, DPL=0, 32-bit Interrupt Gate, kernel
  code selector.  A future software-interrupt (syscall) vector will get
  DPL=3 so ring-3 code can invoke it.
- **Asm stubs:** one entry per vector.  Each stub pushes a dummy error
  code (unless the CPU already pushed a real one for vectors 8/10–14/17),
  pushes its vector number, and jumps to `isr_common`.  The common
  sequence does `pusha`, saves the data segment registers, loads kernel
  data selectors, calls the C `isr_handler`, restores state, and `iret`s.
- **PIC remap:** legacy 8259 master + slave reprogrammed so IRQ N arrives
  on vector `32 + N` (0x20..0x2F), which avoids the exception overlap.
  `irq_install(n, handler)` stores the handler and unmasks the line; the
  common dispatcher issues EOI after the handler returns.
- **Dispatch rules in `isr_handler`:**
  - `int_no < 32` → exception.  Log name + cs:eip + err_code and
    `cli; hlt` forever.  (Recovery path is a later milestone.)
  - `32 ≤ int_no < 48` → IRQ.  Call `irq_handlers[int_no - 32]`, then EOI.
  - Anything else → unexpected, log and continue.
- **Current IRQ handlers:** only IRQ1 (keyboard).

### 4.7 Multiboot info (`kernel/core/multiboot.c`)
- **Input:** the 32-bit physical pointer GRUB passes in `%ebx` on entry
  (see Multiboot Specification §3.3).  `mboot_init(magic, ptr)` validates
  the loader magic (`0x2BADB002`) and caches the pointer; later callers
  read the fields lazily.
- **Flag bits that matter today:** bit 0 (`mem_lower`/`mem_upper`), bit 6
  (`mmap_addr`/`mmap_length`), bit 11 (VBE info), bit 12 (framebuffer).
- **Memory map iteration quirk:** each entry starts with a `size` field
  that excludes itself, so to advance to the next entry add `size + 4`
  rather than the struct size.  Types 1..5: AVAILABLE, RESERVED,
  ACPI RECLAIM, ACPI NVS, BAD RAM.
- **`meminfo` shell command:** prints mem_lower/upper and every mmap
  entry, plus a total of available RAM in MiB.
- **Planned users:** the PMM will build its bitmap from the AVAILABLE
  regions; the VBE milestone will read the framebuffer fields.

### 4.8 Physical Memory Manager (`kernel/mem/pmm.c`) — **buddy allocator (M19)**
- **Granularity:** 4 KiB page frame.  `PMM_FRAME_SIZE` is the single
  source of truth.
- **Algorithm:** classic binary buddy with per-zone free lists.  Each
  zone has `BUDDY_MAX_ORDER + 1` (= 11) free lists; order 0 holds
  single frames, order 10 holds 4 MiB blocks.  The free-list link is
  stored inside the free page itself — first 4 bytes hold the physical
  address of the next free block at the same order, terminated by 0.
  No external linked-list metadata; the only side table is `page_state[]`
  (1 byte per frame, 256 KiB for the 1 GiB BUDDY_MAX_FRAMES cap).
- **Page state encoding** (`page_state[pfn]`):
  - `0xFF`: frame doesn't exist (BIOS-reserved, beyond memory, or
    explicitly carved out).
  - `0xFE`: frame allocated (head or interior of any block).
  - `0..10`: frame is the HEAD of a free buddy block at that order.
    Interior frames of free blocks read as `0xFE` plus list
    membership; only the head is reachable from the free list.
- **Zones:**
  - `ZONE_DMA`: `pfn < 4096` (first 16 MiB) — legacy ISA / small DMA.
  - `ZONE_NORMAL`: `pfn ∈ [4096, BUDDY_MAX_FRAMES)` — bulk of RAM.
  - `ZONE_HIGHMEM`: declared, not populated.  Reserved for the
    eventual extension when we map memory beyond the 256 MiB identity
    region.
  - Coalesce refuses to merge across a zone boundary (a DMA buddy
    never pairs with a NORMAL block).
- **API:**
  - `page_alloc(order, zone_hint)` — order-aware.  `ZONE_DEFAULT` tries
    NORMAL → DMA; explicit `ZONE_DMA` returns DMA-only; explicit
    `ZONE_NORMAL` returns NORMAL-only.
  - `page_free(addr, order)` — coalesces with the buddy at the same
    order, up to `BUDDY_MAX_ORDER`.
  - Legacy wrappers `pmm_alloc_frame` / `pmm_alloc_contiguous(n)` /
    `pmm_free_frame` are kept stable so existing drivers (xhci,
    virtio-blk, ramfs, block_cache) compile unchanged.
- **Init sequence:**
  1. Zero `page_state[]` to all `0xFF` (nothing exists).
  2. Walk the multiboot mmap; for each AVAILABLE frame, tag it
     `PS_USED` (= "allocated, not in any free list yet").
  3. Re-carve protected regions (frame 0, below 1 MiB, kernel image,
     multiboot info + mmap, AP trampoline window).  These overwrite
     `PS_USED` with `PS_NONE` so they never enter a free list.
  4. Walk frames one more time: every `PS_USED` frame is released
     into its zone at order 0.  The standard coalesce path
     automatically builds up max-order blocks where contiguous free
     ranges exist.  No two-phase "find longest aligned run" — same
     end state, much less code.
- **Concurrency:** one spinlock per zone (M18 cmpxchg + IRQ-save).
  All alloc / free paths are IRQ-safe.  Cross-zone allocations don't
  serialize against each other.
- **Stats:** `meminfo` prints a one-liner; `buddyinfo` shows free-block
  counts per (zone × order); `pmm_print_stats` is the underlying
  formatter.  Sample after boot on `-m 256M`:
  - DMA: 8 free blocks (mostly small orders + one order-10).
  - NORMAL: 59 order-10 blocks (= 236 MiB in 4 MiB chunks).
- **Lesson learned:** linear bitmap scan was O(N/32) but cache-hot
  (128 KiB bitmap).  Buddy is O(log N) per call but has a larger
  metadata footprint (256 KiB `page_state[]`).  The microbench shows
  10000 × 64-byte kmalloc round-trips in 0–9 ms across builds, well
  under the slab+buddy combined budget.

### 4.9 Virtual Memory Manager (`kernel/mem/vmm.c`)
- **Mode:** 32-bit protected mode, 4 KiB and 4 MiB (PSE) pages.
  CR4.PSE enabled before CR0.PG.
- **Initial state:** identity map covering the first 256 MiB of physical
  memory via 4 MiB PSE PDEs (entries 0..63 in `kernel_pd`).  No
  translation happens for those addresses, so pointers keep working
  across the paging-on transition.
- **`vmm_map(virt, phys, flags)`:** installs a 4 KiB mapping using
  conventional two-level paging.  If the relevant PDE is absent, a new
  page table is taken from the PMM, zeroed, and installed.  Refuses
  when the PDE is a 4 MiB PSE entry — i.e. the caller must stay outside
  the initial identity region (virt ≥ 256 MiB).
- **`vmm_unmap`:** clears the PTE and invalidates that single TLB entry
  with `invlpg`.  Leaves the parent PT allocated; reclamation is a
  future optimization.
- **`vmm_translate`:** walks PD → (PSE or PT) → physical address for
  diagnostics.
- **PDE/PTE flag bits (matching hardware layout):**
  - `PDE_P` / `PTE_P` (0x001): Present
  - `PDE_RW` / `PTE_RW` (0x002): writable
  - `PDE_US` / `PTE_US` (0x004): user accessible
  - `PDE_PS` (0x080): PDE is a 4 MiB page, not a pointer to a PT
- **Self-test at boot:** `kernel_main` allocates a frame, maps it at
  virtual 0xE0000000, writes `0xDEADBEEF`, reads it back, then unmaps
  and frees.  Any failure in the PD/PT install path shows up as either
  a triple fault or a mismatch line in the boot log.

### 4.X Tasks + scheduler (`kernel/core/task.c`, `kernel/hal/x86/switch.s`, `kernel/core/lock.c`)
- **Model:** kernel-mode tasks only (each shares the kernel page
  directory).  When per-process address spaces land, `struct task`
  gains a `vmm_space*`.
- **`struct task`:** name (≤31), pid, state (RUNNABLE/SLEEPING/DEAD),
  saved esp, kmalloc'd stack base, run-queue link.
- **Run-queue:** intrusive circular singly-linked list rooted at
  `current`.  Insertion is "right after current" (FIFO-ish locally).
- **Bootstrap:** `task_init` synthesizes pid 0 ("kernel") from the
  running `kernel_main` context — no separate stack allocated; its
  esp gets populated by the very first `context_switch` away from it.
- **Spawn:** `task_spawn(name, entry)` allocates a 4 KiB kstack and
  pre-builds it so `context_switch`'s pop+ret lands at a trampoline
  that `sti`s and calls `entry` then `task_exit`.  The user's `entry`
  is carried through ebx; the trampoline reads it and calls.
- **Yield (cooperative):** `task_yield` is now a thin wrapper around
  `schedule()` — the same routine the IRQ-driven path uses.  Walks
  the ring for the next RUNNABLE task and calls
  `context_switch(&prev->esp, next->esp)` (switch.s).
- **Preemption (M13):**
  - PIT IRQ (1 kHz) increments a per-tick counter; on every quantum
    boundary (`SCHED_QUANTUM_TICKS = 50`, i.e. 50 ms) it calls
    `schedule_request()` which sets a deferred `need_resched` flag.
  - The IDT's `isr_handler` calls `schedule_check()` AFTER `pic_eoi`.
    If `need_resched` is set and `preempt_count() == 0`, that runs
    `schedule()` from IRQ context — which context-switches to whatever
    the run-queue picks next.
  - Why deferred (flag + check) rather than switching directly from
    `pit_irq`: if we pivoted to a different task mid-handler, `pic_eoi`
    would never fire on IRQ0 for the outgoing task, and the PIC would
    consider the line still in-service and stop delivering further
    timer ticks.  The flag lets the EOI complete on the old stack
    first, then the rescheduling happens.
  - Brand-new tasks have never been through `schedule()`, so the
    trampoline explicitly `sti`s before calling the entry — otherwise
    a freshly-spawned task would inherit IF=0 and could never be
    preempted.
- **Locking primitives (`kernel/includes/lock.h`):**
  - `spinlock_t` with `spin_lock_irqsave` / `spin_unlock_irqrestore` —
    UP-stub today (cli+saved-EFLAGS is the real synchronization), but
    the API is the one the SMP cut will keep.  The scheduler itself
    does not use spinlocks — UP's cli/sti is enough and is cleaner
    than the lock-handoff pattern across `context_switch`; `spinlock_t`
    exists for other subsystems that need the API shape.
  - `preempt_disable()` / `preempt_enable()` — reentrant counter.
    `schedule_check()` skips its work if `preempt_count() > 0`, so
    short kernel critical sections that don't actually need IRQs off
    can ban context switches cheaply without masking interrupts.
- **Exit:** `task_exit` flips state to DEAD and calls `schedule_locked()`
  forever.  Stack reclamation TODO (a janitor task at idle is the
  cleanest follow-up).
- **Boot self-test:** spawns a `for(;;) counter++` hog and sleeps the
  kernel thread on `hlt` for 500 ms.  Under cooperative scheduling
  this would freeze; under preemption the PIT IRQ pulls the kernel
  back every quantum.  Reports PASS with the hog's counter value to
  prove both sides got CPU.
- **`loop` shell command:** spawns the same kind of CPU hog at the
  prompt — the user-facing version of the boot self-test.  With
  cooperative scheduling the shell would lock up; under M13
  preemption the prompt stays responsive.

### 4.X SMP — APIC, AP boot, per-CPU, real spinlocks (M18)

The single-CPU UP build became a real multiprocessor.  Boot order:

1. **ACPI MADT** parsed alongside the FADT in `acpi_init` — exports
   `acpi_lapic_phys()`, `acpi_ncpus()`, `acpi_cpu_apic_id(i)`,
   `acpi_ioapic_phys()`, `acpi_irq_override(isa_irq)`.
2. **LAPIC** brought up on BSP (`kernel/hal/x86/lapic.c`) — MMIO
   mapped cache-disabled, SIVR.APIC_EN set, LVT lines masked, IDT
   vector layout unchanged so the same `isr_common` stubs work.
3. **IOAPIC** programmed (`kernel/hal/x86/ioapic.c`) — every
   redirection entry starts masked; `ioapic_route_isa` programs a
   single vector with the BSP's APIC ID, honoring ACPI ISO
   overrides (very common: IRQ0 → GSI 2 on QEMU).
4. **8259 PIC disabled** in `idt_use_apic` — both halves masked,
   `irq_install` re-routes already-installed handlers (PIT, PS/2)
   via the IOAPIC, EOIs go to LAPIC instead of PIC.
5. **Per-CPU table** (`kernel/core/percpu.c`) — array of `struct
   percpu` indexed 0..ncpus-1; sparse LAPIC ID → dense slot map via
   `apic_to_index[256]`.  `this_cpu_id()` reads LAPIC ID and
   looks up; constant-time, two MMIO accesses.
6. **AP bring-up** (`kernel/hal/x86/smp.c` + `ap_trampoline.s`) —
   16-bit real-mode trampoline assembled as flat binary, linked
   into the kernel via `objcopy --input-target=binary`, copied to
   physical 0x8000 at runtime.  INIT + SIPI + SIPI sequence per
   Intel SDM Vol 3 §8.4; each AP runs `ap_main` which calls
   `lapic_init_ap` + `percpu_init_ap`, kprintf's its arrival, then
   enters `for(;;) hal_cpu_idle();`.

**Real spinlocks** (`kernel/core/lock.c`) — `cmpxchg`-based
test-and-set with `hal_cpu_pause` backoff; replaces the M13 UP-stub.
Acquire-release memory ordering via `atomic_store_release` /
`atomic_load_acquire`.  IRQs-off-on-this-CPU comes first so we
can't preempt ourselves mid-critical-section.

**Lock-handoff trick** (`task_finish_first_switch` in task.c +
`task_arch.c` trampoline) — when `schedule()` switches into a
brand-new task, the lock acquired by the spawning schedule was
never released.  An established task's schedule pairs its own
acquire with its own release; a brand-new task has no schedule
frame on its stack.  The trampoline calls `task_finish_first_switch`
which drops the runqueue lock, then `sti`s and calls the entry.

**Per-CPU `current` task** — `task->esp` is now per-CPU via
`this_cpu()->current`.  `schedule_locked` walks the global runqueue
skipping tasks that other CPUs already have scheduled
(`task_running_elsewhere`); single-CPU is the trivial no-skip path.
Per-CPU runqueues + a load-balancer is a §M19 follow-up; the
global queue + spinlock is fine until contention shows up.

**Shell command:** `lscpu` — lists every percpu slot with APIC ID
and online state.  Marks `<this>` on the slot the calling shell
task is running on.

**Verified on QEMU `-smp 4`:**
```
ACPI: MADT — 4 CPU(s), lapic=0xfee00000 ioapic=0xfec00000
lapic: BSP enabled at 0xfee00000 (id=0)
ioapic: 24 entries at 0xfec00000, gsi_base=0
apic: routing live (bsp_apic_id=0), 8259 disabled
percpu: 4 CPUs known, BSP at slot 0 (apic_id=0)
ap: cpu 1 (apic_id=1) online
ap: cpu 2 (apic_id=2) online
ap: cpu 3 (apic_id=3) online
smp: 3 AP(s) started (of 4 total CPU(s))
preempt self-test: PASS — kernel ran while hog tight-looped (hog ticks=...)
```

And `lscpu` from the shell:
```
CPU  APIC_ID  STATE
0    0        online <this>
1    1        online
2    2        online
3    3        online
```

**M18.5 — APs actually scheduling (closed):**

- **LAPIC timer per-CPU** (`lapic_timer_calibrate / _start_periodic
  / _stop` in `kernel/hal/x86/lapic.c`).  BSP calibrates against PIT
  once during init (typical QEMU result: ~78000 ticks/ms, count ~780k
  for 100 Hz with divide-by-16).  Every CPU programs its own LAPIC
  with the calibrated count — they all run at the same rate without
  re-calibrating per core.
- **IDT vector 0x40** added (`isr64` stub + `set_gate` in `idt.c`)
  for the LAPIC timer.  `isr_handler` dispatches it: `schedule_request`
  (sets the M13 deferred-reschedule flag), `lapic_eoi`, `schedule_check`
  (consumes the flag and may context-switch).  Vector 0x41 reserved
  for a future cross-CPU preempt IPI (stub only today).
- **`idt_load`** exposed so every AP can `lidt` the shared IDT data
  structure on its own CPU (IDTR is per-CPU even though the table is
  one in memory).
- **AP-side idle task** (`task_install_ap_idle` in `task.c`).  Each
  AP synthesizes a `struct task` for its current ap_main context,
  splices it into the global ring with `is_idle = 1`, and stamps
  this CPU's `current` + `idle` pointers.  Reuses the existing
  `kstack_base = NULL` trick from BSP pid 0.
- **BSP idle task** synthesized at `task_init` time (separate from
  kernel_main pid 0).  Without this, if kernel_main eventually
  `task_exit`s, BSP would have no fallback and halt forever — and
  that halts PIT delivery, which freezes `timer_ticks_ms` on every
  other CPU too.  See lesson-learned in PLAN §M18.5.
- **Scheduler policy** (`pick_next_locked` in `task.c`) — round-robin
  among RUNNABLE non-idle tasks not running elsewhere; idle is a
  fallback only when no real work is available.  Keeps cores from
  pointlessly bouncing into idle and back when they have work to do.
- **`ap_main`** wired end-to-end: `lapic_init_ap` → `percpu_init_ap`
  → `idt_load` → kprintf → `task_install_ap_idle` →
  `lapic_timer_start_periodic` → idle loop (sti + halt + yield).
  Once IRQs are on the LAPIC timer fires every 10 ms and the
  scheduler picks up any RUNNABLE task in the ring.
- **Parallel self-test** at boot — spawn two CPU-bound hogs, busy-
  wait 500 ms, check both counters > 0.  Verified on `-smp 2` and
  `-smp 4` that both hogs make progress concurrently.

**Still deferred (genuine M19/later work):**

- Per-CPU runqueues + load balancer — global queue + spinlock holds
  up fine to ncpus≤8 under our scheduling rate; per-CPU rq is the
  long-term shape.
- `preempt_count` is still a plain global — needs to move per-CPU
  before more than one CPU exercises preempt_disable bracketing.
- Task affinity / pinning (`taskset`-style).
- Cross-CPU preempt IPI (vector 0x41 is reserved; sender not built).
- `vmm.c` CR0/CR3/CR4 pokes remain x86-only; M17 deferred their
  HAL wrap-up to be done with the x64 port.

### 4.X HAL — arch-independent interface (`kernel/includes/hal_api.h`)

M17 walled off the arch-specific CPU/interrupt/task-bring-up calls
behind a portable interface so x64 and aarch64 ports drop in as new
implementations rather than core refactors.

**Surface (hal_api.h):**

| Function                              | Purpose                                         |
|---------------------------------------|-------------------------------------------------|
| `hal_cpu_halt`                        | Park CPU until next IRQ (x86 `hlt`, arm `wfi`)  |
| `hal_cpu_pause`                       | Spin-loop hint (`pause` / `yield`)              |
| `hal_cpu_idle`                        | **Atomic** enable-interrupts + halt (`sti; hlt` pair on x86 — the CPU guarantees no IRQ delivery between the two, so a "check ring, then sleep" idiom is race-free against an IRQ that fires between the check and the halt) |
| `hal_intr_enable` / `hal_intr_disable`| Direct IF set/clear                             |
| `hal_intr_save` / `hal_intr_restore`  | Save+disable / restore pair (cookie is opaque)  |
| `hal_arch_early_init`                 | One-shot arch bring-up (x86: TSS+GDT+IDT)       |
| `hal_task_init_stack`                 | Pre-build a fresh kernel stack so first `context_switch` lands in an arch-specific trampoline that `sti`s and calls `entry` |
| `hal_syscall_exit_to_kernel`          | Restore saved kernel SP/PC for SYS_EXIT (noreturn) |

**x86 implementation:** `kernel/hal/x86/hal_arch.c` (single-instruction
wrappers + delegation to existing gdt/idt/tss inits) and
`kernel/hal/x86/task_arch.c` (the brand-new-task trampoline + stack
layout).  Both files are tiny — the interface intentionally exposes
just what core code actually calls.

**Migrations done in M17:**
- `kernel/core/task.c` — `local_irq_save`/`restore` → `hal_intr_*`;
  the `task_trampoline` + stack-build moved out to `task_arch.c`.
  `struct task.esp` typed `uintptr_t` so signatures match on any
  arch.  `context_switch`'s extern decl widened the same way.
- `kernel/core/lock.c` — `spin_lock_irqsave`/`unlock_irqrestore` now
  delegates to `hal_intr_save`/`restore`.
- `kernel/core/vc.c` — `sti; hlt` → `hal_cpu_idle()`.
- `kernel/core/kernel.c` — boot order swaps `tss_init() / gdt_init() /
  idt_init()` for `hal_arch_early_init()`; boot self-test halts via
  `hal_cpu_halt()`; the kernel idle loop too.
- `kernel/core/syscall.c` — SYS_EXIT ESP/EIP rewrite moved out to
  `hal_syscall_exit_to_kernel`.
- Legacy PC drivers (`pit.c`, `ps2_keyboard.c`) — their port I/O
  stays direct (driver is x86-only), but their `sti; hlt` idle uses
  `hal_cpu_idle`.
- `kernel/drivers/block/virtio_blk.c` — `pause` → `hal_cpu_pause`.

**Verified end-to-end:** boot self-test results unchanged (vmm,
kmalloc, exFAT, bcache, preempt 104M ticks, VC, shell), no behavioral
regressions.

**Deliberately NOT done in M17 (deferred):**
- `kernel/mem/vmm.c` still pokes CR0 / CR3 / CR4 / invlpg directly.
  Hiding those behind a `hal_map` / `hal_unmap` interface is best
  done at the same time the x64 4-level / aarch64 granule paging
  lands — premature now.  Tracked in PLAN §M17.
- `kernel/core/syscall.c` still includes `idt.h` for the
  arch-specific `struct int_frame`.  The clean fix is to split the
  syscall dispatcher into a portable arg-marshalling layer and an
  arch-specific frame-unpack — also a follow-up.

### 4.X Keyboard layouts (`kernel/core/keymap.c`, `kernel/core/layouts.c`, `kernel/includes/keymap.h`)

M16 introduces a layered translation pipeline shared between every
input driver:

```
Hardware ──► [driver]: scancode/usage → universal keycode + modifier
                                                │
                                                ▼
                            keymap_translate(keycode, modifiers)
                                                │
                                                ▼
                                          ASCII char  ──► vc_kbd_push
```

**Universal keycode = USB HID Usage ID** (HID 1.11 §10, Page 0x07).
That choice means the USB HID driver does zero scancode translation
(it passes `report->keys[i]` straight through), and the PS/2 driver
only has to carry one small "set-1 → HID usage" table.  New input
classes (serial-console escape sequences, virtual KB over RPC, …)
just need to produce the same keycode + modifier pair.

**Modifier bitmask** (`KBD_MOD_*`) mirrors the HID boot-report layout
bit for bit, so the USB driver's `report->modifiers` byte is also
zero-conversion.  Only `KBD_MOD_SHIFT_MASK` and `KBD_MOD_RALT`
influence the layout lookup:

- BASE        → `maps[0]`
- + SHIFT     → `maps[1]`
- + RAlt      → `maps[2]`  (AltGr column)
- + both      → `maps[3]`

Ctrl/Alt/GUI are policy-only — the input driver intercepts what it
wants (e.g. PS/2 grabs `LAlt+digit` for `vc_focus_by_id` BEFORE
calling keymap_translate) and the rest pass through unchanged.

**Layouts** ship as static tables in `layouts.c`:

| Name | Notes                                                          |
|------|----------------------------------------------------------------|
| `us` | The previous hardcoded US table from ps2_keyboard.c / usb_hid.c, now the single source of truth.                                                                |
| `hu` | Magyar 102-key QWERTZ.  Z ↔ Y swap, magyar shifted number row (`!`, `"`, `+`, etc.), AltGr column with ASCII-only symbols (`\`, `|`, `@`, `[`, `]`, `{`, `}`, etc.).  Accented vowels (á, é, ő, ű, ...) are intentionally left blank — the 8×8 ASCII glyph font can't render them; populate when the font grows. |

**Active-layout selection.**  `keymap_init()` (called from kernel_main
right after `config_init()`) reads `keyboard.layout` from the config
(default `"us"`) and activates the matching layout, falling back to
`"us"` if the name is unknown.

**Runtime switch.**  The active-layout pointer is updated only from a
shell-task; IRQ handlers read it from `keymap_translate`.  On x86 a
pointer-sized write is atomic and the rare "switched mid-keystroke"
race produces one char from the new layout — harmless.

**Shell commands:**
- `lslayout`              — list registered layouts, mark the active one.
- `setlayout <name>`      — switch active layout (e.g. `setlayout hu`).
- `setconf keyboard.layout <name> && saveconf` to make it stick.

**Verified path (M16 boot test):**
- `keymap: active layout 'us' (2 available)`
- `echo yz` under `us` → `yz`.
- `setlayout hu` → `layout: now 'hu'`.
- `echo yz` under `hu` → `zy` (Z↔Y QWERTZ swap visible end-to-end).
- The very next attempt to type `lslayout` lands as `lslazout`
  because the user's 'y' keypress now produces 'z' under the active
  layout — the cleanest live demo of "this actually does something."

**Out of scope (M16 follow-ups, tracked in PLAN.md §M16):**
- Extended font (CP437 magyar / ISO-8859-2 / UTF-8) so HU's accented
  vowels actually render.  Today they're 0 in the layout table.
- DE, FR, etc. — straightforward additions once the abstraction is in.
- Compose / dead-key sequences — useful for international layouts that
  build accented chars from base + accent.
- Per-VC layout selection — today's `keyboard.layout` is global.

### 4.X USB host stack — xHCI + HID boot keyboard (`kernel/drivers/usb/`)

M15's first cut: bring up a single xHCI controller, enumerate one
device on a root port, recognize an HID boot-protocol keyboard, and
plumb its 8-byte reports through to `vc_kbd_push` so USB keypresses
feel identical to PS/2 inside the shell.

**Files:**
- `kernel/drivers/usb/xhci.c` — host controller driver (init, command/
  event/transfer rings, port reset, Enable Slot, Address Device,
  Configure Endpoint, periodic event-ring drain).
- `kernel/drivers/usb/usb_hid.c` — HID class driver (8-byte boot-report
  decode, Shift/Alt handling, USB HID Usage ID → ASCII translation).
- `kernel/includes/usb.h` — shared constants, descriptor structs,
  HID modifier bits, the `xhci_poll` + `usb_hid_kbd_handle_report`
  prototypes.

**Controller bring-up (xhci.c):**
- Discovered via `pci_scan` matching class 0x0C subclass 0x03 prog_if
  0x30; the driver registers via `DRIVER()` so it only initializes
  when the controller actually exists.
- BAR0 → MMIO base, mapped with one 4 MiB PSE PDE (cache-disabled)
  via `vmm_map_4mib`.
- Cap-regs read CAPLENGTH / RTSOFF / DBOFF / HCSPARAMS1 to locate
  the operational, runtime, and doorbell register banks.  We refuse
  CSZ=1 (64-byte contexts) and any non-zero scratchpad-buffer count
  to keep the first cut small; qemu-xhci satisfies both.
- Reset sequence: halt (clear R/S), HCRST=1 + wait, wait CNR=0,
  CONFIG.MaxSlotsEn = MaxSlots.

**Data structures (all DMA-coherent, PMM-frame allocated):**
- `DCBAA` — 256-entry Device Context Base Address Array.
- `Command Ring` — 256 16-byte TRBs in one 4 KiB frame.  Last slot is
  a Link TRB with TC=1 that flips our Producer Cycle State on wrap.
- `Event Ring` — 1 segment of 256 TRBs + a 4-dword ERST entry
  pointing at it.  ERDP is written with Event Handler Busy (bit 3) on
  every dequeue update.
- `Transfer Ring` — one per active endpoint (EP0 and the HID interrupt
  IN endpoint).  Same Link-TRB cycle-flip trick.

**Enumeration:**
- Walk PORTSC, find a port with CCS=1, drive PR=1 + PP=1, wait for
  PRC (Port Reset Change), confirm PED (Port Enabled).
- Enable Slot command → slot ID.
- Allocate Device Context, Input Context, EP0 Transfer Ring.
- Build Input Context's Slot Context (Speed, Root Port) and EP0
  Endpoint Context (Control type, MaxPacketSize₀ from port speed, TR
  Dequeue Pointer).
- Address Device command → device responds to its assigned address.
- `GET_DESCRIPTOR(device, 18)`, `GET_DESCRIPTOR(config, 256)`,
  walk the config blob for an interface whose
  (class, subclass, protocol) == (HID, Boot, Keyboard) and grab its
  Interrupt IN endpoint.
- `SET_CONFIGURATION`, `SET_PROTOCOL(BOOT)` on the HID interface,
  Configure Endpoint command with a new Input Context describing the
  interrupt IN slot.
- Queue the first Normal TRB on the interrupt IN ring; ring its
  doorbell.  From that point onward the controller DMAs the next
  HID report into our buffer whenever a key event happens and posts
  a Transfer Event.

**Polling (no MSI/MSI-X yet):**
- `xhci_poll()` drains the Event Ring; called from the PIT IRQ every
  `USB_POLL_TICKS` ticks (10 ms).  On a Transfer Event with completion
  code 1 we hand the 8-byte buffer to `usb_hid_kbd_handle_report` and
  re-arm with another Normal TRB.

**HID class driver (usb_hid.c):**
- USB HID Usage Page 0x07 → ASCII lookup tables (`usb_hid_kbd_lower`,
  `usb_hid_kbd_upper`), populated for the printable subset
  (0x04..0x38).  Special codes: 0x28→\n, 0x29→ESC, 0x2A→\b, 0x2B→\t,
  0x2C→space.
- Diff successive reports for new key-down events (skip phantom-keys
  0x01..0x03).  Shift modifier picks the upper table.
- Alt + digit-row (USB 0x1E..0x26 = '1'..'9') → `vc_focus_by_id(n)`,
  mirroring the PS/2 driver's behavior so USB Alt-N pane switching
  works identically.

**Deliberately out of scope (M15 follow-ups, tracked in PLAN.md §M15):**
- Hubs — root ports only; no recursive enumeration.
- Multiple simultaneous devices — single enumerated device per HC.
- MSI / MSI-X — periodic poll is good enough for HID and avoids the
  pile of PCI-config work IRQ delivery would need.
- Bulk / isochronous endpoints — required for mass storage / audio.
- Full HID report-descriptor parsing — only the boot protocol's
  fixed 8-byte report is handled.
- 64-byte device contexts (CSZ=1) — qemu-xhci uses 32-byte.
- Scratchpad buffers — qemu-xhci reports 0 required, we abort init
  if any HC asks for them.

**Test path:**
- QEMU: `-device qemu-xhci -device usb-kbd`.
- Serial log shows: `xhci: cap_len=… slots=… ports=… ctx=32` →
  `xhci: device on port N speed=…` → `xhci: slot 1 assigned` →
  `xhci: device vid=…` → `xhci: HID kbd iface=0 ep=N pkt=8 interval=…`
  → `xhci: ready, polling for HID reports`.
- Once the prompt is up, sendkey-driven characters land in the shell
  via the USB pipeline (verified during bring-up with a temporary
  `kprintf("hid: …")` on every report — removed before ship).

### 4.X Virtual consoles / pane split (`kernel/core/vc.c`, `kernel/includes/vc.h`)

The screen is partitioned by a binary split tree.  Each leaf is a
`struct vc` with its own rect, cursor, input ring, and bound shell
task; each internal node is a horizontal or vertical split with two
children, divided 50/50.  M14 ships with up to 9 simultaneous panes
(`VC_MAX`); Alt-1..Alt-9 focuses the Nth.

**Data model:**
- `struct vc`: id (1..VC_MAX), back pointer to its tree leaf, cursor
  (cur_col, cur_row), fg/bg colors, SPSC input ring
  (`in_buf[VC_INBUF_SZ]`, head/tail), and bound shell task.
- `struct vc_node` (opaque to callers): `kind ∈ {LEAF, SPLIT}`, parent,
  rect (x, y, w, h in cells), plus either a `vc*` (leaf) or
  `dir + a + b` (split).
- Globals: `root` (root node), `focused` (current input target), and a
  `vcs[]` array indexed by id-1 for fast Alt-N lookup.

**Layout:** every tree mutation triggers a top-down recursive pass that
assigns rects to every node.  Splits divide the parent's rect into a
50/50 floor/remainder (so the sum always equals the parent regardless
of odd dimensions).  VC_MAX = 9 means the tree is always tiny, so
incremental layout would be premature optimization.

**Split:**
- `vc_split(v, dir)` allocates a new VC + two leaf nodes, then converts
  v's existing leaf node into a SPLIT node IN PLACE (we can't swap the
  node pointer because v's parent points at the old address; the
  mutation has to keep the address stable).  Children: `a = v's old leaf`
  (now renamed under the new split), `b = new VC's leaf`.
- Mutation is bracketed by `preempt_disable`/`preempt_enable` so the
  IRQ side (vc_kbd_push) cannot observe a half-converted tree.
- Layout is recomputed from root; `repaint_all` clears every leaf.
  Pane contents are NOT preserved across splits — a scrollback buffer
  per VC is a future M14 follow-up.
- The newly-created VC takes focus.

**Output:**
- `vc_putchar(v, c)` is the only renderer.  Handles \n, \r, \b,
  wraparound and rect-scoped scroll via `fb_scroll_cells_up`.
- console.c calls into vc_putchar via the per-task emit hook, so each
  shell task's `kprintf` automatically lands in its bound VC.

**Input:**
- IRQ side: `vc_kbd_push(c)` reads `focused` (pointer-sized atomic on
  x86) and writes to that VC's ring.  Drops on full.
- Owner side: `vc_getchar(v)` blocks via the same `sti; hlt;
  task_yield()` pattern as the legacy `keyboard_getchar` — wakes on
  any interrupt, re-checks its own ring, naps again if still empty.
- Alt scancode (0x38 make / 0xB8 break) sets a modifier flag in the
  PS/2 driver.  When Alt is held and a digit-row scancode (0x02..0x0A)
  arrives, the driver calls `vc_focus_by_id(N)` instead of pushing the
  character.

**Per-task console binding:**
- `task->out_console` (in task.h) is an opaque `void*`.  The shell-task
  spawner sets it to the bound VC BEFORE the task is first scheduled
  (under `preempt_disable`), so the new task's very first `kprintf`
  already routes to the right pane.
- `console_putchar` always broadcasts to active sinks (serial → debug
  log), THEN, if the current task has an `out_console`, delivers to
  the per-task hook (which vc_init wired to `vc_putchar`).

**Shell as a task:**
- `shell_run(struct vc* v)` is the per-pane REPL — reads from `v`'s
  ring, dispatches commands, prints with `kprintf` (which routes to
  `v`'s rect automatically).
- `shell_task_entry()` is the task_spawn entry: reads the bound VC out
  of `task_current()->out_console` and tail-calls shell_run.
- kernel_main spawns the first shell on the root VC and then becomes
  the idle task (hlt + yield forever).  pid 0 stays "kernel" — every
  shell pane is its own RUNNABLE task.

**Shell commands added:**
- `pane`               — list every leaf VC with rect, owner pid, focus.
- `pane split horizontal` — split current pane into top/bottom halves.
- `pane split vertical`   — split current pane into left/right halves.

**Out of scope (M14 follow-ups, tracked in PLAN.md §M14):**
- `pane kill` to reap a pane + free its node + reflow the tree.
- Scrollback buffer per VC so split doesn't lose content.
- Resize a split (today: always 50/50).
- Visible focus indicator (border / titlebar / colored cursor).
- Per-VC config (prompt, fg/bg) — today `shell.prompt` is global.

### 4.X Ring 3 / user mode (`kernel/hal/x86/`, `kernel/core/syscall.c`)

**GDT entries (`gdt.c`):** added user code (DPL=3, selector 0x1B) and
user data (DPL=3, selector 0x23) descriptors plus a TSS descriptor at
selector 0x28.  `gdt_init` calls `ltr` to load the task register after
the GDT is in place.

**TSS (`tss.c`):** single static `struct tss32`.  Only `ss0` and `esp0`
matter — they tell the CPU which kernel stack to switch to on a
ring-3 → ring-0 transition (interrupt or syscall).  `esp0` points at a
dedicated 4 KiB syscall stack so int 0x80 doesn't trample the kernel
context saved by `enter_user_mode_wrap`.

**Ring transition (`usermode.s` + `syscall.c`):**
- `enter_user_mode_wrap(eip, esp)`:
  1. `pushad` — save kernel callee + caller state on the kernel stack.
  2. Stash the current ESP and the address of a `.return` label.
  3. Build an iret frame: SS=0x23, ESP=user, EFLAGS|=IF, CS=0x1B,
     EIP=user_eip.
  4. `iret` — CPU drops to ring 3, runs the user program.
- User program issues `int 0x80` with the syscall number in EAX.
- IDT vector 0x80 is installed with **DPL=3** so ring 3 may invoke it.
  `isr128` (in isr_stubs.s) is a regular ISR stub; its int_frame
  carries the user's saved EAX/EBX/ECX/EDX.
- `syscall_dispatch` reads EAX and routes:
  - `SYS_PRINT` (0): walks the C string at EBX and prints it.
  - `SYS_EXIT`  (1): asm trick — sets `esp = saved_esp` and `jmp
    saved_eip`.  We land at the `.return` label in
    `enter_user_mode_wrap`, popad + ret to the original kernel
    caller.

**Demo (`ringtest` shell command):** allocates two physical frames
USER-mapped at virt 0x40000000 (code+msg) and 0x40001000 (stack).
Hand-codes a tiny program that calls SYS_PRINT then SYS_EXIT.  Drops
into ring 3 via `enter_user_mode_wrap`.  When SYS_EXIT teleports back,
control returns to the shell command and the user pages are freed.

**Limits today:** no real process address spaces (everything shares
one page directory).  The "user" page table entries set the USER bit
but the supervisor (kernel) can still read them, which is how
`SYS_PRINT` walks the string.  Real isolation lands when M7 gives
each task its own VMM context.

### 4.X Block layer + virtio-blk (`kernel/core/block.c`, `kernel/drivers/block/virtio_blk.c`)

**Abstract block_device (`block.h`):**
- `struct block_device { name, sector_size, sector_count, read, write,
  flush, priv, next }`.
- `blk_register` / `blk_find` / `blk_for_each` / `blk_list` — simple
  linked-list registry.
- Filesystems sit on top: never call a specific driver directly.

**PCI enumeration (`kernel/hal/x86/pci.c` + `kernel/includes/pci.h`):**
- Port-I/O config space (0xCF8 address + 0xCFC data).
- `pci_scan(fn, ctx)` walks bus 0 + multi-function slots.
- `pci_find_device(vendor, device, *out)` — first-match lookup.
- x86-specific today; under §M17 portability cut moves behind a HAL
  hook so ARM (ECAM) / x86_64 (MMConfig) can implement the same API.

**virtio-blk (`kernel/drivers/block/virtio_blk.c`):**
- Legacy (transitional) virtio over PCI I/O port transport.
- vendor `0x1AF4`, device `0x1001`.
- Single virtqueue, polling-based, one outstanding request at a time.
- **Queue layout pitfall:** legacy QUEUE_SIZE is read-only.  QEMU
  reports 256 entries; we MUST size desc/avail/used to match — the
  device computes offsets in our queue using its own qsize, so a
  smaller QSIZE causes silent address mismatch and indefinite hang.
  We compile with `QSIZE=256` and allocate 3 contiguous PMM frames
  (~12 KiB).
- **DMA address pitfall:** descriptor.addr fields are physical
  addresses (the device's view of memory).  Driver-internal buffers
  (request header, status byte) come from `pmm_alloc_frame` so virt
  == phys (identity-mapped 0–256 MiB).  Caller-provided data buffers
  are translated via `vmm_translate`.  Heap-backed buffers (virtual
  `0xD0000000+`) MUST be translated; identity-mapped ones translate
  to themselves and work either way.
- **Single-frame buffers only.**  A buffer spanning two virtual pages
  may straddle two non-adjacent physical frames; the single-
  descriptor DMA would read into garbage.  M11 callers (blktest,
  devfs adapter) use frame-sized allocations.  Larger reads/writes
  need per-page descriptor chaining (future work).
- Exposed via devfs as `/dev/vda`.

**`/dev/vda` (devfs adapter):**
- read/write at byte offset; must be sector-aligned (512 bytes) for
  both offset and length.
- Backs `cat /dev/vda` (read), `write /dev/vda` (write — not very
  useful without sector-aligned shell tools, but exercises the path).

**Disk image workflow:**
- Create once: `dd if=/dev/zero of=build/test.img bs=1M count=4`
- Run with: `qemu-system-i386 -cdrom build/d-os.iso -drive
  if=virtio,file=build/test.img,format=raw`
- Without `-drive`, lsdrv shows `virtio_blk — absent` (clean fail).

### 4.X procfs — kernel state as files under /proc (`kernel/fs/procfs.c`)
- **Model:** synthetic files attached under `/proc` (created by ramfs).
  Each file's content is generated lazily on first read after open via
  a `gen` callback that fills a growing `procfs_writer`.  The buffer
  is cached in `f->private` for subsequent slices and freed on close;
  re-opening regenerates fresh content.
- **Public API:**
  - `struct procfs_node { name, gen, _next }`
  - `procfs_register(struct procfs_node*)` — same queue/flush dance as
    devfs.
  - `pw_putc / pw_puts / pw_put_uint / pw_put_hex` — append helpers.
- **Built-in nodes (read-only today):**

| Path             | Contents                                       |
|------------------|------------------------------------------------|
| `/proc/version`  | `d-os 0.0.1 (i386)`                            |
| `/proc/uptime`   | `h:mm:ss.mmm` since boot                       |
| `/proc/meminfo`  | PMM frame stats + heap stats (key=value)       |
| `/proc/modules`  | `MODULE()` registry                            |
| `/proc/drivers`  | `DRIVER()` registry with state per entry       |
| `/proc/console`  | console_sink registry with active flag         |
| `/proc/tasks`    | task list with pid/state/name                  |
| `/proc/config`   | full key/value cache                           |
| `/proc/kmsg`     | klog ring, `[  sec.mmm] LEVEL tag: msg` (M28)  |

- **New iterators added for procfs's sake:**
  - `console_for_each(fn, ctx)` in `console.c`
  - `task_for_each(fn, ctx)` in `task.c`
  - `config_for_each(fn, ctx)` in `config.c`
- **Init order:** `procfs_init()` runs after `devfs_init()` in
  `kernel_main` — both need the FS up.

### 4.X devfs — drivers as files under /dev (`kernel/fs/devfs.c`)
- **Model:** synthetic files attached under the existing `/dev`
  directory (created by ramfs).  Not a separate mounted filesystem
  yet — just per-node inodes whose VFS `file_ops` forward to the
  driver's read/write/ioctl callbacks.  Linux divergence: this is
  closer to devtmpfs than devfs proper; we'll wrap it in a
  `struct fs_type` only if we need namespace-style mount semantics.
- **Public API:**
  - `struct devfs_node { name, kind, read, write, ioctl, ctx }`
  - `devfs_register(struct devfs_node*)` — drivers call it from
    their MODULE init.  Pre-init calls queue; `devfs_init` flushes
    the queue + adds built-ins.
- **Built-ins:**
  - `/dev/null`  — read returns 0 (EOF), write swallows.
  - `/dev/zero`  — read fills with zeros (caller bounds), write
    swallows.
- **Driver-registered today:**
  - `/dev/com1`     — write goes to COM1 UART.
  - `/dev/keyboard` — read blocks for keystrokes (returns ASCII).
- **Init order:** `devfs_init()` runs in `kernel_main` after
  `module_init_all` (so ramfs has mounted `/`) and after
  `driver_init_all` (so DRIVER() entries had a chance to queue).

### 4.X Configuration store (`kernel/core/config.c`)
- **Public API:** `config_get(key, default)` / `config_set(key, val)` /
  `config_save()` / `config_load()` / `config_dump()`.
- **Cache:** singly-linked list of `{key, value}` pairs on the kernel
  heap.  O(N) lookup; trivial for the few dozen entries we care about.
- **Defaults:** `builtin_defaults[]` table baked into config.c.
  Loaded into the cache at `config_init`; can be overridden by
  `/etc/d-os.conf` or by runtime `setconf`.
- **File format:** `key = value`, one per line; `#` starts a comment;
  blank lines OK; values trimmed of leading/trailing whitespace.  No
  quoting, no escapes.
- **Init order:** `config_init` runs after `module_init_all` because it
  needs the filesystem to be mounted and ramfs to be present.
- **First consumer:** the shell reads `shell.prompt` on every
  iteration, so `setconf shell.prompt foo>` takes effect immediately.

### 4.X Filesystem layer (`kernel/fs/`, `kernel/includes/vfs.h`)

The VFS was rebuilt in M12 to host real filesystems alongside the
in-memory ones.  Key shape changes from the M4 baseline:

- All sizes are `uint64_t` (was `size_t` / 32-bit).
- `file_ops.read` / `write` take an explicit byte `off` argument; the
  VFS layer owns `f->pos` and bumps it by the byte count returned.
  Filesystem implementations are now pure offset-addressed — the
  natural shape for FAT / exFAT / NTFS / ext.
- `struct inode_ops` carries directory mutators: `lookup`, `create`,
  `mkdir`, `unlink`.  Lazy filesystems (exFAT) supply `lookup`; eager
  ones (ramfs / devfs / procfs) leave it NULL and rely on their
  fully-cached dentry tree.
- `fs_type.mount(struct block_device* dev, struct dentry* mp)` —
  receives the backing block device.  In-memory filesystems pass
  `NULL` via `vfs_mount(fs, path, NULL)`.

**VFS (`vfs.c`):**
- Owns the root dentry (`vfs_root()`) and the registered-fs list.
- Path conventions: absolute paths only, '/'-separated, components up
  to `VFS_NAME_MAX` (63) bytes.  No `.` / `..` / symlinks yet.
- Path resolution walks the dentry tree (`parent->children` →
  `sibling` chain) with a fallback to `parent->inode->dir_ops->lookup`
  on cache miss — successful lazy lookups are attached so subsequent
  resolutions are O(1).
- `vfs_mount(fs_name, path, dev_name)` — `dev_name` may be NULL for
  in-memory fs.  Non-root mountpoints have their placeholder inode
  detached automatically so the fs can install its own root inode.
- `vfs_create` / `vfs_mkdir` dispatch to the parent directory's
  `dir_ops` — no more `extern ramfs_create_in` from vfs.c.
- Public API: `vfs_init` / `vfs_register_fs` / `vfs_mount` /
  `vfs_open` / `vfs_close` / `vfs_read` / `vfs_write` / `vfs_readdir`
  / `vfs_mkdir` / `vfs_create`.  See `kernel/includes/vfs.h`.

**Inode model:**
- `enum inode_type { INODE_FILE, INODE_DIR, INODE_DEVICE }`.
- `struct inode { type, size (uint64), private (fs-defined), ops,
  dir_ops }`.
- `struct file_ops { read(file, buf, n, off), write(file, buf, n,
  off), readdir(file, dirent), close(file) }`.
- `struct inode_ops { lookup, create, mkdir, unlink }` — every field
  optional (NULL means the op is unsupported).

**ramfs (`ramfs.c`):**
- Inodes + dentries live on the kernel heap.
- File content is a single `kmalloc`'d buffer with grow-on-write
  doubling capacity each time.
- Directory readdir uses `f->pos` as a 0-based child index iterating
  over `dentry->children`.  `inode_ops` populates `create` and
  `mkdir`; `lookup` is NULL (eager tree).
- Pre-creates `/etc`, `/dev`, `/tmp`, `/proc`, `/mnt` at mount.
- Registered as a `fs` class module (`MODULE("ramfs", "fs", ...)`)
  that calls `vfs_register_fs` then `vfs_mount("ramfs", "/", NULL)`.

**Init order subtlety:** `vfs_init()` MUST run before
`module_init_all()` because ramfs's module init calls
`vfs_register_fs` and `vfs_mount`.  `kernel_main` enforces this
order explicitly.

### 4.X Block cache (`kernel/core/block_cache.c`)

Refcounted, write-back, LRU buffer cache between filesystems and the
block layer.  Used by exFAT (and any future fs) to avoid pounding
the disk for repeated FAT / bitmap reads.

- **Slot pool:** fixed 64 slots, each backed by a PMM-allocated frame
  (4 KiB) so the buffer is physically contiguous — required by the
  virtio-blk DMA path.  Only the first `dev->sector_size` bytes of
  each frame are used.
- **API (`block_cache.h`):**
  - `bcache_init()` — one-shot, called from `kernel_main`.
  - `bcache_get(dev, lba) → struct bcache_buf*` — refcount++; on miss
    evicts the lowest-tick refcount-0 victim (after writing it back if
    dirty), then `dev->read`s the sector into the slot.
  - `bcache_release(buf)` — refcount--.
  - `bcache_mark_dirty(buf)` — write-back deferred until eviction or
    explicit `bcache_sync(dev)`.
  - `bcache_sync(dev)` — flush every dirty buffer owned by `dev`,
    then `dev->flush` if implemented.
  - `bcache_get_stats(out)` / `bcache_print_stats()` — instrumentation
    used by `bctest` shell command and the boot self-test.
- **Concurrency:** single-threaded today.  Layout reserves room for a
  per-slot lock once §M18 lands.
- **Self-test in `kernel_main`:** if `vda` is present, `bcache_get`
  is called twice for sector 2 — second call must return the same
  slot, demonstrating the cache is live.  Reported on serial as
  `bcache self-test: hit=N miss=M (same slot reused)`.

### 4.X exFAT (`kernel/fs/exfat.c`)

First persistent filesystem.  Implements the subset of the exFAT
specification needed for the M12 DOD: mount, readdir, read, create,
write, persistence across reboot.  Validated against `fsck.exfat`
from Linux's exfatprogs after each write.

- **On-disk parsing:** boot sector at LBA 0 must carry the
  `"EXFAT   "` signature; otherwise mount returns an error.  Fields
  parsed: `FatOffset`, `FatLength`, `ClusterHeapOffset`,
  `ClusterCount`, `FirstClusterOfRootDirectory`,
  `BytesPerSectorShift`, `SectorsPerClusterShift`.
- **Mount:** scans the root cluster for the Allocation Bitmap entry
  (type 0x81) to record `bitmap_cluster` + `bitmap_size`; needed for
  cluster allocation on write.  The Up-case Table is ignored —
  lookup is case-sensitive (acceptable while we both produce and
  consume the names).
- **Directory entries:** 32 bytes each; a File entry-set is one
  `0x85` File entry followed by `SecondaryCount` entries (one `0xC0`
  Stream Extension + 1..2 `0xC1` Name entries).  Names are UTF-16
  little-endian; the driver accepts up to 30 ASCII characters per
  filename (any high byte set means non-ASCII and the entry is
  skipped).
- **Cluster chain:** walked via the FAT, or via `+1` increments when
  the Stream Extension's `NoFatChain` flag is set.  Sectors are
  fetched through the block cache (`bcache_get`).
- **Write path:** new files are created with no clusters; the first
  write allocates one cluster (via the allocation bitmap) and writes
  it into the Stream Extension's `FirstCluster` field.  Subsequent
  writes that extend past the existing chain allocate more clusters
  and link them into the FAT (`fat_set`).  After every write the
  driver re-fetches the entry set, patches `ValidDataLength` +
  `DataLength` + `FirstCluster`, recomputes the SetChecksum on the
  File entry, and writes it back.  `close` flushes the cache to disk.
- **Out of scope for M12:** `mkdir`, `unlink`, names >30 chars, non-
  ASCII names, case-insensitive lookup via the Up-case Table,
  ActiveFat / VolumeDirty bit management.  Tracked under §M12 in
  PLAN.md.
- **Self-test in `kernel_main`:** if `vda` carries an exFAT volume,
  the kernel mounts it at `/mnt` and looks for `/mnt/dos-marker.txt`.
  Missing → creates + writes `"wrote-from-dos"`.  Present → reads it
  back.  Two consecutive boots therefore demonstrate the full
  round-trip on the serial log alone.  Linux `fsck.exfat -y` reports
  `clean. directories 1, files 1` against the resulting image.

### 4.X Timer (`kernel/drivers/timer/pit.c`)
- **Hardware:** legacy 8254 PIT.  Channel 0 ports 0x40 (data) / 0x43
  (command).  Routed to IRQ0 / vector 32 after PIC remap.
- **Programming:** mode 3 (square wave), 16-bit binary divisor =
  1193 → 1000.15 Hz, treated as 1 ms per tick.
- **State:** monotonic 64-bit `ticks_ms` updated in the ISR.
- **Public API (`timer.h`):**
  - `timer_ticks_ms()` — read the counter.
  - `timer_msleep(ms)` — `sti; hlt` until enough ticks accrue.
- **Module class:** `timer`.  When HPET / TSC-deadline / ARM generic
  timer arrive, they register under the same class and the same
  public API consumes the highest-precedence active timer.
- **64-bit math note:** `uptime`'s formatting uses 64-bit `% 1000` /
  `/ 60000`, which expand to `__umoddi3` / `__udivdi3` calls supplied
  by libgcc.  The Makefile resolves the absolute path via
  `gcc -m32 -print-libgcc-file-name` and links it explicitly because
  we link with `-nostdlib`.

### 4.10 Kernel heap (`kernel/mem/kmalloc.c` + `kernel/mem/slab.c`) — **slab + page_alloc (M19)**
- **Public API unchanged since M1:** `kmalloc / kfree / kcalloc /
  kmalloc_init / kmalloc_stats`.  Drivers compile unchanged; the
  K&R block free-list under the hood is gone.
- **Two-layer dispatch** based on requested size:
  - `size <= 2048 B` → size-class **slab** cache (16, 32, 64, 128,
    256, 512, 1024, 2048 — powers of two).
  - `size  > 2048 B` → buddy `page_alloc(order)` where
    `order = ceil_log2(ceil(size / 4 KiB))`.  Side table
    `big_alloc_order[]` (1 byte per frame) records the order of every
    live big allocation so `kfree` can pass it back to `page_free`.
- **Slab layout (one slab = one 4 KiB page):**
  - First 24 bytes = `struct slab` header: `magic` (`0xC0DEBABE`),
    back pointer to the owning `slab_cache`, intrusive next/prev,
    in_use + capacity + free_head.
  - Remaining bytes = array of `capacity` slots, each `slot_size`
    bytes (= obj_size rounded up to 8).
  - **Free list inside the slab:** each free slot's first 2 bytes
    holds the index of the next free slot, or `FREE_END` (0xFFFF)
    for the tail.  No external bitmap.  Allocation = pop
    `free_head`; free = push back onto `free_head`.
- **Per-CPU magazines** (the M19 showcase of M18's percpu infra):
  - One `struct mag` per CPU per cache (`mag[ACPI_MAX_CPUS]`),
    fixed-size array of object pointers (`MAG_CAPACITY = 32`).
  - **Alloc fast path:** IRQ-off, pop from `mag[this_cpu_id()]`.
    No spinlock acquired; cross-CPU contention is invisible to
    the fast path.
  - **Free fast path:** IRQ-off, push to `mag[this_cpu_id()]`.
  - **Slow paths** (mag empty / mag full) call `mag_refill` or
    `mag_flush`, which batch `MAG_BATCH = 16` objects under the
    cache's spinlock and touch the partial/full slab lists.
  - IRQ-off (not just spinlock) is required because an IRQ handler
    that allocates could race the magazine with itself otherwise.
    Per-CPU index is stable across an IRQ-off window because
    migration is also gated by IF.
- **kfree dispatch:**
  - Mask `p` to the page boundary.  If `((struct slab*)page)->magic
    == SLAB_MAGIC`, route via `slab_free(slab->cache, p)`.
  - Else check `big_alloc_order[pfn]` — if not `0xFF`, it's a
    page-alloc-backed allocation; `page_free(p, order)`.
  - Else complain (pointer wasn't from us).
  - No per-object header in the slab path; the slab page tells you
    everything.
- **Returned-pointer alignment:** 8-byte aligned for slab objects,
  4 KiB aligned for big allocations (because `page_alloc` returns
  frame addresses directly).
- **Diagnostics:**
  - `meminfo`: PMM + zone + kheap summary (unchanged interface).
  - `slabinfo`: per-cache obj_size / slot_size / slab count / in_use
    / free / magazine total.
  - `buddyinfo`: per-zone free-block counts at each order.
- **Self-test + microbench** (at boot):
  - `alloc(64) / alloc(128) / free / alloc(48)` round trip; the third
    allocation must land at the same address as the first to
    demonstrate magazine LIFO reuse.
  - 10000 × `{alloc(64) + free}` microbench, measured in ms; gives
    a baseline number to spot regressions in future allocator work.
- **Concurrency:** per-cache spinlock + per-CPU magazines + per-zone
  spinlock in the buddy.  Safe to call from IRQ context.
- **Memory budget added by M19:** `page_state[]` (256 KiB) +
  `big_alloc_order[]` (256 KiB) in `.bss`, BUDDY_MAX_FRAMES = 1 GiB
  cap.

### 4.11 Serial debug (`kernel/drivers/serial/serial.c`)
- **Hardware:** 8250/16550 UART on COM1 (base I/O 0x3F8).
- **Config:** 38400 baud, 8N1, FIFO on.  Output only.
- **Init order:** `serial_init` runs **first** in `kernel_main` — it
  has no preconditions, and everything that runs after it can log via
  serial even before the terminal is up.
- **Integration with kprintf:** `kprintf`'s `emit` helper tees every
  byte to both `terminal_putchar` and `serial_putchar`, so diagnostics
  survive the window between boot and framebuffer init.  When QEMU is
  launched with `-serial stdio`, the log appears on the host terminal.
- **Limitations:** no receive, no flow control, no fall-back if no
  UART is present (a dead transmitter would spin forever waiting for
  THR-empty).  Fine on QEMU; add a probe before using on real hardware.

### 4.X Supported architectures — i386 + x86_64 (M20)

d-os builds on two arches today; a third (aarch64) is the next
portability stress test on the roadmap.

| Arch    | Status                                    | Boot path                            |
|---------|-------------------------------------------|--------------------------------------|
| i386    | Full — reference port                     | Multiboot1 + 32-bit ELF              |
| x86_64  | Full — SMP + APIC + ring-3 via int 0x80   | Multiboot2 + 64-bit ELF, long mode   |
| aarch64 | Planned (M21)                             | UEFI / U-Boot, EL1 entry             |

x86_64 polish backlog: SYSCALL/SYSRET instruction path (currently
ring 3 reaches the kernel via `int 0x80` only — same as i386); USB
host (xHCI 64-bit DMA revisit); block layer (virtio-blk + exFAT
64-bit DMA revisit).

**Per-arch source tree:**
- `kernel/hal/x86/`    — i386 HAL (boot.s, gdt, idt, tss, isr_stubs,
  switch, usermode, task_arch, hal_arch, vmm, io, lapic, ioapic,
  smp, syscall, pci, ap_trampoline).  `lapic.c` and `ioapic.c` are
  also compiled into the x86_64 build (M20.5 Phase A) — they are
  pure MMIO + MSR with no port I/O.
- `kernel/hal/x86_64/` — x86_64 HAL (boot.s, gdt, idt, tss,
  isr_stubs, switch, usermode, task_arch, hal_arch, vmm, io, mb2,
  main_entry, smp, syscall, ap_trampoline, m20_stubs).  M20.5 Phase
  B brought up SMP via ap_trampoline.s + smp.c; Phase C added
  ring-3 via usermode.s + syscall.c.  m20_stubs.c is down to one
  symbol (xhci_poll) and will be deleted when xHCI is ported.

**x86_64 boot path:**
1. GRUB parses the multiboot2 header in `boot.s` (`.multiboot`
   section) and loads the ELF64 kernel at 1 MiB.  Entry is in
   32-bit protected mode (mb2 §3.1.5 default).
2. `_start` (32-bit code in `boot.s`) stashes the loader magic +
   info pointer, runs a CPUID long-mode check, then builds an
   identity-mapped page hierarchy: PML4[0] → PDPT[0] → PD[0..511]
   as 2 MiB large pages (PS=1), covering the first 1 GiB.  Three
   .bss-allocated 4 KiB frames total.
3. Intel SDM Vol 3A §9.8.5 long-mode entry sequence:
   CR4.PAE → CR3 = pml4 → EFER.LME → CR0.PG.  CPU is now in
   long-mode compatibility submode (32-bit code with 64-bit paging).
4. Far-jmp through a tiny 64-bit GDT into `long_mode_entry` (true
   64-bit code).  Reload data segs, print
   "Hello from x86_64 long mode\r\n" via polled COM1 as a sentinel,
   call `x86_64_main_entry(magic, info)`.
5. `x86_64_main_entry` (in `main_entry.c`) validates the mb2 magic
   (0x36d76289) and translates the mb2 tag stream into a static
   `struct mboot_info` (mb1 shape) via `mb2_translate_to_mb1`.
   Then calls `kernel_main(MULTIBOOT_BOOTLOADER_MAGIC, mb1_ptr)` —
   so the rest of the kernel (pmm.c, fb_terminal.c, mboot_print_*,
   ...) sees the familiar mb1 layout regardless of how we booted.
6. `kernel_main` runs the standard boot sequence — no arch-gated
   blocks since M20.5 Phase C.  Both archs run the same flow:
   - APIC bring-up + LAPIC-timer programming + smp_boot_aps.  On
     `-smp N`, all N CPUs come online and accept scheduled work
     (Phase B's x86_64 AP trampoline).
   - Ring-3 reachable via `int 0x80` (Phase C).  Shell `ringtest`
     drops to ring 3 with a hand-coded user program that prints
     "hello from ring 3!" via SYS_PRINT and returns via SYS_EXIT
     teleport — same flow that i386 has shipped since M6.
   - SYSCALL/SYSRET instruction path is NOT wired up on x86_64
     yet (the GDT slot layout doesn't satisfy SYSRET's
     STAR[63:48]+16 / STAR[63:48]+8 selector convention; a GDT
     reorganization is the natural follow-up).

**HAL API status — vmm.h widening:**
- `vmm_map / vmm_map_4mib / vmm_unmap / vmm_translate /
  vmm_kernel_pd_phys` all take `uintptr_t` for virt/phys/return
  types so the same prototype serves both archs.  i386 callers
  see no source change (uintptr_t = uint32_t there).
- `vmm_map_4mib` semantics: on i386 it's literally a 4 MiB PSE PDE;
  on x86_64 it installs TWO adjacent 2 MiB large PD entries to
  preserve the 4 MiB contract for callers like fb_terminal.c and
  xhci.c.

**HAL API status — idt.h, tss.h, multiboot.h:**
- `struct int_frame` (in idt.h) is `#if defined(__x86_64__)`-gated:
  i386 layout pushes ds/es/fs/gs + pusha + iret frame; x86_64
  layout pushes 15 GPRs + always-5-quadword iretq frame.  Field
  names int_no / err_code identical across archs so portable IRQ
  handlers (pit_irq, keyboard_irq) compile for both.
- `tss_set_kernel_stack`, `tss_get_addr` take/return `uintptr_t`
  (i386: 32-bit ESP, x86_64: 64-bit RSP).
- `mboot_init`, `kernel_main` take `uintptr_t info_ptr`.  Boot.s
  on each arch passes the appropriate value.

**What landed across M20.5 (2026-06-29):**

- **Phase A** — LAPIC + IOAPIC compile for x86_64 (`kernel/hal/x86/
  lapic.c` and `ioapic.c` listed under the x86_64 source set in the
  Makefile, `phys` params widened to `uintptr_t`).  `kernel.c`
  arch-gates around APIC bring-up dropped.  `kprintf` gained `%l`
  / `%ll` / `%z` length modifiers and uintptr_t-width `%p`.
- **Phase B** — x86_64 SMP AP bring-up.  New
  `kernel/hal/x86_64/ap_trampoline.s` (16→32→64-bit chain via
  inline trampoline GDT, then lgdt + far-ret into the kernel GDT)
  + `kernel/hal/x86_64/smp.c`.  `-smp 4` brings up all 4 CPUs;
  parallel self-test PASSes with hog ticks ~2-4× UP baseline.
- **Phase C** — x86_64 ring-3 via `int 0x80`.  New
  `kernel/hal/x86_64/usermode.s` (5-quadword iretq frame +
  SYS_EXIT teleport) + `kernel/hal/x86_64/syscall.c` (mirror of
  i386 dispatcher with rax/rbx fields).  Moved
  `kernel/core/syscall.c` to `kernel/hal/x86/syscall.c` —
  closes one of the M17 deferred items.  `m20_stubs.c` shrank to
  one symbol: `xhci_poll`.

**What remains (x86_64 polish backlog):**
- SYSCALL/SYSRET instruction path.  Requires GDT slot
  reorganization (SYSRET wants user data 8 below user code64 from
  STAR[63:48], which our 0x18 user-CS / 0x20 user-DS layout
  doesn't satisfy).  `int 0x80` covers all current ring-3 needs.
- USB host (xHCI) and block layer (virtio-blk + exFAT) for x86_64:
  drivers currently compiled out of the x86_64 build because their
  DMA paths assume <4 GiB phys addressing and need a 64-bit revisit.

**Lessons learned (filed in source comments + the M18.5 / M19 /
M20 change-log entries):**
- Multiboot2 framebuffer-request tag (type 5 in the header) is
  mandatory to get GRUB to deliver a runtime framebuffer info tag.
  Without it, fb_terminal stays inert and `vc_init` bails.
- `objcopy --input-target=binary` mints symbol names from the
  input filename — keep `.bin` artifacts at their source-relative
  paths so the symbols smp.c references remain stable across
  ARCH-specific build trees.
- IDTR is per-CPU even though the IDT data is shared; each AP
  (and the x86_64 BSP) must run its own `lidt`.
- `lapic.c` / `ioapic.c` are arch-family-shared, not "x86 only" —
  pure MMIO + MSR with `rdmsr`/`wrmsr`/`pause` instructions that
  encode identically in 32-bit and 64-bit mode.  They live under
  `kernel/hal/x86/` for historical reasons but participate in both
  builds (M20.5 Phase A).
- On x86_64 long mode, EVERY level of the 4-level page-table walk
  checks the US bit, not just the leaf PT entry.  boot.s builds
  the bootstrap PML4[0] / PDPT[0] / PD[i] with US=0 (kernel-only);
  the first user mapping under that subtree #PFs with err=5 (P+U
  set) because PML4[0]'s US=0 is the binding constraint.
  walk_to_pt was patched in Phase C to OR US into existing
  intermediate entries when the caller's flags request it.  Safe:
  we only widen permissions, never tighten, and the actual page
  protection still lives in the leaf PT.

### 4.12 ACPI (`kernel/acpi/acpi.c`)
- **Purpose:** discover ACPI tables at boot, parse `_S5_` from DSDT, enable
  proper `hal_shutdown` on real hardware.
- **Discovery sequence:** `find_rsdp()` scans the EBDA first 1 KiB plus the
  `0xE0000 – 0xFFFFF` range on 16-byte boundaries for the `"RSD PTR "`
  signature with a valid 20-byte checksum. `acpi_init()` then follows
  `rsdp.rsdt_address` to the RSDT, walks its entry array, validates each
  SDT's signature + checksum, and grabs the FADT (`"FACP"`). From the FADT
  it caches `PM1a_CNT` / `PM1b_CNT` I/O ports and follows the `dsdt`
  pointer. The DSDT body is then scanned for the `_S5_` byte sequence,
  followed by a very small AML parse (PackageOp + NumElements + two
  constants) to extract `SLP_TYPa` / `SLP_TYPb`.
- **`acpi_shutdown()`:** writes `(SLP_TYPa << 10) | (1 << 13)` to
  `PM1a_CNT` (and the b variant if non-zero). On success, the machine
  powers off and the function never returns.
- **Known limits:** no AML interpreter — we find `_S5_` by byte search,
  so boards with unusual encodings fall through. No XSDT support
  (64-bit pointers); only RSDT revision 0 is handled.

---

### 4.13 GUI — compositor, WM, widgets, apps (M22 – M22.5)

Files: `kernel/gui/gfx.c` + `gfx.h` (primitives + surfaces),
`kernel/gui/gui.c` + `gui.h` (compositor + WM + taskbar + windows),
`kernel/gui/widget.c` + `widget.h` (widget toolkit),
`kernel/gui/fileman.c` (file manager app),
`kernel/drivers/mouse/ps2_mouse.c` + `mouse.h` (pointer input),
`kernel/drivers/rtc/cmos_rtc.c` + `rtc.h` (taskbar clock source).
Arch-independent (pure C on the 32-bpp linear FB); works on i386 and
x86_64 at 1280×800 (requested via the multiboot headers).  Started
from any shell with the `gui` command.

- **Object model is Wayland-shaped by design** (per the 2026-07-03
  §M22 evaluation): a window owns an off-screen content *surface*,
  output is *committed* by marking damage, input follows a *seat*
  model (keyboard → focused window, pointer → hit-tested window).
  §M26 can put the real wire protocol on top without a rewrite.
- **gfx layer:** `struct gfx_surface` = w/h/stride + ARGB pixel
  buffer, either wrapping the framebuffer (`gfx_fb_surface`) or
  kmalloc-backed off-screen (`gfx_surface_init`).  Primitives (all
  clipped): `gfx_fill`, `gfx_line` (Bresenham), `gfx_blit`,
  `gfx_blend_fill` (src-over alpha — used for window drop shadows),
  `gfx_vgradient`, `gfx_text` (8×8 font re-exported from fb_terminal
  via `fb_font_glyph`).
- **Terminal windows reuse the whole shell stack.**  `struct vc` grew
  an optional `emit` hook: `vc_create_offscreen(emit, ctx)` returns a
  VC outside the split tree whose output bytes flow to the hook
  instead of the FB cell grid.  A window's hook ("gterm") renders
  glyphs into the content surface with its own cursor/scroll state.
  Shell tasks are spawned exactly like `pane split` does —
  `task_spawn(shell_task_entry)` + `task_set_out_console` — so
  shell.c needed zero changes for windows to host shells.
- **Compositor task** ("compositor" in `ps`): sleeps on
  `hal_cpu_idle + task_yield`, wakes on the `need_frame` damage flag,
  recomposes wallpaper → windows (bottom→top: shadow, frame, title
  gradient, content blit, resize grip) → cursor sprite into a
  backbuffer, then pushes one full-screen blit to the FB (no flicker,
  no save-under).
- **Window manager (in the mouse IRQ path):** click = focus + raise +
  `vc_focus` (keyboard follows); title-bar drag = move; bottom-right
  grip drag = wireframe (rubber-band) resize — the surface is
  reallocated once on release, on the compositor task, never in IRQ
  context.  Content is not preserved across resize (same policy as
  pane splits).
- **Locking:** `state_lock` (WM geometry/z-order/drag — IRQ writer,
  compositor snapshots under irqsave) and per-window `win->lock`
  (surface pixels + pointer — shell emit vs. compositor blit vs.
  resize swap).  The two never nest across actors in opposite order.
- **Pane interaction:** `vc_screen_suppress(1)` — while the GUI owns
  the screen, leaf VCs drop their FB rendering (their shells keep
  running) and Alt-N pane switching is disabled.
- **PS/2 mouse:** 8042 aux port, IRQ12, 3-byte packets with bit-3
  sync check, sign extension from byte 0, Y-axis flipped to screen
  convention.  Listener interface (`mouse_set_listener`) mirrors the
  keyboard pipeline so a USB HID mouse can slot in later.
- **Widget toolkit (M22.1, PLAN §M22 stage 6):** flat per-window
  widget list, each widget a struct with `struct widget` as first
  member (label, button, listview with scroll strip + selection +
  double-click activate, single-line textinput with caret + Enter
  submit).  Callbacks run on the COMPOSITOR task, never in IRQ: the
  mouse IRQ enqueues content-relative click events (SPSC ring) and
  the keyboard hook (`vc_set_kbd_hook`) diverts typing to a key queue
  whenever the focused window is an APP window — so widget code may
  freely use the VFS, kmalloc, or open new windows.
- **Two window kinds:** TERMINAL (shell via offscreen VC) and APP
  (widgets; gets a close X button — teardown runs on the compositor
  task, freeing widgets + surface + app ctx, with an optional
  on_close hook for app singletons).
- **Desktop shells are swappable (M22.2):** the chrome (taskbar,
  launcher menu, clock, wallpaper hints) lives behind
  `struct desktop_shell` (desktop.h), registered via
  `DESKTOP_SHELL()` into a linker section and selected by the
  `gui.shell` config key at `gui` time.  Two registrations today:
  **vista** (`shell_vista.c`, default — 34 px taskbar with green
  Start button + menu, one button per open window, RTC clock
  repainting once per second) and **bare** (`shell_bare.c` — no
  chrome at all; apps start via the `launch` shell command).  The
  Start menu is built from the GUI_APP registry — the shell names no
  app; power items (Reboot/Shut Down) are fixed tail entries that
  queue to the same HAL calls the shell commands use.
- **Content-preserving resize (M22.1):** terminal windows keep a
  character backing store (`cells[]`, sized for the largest grid) and
  re-render it into the new surface on resize — if the grid shrinks
  below the cursor row the store scrolls so the tail stays visible.
  App windows re-run their `on_layout` + widget redraw.  Resize stays
  wireframe-style (rubber band, one realloc on release).
- **Apps self-register (M22.2):** `GUI_APP("Name", launch_fn)` drops
  an entry into the `gui_apps` linker section (same pattern as
  MODULE()/DRIVER()); the Start menu and the `launch [app]` shell
  command walk it.  gui.c references no app by symbol — swapping the
  file manager for another implementation is a Makefile-only change.
  Registered today: File Manager, About d-os, New Shell, Hello
  (the documented sample), all under `kernel/gui/apps/`.
- **File manager (`apps/fileman.c`):** singleton app window — path label,
  Up / MkDir / Touch / Del / View buttons, directory listview
  (single-click select, double-click descend/open), name textinput
  (Enter = create file), status line.  Del uses `vfs_unlink` (new in
  M22.1: VFS-level unlink + ramfs implementation, files and empty
  dirs; exFAT still refuses).  View opens a read-only viewer window
  (first 8 KiB, line-split into a listview).
- **CMOS RTC (`cmos_rtc.c`):** MC146818 read with update-in-progress
  double-read guard, BCD + 12h handling.  QEMU is fed
  `-rtc base=localtime` by run_qemu.sh so the clock matches the host.
- **Window lifecycle (M22.3):** every window has minimize (_) and
  close (x) buttons.  Closing a TERMINAL window is a retried state
  machine on the compositor: task_kill the hosted shell (cooperative,
  kthread_stop-style — see task.h), wait for DEAD, task_reap (stack +
  struct reclaimed), vc_destroy (VC slot reusable), then normal
  teardown.  Taskbar buttons follow Windows semantics via
  gui_wm_taskbar_activate_locked (minimized→restore, focused→
  minimize, else→raise); Alt-Tab (raw-keycode hook
  vc_set_raw_kbd_hook, pre-keymap dispatch from both keyboard
  drivers) demotes the active window and activates the next visible
  one.
- **Damage-rect composition (M22.3):** gfx surfaces gained a clip
  box; the compositor accumulates a dirty rect (typing damages one
  window, pointer glides damage two cursor-sized rects, the clock
  damages the chrome strip) and recomposes ONLY that region — correct
  because the backbuffer persists between frames.  `gui stats`
  prints the full/partial frame counters (typing: partial dominates,
  e.g. 1 full / 20 partial).
- **Task manager (`apps/taskman.c`):** GUI_APP singleton; lists every
  task (pid, state, CPU ms — per-task accounting added to the
  scheduler at the context-switch boundary), ~1 Hz auto-refresh via
  the new gui_window_set_tick hook, "End task" button → task_kill
  (compositor guarded by name; pid 0 + idles refused by task_kill).
  CLI siblings: `kill <pid>`, and `ps` grew a CPUMS column.
- **Compositor smoothness (M22.4):** three stacked artifacts fixed /
  bounded.  (1) *Cursor-damage race:* compose() snapshots the damage
  rect BEFORE the WM state, so an IRQ-submitted cursor rect could be
  older than the cursor position actually drawn — erase and redraw
  landed in different frames (flicker/ghosting on glide).  Now the
  compositor keeps `last_cur_x/y` (where it LAST DREW the sprite) and
  unions both the previous and the fresh cursor rects into the clip
  region itself; a pointer glide from the IRQ is a bare `need_frame`
  wake with no rects.  (2) *Drag damage:* a DRAG_MOVE motion damages
  only old-rect ∪ new-rect (with the shadow margin) instead of
  `gui_damage_all()` per motion event — dragging stays on the
  partial-frame path (verified: 52 partial / 5 full over a scripted
  glide+drag).  Press/release and the resize rubber band keep the
  full recompose (rare, z-order/focus changes).  (3) *Tearing:*
  QEMU std-VGA has no vblank/present boundary, so a large blit can
  shear mid-scanout — (1)+(2) shrink typical blits below perception.
  **Superseded by M22.6** (page-flip double buffer) — the residual
  shear is gone; see below.
- **Tear-free presentation + display scaling (M22.6):** two separate
  things were conflated under "the picture wiggles".  (a) *Host-side
  scaling shimmer* — `run_qemu.sh` used `-display cocoa,zoom-to-fit=on`,
  which bilinearly rescales the 1280×800 guest onto a non-integer
  Retina window; every small screen update re-presents the whole
  scaled frame and the interpolation nudges static edges ±1 px, a
  continuous shimmer that tracks mouse motion (NOT compositor tearing:
  a pointer glide only re-blits the ~14×20 cursor rect, the rest of
  VRAM is untouched and cannot move).  Fixed by `zoom-to-fit=off`
  (crisp 1:1).  (b) *Real compositor tearing* — the final present was a
  direct blit into the LIVE scanout buffer.  Now, when the display is
  the Bochs-VBE device (QEMU `-vga std`; DISPI ID 0xB0Cx), the driver
  reserves a second frame's worth of VRAM (DISPI VIRT_HEIGHT = 2×H,
  ~4 MiB extra, fits the 16 MiB default) and the compositor composes
  into the hidden buffer, then pans the scanout origin to it in one
  register write (DISPI Y_OFFSET) — a hardware page flip, no vblank
  IRQ needed, QEMU only ever scans out a complete buffer.  The
  page-flip has *buffer age 2* (ping-pong), so each present copies
  `dirty_N ∪ prev_dmg` from `backsurf` (always a complete frame) into
  the hidden buffer to keep it consistent.  Graceful fallback: on a
  non-Bochs display (real hardware / plain VESA) `fb_flip_init` fails
  and the compositor keeps the single-buffer direct blit.  API:
  `fb_flip_init` / `fb_flip_to` (fb_terminal.c); enable log line
  `gui: page-flip present enabled`.
- **1920×1200 desktop (M22.6).**  The multiboot header (both
  `kernel/hal/x86*/boot.s`) now requests 1920×1200×32.  Two knock-on
  requirements: (a) *VRAM* — two 1920×1200 frames are ~18.4 MiB, over
  the std-VGA 16 MiB default, so `run_qemu.sh` creates the display with
  `-vga none -device VGA,vgamem_mb=32` (note: `-global VGA.vgamem_mb=`
  is silently ignored — it does not match the auto-created device).
  Without the bump `fb_flip_init` clamps and falls back to the tearing
  single-buffer path.  (b) *Heap* — a full-screen surface is 9.2 MiB,
  and `gfx_surface_init` needs it contiguous.  `BUDDY_MAX_ORDER` was 10
  (4 MiB max single alloc), so it is raised to 12 (16 MiB).  Power-of-2
  rounding then wastes up to ~7 MiB per full-screen surface — fine for
  the handful of them (backbuffer + wallpaper + maybe one maximized
  window); a vmalloc-style scatter map would remove the waste (noted in
  pmm.h).  `run_qemu.sh` also sets `-m 256M` (past QEMU's 128 MiB i386
  default once these surfaces are allocated).
- **Terminal window auto-close on hosted-task death (M22.6).**  A
  WIN_TERM window hosts a task via `win->vc->task` (shell, or a
  `gui_window_create_task` app like BASIC).  Previously only the X
  button tore the window down; a shell killed *externally* (Task
  Manager "End task", CLI `kill`, or the task returning from its entry)
  died but left its inert, un-typeable window on screen.  Now the
  compositor's `apply_pending` flags such a window for close as soon as
  its hosted task reaches **TASK_DEAD** — reusing the existing
  want_close teardown (kill+reap+vc_destroy+destroy_window).  The
  trigger is *actual death*, not the kill request: a task merely FLAGGED
  to stop (kill_pending set, still RUNNABLE until its next yield) keeps
  its window until it truly terminates — the "instruction to stop" vs
  "has stopped" distinction.  Because the teardown reaps the task, the
  row also drops off the Task Manager within a frame (the taskman's own
  reap pass still skips vc_task_bound tasks so it never races the
  window teardown for the same pid).  Log line: `gui: window '…'
  auto-closing (hosted pid N died)`.
- **Task lifecycle → Task Manager (M22.4):** `task_set_change_hook`
  (task.h) fires on spawn/kill/exit/reap; the GUI installs a hook
  that makes the compositor run every window's on_tick immediately,
  so a closed/killed program leaves the Task Manager list within one
  frame instead of at the next 1 Hz beat.  Each taskman refresh also
  starts with an opportunistic reap pass: DEAD tasks not bound to any
  VC (`vc_task_bound`) are `task_reap`ed, so closed programs drop off
  the list instead of accumulating as DEAD rows.  VC-bound DEAD tasks
  (a pane shell killed by hand) stay listed on purpose — their
  vc->task pointer is still owned by the pane/window teardown path.
- **Navigation keys end-to-end (M22.5):** the PS/2 driver now decodes
  the E0-prefixed cursor cluster (arrows, Home/End, PgUp/PgDn,
  Insert/Delete, keypad Enter) into HID usages — same wire format the
  USB HID driver already produced.  The GUI's raw-keycode hook
  (the Alt-Tab hook) consumes them (plus Ctrl+letter combos) whenever
  the focused window is an APP window and queues them to the
  compositor, which delivers them to the focused widget through the
  new `widget_ops.keycode(w, kc, mods)` callback.  Widgets also
  gained an optional `destroy` op (free owned heap objects at window
  teardown).  Listviews take keyboard focus on click and navigate
  with arrows/PgUp/PgDn/Home/End; Enter activates like double-click.
- **Multiline editor widget (M22.5, `w_editor.c`):** contiguous
  grow-by-doubling text buffer, implicit '\n' lines, byte-offset
  cursor + selection anchor (Shift+movement extends, unshifted drops),
  sticky preferred column for vertical motion, viewport tracking with
  horizontal scroll (no wrapping), mouse click-to-position.
  Ctrl+C/X/V talk to the kernel clipboard (no selection = whole
  line), Ctrl+A selects all; unclaimed Ctrl+letters forward to the
  app via `on_shortcut`.  O(len) line math — fine at teaching-kernel
  file sizes; gap buffer is the known upgrade path.
- **Kernel clipboard (M22.5, `clipboard.c`):** one global text slot
  (spinlocked, 64 KiB cap) behind clipboard_set/get/len — used by the
  editor widget and the single-line textinput (Ctrl+C/X/V).
- **Editor app (M22.5, `apps/editor.c`):** path bar + Open/Save
  buttons around a w_editor; Ctrl+S saves, Ctrl+O loads; save-as =
  edit the path, then Save (vfs_open with CREATE|TRUNC).  NOT a
  singleton — two files edit side by side.  Retitles its window to
  the open file (new `gui_window_set_title`).
- **Tiny-BASIC (M22.5, `kernel/core/basic.c` + `apps/basic.c`):**
  line-numbered dialect (PRINT/INPUT/LET/IF..THEN/GOTO/GOSUB/RETURN/
  FOR..NEXT/REM/CLS/END, integer vars A–Z, RND/ABS), recursive-descent
  expressions, REPL with RUN/LIST/NEW/LOAD/SAVE/BYE.  The GUI app is
  a TERMINAL window hosting the interpreter task instead of a shell
  (new `gui_window_create_task`) — output/input reuse the whole gterm
  + VC plumbing, closing the window kills the interpreter under the
  kthread contract (basic_run polls task_should_stop + yields every
  64 statements).  CLI sibling: `run <path.bas>`.  Interpreter, not
  codegen — the compile story arrives with §M25 userland.
- **File manager 2.0 (M22.5):** editable path bar (Enter navigates),
  size column + dirs-first name-sorted listing, keyboard navigation,
  Ren/Copy buttons (new `vfs_rename` — same-directory, inode_ops
  `rename` op, ramfs implements, exFAT defers; `vfs_copy` with a
  self-copy guard), Del deletes files immediately and arms a two-step
  confirm for non-empty directories (second press within 8 s runs the
  new `vfs_unlink_recursive`, depth-capped at 8 with a shared path
  buffer).  Double-click/Enter consults the file-type association
  registry: `GUI_APP_ASSOC(name, launch, open_path, "ext ext ...")`
  extends GUI_APP; `gui_app_for_path` matches the extension —
  .txt/.conf/.md/.cfg/.log open in the Editor, .bas in BASIC,
  unclaimed types fall back to the read-only viewer.
- **Maximize/restore (M22.5):** third title-bar button (□ between _
  and x) or double-click on the title bar; saved normal geometry,
  work-area aware (fills the screen minus the shell's bottom
  reserve); move/resize disabled while maximized; the geometry change
  rides the pending-resize handoff so the surface realloc stays on
  the compositor task.
- **Known limits (deferred):** no widget nesting/containers; killing
  a CPU-bound kernel thread requires it to poll task_should_stop()
  (the kthread contract — forced kill needs ring-3 processes, §M25);
  cursor is IRQ-latency bound (one tick worst case); textinput has no
  in-line cursor (caret at end); exFAT lacks rename/unlink; terminal
  scrollback and fileman icon/tree views tracked separately.

---

### 4.14 GUI development — writing apps and desktop shells (M22.2)

The GUI is layered so both the desktop and the applications are
replaceable registrations, mirroring the driver framework:

```
  apps (kernel/gui/apps/*)        desktop shells (shell_vista/bare)
      │  GUI_APP() registry            │  DESKTOP_SHELL() registry
      ▼                                ▼
  gui.h + widget.h  ◄──────────  desktop.h + gui_internal.h
      │                                │
      └───────────►  gui.c — compositor + WM core  ◄───────────┘
                          │
                     gfx.h (surfaces + primitives)
```

**Threading rules (memorize these three):**
1. Widget callbacks (`on_click`, `on_activate`, `on_submit`, key
   handlers) and app `launch` functions run on a normal TASK (the
   compositor, or a shell task via `launch`).  VFS, kmalloc and
   window creation are all fine there.
2. Desktop-shell `click`/`motion` run in the MOUSE IRQ with the WM
   lock held: shell-local state + `*_locked` services +
   `gui_queue_*` only — never allocate, never call an app.
3. Shell `draw`/`second_tick` run on the compositor task; slow I/O
   (RTC ports) belongs in `second_tick`.

**Writing an app** (the complete `apps/hello.c` pattern):
1. `#include "gui.h"`, `"gui_app.h"`, `"widget.h"`.
2. A launch function: `gui_app_window_create(title, x, y, w, h,
   on_layout_or_NULL, ctx_or_NULL)` + `w_label_create` /
   `w_button_create` / `w_listview_create` / `w_textinput_create`
   + `gui_window_request_redraw(win)`.
3. `GUI_APP("Menu Label", launch_fn);` at the bottom.  Add the .c to
   the Makefile — done: it appears in the Start menu and `launch`.
- The window kfree's `ctx` on close; use `gui_window_set_on_close`
  to clear app singletons (see fileman.c / about.c).
- `on_layout` repositions widgets from
  `gui_window_content_size(win, &w, &h)` after every resize.
- **File-type association (M22.5):** register with
  `GUI_APP_ASSOC("Label", launch_fn, open_path_fn, "txt md")` instead
  — the file manager double-click resolves extensions through
  `gui_app_for_path()` and calls your `open_path(abs_path)` (runs on
  the compositor task, VFS is fine).  See apps/editor.c.
- **Keyboard (M22.5):** printable chars arrive at the focused
  widget's `key` op; arrows/Home/End/PgUp/PgDn/Delete + Ctrl+letter
  combos arrive at the `keycode(w, kc, mods)` op (KC_* from
  keymap.h).  A widget owning heap memory frees it in the `destroy`
  op.  For a text area, embed `w_editor_create` (see apps/editor.c —
  selection/clipboard/scrolling come for free; app-level shortcuts
  like Ctrl+S via `on_shortcut`).
- **Terminal-style apps (M22.5):** `gui_window_create_task(title,
  x, y, w, h, task_name, entry)` gives you a terminal window running
  YOUR task instead of a shell: kprintf lands in the window, read
  keys with `vc_getchar(task_current()->out_console)`, poll
  `task_should_stop()` in loops (window close kills you
  cooperatively).  See apps/basic.c.

**Writing a desktop shell** (template: `shell_bare.c`):
1. `#include "desktop.h"`, `"gui_internal.h"`, `"gui_app.h"`.
2. Fill a `struct desktop_shell` (any callback may be NULL) and
   register with `DESKTOP_SHELL(name) = { ... };`.
3. `bottom_reserve()` carves the work area; `draw(back)` paints
   chrome after the windows, below the cursor; `click(x,y)` returns
   non-zero to consume; build launchers from `gui_app_count()` /
   `gui_app_at()` and start them with `gui_queue_launch()`.
4. Select with `setconf gui.shell <name>` before `gui` (persist via
   `saveconf`).

**Testing:** the QEMU-monitor pattern from §5 (sendkey / mouse_move /
mouse_button / screendump) drives the whole GUI headlessly — see the
M22 change-log entries for the exact scripts used.

---

### 4.15 Process model — init, hierarchy, reaper, kill-tree (M27)

Before M27 tasks were a flat set with no parentage, and a DEAD task was
only reclaimed if the Task Manager happened to be open (its refresh
reaped) or a GUI window tore down its own shell — an exited background
kernel thread otherwise leaked as a permanent DEAD entry.  M27 gives
tasks a real parent/child model and a universal reaper.

**`struct task` additions.**
- `ppid` — parent pid (a stable int, never a dangling pointer).  Set at
  spawn to the caller's pid (or 0 very early in boot).
- `exit_code` — recorded by `task_exit_code(int)`; `task_exit()` is the
  code-0 wrapper.  Shown by `ps` / logged by init on reap.
- `reap_owned` — "a subsystem owns this task's reap; init keep out."
  The GUI sets it on window shells (it reaps them in its own teardown);
  init's universal reaper skips them so the two never race for the same
  struct.  Replaced the taskman's old `vc_task_bound()` reap gate.

**init — the universal reaper (`task_start_init`, called from
kernel_main before the shell).**  A tiny always-on task (the first thing
kernel_main spawns) that sweeps DEAD, non-`reap_owned` tasks at ~100 Hz
(hlt-then-yield, the same idle-loop shape as the compositor — cheap when
quiet, effectively event-driven since DEAD tasks are rare).  On each
reap the victim's surviving children re-parent to init (in `task_reap`,
under master_lock, before the struct is freed) so no ppid dangles.
**pid 0 (the boot "swapper" — kernel_main task_exit()s after boot) and
init itself are explicitly skipped**, matching the Unix convention that
those roots are permanent (and avoiding an alarming "reaped kernel" log).

**`task_kill_tree(pid)`** — cooperative termination of a pid *and all
descendants*.  The subtree is grown to a fixpoint under master_lock
(each pass adopts tasks whose parent is already marked), then the pids
are flagged after the lock is released (task_kill takes master_lock, so
flagging under it would self-deadlock).  Still the kthread contract:
each victim dies at its next yield.  The GUI window close uses kill-tree,
so closing a shell window takes anything that shell `spawn`ed down with
it instead of orphaning it.

**`task_spawn_detached()`** — like `task_spawn` but parents the new task
to **init**, not the caller.  An independent/daemon task: not in the
caller's subtree, survives the caller's death, immune to a kill_tree on
the caller.  (The substrate for M29 services; unused until then.)

**Visibility.**  `ps` and `/proc/tasks` grew a **PPID** column; the Task
Manager renders a real process **tree** (children indented under their
parent — a heap-side snapshot walked into tree order, so it never nests
`task_for_each` under the master lock, and the 96-row buffer lives in
the taskman struct, not on the compositor's 4 KiB stack).

**Scope note — death goes down, notification goes up.**  kill-tree
propagates termination downward; a child dying does NOT kill its parent.
The *upward* half (a parent being notified + applying policy on child
death — the supervision-tree / `wait()` pattern) and *freeze detection*
(a heartbeat watchdog for a task that is alive but wedged) are separate
problems, deferred to §M29 (services supervisor) and §M31 (watchdog).

---

### 4.16 Per-task GUI apps (M22.7 Stage A)

Before M22.7 the compositor ran every APP window's callbacks — widget
hit-test, key/mouse handlers, the ~1 Hz tick, and the redraw — on its
OWN task.  So apps were not processes (invisible in the Task Manager,
not independently killable) and a slow app handler (a big directory
read, say) froze the entire GUI.  M22.7 gives each WIN_APP window its
own **app-host task**; the compositor becomes a surface-compositor +
input router.

**Launch → host.**  The taskbar and the `launch` command both enqueue
via `gui_queue_launch`; the compositor's `dispatch_launches` spawns an
`app:<name>` task (`task_spawn_arg`, passing the app's open fn through
`start_arg`) and marks it `reap_owned`.  `app_host_main` reads the open
fn, runs it (so the window + widgets are created ON the host task), then
loops over the window(s) that task owns.  Calling the open fn on the
caller (the old path) would run it with no event loop — hence the
`launch` command had to switch from `app->launch()` to the queue.

**Input + render off the compositor.**  `dispatch_events/keys/keycodes`
no longer touch widgets; they push into the target window's per-window
ring `win->aq` (SPSC: compositor produces, host consumes).  The host
does the widget hit-test + dispatch (`app_dispatch_event`) and
`app_redraw` (widgets → `win->surf` under `win->lock`).  The compositor
still blits `win->surf` under the same lock — unchanged.  `on_tick` /
`on_layout` are now `tick_pending` / `layout_pending` flags the
compositor raises and the host consumes (so a slow tick can't stall
compositing).

**Teardown — a two-actor dance.**  On `want_close` the host runs
on_close, frees its widgets + app_ctx (`app_widgets_free`), and sets
`host_released`; the compositor then disposes the window struct
(`destroy_window` skips the on_close/widget-free it already did) and
reaps the host once its last window is gone (`reap_gui_host`).  Because
the host is `reap_owned`, init leaves it to the compositor.  Edge cases:
a host killed externally (host_task DEAD without releasing) → the
compositor does the cleanup itself; a singleton whose open fn only
raised an existing window creates nothing and exits immediately → a
`reap_dead_gui_hosts` sweep (on task-set change) reaps it.

**Concurrency.**  `window_alloc` now claims its pool slot under
`state_lock` (multiple hosts create windows concurrently); all fields
are set before `used = 1` (the last store — x86 TSO needs no barrier).
Widgets are touched ONLY by the owning host, so no widget lock is needed
— `win->lock` guards just the surface, shared with the compositor's
blit.  A multi-window app (File Manager + its viewer) is one host
driving several windows; it exits when they all close.

**Verified (i386 + x86_64):** About / Task Manager / File Manager launch
as `app:<name>` tasks and render off the compositor (the Task Manager
tick populated its list — text rendered; the File Manager showed its
directory listing); the X button tears a window down cleanly (host
cleanup + reap, no fault); apps now show in the Task Manager as tasks.

**Stage B — the desktop shell / taskbar as its own task.**  The
`desktop_shell` (taskbar, launcher, clock) used to run on the
compositor: it drew chrome onto the backbuffer and its
click/motion/second_tick ran there.  It now runs on a dedicated
**`desktop` task** that renders into a full-screen **`panelsurf`** at
screen coordinates — so shell_vista's draw/click/motion code is
*unchanged*.  The compositor composites only the OPAQUE parts of
panelsurf on top of the windows: the taskbar strip (always) plus the
launcher popup rect while open (the rest never occludes).  The shell
publishes its popup extent through `gui_panel_set_popup`; the
compositor uses it to composite the popup *and* to route input
(`in_panel_region`).  Clicks/motion over the chrome go to a `pevq` the
desktop task drains, running shell->click/motion under `state_lock`
(their old IRQ-held contract, now honoured by the panel task).  The
clock (RTC `second_tick`) and every chrome redraw happen on the desktop
task; the compositor no longer calls the shell.  Result: **the
compositor is a pure surface-compositor + input router; windows, apps,
and the panel are each drawn by their own task** — the M26 Wayland
shape with the internal API.  (Caveat: the `bare` shell reserves no
strip, so its hint line is no longer composited — fine for a rescue
shell.)  Verified i386 + x86_64: taskbar + Start menu render, a menu
item launches an app as its own host, no fault.

---

### 4.17 ARM64 (AArch64) port — Phases A–M (M21, full x86 parity)

The third architecture, and the real HAL-portability torture test: no
port I/O (every device is MMIO), a GIC instead of the APIC, and
exception levels (EL1 kernel / EL0 user) instead of privilege rings.
Like the x86_64 port (M20 → M20.6), it lands in phases; **Phase A** is
the boot + arch-essentials foundation.

**Boot model — nothing like the x86 ports.**  There is no GRUB and no
multiboot.  QEMU's `-M virt` machine loads the raw kernel ELF via
`-kernel`, copies its PT_LOAD segments to physical RAM (`virt` RAM base
= 0x40000000), and jumps to the ELF entry with the MMU off, caches
cold, at EL1 (or EL2 with `virtualization=on`).  `linker-aarch64.ld`
links at **0x40080000** (just above the RAM base; the low 512 KiB is
left for QEMU's boot shim + DTB) and exports `__bss_start/__bss_end`
and `__stack_top` for the assembler.

**Files (`kernel/hal/aarch64/`):**

| File | Role |
|------|------|
| `boot.S` | Reset entry `_start`.  Reads `CurrentEL`; if EL2, sets `HCR_EL2.RW`=1 (EL1 is AArch64), grants EL1 the arch timer (`CNTHCTL_EL2` bits 0–1, `CNTVOFF_EL2`=0), loads an MMU-off `SCTLR_EL1` (0x30d00800, RES1 bits set), and `eret`s to EL1h.  Then SP←`__stack_top`, zeroes `.bss`, `bl aarch64_main_entry(dtb)`. |
| `uart.c` | PL011 UART at MMIO 0x09000000 — dependency-free polled console (ARM analogue of the x86 boot.s inline COM1 print).  `uart_early_{putc,puts,puthex}`.  No baud/line-control setup — QEMU's chardev ignores it, same as the x86 COM1 trick. |
| `vectors.S` | The architecturally-fixed EL1 vector table: one 2 KiB-aligned block of 16 × 128-byte slots (4 groups {Sync,IRQ,FIQ,SError} × {SP0, SPx, lower-EL64, lower-EL32}).  Each slot saves a 272-byte trapframe (x0–x30 + ELR_EL1 + SPSR_EL1) and tail-calls the C dispatcher. |
| `exceptions.c` | Installs the table into `VBAR_EL1`; the dispatcher dumps ESR_EL1/FAR_EL1/ELR + halts on Sync/SError, and routes IRQ to a **weak** `aarch64_irq_dispatch` hook (Phase B fills it in). |
| `mmu.c` | Stage-1 identity map.  4 KiB granule, 39-bit VA (`TCR_EL1.T0SZ`=25) so the TTBR0 walk starts at level 1, where each entry is a **1 GiB block** — a single 512-entry L1 table maps everything with no lower levels.  Index 0 = Device-nGnRnE (peripheral window: UART + GIC), 1..3 = Normal WB inner-shareable RAM.  `MAIR_EL1` slot 0 = 0x00 (device), slot 1 = 0xFF (normal WB).  Then `SCTLR_EL1.{M,C,I}` turn the MMU + caches on. |
| `gic.c` | **(Phase B)** GICv2 driver — distributor (GICD @0x08000000) + CPU interface (GICC @0x08010000).  `gic_init`, `gic_enable_irq`, `gic_register_handler`, and the strong `aarch64_irq_dispatch` (ack→dispatch→EOI).  The ARM half of the IRQ-install API (replaces the x86 IOAPIC routing). |
| `timer.c` | **(Phase B)** ARM architected generic timer — the non-secure EL1 physical timer (CNTP_*), INTID 30 on `virt`.  `timer_init(hz)` arms + starts it; the ISR re-arms per interrupt (no auto-reload reg) and bumps a monotonic tick.  `timer_ticks{,_ms}()`, `timer_raw_count()` (CNTPCT — the TSC analogue). |
| `switch.S` | **(Phase C)** `context_switch(save_sp, new_sp)` — pushes the 12 AAPCS64 callee-saved regs (x19–x30), swaps SP, pops, `ret` to the restored LR.  The ARM analogue of the x86 switch.s. |
| `task_arch.c` | **(Phase C)** `hal_task_init_stack` — synthesises a brand-new task's stack frame (LR = `task_trampoline`, x19 = entry) matching switch.S's layout; the trampoline recovers x19, releases the first-switch lock, unmasks IRQs, runs entry. |
| `hal_arch.c` | **(Phase C)** the arch-independent HAL (hal_api.h): `hal_intr_*` via PSTATE.DAIF, `hal_cpu_*` via wfi/yield, `hal_arch_early_init` (= exceptions + MMU), `hal_extend_identity_map`, `hal_syscall_exit_to_kernel` placeholder. |
| `stubs.c` | **(Phase C)** UP glue so the stock core links: single-CPU `lapic_id`/`acpi_*`/`smp_*` stubs, a synthesised `struct mboot_info` + AVAILABLE mmap entry for the `virt` RAM (so pmm.c's mmap walk works), and the PL011 `console_sink` registration (kprintf → serial). |
| `lib.c` | **(Phase C)** freestanding `mem{set,cpy,move,cmp}` (gcc emits calls to these on ARM) + a `__getauxval` stub for libgcc's LSE-atomics init. |
| `serial_shell.c` | **(Phase D)** the interactive REPL — runs as a scheduler task, reads lines from the PL011 (poll + task_yield), and drives the portable services: help/echo/clear, meminfo/free (PMM), uptime, ps, and the ramfs (ls/cat/mkdir/write/rm). |
| `smp.c` + `smp_entry.S` | **(Phase E)** SMP via PSCI — `PSCI_CPU_ON` HVC starts each secondary core (`smp_entry.S` trampoline → `smp_secondary_main`: MMU on, VBAR, `gic_cpu_init`, `percpu_init_ap`, `task_install_ap_idle`, own timer).  Provides the percpu.c topology hooks (`lapic_id` = MPIDR.Aff0) + a GIC-SGI `smp_send_reschedule`. |
| `virtio_mmio_blk.c` | **(Phase F)** virtio-blk over the virtio-MMIO transport (modern/version-2): slot scan, feature negotiation, one split virtqueue, polled 512-byte sector read/write; registers `/dev/vda` with the stock block layer.  The ARM counterpart of the PCI `virtio_blk.c` (no port I/O). |
| *(no new arch file)* | **(Phase G)** exFAT — the stock `block_cache.c` + `fs/exfat.c` link + run unchanged (arch-independent); `main_entry` runs `bcache_init()` + `vfs_mount("exfat", "/mnt", "vda")`.  The shell's ls/cat/write/rm then hit real, persistent disk under /mnt. |
| `dtb.c` | **(Phase H)** minimal big-endian FDT/device-tree parser — locates the DTB (x0 → fixed load addr → RAM scan) and extracts the `/memory` reg (RAM base+size) + `/cpus/cpu@*` count.  `dtb_ram_size()` then drives the PMM map size (stubs.c) instead of a hard-coded constant. |
| `pci.c` | **(Phase M)** PCIe access via ECAM (config space at MMIO 0x40_1000_0000) — `pci_read/write*` + `pci_scan` with BAR assignment from the 32-bit MMIO window (no firmware on the raw `-kernel` boot).  Same pci.h API as x86 pci.c, so the stock `xhci.c` (MMIO + polled from the timer ISR) + `usb_hid.c` link unchanged → USB HID keyboard on `-device qemu-xhci -device usb-kbd`. |
| `virtio_input.c` + `pl031_rtc.c` | **(Phase J/K)** GUI input + clock.  `virtio_input.c`: keyboard (evdev keycode → HID usage → shared keymap → `vc_kbd_push`) + mouse (REL deltas + buttons → `mouse_set_listener`, the seam gui.c registers on) over virtio-MMIO input devices; drained by a poll task.  `pl031_rtc.c`: the ARM PL031 RTC (`rtc_read`, epoch-seconds → civil date) for the taskbar clock (QEMU `virt` has no CMOS).  With these the portable `vc.c` + `shell.c` + the whole M22 `gui.c`/widgets/apps link and run on ARM (the `gui` command). |
| `vmm.c` + `usermode.S` + `syscall.c` | **(Phase L)** EL0 userspace substrate — the M25 prerequisite.  `vmm.c`: per-process TTBR0 address spaces (private L1 table with the kernel's low-4-GiB identity blocks copied in) + page-granular EL0 mappings (`aarch64_vmm_map_user`, AP=01 + PXN, UXN cleared only for code) + `aarch64_vmm_switch`.  `usermode.S`: `aarch64_enter_user` (stash kernel SP/LR → set SP_EL0/ELR/SPSR → `eret` to EL0) + the SYS_EXIT teleport `aarch64_user_exit` + a PC-relative `user_stub`.  `syscall.c`: the SVC dispatcher (x8=number, x0..x5=args, shared `syscall.h` numbers) servicing SYS_PRINT/SYS_EXIT + the `usertest` self-test.  The ARM analogue of x86's M6/M20.5 ring-3 + `int 0x80`. |
| `virtio_gpu.c` | **(Phase I)** virtio-gpu (2D) over the virtio-MMIO transport — the ARM framebuffer (QEMU `virt` has no VGA/Bochs-VBE/VRAM BAR).  Same modern-transport handshake + control virtqueue as the blk driver; brings up a 1280×800 B8G8R8X8 scanout backed by a contiguous RAM framebuffer (`pmm_alloc_contiguous`), then hands it to the PORTABLE `fb_terminal.c` via `fb_term_init_direct()`.  Implements the `fb_present` backend: `fb_present_map` = no-op (RAM already mapped), `fb_present_flush` = `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` of the dirty rect.  Net: the same 8×8-font console x86 uses now renders the boot log + interactive shell graphically on ARM. |
| `main_entry.c` | Own bring-up (aarch64 does NOT share the x86-coupled `kernel_main`): banner + EL report, `hal_arch_early_init()` **(A)**, serial console + PMM + kmalloc **(C)**, `virtio_gpu_init()` (framebuffer console) **(I)**, `task_init()` **(C)**, `gic_init()` + `timer_init(100)` **(B)**, a quick preemption check **(C)**, `vfs_init()` + `module_init_all()` (ramfs at /) then the EL0 `aarch64_usertest()` **(L)**, and spawns the serial shell **(D)**; pid 0 → idle. |

**Build.**  A separate `Dockerfile.aarch64` image carries the
`aarch64-linux-gnu` cross toolchain — Ubuntu's cross gcc declares a
hard `Conflicts:` against `gcc-multilib` (needed for the i386 `-m32`
build), so the two cannot share one image.  The Makefile `ARCH=aarch64`
branch uses the cross gcc/ld, `-mgeneral-regs-only` (no FP/NEON — we
don't save the SIMD file, mirroring x86_64 `-mno-sse`), and assembles
`.S` through the C compiler (cpp + gas, no nasm).  `scripts/build.sh`
picks the aarch64 image + the `kernel` target (raw ELF, no ISO);
`scripts/run_qemu.sh` boots `-M virt -cpu cortex-a72 -nographic
-kernel build/aarch64/kernel.bin`.

**Phase B — interrupt controller + periodic timer.**  The ARM analogue
of the x86 IOAPIC + PIT/LAPIC-timer:
- **`gic.c` — GICv2.**  Two banks: the global *distributor* (enable /
  priority / CPU-target / config per INTID) and the per-CPU *CPU
  interface* (priority mask + the IAR/EOIR ack handshake).  INTID
  ranges: 0–15 SGIs (IPIs), 16–31 PPIs (per-CPU, banked), 32+ SPIs
  (shared).  `gic_init` enables the CPU-IF (PMR=0xF0 all-pass) + the
  distributor; `gic_enable_irq(intid)` unmasks a line; the strong
  `aarch64_irq_dispatch` reads GICC_IAR → runs the handler → writes
  GICC_EOIR.  This is the ARM half of the "IRQ install API".
- **`timer.c` — generic timer.**  System-register timer (no MMIO):
  CNTFRQ_EL0 (rate, 62.5 MHz on `virt`), CNTPCT_EL0 (free-running
  counter), CNTP_TVAL_EL0 (down-counter, fires at 0), CNTP_CTL_EL0
  (enable/mask).  We use the NS EL1 physical timer → `virt` PPI 14 =
  INTID 30 (EL1 access was granted in boot.S).  No auto-reload register,
  so the ISR rearms TVAL each interrupt — the standard architected-timer
  tick.  A monotonic `tick_count` is the Phase-C scheduler quantum base.
- **IRQ unmask** = `msr daifclr, #2` (the `sti` analogue).
- **`run_qemu.sh`** pins `-M virt,gic-version=2` so the hard-coded GIC
  MMIO layout always matches (newer QEMU may default the board to v3).

**Verified (serial log):**
```
=== d-os AArch64 (M21 Phase A+B) ===
aarch64: booted at EL1
aarch64: exception vectors installed (VBAR_EL1)
aarch64: MMU + caches enabled (identity map)
aarch64: RAM read-back = 0xd05cafe5d05cafe5  [OK]
aarch64: GICv2 initialised (GICD @0x08000000, GICC @0x08010000)
aarch64: generic timer armed (CNTP, INTID 30)
aarch64: timer tick milestone = 0x...0064   (1 s)
aarch64: timer tick milestone = 0x...00c8   (2 s)
aarch64: timer tick milestone = 0x...012c   (3 s)
aarch64: timer self-test PASS (300 periodic IRQs).
```
The post-MMU read-back proves the identity map + cache attributes; the
tick milestones prove the full IRQ path (GIC delivery → EL1 IRQ vector
→ dispatcher → timer ISR → EOI) fires periodically with no fault.

**Phase C — preemptive scheduler + memory manager.**  The kernel's heart
on ARM.  aarch64 runs its OWN bring-up (`main_entry.c`) rather than the
x86-coupled shared `kernel_main`, calling the *portable* core directly:
- **Context switch** (`switch.S` + `task_arch.c`) over the 12 AAPCS64
  callee-saved registers; a brand-new task's frame carries LR =
  `task_trampoline`, x19 = entry.
- **Full HAL** (`hal_arch.c`) — DAIF-based interrupt masking, wfi/yield
  CPU control, identity-map hook.
- **Memory** — the stock `pmm.c`/`slab.c`/`kmalloc.c` run unchanged, on
  two enablers: `BUDDY_MAX_FRAMES` at the 4 GiB cap for aarch64 (RAM is
  at pfn 0x40000, past the 1 GiB cap) and a synthesised multiboot mmap
  (`stubs.c`) for the `virt` RAM so the mmap-walking pmm needs no ARM
  awareness.
- **Scheduler** — the stock `task.c`/`percpu.c`/`lock.c` link with UP
  stubs (`lapic_id`→0, `acpi_*`→1-CPU, `smp_*`→no-op).  The timer ISR
  calls `schedule_request`; the GIC IRQ-exit calls `schedule_check`.
- **Freestanding libc** (`lib.c`) — `mem*` + `__getauxval`; built with
  `-mno-outline-atomics -fno-tree-loop-distribute-patterns`.

**Verified (serial log):**
```
pmm: buddy ready — ... NORMAL managed=64976 free=64976 (253 MiB total free)
kmalloc: slab + page_alloc backend ready
aarch64: kmalloc self-test a=0x402b03e8 b=0x402b03a8 c=0x402b03e8 [reuse=yes]
task: pid 0 (kernel) installed
aarch64: spawning two never-yielding hog tasks...
aarch64: hogA=501036773 hogB=509458588
aarch64: scheduler self-test PASS (both tasks ran — timer preemption works).
```
Both never-yielding hogs making ≈equal progress proves the timer IRQ
preempts and the context switch is correct.

**Phase D — interactive serial shell + filesystem.**  The x86 `shell.c`
reads from a framebuffer VC and its commands are welded to x86-only /
not-yet-ported subsystems (GUI/VC, ring-3 usermode, vmm.c, block/USB),
so a dedicated **serial shell** is brought up instead:
- **`uart.c` `uart_early_getchar`** — non-blocking PL011 RX; the shell
  polls + `task_yield()`s while idle (timer preemption stays live).
- **`serial_shell.c`** — a REPL on an ordinary scheduler task driving the
  portable services: `help`/`echo`/`clear`, `meminfo`/`free` (PMM stats),
  `uptime`, `ps` (task_for_each), and the ramfs (`ls`/`cat`/`mkdir`/
  `write`/`rm`).
- **VFS + ramfs** — the stock `vfs.c` + `ramfs.c` (+ `block.c` for symbol
  closure, `module.c` for the registry) link unchanged; `vfs_init()` +
  `module_init_all()` mount ramfs at `/`.

**Verified (scripted REPL over the UART):**
```
d-os> ls /
  mnt/  proc/  tmp/  dev/  etc/
d-os> mkdir /foo → write /foo/a.txt hello-from-arm64 → ls /foo
  a.txt
d-os> cat /foo/a.txt
hello-from-arm64
d-os> ps        (shell = current pid, + idle + kernel)
d-os> meminfo   memory: ... free 259856 KiB
```

**Phase E — SMP via PSCI.**  The torture test of the "SMP-ready on UP"
abstraction on a third arch: the STOCK per-CPU runqueue + load balancer +
`percpu.c` now drive secondary cores on ARM.
- **`smp.c`** — `PSCI_CPU_ON` (HVC to QEMU's emulated PSCI) starts each
  secondary vCPU; no INIT-SIPI-SIPI / no low-memory trampoline.  Provides
  the percpu.c topology hooks (`lapic_id` = MPIDR.Aff0, linear ACPI
  topology) so the stock apic_id→index map works, and a GIC-SGI
  `smp_send_reschedule`.
- **`smp_entry.S`** — the secondary entry: MMU-off SCTLR, per-CPU stack
  from `ap_sp[]`, call `smp_secondary_main(cpu)`.
- **`smp_secondary_main`** — MMU on FIRST (cache coherency before any
  lock), VBAR, `gic_cpu_init` (banked GICC + PPIs are per-CPU),
  `percpu_init_ap` + `task_install_ap_idle`, own generic timer.
- Enablers: `mmu.c` split into build-once + `mmu_enable_this_cpu`; `gic.c`
  split out `gic_cpu_init`.  Configurable via `AARCH64_MAX_CPUS` + `-smp`.

**Verified (serial log):**
```
percpu: 2 CPUs known, BSP at slot 0 (apic_id=0)
aarch64: secondary CPU 1 online
aarch64: SMP — 2 CPU(s) online
aarch64: preemption OK (hogA=... on CPU1, hogB=... on CPU0)
aarch64: parallelism PASS (2 CPUs online; hogs on CPU1 + CPU0)
```
Two never-yielding hogs ending up on two DIFFERENT cores proves genuine
parallel execution driven by the stock load balancer.

**Phase F — virtio-MMIO block device.**  The ARM proof of "every device is
MMIO": the PCI `virtio_blk.c` is meaningless here, so `virtio_mmio_blk.c` is
a fresh driver for QEMU `virt`'s virtio-MMIO transport (32 slots at
0x0a00_0000).  It runs the modern (version-2) init handshake, sets up one
split virtqueue (desc/avail/used via the Desc/Driver/Device Low/High
registers), and does polled synchronous 512-byte sector read/write
(3-descriptor requests).  Registers `/dev/vda` with the stock block layer,
so nothing downstream knows the transport is MMIO.  Requires
`-global virtio-mmio.force-legacy=false` (QEMU `virt` defaults to legacy) +
a `-device virtio-blk-device`.

**Verified (serial log):** `/dev/vda ready (8192 sectors, 4 MiB)`; a
write→read round-trip PASSes on sector 100; the shell's `blk 0` hexdumps the
on-disk bytes:
```
  00: 00 00 00 44 2d 4f 53 2d 41 52 4d 36 34 2d 44 49  ...D-OS-ARM64-DI
  10: 53 4b 2d 53 45 43 54 4f 52 30 2d 48 45 4c 4c 4f  SK-SECTOR0-HELLO
```

**Phase G — exFAT on /dev/vda (persistent storage).**  The stock
`block_cache.c` + `fs/exfat.c` are arch-independent (exfat.c carries its own
`memcpy_`/`memset_`; no RTC/port-I/O), so they link + run unchanged — the
payoff of a portable fs layer.  `main_entry` runs `bcache_init()` +
`vfs_mount("exfat", "/mnt", "vda")`; the serial shell's ls/cat/write/rm then
operate on real disk under `/mnt`.  Test images are `mkfs.exfat`'d in the x86
build image (exfatprogs) and attached as the virtio-blk disk — no `-boot d`
gotcha, since the ARM `-kernel` path is not BIOS-based.

**Verified (serial log + reboot):**
```
exfat: mounted dev=vda clusters=7680 bps=512 spc=8 root=5 bitmap=2
d-os> write /mnt/hello.txt hi-from-arm-exfat   → wrote 18 bytes
d-os> ls /mnt                                  →  hello.txt
d-os> cat /mnt/hello.txt                        → hi-from-arm-exfat
--- fresh boot, same disk ---
d-os> cat /mnt/hello.txt                        → hi-from-arm-exfat   (persisted!)
```
Full chain end-to-end: virtio-MMIO → block cache → exFAT → VFS → shell, with
writes surviving a reboot.

**Phase H — device-tree (FDT/DTB) discovery.**  ARM has no BIOS/ACPI
enumeration; firmware hands over a device tree.  `dtb.c` is a minimal
big-endian FDT parser that walks the structure block for the `/memory` node's
`reg` (RAM base + size) and counts `/cpus/cpu@*` nodes.  QEMU's direct-ELF
`-kernel` entry provides neither an x0 pointer nor an in-memory DTB, so the
run script loads one at a fixed address (`-device loader,addr=0x48000000`,
generated per config via `-machine dumpdtb`); `fdt_find` checks x0 → that
address → a RAM scan.  `aarch64_boot_meminfo_init` then sizes the PMM map to
the DTB-discovered RAM instead of the baked-in 256 MiB, with a fallback.

**Verified (serial log):** with `-m 512M -smp 4` + the loaded DTB:
```
dtb: found @ 0x48000000 — RAM 512 MiB @ 0x40000000, 4 CPU(s)
pmm: buddy ready — ... 509 MiB total free
```
vs. no DTB → `dtb: no device tree found (using built-in defaults)` → 253 MiB.
The kernel adapts to the actual machine config.

**Phase I — virtio-gpu framebuffer (the SAME fb_terminal renderer).**  QEMU's
`virt` board has no VGA/Bochs-VBE and no linear-VRAM BAR — the display is a
virtio-gpu device on a virtio-MMIO slot.  Unlike a plain framebuffer it is a
COMMAND device: the guest owns a RAM buffer, tells the host to treat it as a
2D resource's backing store, binds it to a scanout, and then — per update —
issues `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`.  To reuse the portable 8×8
console rather than fork it, the one x86-only part of `fb_terminal.c` (the
Bochs-VBE port I/O + the vmm identity map) was hoisted behind a tiny
presentation backend, `fb_present.h`:
  - `fb_present_map(phys, size)` — x86: 4 MiB PSE identity map via the vmm;
    aarch64: no-op (the RAM-backed buffer is already mapped Normal-WB).
  - `fb_present_flush(x, y, w, h)` — x86: no-op (linear FB is the scanout);
    aarch64: the virtio-gpu transfer + flush of that rect.
  - the Bochs-VBE double-buffer page flip (`fb_flip_init`/`fb_flip_to`, used by
    the M22.6 compositor) moved from `fb_terminal.c` to
    `kernel/hal/x86/fb_present.c` unchanged — `gui.c` needs no edit.
`fb_terminal.c` is now arch-portable and every render primitive self-flushes
its dirty rect.  On aarch64, `virtio_gpu.c` allocates a contiguous 1280×800
framebuffer (`pmm_alloc_contiguous`, ~4 MiB), stands up the scanout, and calls
`fb_term_init_direct()`.

**Verified (QEMU screendump, `-device virtio-gpu-device`):** the boot log
renders graphically at 1280×800 (grid 160×100) AND to the serial log; typing
`help` / `ls /` / `meminfo` into the shell shows crisp command output on the
framebuffer.  The i386 GUI (compositor page-flip through the moved
`fb_present.c`) was re-verified regression-free — the desktop, taskbar, clock
and cursor compose correctly.  (Bring-up lesson: on SMP the serial-shell banner
raced pid 0's hand-off line on the shared console — visible as cursor corruption
on the FB; fixed by printing the hand-off line *before* spawning the shell so
pid 0 then only idles.)

**Phase L — EL0 userspace substrate (the M25 prerequisite).**  Everything so
far ran at EL1 (the ARM analogue of ring 0).  M25 (userland foundation) needs to
run code at EL0 in its own address space and take syscalls — the capability the
x86 ports have had since M6/M20.5 (ring 3 + `int 0x80`).  This phase brings
aarch64 to that same baseline:
- **Per-process VMM (`vmm.c`).**  `mmu.c`'s coarse identity map (1 GiB blocks,
  EL1-only) is what turns the MMU on; `vmm.c` adds page-granular, EL0-accessible
  mappings in a private TTBR0 table.  `aarch64_vmm_create()` allocates a level-1
  table and copies the kernel's low-4-GiB identity blocks into it (so the kernel
  + peripherals stay reachable at EL1 in every space — the syscall handler runs
  at EL1 with the process's TTBR0 still loaded).  `aarch64_vmm_map_user()` maps
  4 KiB pages at VA ≥ 4 GiB (never colliding with the kernel blocks) with AP=01
  (EL0+EL1 RW) + PXN, UXN cleared only for code.  `aarch64_vmm_switch()` loads
  TTBR0 + `tlbi vmalle1` — the primitive M25's context_switch will call per task.
- **EL0 entry + SVC syscall (`usermode.S` + `syscall.c`).**  `aarch64_enter_user`
  stashes the kernel SP/LR, sets SP_EL0 + ELR_EL1 + SPSR_EL1 (=0 → EL0t, IRQs on)
  and `eret`s to EL0.  A `svc #0` traps to the EL0 synchronous vector; `exceptions.c`
  decodes ESR_EL1.EC == 0x15 and calls the dispatcher (x8=number, x0..x5=args,
  return in x0; numbers shared via `syscall.h`).  SYS_EXIT teleports back to the
  kernel via `aarch64_user_exit` (restore stashed SP/LR + `ret`), mirroring the
  x86 SYS_EXIT trick — no TSS needed because the CPU auto-selects SP_EL1 on an
  EL0→EL1 exception.
- **Self-test (`usertest`).**  `aarch64_usertest()` (run at boot + as a serial-
  shell command) creates a space, maps a code + stack page, copies in the
  position-independent `user_stub`, drops to EL0, and the stub SYS_PRINTs then
  SYS_EXITs.

**Verified (serial):** `usertest: dropping to EL0 at 0x100000000...` →
`hello from EL0 (aarch64 userspace)!` (printed by the EL0 program via `svc`) →
`usertest: back at EL1 (SYS_EXIT teleport OK)`.  The x86 `ringtest` (ring 3 +
`int 0x80`) was re-verified identical on i386 and x86_64 — **all three arches
now run a user program and service a syscall, the baseline M25 builds on.**

**Phase J + K — the framebuffer shell.c + the M22 GUI (2026-07-10).**  With a
framebuffer present (virtio-gpu), aarch64 now runs the *same* full `shell.c` the
x86 ports run, on a virtual console (`vc.c`), with virtio-input keyboard + mouse
— and the `gui` command brings up the M22 desktop (compositor + taskbar + PL031
clock + windows).  The whole GUI+shell bundle links via a handful of portability
shims (see the change-log entry): `arch_ringtest()`, PSCI `hal_shutdown/reboot`,
`pl031_rtc.c`, `fb_flip_*` stubs + `fb_present_flush()` in `gui.c`'s present
path, and `virtio_input.c`.  **Lesson learned:** pid 0's idle loop must
`hal_intr_enable()` each pass (as `cpu_idle_entry` does) — a bare `for(;;)
hal_cpu_halt()` leaves the CPU wedged if DAIF ever masks IRQs (wfi wakes but does
not take a masked IRQ), so its timer tick stops, it stops scheduling, and every
task homed on that CPU (the input poll task) starves.  This was the flaky
"renders on -smp 2 but not -smp 1 / no keyboard" symptom; the fix makes both
deterministic.

**Phase M — USB (2026-07-10).**  xHCI + USB HID keyboard over a new PCIe-ECAM
layer (`pci.c`; see the change-log entry) — full x86 feature parity.

**Net:** aarch64 now covers the same ground as the x86 ports (boot → SMP →
virtio-blk → exFAT → framebuffer → VC → full shell.c → M22 GUI → EL0 userspace →
USB HID), minus x86-only accidents (PS/2, legacy PIC/PIT, multiboot).  Remaining
ARM-specific follow-ups are open-ended (EL0 multitasking beyond the self-test,
per-process context_switch — the M25 line).

#### M25-readiness matrix (2026-07-10)

| Capability                        | i386 | x86_64 | aarch64 |
|-----------------------------------|:----:|:------:|:-------:|
| User mode (ring 3 / EL0)          | ✅ M6 | ✅ M20.5 | ✅ M21-L |
| Syscall entry (`int 0x80` / `svc`)| ✅   | ✅     | ✅ (SVC) |
| Shared syscall table (PRINT/EXIT) | ✅   | ✅     | ✅ |
| User-page mapping primitive       | ✅ `vmm_map(USER)` | ✅ | ✅ `aarch64_vmm_map_user` |
| Per-process address space create  | (M25 stage 1) | (M25 stage 1) | ✅ `aarch64_vmm_create` |
| `usertest`/`ringtest` self-test   | ✅   | ✅     | ✅ |

All three architectures can enter user mode and service a syscall — **M25
(per-process spaces, ELF loader, fd table, …) can begin on any of them.**

---

### 4.18 System log — klog ring buffer + dmesg (`kernel/core/klog.c`, M28)

**What it is.**  A structured, in-memory kernel log with severity levels,
source tags, and history — the thing you review after a boot with `dmesg`.
Before M28 every diagnostic went straight out through `kprintf` to the
console sinks (serial + framebuffer) with no levels, no tags, and no
history: once a line scrolled off it was gone.

**Ring.**  `klog.c` owns a *static* array of 512 `struct klog_record`
(`seq`, boot-relative `t_ms`, `level`, `tag[16]`, `msg[200]`).  Static so
it works from the very first boot `kprintf`, long before the heap exists;
circular so it self-trims.  Timestamps are monotonic ms since boot
(`timer_ticks_ms`), rendered dmesg-style as `[  sec.mmm]`.  (Absolute
CMOS-RTC wall-clock stamping is a noted follow-up, not needed for v1.)

**Two ways in.**
- **Automatic tee.**  `printf.c`'s `emit()` calls `klog_feed_char` for
  every output byte.  That assembles a line in a staging buffer and, on
  the terminating `\n`, commits a record.  So *all* existing `kprintf`
  output is captured with zero call-site changes, at the default level
  `KLOG_INFO` / tag `"kernel"`.  Blank lines are skipped.
- **Structured.**  `klog(level, tag, fmt, …)` sets the pending level+tag,
  then formats through the *same* `kvprintf` machinery (so the message
  still reaches the console), and the trailing `\n` commits the record
  with that severity + tag.  Levels are printk/syslog-ordered
  (`KLOG_EMERG`=0 … `KLOG_DEBUG`=7; smaller = more severe).

**Read paths.**  `klog_for_each(fn, ctx)` snapshots the live range and
replays each record oldest→newest, copying one slot at a time under the
ring lock and calling `fn` with the lock *released* (so a callback may
re-enter `kprintf`→klog without deadlocking).  Two consumers:
- `dmesg [-l <level>]` (shell) — renders `[  sec.mmm] LEVEL tag: msg`,
  filtering to records at least as severe as the threshold (level name
  `emerg…debug` or digit `0…7`).  It renders via `console_*` **not**
  `kprintf` on purpose — printing the log through the tee would append
  every rendered line back into the ring and evict the boot messages.
- `/proc/kmsg` (procfs) — the same rendering as a readable file.

**Concurrency.**  Lock-light and SMP-safe at *record* granularity: the
commit (slot write + seq bump) and the read (slot copy-out) run under one
spinlock.  The staging buffer + pending level/tag are single-writer state
carrying the same non-reentrancy caveat `kprintf` already documents —
concurrent emitters can interleave a line exactly as they can already
interleave console output.

**Portability.**  Entirely arch-independent core; links on all three
arches (`klog.c` in every `CORE_C_SRCS`).  `dmesg` works wherever `shell.c`
links (incl. aarch64); `/proc/kmsg` appears wherever procfs links
(i386/x86_64 — aarch64 has no procfs yet).

**Pitfall (recorded for posterity).**  Extracting a `kvprintf(const char*,
va_list)` core out of `kprintf` corrupted *all* formatted output on
x86_64 while i386 stayed fine.  Cause: the x86_64 SysV ABI makes `va_list`
an *array* type, so a `va_list` **parameter** decays to a pointer — making
`&ap` a pointer-to-pointer, the wrong type for the `va_list*`-taking
`fetch_signed`/`fetch_unsigned` helpers.  i386's scalar `va_list` has no
such decay, so it masked the bug.  Fix: `va_copy` the incoming list into a
genuine local array and format off that (`&aq` is then correctly a
pointer-to-array on both arches).  Lesson: any helper that forwards a
`va_list` by pointer must own a real `va_list` local (via `va_copy`), never
take the address of a `va_list` parameter.

---

### 4.19 Userland foundation — processes, fds, IPC, libc (M25)

**What it is.**  The layer that makes a d-os task a real *user process*: its
own address space, a file-descriptor table, a POSIX-ish syscall surface, shared
memory, unix-socket IPC with fd passing, poll, and an in-tree libc — the
Wayland prerequisites, useful in their own right.  Built stage by stage; each
stage has a self-test shell command.

**Address spaces (`vmm.h` `vmm_space_*`).**  A `vmm_space` is a private
top-level page table that keeps the kernel mapped (so ring-0 code + stacks
survive a CR3/TTBR0 switch) but owns a private user region.  `struct task.mm`
points to it (NULL = kernel thread); the scheduler calls `vmm_space_switch`
before `context_switch`, reloading the hardware register **only when it
changes** (kernel-thread → kernel-thread is free).  `vmm_user_base()` gives the
arch's user-region base (1 GiB on x86, 4 GiB on aarch64).  **x86_64 subtlety:**
the whole kernel lives under PML4[0], so a space gets a *private PDPT* under
PML4[0] (kernel PD subtrees shared by pointer, user region private) — a bare
PML4 copy would leak the user region into the shared kernel table.

**Loading + running ELFs (`elf.c`, `proc.c`).**  `elf_load` parses either ELF
class at runtime and maps PT_LOAD segments into a space with R/W/X.
`proc_exec_elf` loads an image, maps a user stack, binds the space to the
calling task, and drops to ring 3 / EL0 at `e_entry` via `enter_user_mode_wrap`
(x86) / `aarch64_enter_user`.  Today this is a **synchronous excursion** on the
caller's task — the program runs to its SYS_EXIT and control returns; fully
independent, preemptible, concurrently-scheduled processes are the deferred
tail (needs per-task TSS.esp0 / SP_EL1, SYS_EXIT→task_exit, blocking syscalls).

**Descriptors (`fd.h`/`fd.c`, `usyscall.c`).**  `struct task.fds[32]` holds
`struct ofile*` — a refcounted tagged handle over a VFS file, a shm object, or
a socket.  fds 0/1/2 are the implicit console.  Portable syscall handlers live
in `usyscall.c`; each arch's dispatcher (`kernel/hal/<arch>/syscall.c`) only
pulls the number + args out of its trapframe (int 0x80 EBX/ECX/EDX/ESI, svc
x0..x3).  Syscalls: `print, exit, write, read, open, close, lseek, mmap,
memfd, socketpair, send, recv, poll`.

**Shared memory (`fd.c` shm).**  `memfd` creates a frame-set object behind an
fd; `mmap` maps it (or fresh anonymous frames) into the space.  A frame that
is *borrowed* (a shm frame mapped into a space that doesn't own it) is tagged
with the **`VMM_SHARED`** PTE bit (x86 bit 10 / aarch64 software bit 55) so
`vmm_space_destroy` drops the mapping without freeing the owner's frame — the
shm object frees its frames once, at its own refcount 0.

**Unix sockets + fd passing (`usock.c`).**  `socketpair` makes two connected
endpoints, each with a receive ring + a passed-fd queue.  `send`/`recv` move
bytes (to the peer's ring) and, SCM_RIGHTS-style, a *file descriptor*: the
sender queues a fresh `ofile` reference on the peer, the receiver installs it
as a new fd.  Because the reference travels, the underlying object (a shm
buffer, a keymap file) outlives the sender's fd — exactly the wl_shm / xkb
handover Wayland needs.  `poll` reports non-blocking readiness (socket
readable iff buffered; writable iff peer + space).

**In-tree libc (`user/`).**  `crt0.s` (calls `main`, then SYS_EXIT) + `libc.c`
(`int 0x80` wrappers, `strlen/memset/memcpy`, `malloc` as a bump allocator over
`mmap`, `puts`/`printf`).  A real compiled-C `hello.c` links against it into a
static ELF at 0x40000000 (`ld -N`, one RWX PT_LOAD), which the Makefile wraps
as a binary blob (`objcopy`) linked into the i386 kernel; `libctest` loads it
via `proc_exec_elf`.  The libc C is arch-neutral — the x86_64/aarch64 port needs
only a per-arch crt0 + link + blob rule (the shell command links everywhere via
weak blob symbols, reporting "not built" where absent).

**Privilege model (north-star, locked M25).**  d-os uses exactly **ring 0 +
ring 3** (EL1 + EL0); rings 1/2 are never used.  Paging's U/S bit is binary, so
a "ring-1 driver" would have full kernel-memory access — no isolation; x86_64
made rings 1/2 vestigial; aarch64 has no rings.  The security axis is *address
spaces + capabilities*, not the count of CPU privilege levels — every richer
trust tier (M33 isolated drivers) is a ring-3 process with a restricted
capability set, not a middle ring.

**Self-tests.**  `userrun` (loaded ELF prints via `write(1)`), `fdtest`
(open/read/lseek/close/reuse), `shmtest` (one memfd, two mappings, shared),
`socktest` (bytes + a memfd passed over a socket → shared on the far side),
`polltest` (readiness transitions), `libctest` (compiled-C libc program in
ring 3).  All green on i386 + x86_64 + aarch64 (libctest: i386).

---

### 4.20 Blocking primitives — wait-queue, task_wait, blocking IPC (Tier A)

**What it is.**  The primitive that was missing under M25: a way for a task to
sleep *until an event*, not just round-robin-yield (`task_yield`) or die
(`task_exit`).  Before this, `TASK_SLEEPING` was an inert enum value and every
"blocking"-looking path polled (the init reaper hlt+yield loop, a would-be
blocking socket read).  Tier A makes `TASK_SLEEPING` real and builds three
things on it: `task_wait`, blocking socket `read`/`recv`, and blocking `poll`.

**Wait-queue (`waitq.h`, implemented in `task.c`).**  `struct waitq { spinlock
lock; struct task* head; }` — an intrusive queue (tasks link via
`task.wq_next`).  API: `waitq_lock/unlock`, `waitq_block` (park the caller),
`waitq_wake_one/all`.  A parked task is fully off every runqueue (zero CPU),
`TASK_SLEEPING`; a wake sets it `RUNNABLE` and re-enqueues it via the normal
scheduler pick (affinity-aware CPU choice + a reschedule IPI if it lands on
another core — so **cross-CPU wake works**).  Parking mirrors `task_exit`'s
lock-handoff discipline: register on the queue + flip to `SLEEPING` while
holding the queue lock, detach from the runqueue, then drop the lock and
`schedule()` away.

**Lost-wakeup safety.**  The queue's own lock IS the condition lock
(pthread_cond_wait discipline): the consumer holds it across check-then-block,
the producer across mutate-then-wake, so a wake can never slip between "saw not
ready" and "parked".  `waitq_block` atomically parks + drops the lock and
re-acquires it before returning — always loop on the condition
(`while (!cond) waitq_block(&wq);`), a spurious wake just re-blocks.  Interrupts
are held off across the tiny unlock→context-switch window; a remote waker there
just leaves the task `RUNNABLE` on a runqueue (re-picked, not lost).

**`task_wait(pid, &code)` (`task.c`).**  POSIX-waitpid-shaped: block until a
child (specific pid, or any child when `pid <= 0`) is DEAD, record its exit
code, reap it, return its pid; `-1` if there is no matching child.  Parked on a
global `child_exit_wq` that `task_exit_code` wakes **after** marking itself DEAD
(set-condition-before-signal-lock → race-free).  Contract: a parent that will
`task_wait` a child claims its reap with `task_set_reap_owned(child, 1)` at
spawn, so init's universal reaper leaves the DEAD struct for `task_wait` to
harvest instead of freeing it first.  This is M29's supervisor building block.

**Blocking IPC (`usock.c`, `usyscall.c`).**  Each socket endpoint gained a
per-endpoint read wait-queue (which now also serialises its receive ring, so
two tasks may safely share a pair).  A blocking `usock_recv` on an empty
endpoint parks until the peer's `usock_send` (fills the ring + wakes) or
`usock_close` (wakes → EOF).  `read(2)`/`recv(2)` block by default; the
non-blocking snapshot path (`block == 0`) is kept for poll's drain.  `poll`
with `timeout < 0` blocks on a global readiness wait-queue that the socket
layer raises via `fd_readiness_signal`; `timeout == 0` is the old snapshot;
a finite positive `timeout` is still a snapshot (a *timed* wakeup is deferred
with cron/watchdog's timed-sleep).

**Self-test.**  `waittest`: (1) `task_wait` blocks on a child that burns CPU
then exits 42 — verifies block + code; (2) a producer task sends to a socket
the shell task blocks reading — verifies cross-task blocking recv; (3) closing
the peer of a blocked reader returns 0 (EOF).  All three green on i386 +
x86_64 + aarch64.

---

### 4.21 Services & the service bus (M29)

**What it is.**  Two self-registered halves (same linker-section story as
DRIVER() / GUI_APP()): a **supervisor** — the "upward" answer to child death
(systemd-lite) — and a **service bus** — named, versioned, transport-abstracted
bindings so subsystems find + call each other without hard-linking.  Built on
Tier A's `task_wait`.

**Supervisor (`service.h`/`service.c`, `svc_demo.c`).**  `SERVICE(name, entry,
autostart, restart)` registers a service; `restart ∈ {no, on-failure, always}`.
One supervisor task (a child of init) owns every service task: it autostarts
enabled services (config gate `service.<name>.disabled=1`), then loops on
`task_wait(-1)` — blocking with zero CPU until a service child exits — and
applies the policy.  It claims each child's reap (`task_set_reap_owned`) so
init's universal reaper leaves the exit for `task_wait` to harvest, making the
exit code authoritative.  A hand-issued `service stop` sets a `stopping` flag
before the kill so a deliberate stop is never "restarted"; a service that dies
quickly after start is backed off (crash-loop guard) before the restart.
Control surface: `service list|start|stop|restart|status <name>`;
`/proc/services` (name, state, pid, restarts, autostart, policy).

**Service bus (`bus.h`/`bus.c`).**  Three concepts (mirroring `hal_api.h`'s
versioned interface): **endpoint** (a flat-namespace name — `greeter.default`),
**contract** (a versioned struct-of-fn-pointers — `Greeter v1`; no IDL),
**transport** (`LocalCall` real; `SharedMemory`/`IPC` reserved for M25).
`BUS_PROVIDER()` publishes a provider at an endpoint with a declared execution
domain (§M33 axis); `bus_bind(endpoint, contract, version, &binding)` resolves
it.  Resolution is **strict on the wire** (exact contract@version).  The
domain↔transport rule is enforced: only a KERNEL/LocalCall provider is
invokable today; a USER/ISOLATED provider fails cleanly (needs M25's non-local
transports) rather than pretending.  `/proc/bus` lists endpoints + adapters.

**Contract versioning (decided 2026-07-10).**  Strict + adapter-shim live in
different layers, so both: the broker only binds an exact match; compatibility
is an opt-in *mechanism* — a `BUS_ADAPTER(contract, from, to)` entry that
synthesises a `from`-shaped iface over a higher-version provider, inserted by
the broker **iff** the `bus.allow-adaptation` config bit is set.  A provider
speaking several versions just registers as its own multi-version adapter —
"backward-compatible" is that special case, no extra policy branch.

**Marshalling discipline (convention #5).**  Contracts are designed as if
marshalled even while only LocalCall exists — arguments are handles / copied
buffers, never freely-shared raw kernel pointers — so a contract can later move
to a USER domain (§M33) by a config flip, not a rewrite.  The demo `Greeter`
passes/returns copied C strings.

**Demonstrators (`svc_demo.c`).**  Services: `heartbeat` (autostart,
restart=always — logs only on start/stop, so a restart shows as a fresh "up"
line + bumped restart count) and `crasher` (manual, restart=on-failure — exits
1 shortly after start, showing supervised restart + backoff).  Bus: a `Greeter`
v2 provider at `greeter.default` + a `Greeter 1→2` adapter.  `bustest` binds v2
exactly, shows a strict v1 bind MISSING with adaptation off and SUCCEEDING via
the shim with it on — all green on i386 + x86_64 + aarch64.

**aarch64 parity note.**  The minimal aarch64 port had skipped procfs; M29
added `procfs.c` to its build + a `procfs_init()` call (ramfs already
bootstraps `/proc`), so aarch64 now exposes `/proc/services` + `/proc/bus` +
the built-ins like x86.

---

### 4.22 Watchdog — freeze detection (M31)

**What it is.**  M27 handles a task that *dies*; the watchdog handles one that
is alive but *wedged*.  A single sweep task (child of init, `watchdog.c`) runs
two detectors every 500 ms:

- **Layer 1 — per-task heartbeat (opt-in).**  A task calls
  `watchdog_register(timeout_ms)` and periodically `watchdog_kick()`s.  The
  sweep flags any registered task past its deadline: logs a `KLOG_ERR` and
  `task_kill_tree`s it.  Because a supervised M29 service is a supervisor
  child, the watchdog kill triggers the supervisor's `task_wait` → **restart** —
  the two subsystems compose (verified: `wd-hang` service killed by the
  watchdog, restarted by the supervisor).  Opt-in, so a legitimately long
  compute that never registers is never watched.
- **Layer 2 — per-CPU softlockup.**  Each CPU's timer tick bumps
  `percpu.ticks` (in `schedule_request`).  The sweep snapshots every online
  CPU's counter and warns about any that stopped advancing — a core wedged with
  IRQs off (spinlock deadlock / IRQ storm), which a per-task heartbeat can't
  see because the sweep itself may be starved there.  Limitation: the single
  sweep runs on one CPU, so a wedge on *that* CPU can starve it; on a 1–2 CPU
  box the common cross-core case is caught.
- **Layer 3 — hardware watchdog: deferred.**  Arming a real/emulated watchdog
  timer (i6300esb / SP805) that resets the box when everything wedges needs a
  per-platform device driver; left as future work — layers 1–2 are the
  detection substrate.

**The cooperative-kill truth (§M22.3).**  The watchdog can *detect* a freeze
but can only *force-kill* a kthread that reaches a yield point (it may hold a
spinlock).  So layer-1 kill+restart works for a task that yields but stopped
kicking (a stuck state machine); a truly wedged kthread that never yields needs
layer 3 / a reboot.  Genuine force-kill of any frozen task arrives with §M25
user processes.

**Introspection + self-test.**  `/proc/watchdog` shows the sweep period, hang /
softlockup event counts, and the watched-task table.  `wdtest` spawns a task
that registers a 600 ms heartbeat then stops kicking; the watchdog detects +
kills it (PASS).  All green on i386 + x86_64 + aarch64 with zero softlockup
false-positives during normal operation.

---

### 4.23 cron — time-based task scheduling (M30)

**What it is.**  The capstone of the M27–M29 cluster and the first genuinely
useful service: a scheduler for *work over time*.  cron (`cron.c`) is **itself
an M29 service** (autostart, restart=always) — it appears in `service list`, is
supervised, and comes back if it dies — whose entry loops on `task_msleep` and,
each 500 ms tick, spawns every due job as one of its children (init reaps them)
and logs the run.  Small because M27 (parent/reap), M28 (klog), M29 (services)
and Tier A (`task_msleep`) already exist.

**Jobs.**  A job is a self-registered `CRON_JOB(name, fn, default_every_ms)`
entry (linker-section registry, like SERVICE()/DRIVER()).  Interval resolution,
lowest priority first: the registered default → an `/etc/crontab` line (`every
<N> <s|m|h> <name>`) → config keys (`cron.<name>.every_ms`,
`cron.<name>.disabled`).  `/etc/crontab` is optional — absent, the defaults
fire, so a job works out of the box.  Missed-tick policy: run-once-on-catch-up
(next-due = now + interval after a fire), never backfill — a starved cron
doesn't stampede.

**Control + introspection.**  `crontab -l` / `cron list|status` shows each job's
interval, ms-until-next, run count, and enabled/disabled state; `cron reload`
re-reads `/etc/crontab` + config; `/proc/cron` renders the same table.

**Demonstrator + DoD.**  A `tick-log` job (`CRON_JOB`, every 5 s) klogs
"scheduled job fired" — verified firing on schedule on i386 + x86_64 + aarch64
(dmesg shows `cron: run 'tick-log'` + the job line + init reaping the job task),
with cron itself listed under `service list` as `running / always`.

**Out of scope (PLAN §M30):** wall-clock cron fields beyond intervals, per-user
crontabs, at/batch one-shots, last-run persistence across reboot.

---

### 4.24 Concurrent user processes + full-arch libc (Tier B — M25 tail)

**What it is.**  The deferred tail of M25: an ELF now runs as an *independent,
preemptible* user process on its own task — several at once, each in its own
address space, each exiting on its own — instead of the single synchronous
"excursion" on the caller's task.  Plus the in-tree libc now builds for all
three arches.

**Per-task privilege-transition stack.**  When a ring-3/EL0 task takes a
syscall or interrupt the CPU switches to a kernel stack: TSS.esp0/rsp0 on x86,
SP_EL1 on aarch64.  With multiple user tasks preemptible at once that stack must
be *per-task*.  The scheduler sets it on every switch-in via a new
`hal_set_kernel_stack(top)` hook: `top = task's kstack top` for an independent
user task, `top = 0` (arch default fixed syscall stack) for kernel threads and
the excursion-model self-tests.  **aarch64** needs no work here — SP_EL1 is the
ordinary EL1 stack pointer that `context_switch` already saves/restores per
task, so it tracks automatically; the hook is a no-op there.

**User task lifecycle (`proc_spawn`, proc.c).**  `proc_spawn(name, image, len)`
loads the ELF into a fresh space, maps a user stack, and spawns a task whose
bootstrap binds the space, marks itself `user_task` (routing ring-3→ring-0 to
its own kstack + making SYS_EXIT terminal), and drops **one-way** to ring 3/EL0
via `enter_user_mode` (a no-save variant of the excursion's
`enter_user_mode_wrap`/`aarch64_enter_user`).  `SYS_EXIT` for a user task closes
its fds and `task_exit`s; init reaps it and `task_reap` frees its address space
(safe there — the space is loaded on no CPU).  The excursion model
(`proc_exec_elf`, teleport-back on the fixed syscall stack) is kept intact for
the `userrun`/`libctest` self-tests, distinguished by the `user_task` flag.

**Full-arch libc (`user/`).**  `libc.c`'s single arch-conditional `syscall3`
(x86 `int 0x80`, aarch64 `svc #0`) + a per-arch `crt0` (`crt0.s` / `crt0_x86_64.s`
/ `crt0_aarch64.S`, each calling `main` then `SYS_EXIT` with the return code) let
the *same* `hello.c` / `spin.c` compile for i386 / x86_64 / aarch64.  The Makefile
gained per-arch USER_* knobs (compile flags, link emulation + base — 1 GiB on
x86, 4 GiB on aarch64 — and blob objcopy targets); each program links as a static
ELF at the arch's user base and is wrapped as a binary blob with per-arch symbols
(`_binary_user_<prog>_<arch>_elf_*`), which shell.c selects at runtime.  New
syscall: `SYS_GETPID`.

**Self-tests.**  `procspawn` launches two copies of `spin` (a getpid + print/burn
loop) as independent user processes; their line-interleaved output + independent
reaps prove concurrent, scheduler-time-sliced ring-3/EL0 processes.  `libctest`
runs the compiled-C `hello` in ring 3/EL0.  Both green on **i386 + x86_64 +
aarch64** (libc now all three, not just i386).

**Still deferred:** force-killing a *wedged* ring-3 task (needs the M25/§M33
isolation guarantees — a pure-ring3 loop never reaches a cooperative-kill yield
point); argv/env/argc; demand paging / COW fork.

---

### 4.25 Networking — virtio-net + TCP/IP stack (M24, i386)

**Files:** `kernel/includes/net.h`, `kernel/core/net.c` (portable stack),
`kernel/drivers/net/virtio_net.c` (NIC driver).  Shell: `lsnic`, `ping`,
`arp`, `nslookup`, `wget`, `nettest`.

A from-scratch IPv4 stack, layered like the block layer: a NIC driver
registers a `struct net_device` (MAC + `transmit` + `poll` callbacks) and the
arch-independent stack (Ethernet → ARP / IPv4 / ICMP / UDP / TCP) sits on top,
coupled to the driver only through `net_register()` + `net_rx()`.  Shipped
i386-only (mirrors the block/USB "i386 first, 64-bit DMA later" rule).

**RX model — poll from the calling task.**  There is no IRQ path yet: the
driver's `dev->poll()` drains the RX ring into `net_rx()`, and the blocking
helpers (ARP resolve, ping, DNS, HTTP) call it in a bounded spin loop.  So
every request runs entirely in one task context → **no locking**.  IRQ-driven
RX + a background `netd` poll task are the documented follow-up; the interface
(`net_rx` callable from an ISR) is already shaped for it.

**Layers (all in `net.c`, well-sectioned):**
- **virtio-net driver** (`virtio_net.c`) — legacy PCI transport (vendor 0x1AF4,
  device 0x1000), *same* virtqueue layout as virtio_blk but with **two queues**
  (0 = receiveq, 1 = transmitq) and **pre-posted RX buffers** (32 device-
  writable frames, recycled after consumption).  Each frame carries a 10-byte
  `virtio_net_hdr` (MRG_RXBUF not negotiated) that TX zeroes and RX skips.
  Negotiates only `VIRTIO_NET_F_MAC`.  DMA memory from the PMM (phys == virt in
  the identity map), exactly like virtio_blk.  Registers `eth0` with the QEMU
  SLIRP defaults (10.0.2.15/24, gw 10.0.2.2).
- **Ethernet** — frame parse/build + demux by EtherType (ARP / IPv4); accepts
  frames addressed to us or broadcast.
- **ARP** — an 8-entry cache; resolves an IP by broadcasting a request and
  polling for the reply, answers requests for our own IP, learns every sender.
- **IPv4** — header build/parse + RFC 1071 checksum; next-hop = destination if
  on-subnet, else the gateway.  `Don't Fragment`, TTL 64.
- **ICMP** — echo reply (answers pings to us) + echo request (`ping`), matching
  replies by id/seq.
- **UDP** — datagram send (checksum omitted, legal for IPv4) + an 8-slot
  port-binding table for receive; backs the DNS resolver.
- **DNS** — a stub resolver: builds an A-query, sends to the SLIRP DNS proxy
  (10.0.2.3:53) over UDP, parses the answer (handles name-compression
  pointers).  The precursor to `getaddrinfo` (§M39).
- **TCP** — a client-only, single-connection implementation: SYN/SYN-ACK/ACK
  handshake, in-order data with ACKs, FIN close, mandatory pseudo-header
  checksum.  **Simplified for the lossless SLIRP link**: no congestion control,
  no retransmit timers, in-order only.  Backs `net_http_get` (HTTP/1.0 GET →
  streams the response → closes) and the `wget` command.

**Boot-tested (i386, QEMU `-netdev user -device virtio-net-pci,disable-modern=on`):**
`nettest` drives all three transports end-to-end to the real internet through
SLIRP:
```
nettest: PASS icmp (3/3 echo replies)          ← ARP + ICMP to the gateway
nettest: PASS dns  (example.com -> 172.66.147.243)  ← UDP + DNS
nettest: PASS tcp  (828 bytes, "HTTP/1.1 200 OK")   ← TCP handshake + HTTP GET
```

**Lessons learned:**
- *The kernel `kprintf` has no width/zero-pad* — `%02x` prints the specifier
  literally AND desyncs the varargs (the next `%s` then eats a byte as a
  pointer).  Format MACs/IPs to a string first (`net_fmt_mac` / `net_fmt_ip`)
  and print with `%s`.  (Same class of bug as the §M28 `va_list` note.)
- *virtio-net needs RX buffers posted before it will ever deliver a frame* —
  unlike blk (which posts per-request), the NIC fills a standing pool; forget
  to refill after consuming and RX silently stalls after the pool drains.
- *TCP checksum is mandatory* (unlike UDP's optional one) — it covers a
  pseudo-header (src/dst/proto/len); a zero or header-only checksum makes the
  peer/SLIRP drop the segment and the handshake never completes.

**Socket API to userland (stage 6 — shipped 2026-07-11).**  A BSD-sockets
surface over the in-kernel stack lets **ring-3** programs do networking.  A new
`FD_NETSOCK` ofile kind (fd.c) + a `struct netsock` (usyscall.c) back the fds;
syscalls `socket`(22)/`connect`(23)/`sendto`(24)/`recvfrom`(25)/`bind`(26)
(libc `socket`/`connect_ip`/`sendto`/`recvfrom`/`bind_port`, + a 5-arg
`syscall5`).  Addresses are host-order IPv4 + port ints (no `struct sockaddr`
yet — a teaching-ABI simplification for §M36/§M39).
- **SOCK_DGRAM (UDP):** the netsock owns a local port + a 4-slot datagram RX
  ring fed by net.c's per-port binding callback; `sendto`/`recvfrom`.  Tested:
  `dnstest` resolves example.com from ring 3 (socket → sendto 10.0.2.3:53 →
  recvfrom → parse A = 104.20.23.154).
- **SOCK_STREAM (TCP):** `connect` runs the handshake (`net_tcp_connect`); plain
  `read`/`write` on the fd map to `net_tcp_recv`/`net_tcp_send` (blocking recv,
  0 = EOF at peer FIN).  One connection at a time (shared `g_tcp`).  Tested:
  `httptest` resolves example.com (UDP socket), opens a TCP socket, connects
  :80, `write`s an HTTP GET and `read`s "HTTP/1.1 200 OK" (829 bytes) — full
  userland networking, the §M39 TLS bridge target.

**Still deferred (later §M24 stages / §M35):** IRQ-driven RX + `netd` task;
a `struct sockaddr` layer + multiple concurrent TCP connections + TX
segmentation; TCP retransmit timers + congestion control + a real state
machine; a listening (server) role; DHCP; IPv6; `/proc/net/*`;
x86_64/aarch64 ports.

---

### 4.26 Audio — AC97 codec + PCM output (M23, i386)

**Files:** `kernel/includes/audio.h`, `kernel/core/audio.c` (portable core),
`kernel/drivers/audio/ac97.c` (codec driver).  Shell: `lsaudio`, `beep`,
`tone`.

An audio subsystem shaped exactly like the block/net layers: a codec driver
registers a `struct audio_dev` (native `rate` + `channels` + a `play` callback
for 16-bit stereo PCM frames), and the arch-independent core (a square-wave
tone generator + `lsaudio`) sits on top, coupled to the driver only through
`audio_register()`.  Shipped i386-only (PCI bus-master DMA, same "i386 first"
rule as block/net/USB).

**AC97 driver** (`ac97.c`, PCI vendor 0x8086 device 0x2415, QEMU `-device
AC97`): the classic two-BAR PC codec — **BAR0 = NAM** (mixer: reset, master +
PCM volume, VRA sample rate) and **BAR1 = NABM** (bus-master DMA engine +
global control).  PCM **output** only (§M23 scope).  Bring-up: enable PCI
I/O + bus-master, deassert the AC-link cold reset, reset the codec + the
PCM-out box, unmute + full volume, set the DAC to 48 kHz via variable-rate
audio.  Playback feeds the engine a **Buffer Descriptor List** (BDL): one
entry pointing at a 128 KB PMM-backed DMA buffer (phys == virt in the identity
map), length in 16-bit samples, `IOC|BUP` so the engine halts cleanly at the
end; the driver polls `PO_SR.DCH` (DMA halted) until playback drains at
real-time rate.

**Portable core** (`audio.c`): `audio_play_tone(freq, ms)` renders a square
wave (±8000 amplitude, half-period toggling) into a static scratch buffer and
hands it to `dev->play`, which copies it into the driver's DMA region.

**Boot-tested (i386, QEMU `-audiodev wav,path=out.wav -device AC97,audiodev=snd`):**
`beep` plays 440 Hz for 400 ms; the captured WAV, analysed as raw S16LE
stereo, is a clean square wave — left channel **min −8000 / max +8000** (the
generated amplitude), every sample non-zero, ~444 Hz by zero-crossing count
(≈ the requested 440).  So the whole path — tone → `audio_dev` → AC97 BDL DMA
→ QEMU backend — is verified end-to-end.  (Note: killing QEMU via the monitor
`quit` leaves the WAV's RIFF/`data` size fields un-backpatched at 0, so a
strict WAV parser rejects the header even though the PCM payload is perfect;
read it as raw PCM to verify.)

**Still deferred (later §M23 / follow-ups):** a `play <path>` WAV-file player
(the tone is the smoke test that proves the DMA path; a WAV parser + streaming
across multiple BDL buffers is the remaining stage), a `/dev/dsp` char device
for raw PCM writes, a mixer (per-stream volume) + multiple concurrent streams
+ resampling, PCM **input** (mic/line), Intel HDA (the heavier modern codec),
IRQ-driven buffer completion (today playback polls `DCH`), x86_64/aarch64.

---

### 4.27 POSIX process model — fork/exec/wait/pipe/signals (M34, i386)

**Files:** `kernel/core/proc.c` (argv stack, execve), `kernel/hal/x86/fork.c`
(fork), `kernel/hal/x86/signal.c` (signal delivery), `kernel/hal/x86/vmm.c`
(COW clone + fault), `kernel/core/usyscall.c` (pipe/dup2/kill/sigaction),
`kernel/hal/x86/syscall.c` (dispatch), `user/*.c` + `user/crt0.s`.  Shell:
`runargs`, `forktest`, `forkexec`, `pipetest`, `sigtest`.

The classic Unix process API on top of the M25 userland (per-process address
spaces, ELF loader, fd table).  i386-first (the fork/signal mechanisms restore
a full register set via `iret`, which is arch-specific); the orchestration is
mostly portable.  New syscalls 14–21: FORK, WAITPID, EXECVE, PIPE, DUP2, KILL,
SIGACTION, SIGRETURN.

**Initial stack (argv/env/auxv).**  `build_initial_stack` (proc.c) lays out the
System V initial process stack — `argc`, `argv[]`, NULL, empty `envp`, an
`AT_NULL` auxv — with argv pointers as user VAs, written through the stack
frame's identity mapping.  Every launch path (`proc_exec_elf`, `proc_spawn`,
`proc_exec_elf_argv`, execve) builds it; the i386 crt0 reads argc/argv off the
stack into `main`.  Slots are `uintptr_t`-wide so the shape is valid on all
arches (only i386 crt0 reads it today).

**fork() — copy-on-write.**  `proc_fork` (fork.c) clones the caller's address
space with `vmm_space_clone` (writable pages shared **read-only + COW** and
ref-counted; read-only code eagerly copied; shm pages shared), duplicates the
fd table (`ofile_ref`), copies signal dispositions, and starts a child task
whose first act (`enter_user_mode_regs`, usermode.s) is to `iret` into ring 3
at the parent's post-`int 0x80` point with **eax = 0**.  The parent gets the
child pid.  The child is `reap_owned` so the M27 universal reaper leaves it for
the parent's `waitpid`.  A write to a COW page faults; `vmm_cow_fault` (vmm.c,
hooked in idt.c's #PF path) hands the writer a private copy (or makes the page
writable in place if it is the last sharer); `vmm_space_destroy` frees a COW
frame only via its last owner.

**waitpid()** = `task_wait` (the Tier A child-exit wait-queue), writing the
exit code to the user status pointer.

**execve()** — `proc_execve` (proc.c) marshals argv out of the old space, loads
the ELF at `path` **from the VFS** into a fresh space, builds the initial
stack, atomically swaps the task's `mm` (freeing the old space) and resumes
ring 3 at the new entry (one-way); fds survive, signal handlers reset to
default.  The embedded user ELFs are installed into the ramfs as `/bin/args`,
`/bin/hello` at shell start (`bin_install`) — the first populated `/bin`.  Note:
execve must be called from a *forked* task, not the synchronous exec excursion
(which would double-free the old space on its own cleanup) — i.e. the standard
fork+exec pattern.

**pipe() + dup2()** — `sys_pipe` makes a connected byte channel (over the usock
ring, like socketpair; fds[0]=read, fds[1]=write), inherited across fork;
`sys_dup2` redirects a real fd (≥ 3).

**Signals.**  Per-task state (`task.h`): `sig_pending` bitmask,
`sig_handler[NSIG]` (SIG_DFL / SIG_IGN / a ring-3 handler), `sig_restorer`.
`kill`/`sigaction` just set state.  Delivery is on the **return-to-user path**:
`signal_deliver` (signal.c, called from idt.c after each syscall) pushes a
signal frame on the user stack (saved context + the sig argument + a trampoline
return address) and rewrites the trapframe to `iret` into the handler; the
handler `ret`s into `__sig_trampoline` (crt0.s) which issues `SYS_SIGRETURN`, and
`signal_sigreturn` restores the pre-signal context.  `SIG_DFL` of
INT/TERM/KILL/SEGV terminates the task.

**Boot-tested (i386, sendkey + serial):**
- `runargs ab cd` → `argc=3, argv={args,ab,cd}` from ring 3.
- `forktest` → child `fork()==0` in a COW-isolated space (its `secret=222`
  leaves the parent's 111 untouched), `waitpid` status=7.
- `forkexec` → child `execv("/bin/args", {args, via-execve})` from the VFS,
  argc=2, exit status 2 to the parent.
- `pipetest` → child writes a pipe; parent `dup2`s the read end to fd 9 and
  reads "hello-through-pipe" back through a blocking read.
- `sigtest` → a SIGUSR1 handler runs in ring 3 on `raise()` and control resumes
  after it via sigreturn.

**Lesson learned:** adding fields to `struct task` needs a full `make clean`
(the project has no header dependencies) — a stale `task.c` kept the old struct
size, so out-of-struct `sig_handler[]` writes corrupted the heap and delivery
jumped to a garbage handler (#PF).  Same class as the M28 `va_list` note.

**Still deferred (later §M34 / §M35):** EINTR (a signal doesn't yet interrupt a
blocked syscall — delivered on the *next* return-to-user); `sigprocmask` /
signal blocking; real-time signals; `vfork`/`posix_spawn`; job control /
sessions / controlling tty; turning a user #PF into SIGSEGV (today a user fault
still panics); x86_64/aarch64 (the fork/signal register-restore is i386 asm).

---

### 4.28 Threads + futex (M35, i386)

**Files:** `kernel/core/proc.c` (`proc_clone`), `kernel/core/futex.c`,
`kernel/hal/x86/syscall.c`, `user/threadtest.c`.  Shell: `threadtest`.

Kernel-scheduled threads on the M34 process model.  A **thread is a task that
shares its creator's address space** (`mm`) — that is what makes several threads
see the same memory — plus a duplicated fd table, starting at a ring-3 entry
with its own stack.  `proc_clone(entry, stack)` (SYS_CLONE) creates it; a new
`task->mm_shared` flag stops `task_reap` from destroying the shared space (the
thread-group owner frees it).  The thread is a `reap_owned` child, joined with
`waitpid` (libc `thread_join`).  libc `thread_create(fn, arg)` mmaps a 64 KiB
stack, lays out `fn`'s argument + a return-to-`__sig`/`__thread_exit_tramp`
(crt0.s) address, then clones.

**futex** (`futex.c`, SYS_FUTEX): the one primitive a threading library needs.
`FUTEX_WAIT(uaddr, val)` parks the caller **iff `*uaddr == val`**, re-checked
under the wait-queue lock so a concurrent wake can't be lost (the Tier-A
block/wake contract); `FUTEX_WAKE` wakes waiters.  Waiters hash (by the physical
address of `uaddr`, via `vmm_translate`) into a small set of Tier-A wait-queues;
distinct addresses may collide into one bucket, so `FUTEX_WAKE` wakes the whole
bucket and every waiter re-checks its own `*uaddr` and re-parks if unchanged
(standard futex spurious-wakeup semantics).  `threadtest` uses a 3-state
(Drepper) futex mutex — the uncontended path is a single atomic op with no
syscall; the kernel is only entered on contention.

**Boot-tested — UP *and* SMP.**  4 threads × 5000 increments of one shared
counter under the futex mutex = **20000/20000 PASS** on both `-smp 1` and
`-smp 2` (truly parallel on two CPUs), proving the shared address space, the
mutex's correctness, `thread_join`, and SMP safety (locked `xchg` in the mutex +
spinlock-guarded kernel wait-queues).

**SMP userland fix — per-CPU TSS (done as part of this milestone).**  Bringing
threads up on `-smp 2` first exposed a *pre-existing* gap: `tss.c` had a **single
global TSS** and the APs never `LTR`'d one, so a ring-3 → ring-0 trap on an AP
had no valid per-CPU kernel stack — `threadtest` **and** `procspawn` hung on
`-smp 2` (never caught before: user tasks had only run on `-smp 1`).  Fixed with
a **per-CPU TSS** (an array in `tss.c`, one dedicated syscall stack each; one GDT
TSS descriptor per CPU at `GDT_TSS_BASE..`; each CPU LTRs its own via
`gdt_load_cpu_tss()` — the BSP from `gdt_init`, each AP from `ap_main`;
`hal_set_kernel_stack` writes `tss[this_cpu_id()].esp0`).  This unblocked **all**
ring-3 tasks on APs — `procspawn`'s two user processes now run + reap on
`-smp 2`, not just threads.

**Thread-local storage (`%gs`).**  i386 reads a `__thread` variable through
`%gs`, whose base comes from a GDT descriptor.  To give each thread its own
base we keep **one user-TLS descriptor per CPU** in the GDT (`GDT_TLS_BASE..`,
files `gdt.c`); the scheduler reloads *this CPU's* descriptor base to the
incoming thread's TLS pointer on switch-in (`hal_set_tls_base`, a HAL hook so
`task.c` stays portable — x86_64/aarch64 stub it, to use `FS.base`/`TPIDR_EL0`
later).  `set_thread_area` (SYS_SET_TLS) records the base, **pins the thread to
its CPU** (its `%gs` selector is per-CPU), programs the descriptor and returns
the ring-3 `%gs` selector; libc `set_tls()` loads `%gs` with it.  A descriptor
edit is picked up on the next `%gs` reload — which the return-to-ring-3 path
(`isr_common` pops `%gs`) does for free.  **Boot-tested UP and `-smp 2`:**
`tlstest`'s 4 threads each read only their **own** id back through `%gs` (0
mismatches over 50000 iterations) — the per-thread base is maintained across
context switches on both.  Scope: this proves the `%gs` mechanism; the
compiler's full `__thread` ABI (a PT_TLS template + variant-II layout, set up
by the runtime) layers on with the libc port (§M36).

**Still deferred (later §M35 / §M36):** the compiler `__thread` ABI runtime
(above); migration-safe TLS threads (today they are pinned to their CPU — a
truly migratable `%gs` needs a per-CPU GDT); priority inheritance / robust
futexes; `gettid`; per-thread signal masks; x86_64/aarch64.

---

### 4.29 Package manager — content-addressed store (M35.5, first slice)

**Files:** `kernel/core/pkg.c`, `kernel/includes/pkg.h`.  Shell: `pkg
build|install|remove|why|list|gc`, `pkgtest`.

The porting-discipline **gate** that must exist before pulling in foreign code
(musl §M36 onward): a **content-addressed store** on the VFS, Nix/Guix-shaped —
*not* dpkg/apt's mutable global `/usr` (the "accidental history" the project
rejects, convention #6).  So the system stays uncluttered, versions coexist
without conflict, and a package depends on exactly its declared deps.

**Store model (implemented):**
- **Content-addressing.**  A package materialises at
  `/store/<hash>-<name>-<version>/`, where the hash folds in the recipe id +
  version + **each dependency's recursive hash** — so a version bump or any
  transitive-dep change yields a *new*, immutable path.
- **Version coexistence.**  Distinct hashes ⇒ distinct paths side by side
  (`hello` 1.0 and 2.0 both in `/store`).
- **Pinned closure.**  Each path carries `.recipe` (text) + `.closure` (its dep
  store dirs).
- **Profile.**  `/etc/pkg/profile` — a symlink-free text "installed view";
  `install` adds a store dir, `remove` drops it (the path survives until GC).
- **Mark-sweep GC.**  Reclaims every `/store` path not reachable from the
  profile's transitive closure.
- Built-in recipes (`hello` 1.0/2.0, `args` deps `hello-2`) carry the embedded
  user ELFs as payload; a text recipe format + source fetch are follow-ups.

**Boot-tested (i386):** `pkgtest` builds both `hello` versions (they coexist
under distinct hashes), installs `hello-2` + `args` (pinning `hello-2` in
`args`'s closure), then `pkg gc` reclaims the unreferenced `hello-1.0` and keeps
`hello-2` + `args`.  (Bring-up bug: `vfs_readdir` returns **>0** per entry, not
0.)

**Still deferred (later §M35.5 / cross-milestone):** hermetic builds from source
(fetch + verify + a §M33-sandboxed compile — needs the §M36 toolchain);
**load-time RPATH isolation** (co-designed with §M37's dynamic linker) +
run-time FS-view isolation (§M25/§M32/§M33); rollback generations; a binary
substituter/cache; package signing (needs §M39 crypto); `/proc/pkg`.

---

### 4.30 POSIX syscall breadth + libc growth (M36 stage 1, i386)

**Files:** `kernel/core/usyscall.c` (handlers), `kernel/includes/syscall.h`
(numbers + shared structs), `user/libc.c`/`libc.h`, `user/posixtest.c`.  Shell:
`posixtest`.

Stage 1 of the native-libc milestone: broaden the syscall surface a real libc
sits on, and grow the in-tree libc toward it.  New syscalls 30–35:
- `stat`/`fstat` → `struct kstat {size, type, mode}` from the VFS inode
  (type 0=file/1=dir/2=device; mode 0644/0755).
- `getdents` → packed `[reclen(2)|type(1)|name\0]` records read from a VFS
  directory handle (`vfs_readdir`).
- `uname` → `struct kutsname` (d-os / 0.1 / i386).
- `clock_gettime(CLOCK_REALTIME|MONOTONIC)` → RTC epoch (`rtc_read` →
  days-since-1970) / timer uptime (`timer_ticks_ms`).
- `nanosleep_ms` → `task_msleep`.
The libc grows the matching structs + wrappers, an **`errno`** (wrappers set it
on a negative kernel return), and a `%o` printf conversion.

**Boot-tested (i386):** `posixtest` prints `uname`, `stat /bin/args`
(size/type/mode 644), a `getdents` listing of `/bin` (hello, args),
`clock_gettime` realtime + monotonic, and a `nanosleep` — all from ring 3.

**Stage 2 (the actual musl port) — deferred, external-toolchain infrastructure:**
cross-compile **musl** against the d-os syscall numbers (a d-os `arch/` under
musl, or a Linux-number alias) as the native libc replacing `user/libc.c`; a
`/bin` + `/lib` convention; a minimal coreutils (`sh`/`ls`/`cat`/`echo`/`env`)
as the first musl-linked programs — all installed into the §M35.5 store.  Also
later: `getcwd`/`chdir` (needs a per-task cwd), `brk`/`mremap`, `epoll`/
`eventfd`/`timerfd`, `getrandom` (§M39), full `struct sockaddr`.  (Stage 2's
approach — a modular Linux-ABI layer rather than forking musl — is §4.31.)

---

### 4.31 Linux i386 syscall-ABI compat layer (M36 stage 2 / §M41, i386)

**Files:** `kernel/hal/x86/linux_abi.c`, `kernel/includes/task.h`
(`linux_abi`), `user/linuxhello.c`, `third_party/MUSL.md`,
`scripts/fetch-musl.sh`.  Shell: `linuxtest`.

The **modular** foundation for running an unmodified **musl** (or any prebuilt
Linux i386) binary: rather than fork musl to d-os's syscall numbers, keep musl a
pristine external dependency and have d-os provide the **Linux i386 syscall ABI**
it already targets.

- **Personality.**  `task->linux_abi` (set at exec time, inherited across
  fork/clone) marks a process that traps `int 0x80` with *Linux* syscall numbers
  + struct layouts.  `syscall_dispatch` (hal/x86/syscall.c) routes such a
  process to the Linux translator; the native d-os switch is untouched, so the
  two ABIs coexist.
- **Translator (`linux_abi.c`).**  An *isolated* module mapping Linux i386
  numbers to d-os primitives — `exit`/`exit_group`, `read`/`write`/`writev`,
  `open`/`close`, `getpid`, `mmap2`, `brk`, `set_thread_area` — extensible
  toward the musl-required set; an unknown number logs once and returns
  `-ENOSYS`.  This is the single place the Linux number space + struct
  translations live.
- **musl vendoring.**  `scripts/fetch-musl.sh` clones a *pinned*, unmodified
  musl into `third_party/musl` (gitignored — fetched, not committed);
  `third_party/MUSL.md` documents the build (`configure`+`make` for i386),
  static link (crt1 + libc.a), and the run path.

**`set_thread_area` (TLS) — the #1 musl-startup blocker — DONE.**  The Linux
`set_thread_area(struct user_desc*)` is translated onto the §M35 per-CPU `%gs`
GDT-TLS mechanism (identical to native `SYS_SET_TLS`): record `base_addr` as the
thread's `%gs` base, pin the thread to this CPU (its selector is per-CPU), load
the descriptor base via `hal_set_tls_base`, then **write the allocated GDT index
back into `user_desc.entry_number`** so Linux userland's `%gs = (entry_number
<<3)|3` reconstructs our selector exactly.  `struct lnx_user_desc` is the single
place that layout lives.

**`auxv` on the initial stack — the 2nd musl-startup blocker — DONE.**  The
SysV initial stack (`build_initial_stack` in `kernel/core/proc.c`, shared by
*all* processes — native + Linux) now carries a real auxiliary vector, not just
`AT_NULL`: **`AT_PAGESZ`** (4096 — musl's page-size global), **`AT_CLKTCK`**
(100), **`AT_RANDOM`** (a pointer to 16 non-zero seed bytes — musl's stack-guard
canary + malloc; non-crypto xorshift for now, §M39 swaps in `/dev/urandom`), and
**`AT_SECURE`** (0).  The native i386 crt0 ignores auxv (reads only argc/argv),
so this is regression-free for native programs.

**Boot-tested (i386):** `linuxtest` runs `user/linuxhello.c` — a freestanding
program using the Linux ABI directly (no d-os libc/crt0) under the Linux
personality.  Its asm `_start` captures the SysV entry `%esp` (→ `argc`), then
it (1) prints via Linux `write`=4, (2) calls `set_thread_area`=243, loads `%gs`
from the written-back index and reads its TLS word back through `%gs:0`
(→ `set_thread_area TLS via %gs:0 OK`), (3) walks the stack the way musl's
`__init_libc` does and verifies `AT_PAGESZ==4096` + a non-zero `AT_RANDOM`
(→ `auxv AT_PAGESZ=4096 + AT_RANDOM OK`), then exits via Linux `exit`=1 — all
routed through `linux_abi.c`.  **Both musl-startup blockers (TLS + auxv) now
work end-to-end without musl built yet.**  Regression-checked:
`runargs`/`posixtest`/`forktest`/`libctest` (native, same initial stack) green.

**REAL musl runs (i386) — the Linux-ABI peer's goal, ACHIEVED.**  `make musl`
builds a static i386 musl (`third_party/musl-i386/`, see `third_party/MUSL.md`);
`user/muslhello.c` — an ordinary `#include <stdio.h>` / `printf` program — links
statically against musl's crt1/crti/libc.a/crtn into a stock Linux i386 ELF
(relocated to 0x40000000 via `-Ttext-segment`, + libgcc for musl's 64-bit
`__udivmoddi4`), embedded as a blob and run by the **`musltest`** shell command
under the Linux personality.  It prints via real musl `printf` (`%d`+`%s`) and
returns 0 with **zero unhandled syscalls** — after the compat layer picked up
the last two musl-startup demands: `set_tid_address` (258 → returns the tid) and
`ioctl` (54 → `ENOTTY`, so musl's `isatty()` reports "not a terminal").  An
unmodified, pristine musl binary now runs on d-os.

**musl coreutils in the store — DONE (i386): `echo`, `cat`, `ls`, `env`.**
Ordinary C programs (`user/echo.c` etc.), musl-linked via the generic
`user/%.muslelf` Makefile pattern (add one by listing it in `MUSL_COREUTILS` +
a `register_coreutil` line in pkg.c), `pkg install`ed into the §M35.5
content-addressed store and run FROM `/store` by the **`pkgrun <name> [args…]`**
shell command — not embedded blobs.  `pkgrun echo store coreutils work` → prints
argv; `pkgrun cat /etc/pkg/profile` → real musl buffered file I/O
(fopen/fread/fclose); `pkgrun ls /store` → readdir via `getdents64` lists the
store; `pkgrun env` → prints the process environment.  The kernel side that
grew: `sys_getdents64` (Linux `dirent64` layout, in usyscall.c) + `getdents64`
(220)/`fcntl64` (221, CLOEXEC no-op) in `linux_abi.c`, and a minimal default
**environment** on the SysV stack (`build_initial_stack`: `PATH=/store`,
`HOME=/`, `TERM=d-os` — native crt0 ignores envp, musl exposes it as `environ`).  **The ABI is data-driven (the swappable seam):** each package DECLARES
its ABI in a `<store>/.abi` file (`pkg_recipe.abi`; "linux" for the musl
coreutils, "native" for the d-os-libc demos), and `pkg_run` maps that string to
the exec personality in ONE place (`abi_to_personality`) — call sites never
hardcode "musl"/"linux", so a second backend (BSD, the native musl-fork) is
additive.  Growing `linux_abi.c` for the coreutils added: Linux→VFS **open-flag
translation** (`linux_open_flags` — Linux `O_*` ≠ d-os `VFS_*`; musl opens with
`O_LARGEFILE|O_CLOEXEC`), `openat`(295, `AT_FDCWD`), `readv`(145), `mprotect`
(125, no-op), `munmap`(91, no-op), and a **fix to the `mmap2` register decode**
(len=ecx/fd=edi, was edx/esi → any malloc-driven mmap had failed; latent until a
program actually `malloc`ed, which `muslhello` never did but `cat`'s `fopen`
does).

**A real (non-interactive) `sh` — DONE (i386): the process model, proven.**
`user/sh.c` (musl-linked) runs `sh -c "cmd1 args; cmd2 args"`: it splits on `;`,
tokenises each command, and runs it with the classic **fork() + execvp() +
waitpid()** dance — a musl process spawning *other* musl processes.  This is the
proof that d-os hosts a genuine Unix process model, not just single-shot
programs: `pkgrun sh -c "echo hello from sh; echo second; ls /store"` forks
three children, each execve's a coreutil from `/bin`, prints in order, rc=0.
`pkg install` now also exposes each binary at `/bin/<name>` (the "profile view";
a copy — ramfs has no symlinks yet), and `PATH=/bin` lets musl's `execvp`
resolve bare names.  What it took in `linux_abi.c`: `fork`(2), `execve`(11),
`waitpid`(7)/`wait4`(114) (mapped to `proc_fork`/`proc_execve`/`task_wait`, with
the exit code re-encoded into the Linux wait-status layout), `rt_sigprocmask`
(175, no-op — musl brackets fork with it).  Two deeper fixes it forced:
- **TLS after fork.**  `proc_fork` now inherits `has_tls`/`tls_base`, and the
  child's ring-3 `%gs` is set to the per-CPU TLS selector on resume
  (`fork_child_bootstrap` → `g_entry_gs` → `enter_user_mode_regs`) — musl
  touches thread-local state (errno, the pthread self pointer) immediately after
  fork, so without this the child faulted.
- **A pre-existing COW double-fork bug** (in `vmm_space_clone`): a page already
  COW from a prior fork has `RW=0`, so the clone misclassified it as read-only
  *code* and eager-copied it read-only; a second fork whose parent had not yet
  resolved the page handed the child a non-COW read-only copy that faulted hard
  on write.  Fixed by routing `VMM_COW` pages through the COW branch too.  (This
  bit musl because its `fork` writes the pthread struct only in the *child*, so
  the parent's page stays COW between forks — but the bug is generic, not
  musl-specific.)

**Interactive `sh` — DONE.**  `sh` with no `-c` runs a REPL: prompt → read a
line → fork/exec → repeat → `exit`.  This needed a real **cooked stdin**: the
old `sys_read(fd 0)` returned EOF ("no tty"); it now reads a line from the
**focused virtual console** (`vc_focused()` + `vc_getchar`, the same input ring
the kernel shell reads) with echo + backspace, returning the line incl. `\n`, so
a musl program blocks on the keyboard through fd 0.  Boot-tested: `pkgrun sh` →
`d-os$ ` prompt, `echo hi` runs and prints, `exit` leaves.  (Next tty work: line
editing beyond backspace, `isatty`, job control / signals.)

**The two-brothers seam, proven with a native backend.**  `pkg_run` prints the
backend it selects, and the SAME `pkgrun` over the SAME store now routes to TWO
real ABI backends by the package's declared `.abi`:

```
pkgrun: hello  [abi=native → d-os native backend]   # in-tree d-os libc, native syscall.c
pkgrun: echo   [abi=linux  → linux-abi backend]     # musl, via linux_abi translator
```

So the minimal "second brother" exists today: the small **in-tree d-os libc**
(`user/libc.c`) is a native-ABI libc that runs with `linux_abi` bypassed.  A
*full* native libc — the musl `arch/dos` fork — is deliberately deferred: the
native ABI is a different *shape* (bare-base `SYS_SET_TLS`, `(len,fd)` mmap,
`kstat`), so it needs musl `src/` patches, not a clean `arch/` add; low
functional value, real fork cost.  Parked in `NATIVE_LIBC.md`.

**Next — see `third_party/MUSL.md`:** interactive `sh` (blocking stdin) + more
coreutils.

---

### 4.32 Wayland display server (M26, stage 1 — wire protocol + handshake)

Wayland is a wire protocol spoken over a unix socket; the client and server
exchange messages `[object_id:u32][ (size<<16)|opcode :u32 ][args…]` (little-
endian; u32/int/object/new_id are one word, strings are a u32 length + bytes
padded to 4, fds travel out-of-band via SCM_RIGHTS).  **Stage 1** (i386 today,
arch-independent) implements the transport + the core objects — **wl_display,
wl_registry, wl_callback** — enough for the canonical handshake:

- `wl_display.get_registry(new_id)` → the server advertises its **globals** via
  `wl_registry.global(name, interface, version)` (currently `wl_compositor` v4,
  `wl_shm` v1, `xdg_wm_base` v2);
- `wl_display.sync(new_id)` → the server answers `wl_callback.done(serial)` then
  `wl_display.delete_id`.

It is the **real** Wayland wire format (a real libwayland client would speak the
same bytes), but there is no libwayland on d-os yet, so `wl_selftest` (shell
`waytest`) drives a **hand-marshalled client** over a `usock_pair` against the
server dispatch — the analogue of `user/linuxhello.c` proving the Linux ABI
before real musl.  Boot-tested: get_registry → 3 globals received, sync →
`callback.done(serial=0)` + `delete_id(3)`.  Lesson: opcode 0 is ambiguous
(both `wl_registry.global` and `wl_callback.done`), so a client MUST dispatch by
the object's *interface*, not the opcode alone.

The whole thing sits on the **M25 substrate** (unix sockets + fd passing + memfd
shm) and the **M22.7 compositor**, which was deliberately built
surface-compositor-shaped so `wl_surface` maps onto a `gui_window`'s
`gfx_surface` 1:1.  `kernel/gui/wayland.c` + `kernel/includes/wayland.h`.

**Stage 2 — the shm buffer path — DONE (i386 + x86_64).**  The hard part of
Wayland (a client sharing pixel memory with the server): `wl_registry.bind`
(registry → `wl_compositor` + `wl_shm`; `wl_shm` then advertises its
`format`s), `wl_compositor.create_surface`, `wl_shm.create_pool` — the client's
shared-memory **fd travels out-of-band via SCM_RIGHTS** (`usock_send`'s
passfile; the server dequeues it with `usock_recv`'s `passfile_out`) —
`wl_shm_pool.create_buffer` (offset/w/h/stride/format), `wl_surface.attach` +
`wl_surface.commit`.  On commit the server reads the buffer's pixels straight
out of the pool's `struct shm` frames (via the kernel identity map, like fd.c)
and logs proof.  Boot-tested: a client fills a 4×4 ARGB buffer with `0x3366CCFF`,
and the server's commit reads back `top-left=3366ccff` + `checksum=366ccff0`
(= 16 × the colour) — the pixels crossed the wire + the fd passing intact — then
sends `wl_buffer.release`.

**Stage 3 — xdg_shell top-level role — DONE (protocol).**  The modern window
protocol: `wl_registry.bind`(`xdg_wm_base`) → `xdg_wm_base.get_xdg_surface`
(wrapping the `wl_surface`) → `xdg_surface.get_toplevel`, at which point the
server sends the initial **configure** pair (`xdg_toplevel.configure(w,h,states)`
+ `xdg_surface.configure(serial)`); the client then `set_title`s and
`ack_configure(serial)`s.  Boot-tested via `waytest`: configure round-trip +
`set_title("d-os window")` + ack, end to end.

**The compositor bridge — a wl_surface's pixels reach the screen — DONE.**  A
`wl_conn` may carry a `target` `gfx_surface` (+ a blit origin); when set,
`wl_surface.commit` paints the committed buffer's pixels straight onto it.
Shell `waydemo` wires the target to the **live framebuffer** (`gfx_fb_surface`)
and commits a 32×32 gradient `wl_shm` buffer at (200,150); the framebuffer
readback confirms `fb[200,150]` == the buffer's top-left (`VISIBLE OK`) — the
full path client shm buffer → SCM_RIGHTS → server read → composite → **on-screen
pixels** works, and is arch-independent (i386 + x86_64).

**WM-managed window, input, and a real ring-3 client — DONE.**
- **`gui_window` target** (`waywin`): `gui_window_blit` paints a raw pixel block
  into a window's content surface (under `win->lock`) + composites it; a
  `wl_conn.window` makes `wl_surface.commit` blit the buffer into a real
  WM-managed window (chrome + move/resize free from the M22 WM).  `gui_window_
  pixel` reads it back: `window[0,0]` == the committed buffer → `IN-WINDOW OK`.
- **`wl_seat` input** (`wayinput`): the server advertises `wl_seat` (v5), sends
  `capabilities(pointer|keyboard)`, and `wl_send_key`/`wl_send_motion` push
  `wl_keyboard.key` / `wl_pointer.motion` events to the client — the hooks the
  M22.7 input router calls to forward real input.
- **A real ring-3 client** (`wayclient`): `user/wlclient.c` is a freestanding
  native-ABI program that speaks the Wayland wire protocol over an inherited
  socket fd (fd 3); the shell hands one end of a `usock_pair` to a spawned
  server task (`wl_conn_serve`) and installs the other as the client's fd 3, then
  execs it.  The client blocks on `read(3)`, the server task answers
  concurrently; the client parses the 4 globals + `wl_callback.done` from user
  space and reports `handshake OK`.  A genuine client/server split over a unix
  socket.

**Server-per-surface + input routing — DONE (`waycomp`).**  With `wl_conn.wm_mode`
set, `xdg_surface.get_toplevel` creates a real WM-managed `gui_window` for the
surface (a client's top-level IS a first-class desktop window); the surface's
committed buffers become the window's contents, and the window's input is routed
to the client via a `gui_window_set_input_hook` that calls `wl_send_key`/
`wl_send_motion`.  Boot-tested: a client binds compositor/shm/xdg/seat, its
`get_toplevel` spawns a desktop window, `commit` fills it (`SURFACE-IN-WINDOW
OK`), and delivered input (key 30, motion 50,40) arrives at the client's
`wl_keyboard`/`wl_pointer`.  The M22.7 "wl_surface = gui_window, input = wl_seat"
design closes cleanly.

**A reusable client library — `user/libwl` (mini-libwayland) — DONE.**  So an
app calls `wl_*` instead of hand-marshalling the wire protocol: `wl_connect(fd)`,
`wl_alloc_id`, `wl_registry_roundtrip` (get_registry + sync, framing partial
reads, recording each global's registry name).  `user/wlapp.c` links it and,
over the inherited fd 3, discovers `wl_compositor`/`wl_shm`/`xdg_wm_base`/
`wl_seat` from ring 3 (shell `wayapp`).  This is the "app links a Wayland client
library" shape; it is NOT upstream libwayland.

**Next (§M40):** port the **upstream libwayland-client** — it needs
`wayland-scanner` + the protocol XML → generated proxies, built as a
musl-linked library — so *unmodified* Wayland apps (GTK/Qt/SDL clients) run.
Also: run the server connection loop as a compositor-hosted task so a real
client's window is created + fed automatically (not just in `waycomp`).

---

### 4.33 Dynamic linking — ld.so / .so / dlopen (M37, i386)

Extends the M25 static ELF loader to run **dynamically-linked** programs.  The
elegant part: the kernel does **no relocation or symbol resolution** — that is
the interpreter's (musl's `ld.so`) job in ring 3.  The kernel only maps the
main object + the interpreter and hands over a correct auxv.

**Build (`make musl`).**  musl is now built **shared as well as static**
(dropped `--disable-shared`): `libc.so` **is** the dynamic linker (the
interpreter `/lib/ld-musl-i386.so.1` is a symlink to it).  A dynamically-linked
program is compiled `-fPIC` and linked `-pie -dynamic-linker
/lib/ld-musl-i386.so.1` against `libc.so` by name (so its `DT_NEEDED` records
the clean soname `libc.so`, not a build path).  Generic Makefile patterns:
`user/%.dynelf` (a PIE program) and a `.so` rule; `libc.so` + `libgreet.so` are
embedded as blobs and written into the VFS at boot by `pkg.c`'s
`ldso_provision()` (`/lib/ld-musl-i386.so.1`, `/lib/libc.so`, `/lib/libgreet.so`).

**Loader (`elf.c`).**  `elf_load_ex()` adds ET_DYN/PIE support: it applies a
caller-supplied load bias to every `p_vaddr`, captures the `PT_INTERP` path,
and reports the in-memory program-header address (from `PT_PHDR`, or derived
from the covering `PT_LOAD` for static images — AT_PHDR must be a real VA, not a
file offset).  `elf_load()` stays as the pre-M37 static wrapper.

**Exec paths (`proc.c`).**  `load_program()` maps the main object at the user
base and, if `PT_INTERP` is present, reads the interpreter from the VFS and maps
it at `user_base + 0x200000` (between the stack and the mmap region, so ld.so's
own mmaps never overlap), then starts execution at the interpreter's entry.
`build_initial_stack()` now emits the full SysV auxv the linker reads:
`AT_PHDR/AT_PHENT/AT_PHNUM/AT_BASE/AT_ENTRY` (plus the existing
`AT_PAGESZ/CLKTCK/RANDOM/SECURE`).  All three exec paths (excursion / execve /
spawn) share it.

**Syscall surface ld.so needs (`usyscall.c` + `linux_abi.c`).**  A separate `.so`
(not the libc-is-interpreter shortcut) forced three real additions:
- **full `mmap2`** (`sys_mmap_full`) — honors `addr`+`MAP_FIXED`, `prot`→VMM
  flags (a text segment maps executable), and **file-backed** mappings (reads
  `len` bytes from the fd at `offset`); the old `sys_mmap` only did anonymous /
  memfd regions.
- **real `mprotect`** (`sys_mprotect` + `vmm_space_protect`) — was a no-op;
  musl's mallocng maps a `PROT_NONE` reservation then mprotects it to R/W, and
  ld.so tightens RELRO to read-only, so it must actually change PTE perms.
- **`fstat64`** — `ld.so`'s `map_library` fstats a `.so` for its size before
  mmapping it; translated to the Linux i386 `struct stat64`.  Also: `open`/
  `openat` now return `-ENOENT` (not a generic `-1`) on failure, or musl's
  library-search loop aborts instead of trying the next path.  (`statx` (383)
  is left ENOSYS — musl falls back to `fstat64`.)

**Verified (boot, i386):** `musldyntest` (a PIE musl hello — ld.so
self-relocates, resolves `printf`, calls `main`, rc=0); `solibtest` (links a
separate `libgreet.so` via `DT_NEEDED`: `greet_add(40,2)=42` main→lib JMP_SLOT,
`greet_msg` lib→libc snprintf, `greet_tag` GLOB_DAT, and a `.so` `__thread`
bumps `101,102,103` — the general-dynamic TLS path `__tls_get_addr` +
DTPMOD/DTPOFF on the §M35 `%gs` pointer); `dlopentest` (`dlopen`/`dlsym`/
`dlclose` of `/lib/libgreet.so` at runtime).  Static musl (`musltest`) stays
regression-free (the AT_PHDR fix: musl reads AT_PHDR even for static binaries to
find `PT_TLS`).

**Open:** x86_64/aarch64; a real `brk` heap (mallocng falls back to mmap today);
mmap reclaim on munmap; pthreads under the Linux ABI (needs `clone` wired in —
the TLS *relocation* model is proven, but multi-thread `__thread` awaits §M44's
musl pthread path); lazy PLT resolution (BIND_NOW-style works today).

---

### 4.34 C++ runtime — libstdc++ + exceptions (M38, i386)

Runs unmodified C++ programs with exceptions, RTTI, and the STL, dynamically
linked against a real musl libstdc++.

**Toolchain (`make musl-cross-i686`).**  A from-source musl C++ cross-toolchain
built with **musl-cross-make** (gcc/g++ 11.2.0 + binutils + musl 1.2.3): it
produces `i686-linux-musl-g++` plus `libstdc++.so.6` + `libgcc_s.so.1` in a musl
sysroot.  Built on the container-local fs (the macOS Docker mount breaks tar's
directory-metadata restore — `--delay-directory-restore`; and the build is
slow under amd64 emulation).  Fetched by `scripts/fetch-musl-cross.sh`.

**Building C++ for d-os.**  Programs compile `-fPIE` and link `-pie` → ET_DYN,
so the §M37 loader relocates them to the user base (a non-PIE EXEC would land
at 0x08048000, inside the kernel region).  PT_INTERP = `/lib/ld-musl-i386.so.1`;
DT_NEEDED = `libstdc++.so.6` + `libgcc_s.so.1` + `libc.so` (+ app `.so`s).  The
runtime `.so`s (stripped: libstdc++ ~2.1 MB, libgcc_s ~112 KB) are embedded as
blobs and provisioned into `/lib` at boot (`pkg.c`), so `ld.so` resolves them.

**Verified (boot, i386):** `cpptest` — a dynamically-linked C++ PIE — runs under
the Linux personality: `std::vector<std::string>` + `std::sort` work; an
exception THROWN in `libcpplib.so` is CAUGHT in `main` **across the `.so`
boundary** (DWARF unwinding via `.eh_frame`/`_Unwind_*` crosses shared objects —
the M38 definition-of-done); a thread-safe local static in the `.so`
(`__cxa_guard_*`) works.

**Open:** x86_64/aarch64; the heavy support libs (zlib/freetype/harfbuzz/ICU/
Skia — §M38 continued); `libc++` as an alternative to libstdc++.

---

### 4.35 Crypto, entropy & TLS (M39, i386)

**Stage 1 — entropy (`kernel/core/random.c`, arch-generic).**  A ChaCha20
CSPRNG (RFC 8439) with **fast key erasure** (the key is overwritten with fresh
never-output keystream after each request → forward secrecy, like Linux's
`get_random_bytes`), seeded from a hardware RNG where present + boot/timing
jitter.  Exposed as `/dev/urandom` + `/dev/random`, the `getrandom` syscall
(native SYS 36 / Linux 355), and the per-exec `AT_RANDOM` auxv.  The only
arch-specific bit is `hal_hw_random` (RDRAND on x86, CPUID-gated; a weak no-op
elsewhere).  Verified: `randtest` (two draws differ, `/dev/urandom` via VFS;
`-cpu max` exercises the RDRAND seed path).

**Stage 2 — crypto library (Mbed TLS v3.6.2).**  Ported pristine (`make
mbedtls`, C, built against our musl); `libmbed{crypto,x509,tls}.a`.  Verified:
`crypttest` passes a SHA-256 known-answer test + an AES-256-GCM encrypt→decrypt
round-trip in ring 3.

**Stage 3 — TLS (`ssltest`).**  A full **verified TLS 1.3** handshake between an
in-process client and server completes, then encrypted application data is
exchanged:

```
handshake OK — TLSv1.3 / TLS1-3-CHACHA20-POLY1305-SHA256
cert verify flags = 0x0 (trusted)      <- real X.509 verification vs the CA
app data over TLS: "hello over TLS from d-os" (match)
```

Exercises ECDHE + ChaCha20-Poly1305 AEAD + ECDSA CertVerify + X.509 chain
verification + the record layer, all seeded from the stage-1 CSPRNG
(`mbedtls_entropy` → `getrandom`).  The two BIO callbacks ferry bytes through
ring buffers; the same `mbedtls_ssl_set_bio` seam takes real M24-socket
send/recv for a network client (stage 3b).  Getting verified TLS working forced
wiring the **Linux-ABI time syscalls** — `time`(13)/`gettimeofday`(78)/
`clock_gettime`(265)/`clock_gettime64`(403) → `sys_clock_gettime` — because
mbedTLS's x509 date check fatals without a working clock (and this benefits
every musl program).  The M25 single-page user stack also had to grow (the TLS
handshake overflowed it): the per-process layout is now image / interp (+64 MiB)
/ stack (+96 MiB, grows down, 1 MiB) / mmap (+128 MiB), non-overlapping.

**Open:** stage 3b `wget https://` to a real server over the M24 sockets (needs
a net-enabled boot + the Mozilla CA bundle at `/etc/ssl/certs`); DNS
`getaddrinfo` + `/etc/resolv.conf`; x86_64/aarch64.

---

### 4.36 On-device C compiler — TinyCC (M43 slice, i386)

Compile + run C **on d-os** — the first slice of §M43 (self-hosting: develop on
d-os, in d-os).  gcc/clang are too big to run on d-os; TinyCC (tcc) is a tiny,
single-pass C compiler that emits runnable ELFs with its own linker.

**Build (`make tcc`).**  tcc is cross-built with the musl C++ toolchain into a
PIE i686-musl binary that runs on d-os under §M37, configured with d-os target
paths (`--elfinterp=/lib/ld-musl-i386.so.1`, `--crtprefix=/lib`,
`--libpaths="{B}:/lib"`, `--sysincludepaths="{B}/include:/usr/include"`,
`--config-musl --config-pie --config-bcheck=no --config-backtrace=no`).  A flat
rootfs archive (`scripts/pack-rootfs.py`, unpacked by `pkg.c`'s
`rootfs_unpack`) provisions the compile inputs into the VFS: tcc's own headers +
`libtcc1.a` at `/usr/lib/tcc`, musl headers at `/usr/include`, crt at `/lib`.

**Use.**  `tcc <args>` runs the embedded tcc (Linux personality, `-B/usr/lib/tcc`)
with the shell args; `exec <path>` loads + runs a VFS ELF.  Verified (boot):

```
d-os$ tcc /hello.c -o /hello        # compile + link a full stdio program
d-os$ exec /hello
hello, compiled on d-os by tcc!     # ...and it runs
```

The **Editor** (M22.5) gains a **"Run" button** that saves the buffer, compiles
it with tcc, and runs the result (`devtools.h`: `dos_tcc_compile`/`dos_run_elf`
— the shared engine).

**Needed (all help every musl program):** Linux-ABI `_llseek`(140) (tcc seeks
in `.o` files — else "invalid object file"), `lseek`(19), `unlink`(10).

**Open:** capture the program's stdout into an editor result pane (it goes to
the console today); a bigger compiler (gcc/clang) for full self-hosting; more
Linux ABI breadth; x86_64/aarch64.

---

## 5. Build & run

```sh
./scripts/build.sh                    # default: ARCH=i386 → build/i386/d-os.iso
./scripts/run_qemu.sh                 # qemu-system-i386 -cdrom build/i386/d-os.iso

ARCH=x86_64 ./scripts/build.sh        # → build/x86_64/d-os.iso
ARCH=x86_64 ./scripts/run_qemu.sh     # qemu-system-x86_64 ...
```

Each ARCH gets its own object tree under `build/$(ARCH)/`, so the two
builds never collide and you can ping-pong between them without `make
clean`.  `make clean` wipes only the current ARCH; `make clean-all`
wipes both.

Host needs Docker (the build is done inside a pinned `ubuntu:22.04 amd64`
container to avoid arm64 Mac package availability issues). Host can
optionally have a native `qemu-system-i386` / `qemu-system-x86_64`
(e.g. `brew install qemu`) for running with a graphical window;
otherwise `run_qemu.sh` falls back to headless qemu inside the Docker
image.

The Makefile has no header-dependency tracking — after editing a
shared header (e.g., `hal_api.h`, `vmm.h`, `idt.h`), run `make clean
ARCH=<arch>` to force a rebuild.  Auto-generated `.d` files via `gcc
-MMD` are on the polish backlog.

## 6. Compiler flags

i386:
```
-m32                      i386 code generation
-ffreestanding            no hosted environment, no libc
-fno-stack-protector      no canary checks
-fno-pie                  generate non-PIC code (we run at a fixed address)
-nostdlib                 don't link libc / crt0
-Wall -Wextra             noisy diagnostics
-std=c11                  stable dialect
```
Linker: `ld -m elf_i386 -T linker-i386.ld -nostdlib`.

x86_64 (additions / changes from i386):
```
-m64                      long-mode code generation
-mno-red-zone             kernel: IRQs share rsp, red zone unsafe
-mno-mmx -mno-sse{,2,3}   no SIMD (FPU/XMM not init'd in our entry)
-mno-3dnow                no 3DNow (very old AMD)
-mcmodel=large            link kernel anywhere in 64-bit address space
```
Linker: `ld -m elf_x86_64 -T linker-x86_64.ld -nostdlib -z max-page-size=0x1000`.

---

## 7. Roadmap / open milestones

- [x] **M1 — GDT:** own Global Descriptor Table, stop relying on GRUB's.
- [x] **M2 — IDT + PIC:** exception handlers, remap PIC, enable IRQ1
  (keyboard) and retire polling.
- [x] **M3 — Multiboot memory map + `meminfo`:** parse the mmap the
  bootloader gave us in `kernel_main` and expose a command.
- [x] **M4 — PMM:** bitmap-based physical memory allocator.
- [x] **M5 — Paging / VMM:** page directory, kernel-space mapping, demand
  mappings.
- [x] **M6 — VBE framebuffer + bitmap font:** graphical text mode so we
  can pick a sane resolution and font size.

---

## 8. Change log

- **2026-07-19 — M43 slice: on-device C compiler (TinyCC), i386 (DOCS §4.36).**
  `tcc /hello.c -o /hello` compiles + links a full stdio C program ON d-os and
  `exec /hello` runs it — the first self-hosting slice.  tcc cross-built PIE
  (musl toolchain, `--config-musl/pie`), provisioned with a rootfs archive
  (tcc/musl headers + crt + libtcc1.a) via `pack-rootfs.py`/`rootfs_unpack`.
  Shell `tcc`/`exec`; Editor "Run" button (`devtools.h`).  Needed Linux-ABI
  `_llseek`/`lseek`/`unlink`.  `DOS_MILESTONE=M43`.
- **2026-07-19 — M38 (C++ runtime) + M39 stages 1–3 (crypto/entropy/TLS), i386
  (DOCS §4.34, §4.35).**  **M38:** a from-source musl C++ toolchain (musl-cross-
  make, g++ 11.2.0) + libstdc++; `cpptest` throws an exception in a `.so` and
  catches it in `main` across the boundary (DWARF unwinding across shared
  objects) + STL, dynamically linked (libstdc++.so.6/libgcc_s.so.1 provisioned
  to `/lib`).  **M39.1:** a ChaCha20 CSPRNG (`random.c`, arch-generic) →
  `/dev/urandom`+`/dev/random`+`getrandom`+`AT_RANDOM` (RDRAND seed on x86).
  **M39.2:** Mbed TLS v3.6.2 ported; `crypttest` passes SHA-256 + AES-256-GCM.
  **M39.3:** `ssltest` completes a **verified TLS 1.3** handshake (ECDHE +
  ChaCha20-Poly1305 + ECDSA CertVerify + X.509 verify, `flags=0x0`) and exchanges
  encrypted app data, seeded from the CSPRNG.  Needed: Linux-ABI time syscalls
  (`time`/`gettimeofday`/`clock_gettime`/`clock_gettime64`) — without a clock
  mbedTLS's x509 date check fatals — and a bigger multi-page user stack (the M25
  single page overflowed during the handshake; new non-overlapping layout:
  image / interp+64M / stack+96M / mmap+128M).  `DOS_MILESTONE=M39`.  Open:
  `wget https` over real sockets (stage 3b), x86_64/aarch64, the §M38 support-lib
  stack (freetype/harfbuzz/ICU/Skia).
- **2026-07-18 — M37: dynamic linking (ld.so / .so / dlopen), i386 (DOCS §4.33).**
  An unmodified musl program now runs **dynamically linked**: the kernel maps the
  PIE main object + the interpreter (`/lib/ld-musl-i386.so.1`) with a full SysV
  auxv (`AT_PHDR/PHENT/PHNUM/BASE/ENTRY`) and hands control to musl's `ld.so`,
  which self-relocates and resolves symbols in ring 3.  `make musl` now builds
  musl **shared** too (`libc.so` == the dynamic linker).  ELF loader gained
  ET_DYN/PIE support (`elf_load_ex`, load bias + `PT_INTERP` + `PT_PHDR`); the
  exec paths gained `load_program` (main + interpreter).  A genuinely separate
  `.so` forced a real syscall surface: **full `mmap2`** (`sys_mmap_full`:
  addr+`MAP_FIXED`, prot→VMM flags, file-backed at offset), **real `mprotect`**
  (`sys_mprotect` + `vmm_space_protect` — was a no-op; mallocng + RELRO need it),
  **`fstat64`**, and `open`→`-ENOENT` (so musl's library search advances).
  Verified on i386 (boot): `musldyntest` (PIE hello), `solibtest` (separate
  `libgreet.so` via `DT_NEEDED` — cross-object JMP_SLOT/GLOB_DAT + a `.so`
  `__thread` via the general-dynamic TLS path 101,102,103), `dlopentest`
  (`dlopen`/`dlsym`/`dlclose`); static `musltest` regression-free.  Open:
  x86_64/aarch64, real `brk`, mmap reclaim, pthreads under the Linux ABI.
- **2026-07-18 — M26: Wayland compositor integration + a client library.**  Three
  remaining points closed (DOCS §4.32): (1) **server-per-surface** —
  `wl_conn.wm_mode` makes `xdg get_toplevel` spawn a real `gui_window` for the
  surface, commits fill it; (2) **input routing** — `gui_window_set_input_hook`
  forwards a window's keyboard/pointer input to the client's `wl_seat`
  (`wl_send_key`/`wl_send_motion`); `waycomp` shows both (`SURFACE-IN-WINDOW OK`
  + key/motion delivered).  (3) **`user/libwl`** — a reusable mini-libwayland
  client library (`wl_connect`/`wl_registry_roundtrip`); `user/wlapp.c` uses it
  to discover the globals from ring 3 (`wayapp`).  Upstream libwayland (unmodified
  Wayland apps) is §M40.
- **2026-07-18 — M26: Wayland WM window + wl_seat input + a real ring-3 client.**
  Three points close the core (DOCS §4.32): (1) `gui_window_blit`/`gui_window_
  pixel` + `wl_conn.window` → a committed `wl_shm` buffer becomes a real
  WM-managed window's contents (`waywin` → `IN-WINDOW OK`); (2) `wl_seat` (v5) +
  `wl_send_key`/`wl_send_motion` → keyboard/pointer events to the client
  (`wayinput`); (3) `user/wlclient.c` — a freestanding native-ABI **ring-3**
  Wayland client speaking the wire protocol over an inherited fd 3, served by a
  spawned `wl_conn_serve` task (`wayclient` → 4 globals parsed from user space).
  Also: the desktop label is now dynamic (`version.h` `DOS_MILESTONE`, shows the
  latest shipped M).  Next: server-per-surface compositor task, route M22.7
  input, port libwayland (§M40).
- **2026-07-13 — M26: Wayland compositor bridge — a wl_surface reaches the
  screen, i386+x86_64.**  `wl_conn.target` (a `gfx_surface` + blit origin); when
  set, `wl_surface.commit` paints the committed buffer's pixels onto it (DOCS
  §4.32).  Shell `waydemo` targets the **live framebuffer** and commits a 32×32
  gradient `wl_shm` buffer at (200,150); the framebuffer readback confirms the
  pixels landed (`VISIBLE OK`).  The full path — client shm buffer → SCM_RIGHTS
  → server read → composite → on-screen pixel — works.  Next: a WM-managed
  `gui_window` target + `wl_seat` input.
- **2026-07-13 — M36: interactive `sh` (cooked stdin), i386.**  `sh` with no `-c`
  runs a REPL (prompt → fork/exec → repeat → `exit`).  `sys_read(fd 0)` now reads
  a cooked line from the focused vc (`vc_focused`/`vc_getchar`, echo + backspace)
  instead of returning EOF, so a musl program blocks on the keyboard.  DOCS
  §4.31.  Boot-tested: `pkgrun sh` → `d-os$` prompt runs `echo`, `exit` leaves.
- **2026-07-13 — M26 stage 2: Wayland shm buffer path — the shared-memory frame,
  i386.**  The hard part of Wayland (DOCS §4.32): `wl_registry.bind` + `wl_shm`
  (format events) + `wl_compositor.create_surface` + `wl_shm.create_pool` (the
  client's memfd passed **out-of-band via SCM_RIGHTS** — `usock_send` passfile →
  `usock_recv` passfile_out) + `wl_shm_pool.create_buffer` + `wl_surface.attach`/
  `commit`.  On commit the server reads the buffer's pixels from the pool's
  `struct shm` frames (identity map).  `waytest`: a 4×4 `0x3366CCFF` buffer →
  server reads `top-left=3366ccff` + matching checksum → `wl_buffer.release`.
  Next: `gfx_blit` to a `gui_window` (visible), then `xdg_shell`.
- **2026-07-13 — x86_64 build parity restored.**  The shared `user/libc.c` +
  `kernel/core/shell.c` had accreted i386-only dependencies (M34/M35 signal +
  thread trampolines; M23/M24/M35/M35.5 audio/net/futex/pkg) that broke the
  x86_64 link.  Fixed by stubbing `__sig_trampoline`/`__thread_exit_tramp` in
  `crt0_x86_64.s` (features are i386-only, but the symbols must resolve) and
  adding the portable cores `net.c`/`audio.c`/`futex.c`/`pkg.c` to the x86_64
  build (the *drivers* stay i386-only → `net_primary`/`audio_primary` return
  NULL and the shell commands report "no device").  x86_64 boots to the shell
  and runs `waytest` (the M26 Wayland handshake) — the stage-1 server is
  arch-independent.  (aarch64 shell has the same latent gap — a follow-up.)
- **2026-07-12 — M26 stage 1: Wayland display server — wire protocol + handshake,
  i386.**  The real Wayland wire format over a unix socket (`kernel/gui/
  wayland.c`): wl_display + wl_registry + wl_callback, enough for the canonical
  handshake — `get_registry` → advertise globals (wl_compositor/wl_shm/
  xdg_wm_base), `sync` → `wl_callback.done` + `delete_id`.  Shell `waytest`
  drives a hand-marshalled client over a usock_pair (à la linuxhello); on the
  M25 socket/shm substrate + the M22.7 surface-compositor.  DOCS §4.32.  Next:
  wl_shm buffers + wl_surface bridged to a gui_window, then xdg_shell.
- **2026-07-12 — M36: the two-brothers ABI seam proven with a native backend,
  i386.**  `pkg_run` now prints the selected backend; `pkgrun hello` (in-tree
  d-os libc, `abi=native`) routes to the native syscall path and `pkgrun echo`
  (musl, `abi=linux`) to the `linux_abi` translator — same store, same pkgrun,
  two real ABI backends chosen by data (DOCS §4.31).  The minimal "second
  brother" is the in-tree native libc; the full native musl (`arch/dos` fork) is
  parked (`NATIVE_LIBC.md`) — it needs musl `src/` shape patches, not just a
  renumbered `arch/`, so it is a separate project.
- **2026-07-12 — M36: a real (non-interactive) musl `sh` — the process model,
  i386.**  `user/sh.c` runs `sh -c "cmd; cmd"` via fork()+execvp()+waitpid() — a
  musl process spawning musl coreutils from `/bin` (DOCS §4.31).  linux_abi
  gained fork(2)/execve(11)/waitpid(7)/wait4(114)/rt_sigprocmask(175); `pkg
  install` exposes binaries at `/bin/<name>` + `PATH=/bin`.  Two deeper fixes:
  **TLS-after-fork** (proc_fork inherits has_tls/tls_base; child's %gs = TLS
  selector via g_entry_gs/enter_user_mode_regs) and a **pre-existing COW
  double-fork bug** in vmm_space_clone (a page already COW from a prior fork was
  misclassified as read-only code and eager-copied RO → hard fault on the second
  fork; fixed by routing VMM_COW pages through the COW branch).  Regression:
  forktest/forkexec/musltest/threadtest(20000)/tlstest green.
- **2026-07-12 — M36: `ls` + `env` musl coreutils, i386.**  `ls` (readdir via a
  new `sys_getdents64` = Linux `dirent64` layout + `getdents64`/`fcntl64` in
  linux_abi) and `env` (a minimal default environment — `PATH`/`HOME`/`TERM` —
  now on the SysV initial stack, `build_initial_stack`).  `pkgrun ls /store` +
  `pkgrun env` clean (0 unhandled).  DOCS §4.31.
- **2026-07-12 — M36/M35.5: musl coreutils run FROM the store, data-driven ABI,
  i386.**  `echo`+`cat` (musl-linked, generic `user/%.muslelf` pattern) are
  `pkg install`ed into the content-addressed store and exec'd from `/store` by
  `pkgrun <name> [args]` (DOCS §4.31).  **Swappable seam:** each package declares
  its ABI (`pkg_recipe.abi` → `<store>/.abi`); `pkg_run` maps it to the exec
  personality in ONE place (`abi_to_personality`) — no hardcoded "musl"/"linux".
  `linux_abi.c` grew: open-flag translation (Linux `O_*`→`VFS_*`), openat/readv/
  mprotect/munmap, + an `mmap2` register-decode fix (len=ecx/fd=edi — latent
  until a program malloc'd). Regression-checked musltest/posixtest/forktest/
  pkgtest green.
- **2026-07-12 — M36 stage 2: REAL, unmodified musl runs on d-os, i386.**  The
  Linux-ABI peer's goal (DOCS §4.31).  `make musl` builds a static i386 musl
  (`third_party/musl-i386/`; fetch-musl.sh pins v1.2.5); `user/muslhello.c` (an
  ordinary stdio/printf program) links against musl's crt1/libc.a into a stock
  Linux ELF (`-Ttext-segment=0x40000000` + libgcc), embedded + run by the
  `musltest` command under `task->linux_abi`.  Prints via real musl `printf`,
  returns 0, **zero unhandled syscalls** — the compat layer picked up the last
  startup demands: `set_tid_address` (258) + `ioctl`→ENOTTY (54, isatty).  See
  `third_party/MUSL.md`.  Next: coreutils → §M35.5 store; native musl-fork peer.
- **2026-07-11 — M36 stage 2 (cont.): `auxv` on the initial stack — the 2nd
  musl-startup blocker — DONE, i386.**  `build_initial_stack` (proc.c, shared by
  all processes) now emits a real auxv: `AT_PAGESZ`=4096, `AT_CLKTCK`=100,
  `AT_RANDOM`→16 seed bytes, `AT_SECURE`=0 (was just `AT_NULL`).  Native crt0
  ignores auxv → regression-free.  `linuxtest` now walks the stack like musl's
  `__init_libc` and verifies `AT_PAGESZ`+`AT_RANDOM` (→ `auxv … OK`).  Both musl
  startup blockers (TLS + auxv) done; next is `make musl`.  Also: parked the
  own-libc debate in `NATIVE_LIBC.md`; §M36 stage 2 reframed as the "two
  brothers" (Linux-ABI peer + native musl-fork peer).  DOCS §4.31.
- **2026-07-11 — M36 stage 2 (cont.): `set_thread_area` TLS — the #1 musl-startup
  blocker — DONE, i386.**  The Linux `set_thread_area(struct user_desc*)` is
  translated onto the §M35 per-CPU `%gs` GDT-TLS mechanism (DOCS §4.31): record
  the base, pin the thread to its CPU, load the descriptor base, and write the
  allocated GDT index back into `user_desc.entry_number` so Linux userland's
  `%gs=(entry_number<<3)|3` reconstructs our selector.  Boot-tested: `linuxtest`
  now calls `set_thread_area` + reads its TLS word back through `%gs:0` (→ `TLS
  via %gs:0 OK`).  Remaining musl-startup weld: `auxv` on the initial stack.
- **2026-07-11 — M36 stage 2: modular Linux i386 syscall-ABI compat layer, i386.**
  The foundation for running an unmodified (vendored, pristine) musl (DOCS §4.31,
  also §M41): a `task->linux_abi` personality + an isolated Linux-i386 syscall
  translator (`kernel/hal/x86/linux_abi.c`) mapping Linux numbers → d-os
  primitives; `syscall_dispatch` routes a Linux-personality process there, the
  native ABI untouched.  `scripts/fetch-musl.sh` pins + fetches musl (gitignored,
  not forked); `third_party/MUSL.md` has the build/link/run plan + the
  Linux-ABI checklist musl needs (set_thread_area/auxv/…).  Boot-tested:
  `linuxtest` runs a freestanding Linux-ABI program (write=4/exit=1) end-to-end
  without musl yet.  See PLAN.md §M36.
- **2026-07-11 — M36 stage 1: POSIX syscall breadth + libc growth, i386.**  The
  surface a real libc sits on (DOCS §4.30): syscalls 30–35 — stat/fstat (kstat
  from the VFS inode), getdents (packed dir records), uname, clock_gettime
  (RTC epoch / timer uptime), nanosleep; libc grows the structs + wrappers +
  errno + a %o printf.  `posixtest` from ring 3: uname, stat /bin/args,
  getdents /bin, realtime+monotonic clock, nanosleep.  See PLAN.md §M36.
- **2026-07-11 — M35.5: content-addressed package store (first slice), i386.**
  The porting-discipline gate before foreign code (DOCS §4.29): a Nix/Guix-shaped
  store on the VFS (`kernel/core/pkg.c`) — content-addressed
  `/store/<hash>-name-version/` paths (hash folds in the recipe + each dep's
  recursive hash), version coexistence, pinned `.closure`, a symlink-free
  `/etc/pkg/profile`, and mark-sweep GC.  Shell `pkg build|install|remove|why|
  list|gc` + `pkgtest`.  Boot-tested: two `hello` versions coexist, install
  `hello-2` + `args`, `pkg gc` reclaims the unreferenced `hello-1.0`.  Deferred:
  hermetic source builds (§M36 toolchain), RPATH isolation (§M37), sandbox
  (§M33), signing (§M39).  See PLAN.md §M35.5.
- **2026-07-11 — M35: threads + futex, i386.**  Kernel-scheduled threads on
  the M34 process model (DOCS §4.28): `proc_clone` (SYS_CLONE) makes a task that
  SHARES its creator's address space (`mm_shared` flag stops the reap from
  freeing it) + dups the fd table, starting at a ring-3 entry/stack; libc
  `thread_create`/`thread_join`.  `futex` (SYS_FUTEX, `kernel/core/futex.c`):
  FUTEX_WAIT parks iff `*uaddr==val` (lost-wakeup-free under the Tier-A queue
  lock) / FUTEX_WAKE, hashed by physical address.  `threadtest` (3-state
  Drepper mutex): 4 threads × 5000 shared-counter increments = 20000/20000
  PASS on **both UP and `-smp 2`**.  Plus **thread-local storage** via `%gs`
  (per-CPU GDT TLS descriptors + `hal_set_tls_base` on switch-in + SYS_SET_TLS +
  libc `set_tls`): `tlstest`'s 4 threads each read only their own id through
  `%gs` (0 mismatches, UP + `-smp 2`).  Also fixed a pre-existing gap it
  exposed — ring-3 tasks didn't run on APs (single global TSS + no per-CPU LTR):
  now a **per-CPU TSS** (array in tss.c + one GDT descriptor per CPU + each CPU
  LTRs its own in gdt_init/ap_main), so procspawn's user processes run on
  `-smp 2` too.  See PLAN.md §M35.
- **2026-07-11 — M24 stage 6: BSD socket API to userland, i386.**  Ring-3
  networking over the in-kernel stack (DOCS §4.25): a new `FD_NETSOCK` ofile +
  `struct netsock` back `socket`/`bind`/`connect`/`sendto`/`recvfrom` (syscalls
  22–26) — UDP via a per-socket datagram RX ring on net.c's port bindings, TCP
  via `net_tcp_connect`/`send`/`recv`/`close` (read/write on the fd, one
  connection at a time).  Addresses are host-order IPv4 + port ints (no
  sockaddr yet).  Boot-tested i386 (-device virtio-net): `dnstest` resolves
  example.com over a UDP socket from ring 3; `httptest` resolves + TCP-connects
  :80 + GETs "HTTP/1.1 200 OK" (829 B) — full userland networking, the §M39 TLS
  bridge.  Deferred: sockaddr, multiple TCP conns, retransmit/server, DHCP,
  IPv6.  See PLAN.md §M24.
- **2026-07-11 — M34: POSIX process model (fork/exec/wait/pipe/signals), i386.**
  The classic Unix process API on the M25 userland (DOCS §4.27): a System V
  initial stack (argc/argv/envp/auxv, crt0 reads it); **copy-on-write fork**
  (`vmm_space_clone` shares writable pages read-only+COW ref-counted,
  `vmm_cow_fault` resolves writes via the #PF path; `enter_user_mode_regs`
  resumes the child at the fork point with eax=0); `waitpid` on the Tier-A
  wait-queue; `execve` loading `/bin/*` from the VFS (bin_install populates
  `/bin`); `pipe`+`dup2` over the usock ring; and **signals** (sigaction/kill/
  raise, delivery on the return-to-user path with a user-stack signal frame +
  `__sig_trampoline`→SYS_SIGRETURN, default-terminate on INT/TERM/KILL/SEGV).
  Syscalls 14–21.  Boot-tested i386: runargs/forktest(COW isolation)/forkexec/
  pipetest/sigtest all green.  Lesson: `struct task` field additions need
  `make clean` (no header deps) or stale object files corrupt the heap.
  Deferred: EINTR, sigprocmask, user #PF→SIGSEGV, x86_64/aarch64.  See PLAN.md
  §M34.
- **2026-07-11 — M23: audio subsystem (AC97 + PCM output), i386.**  An
  `audio_dev` registry shaped like the block/net layers (DOCS §4.26) + an AC97
  codec driver (PCI 0x8086:0x2415, two BARs — NAM mixer + NABM bus-master) doing
  PCM output via a Buffer Descriptor List over a 128 KB PMM-backed DMA buffer +
  a portable square-wave tone generator.  Shell: `lsaudio`, `beep`, `tone`.
  **Boot-tested through QEMU's `-audiodev wav` backend:** `beep` (440 Hz, 400 ms)
  produced a clean square wave in the captured WAV — left channel min −8000 /
  max +8000, all samples non-zero, ~444 Hz by zero-crossing — verifying the
  tone → audio_dev → AC97 BDL DMA → backend path end to end.  Deferred: a `play
  <path>` WAV player, `/dev/dsp`, mixer/multi-stream, input, Intel HDA, IRQ
  completion, x86_64/aarch64.  See PLAN.md §M23.
- **2026-07-11 — M24: network stack (virtio-net + TCP/IP), i386.**  A
  from-scratch IPv4 stack (DOCS §4.25): a `struct net_device` registry mirroring
  the block layer, a legacy virtio-net PCI driver (two queues + pre-posted RX
  buffers, same virtqueue layout as virtio_blk), and the arch-independent stack
  — Ethernet demux → ARP (cache + resolve + reply) → IPv4 (+ RFC 1071 checksum)
  → ICMP (ping) → UDP (+ an 8-slot port-bind table) → a DNS stub resolver → a
  client-only TCP (handshake / in-order data + ACK / FIN close / pseudo-header
  checksum) backing `net_http_get`.  Shell: `lsnic`, `ping`, `arp`, `nslookup`,
  `wget`, `nettest`.  RX is polled from the calling task (no IRQ, no lock yet).
  **Boot-tested end-to-end through QEMU SLIRP to the real internet:** `nettest`
  → ICMP 3/3 replies from the gateway, DNS resolves example.com, TCP fetches
  `HTTP/1.1 200 OK` (828 bytes).  Lessons: kernel `kprintf` has no width/pad
  (`%02x` corrupts varargs — format MAC/IP to a string, print `%s`); virtio-net
  needs RX buffers pre-posted + refilled or RX stalls; TCP checksum is mandatory
  (pseudo-header) where UDP's is optional.  Deferred: IRQ RX + `netd`, the BSD
  socket *syscall* API to userland, TCP retransmit/congestion + server role,
  DHCP, IPv6, x86_64/aarch64.  See PLAN.md §M24.
- **2026-07-10 — Tier B: concurrent user processes + full-arch libc (M25
  tail).**  An ELF now runs as an independent, preemptible user process
  (`proc_spawn`) on its own task — several at once, each in its own address
  space, each ending via SYS_EXIT → task_exit (init reaps + frees the space) —
  not just the single synchronous excursion.  Per-task ring-3→ring-0 stack via a
  new `hal_set_kernel_stack` hook the scheduler drives (TSS.esp0/rsp0 on x86;
  no-op on aarch64 where SP_EL1 tracks via context_switch); a one-way
  `enter_user_mode`; a `user_task` flag distinguishing user tasks from the
  kept excursion self-tests.  The in-tree libc now builds for all three arches
  (arch-conditional `syscall3` + per-arch `crt0` + per-arch Makefile USER_*
  knobs + per-arch blob symbols); new `SYS_GETPID`.  `procspawn` (two
  interleaving `spin` processes) + `libctest` (compiled-C `hello` in ring 3/EL0)
  green on i386 / x86_64 / aarch64.  See §4.24.
- **2026-07-10 — M30: cron (time-based scheduling).**  The first real service
  (`cron.c`): cron is itself an M29 service (autostart, restart=always), loops
  on `task_msleep`, and spawns each due `CRON_JOB(name, fn, every_ms)` as a
  child (init reaps).  Interval from registered default → `/etc/crontab` (`every
  N s|m|h name`) → config; missed-tick = run-once-no-backfill.  `crontab -l` /
  `cron list|reload` + `/proc/cron`.  Demo `tick-log` (every 5 s) fires + logs
  on i386 / x86_64 / aarch64; cron shows under `service list`.  See §4.23.
- **2026-07-10 — M31: watchdog (freeze detection).**  A sweep task (child of
  init, `watchdog.c`) runs every 500 ms: **layer 1** per-task heartbeat
  (`watchdog_register`/`watchdog_kick`; a missed deadline → `KLOG_ERR` +
  `task_kill_tree`, and a supervised M29 service is then restarted by its
  supervisor — the two compose), **layer 2** per-CPU softlockup (a
  `percpu.ticks` counter bumped in `schedule_request`; a CPU whose counter
  stalls is warned).  Layer 3 (hardware watchdog) deferred — needs a
  per-platform device driver.  `/proc/watchdog` + `wdtest` self-test green on
  i386 / x86_64 / aarch64, zero softlockup false-positives.  See §4.22.
- **2026-07-10 — M29: services + service bus.**  Two self-registered halves on
  Tier A.  (1) **Supervisor** (`service.h`/`service.c`) — `SERVICE(name, entry,
  autostart, restart{no|on-failure|always})`; one supervisor task (child of
  init) autostarts enabled services (config gate `service.<name>.disabled`),
  loops on `task_wait(-1)`, restarts per policy with a crash-loop backoff; a
  `stopping` flag distinguishes a hand stop from a crash; `service
  list|start|stop|restart|status` + `/proc/services`.  (2) **Service bus**
  (`bus.h`/`bus.c`) — endpoint / contract@version / transport; `bus_bind`
  strict-on-the-wire, domain↔transport enforced (only KERNEL/LocalCall real,
  USER/ISOLATED reserved for M25); opt-in `BUS_ADAPTER` shim gated by
  `bus.allow-adaptation`; `/proc/bus`.  Demonstrators (`svc_demo.c`): heartbeat
  (always) + crasher (on-failure) services; a Greeter v2 provider + 1→2 adapter.
  `bustest` (exact-v2 / strict-v1-miss / adapted-v1→v2) green on i386 / x86_64 /
  aarch64.  Also added `task_msleep` (cooperative sleep) and brought procfs to
  the aarch64 build (`/proc` parity).  See §4.21.
- **2026-07-10 — Tier A: blocking primitives (wait-queue + task_wait + blocking
  IPC).**  Makes `TASK_SLEEPING` real — the missing "sleep until an event"
  primitive under M25.  (1) **Wait-queue** (`waitq.h`, impl in `task.c`):
  `struct waitq` + `waitq_block`/`waitq_wake_one`/`waitq_wake_all`, lost-wakeup-
  free via the pthread_cond_wait discipline (queue lock = condition lock), a
  parked task off every runqueue, SMP cross-CPU wake (re-enqueue + reschedule
  IPI).  (2) **`task_wait(pid,&code)`** — parent blocks until a child exits,
  gets its code, reaps it; woken by `task_exit_code`; the reap-ownership
  contract (`task_set_reap_owned`) keeps init from harvesting a waited child.
  (3) **Blocking IPC** — per-endpoint socket read wait-queue (also serialises
  the ring for two-task use), blocking `read`/`recv` (empty → sleep; peer send
  wakes; peer close → EOF), blocking `poll(timeout<0)` on a global readiness
  queue raised by `fd_readiness_signal`.  Self-test `waittest` green on i386 /
  x86_64 / aarch64; socktest/polltest/fdtest/shmtest regression-clean.  Tier A
  is the foundation the M29 service supervisor + M30/M31 build on.  See §4.20.
- **2026-07-10 — M25: userland foundation (stages 1–7).**  The substrate that
  turns d-os kernel threads into real user processes, built + verified on
  i386 / x86_64 / aarch64 (libc: i386 reference).  (1) **Per-process address
  spaces** — a portable `vmm_space` (create/destroy/map/unmap/switch +
  `vmm_user_base`); `struct task.mm`; the scheduler loads CR3/TTBR0 on switch,
  reloading only on change.  (2) **ELF loader** (`elf.c`) — both classes at
  runtime, maps PT_LOAD segments into a space.  (2b) **Run a loaded ELF**
  (`proc.c` `proc_exec_elf`) — ring-3/EL0 drop at `e_entry`, isolated space.
  (3) **fd table** — `write/read/open/close/lseek` over `task->fds`.  (4)
  **mmap + memfd shared memory** — generic `struct ofile` (VFS/shm/socket,
  refcounted) + a `VMM_SHARED` PTE bit so borrowed frames aren't double-freed
  at space teardown.  (5) **unix `socketpair` + fd passing** (`usock.c`) —
  SCM_RIGHTS: a descriptor (e.g. a memfd) travels across the socket and lands
  in the receiver's fd table.  (6) **`poll`** — non-blocking readiness.  (7)
  **in-tree libc** (`user/`) — `int 0x80` wrappers + string/malloc(over
  mmap)/printf, linked with a compiled-C `hello.c` into a static ELF embedded
  as a blob; `libctest` runs real C in ring 3.  Self-test shell commands:
  `userrun / fdtest / shmtest / socktest / polltest / libctest`.  **Design
  fork recorded:** exactly two privilege levels (ring 0/3, EL1/EL0) forever —
  paging is binary so a "ring 1 driver" gets no memory isolation, x86_64 made
  rings 1/2 vestigial, and ARM has no rings; the security axis is address
  spaces + capabilities, not CPU ring count.  **Pitfalls:** x86_64 needs a
  *private PDPT* under PML4[0] (a bare PML4 copy shares the user region →
  isolation silently fails); the CR3/TTBR0 reload MUST be skipped when
  unchanged (else a full TLB flush every context switch); user-VA base is
  arch-specific (`vmm_user_base()`).  **Deferred tail:** fully independent,
  preemptible, concurrently-scheduled user processes (per-task TSS.esp0 /
  SP_EL1, SYS_EXIT→task_exit, blocking syscalls) + the x86_64/aarch64 libc
  port — today a user program runs as a synchronous excursion on the calling
  task.  See §4.19.

- **2026-07-10 — M28: system log (klog ring buffer + `dmesg` + `/proc/kmsg`).**
  The kernel gained a structured, reviewable log.  New `kernel/core/klog.c`: a
  static 512-record ring (usable from the very first boot kprintf — no heap),
  each record carrying a monotonic seq, a boot-relative ms timestamp, a
  printk-style severity (EMERG…DEBUG), a short source tag, and the message.
  `printf.c`'s `emit()` **tees every byte** into `klog_feed_char`, which
  assembles lines and commits a record on `\n` — so all existing `kprintf`
  boot/runtime output is captured automatically (level INFO, tag "kernel") with
  zero call-site churn.  The richer `klog(level, tag, fmt, …)` stamps a record's
  severity + tag while still reaching the console.  Read paths: the `dmesg
  [-l <level>]` shell command (renders `[  sec.mmm] LEVEL tag: msg`, filters by
  severity) and a `/proc/kmsg` procfs node (same format).  Two genuine boot
  lines were reclassified to exercise the structured path (`config` missing →
  NOTICE, `vfs` no-vda → WARN).  **Pitfall fixed en route:** extracting a
  `kvprintf(fmt, va_list)` core out of `kprintf` silently corrupted *all*
  formatted output on **x86_64** — the SysV ABI makes `va_list` an array type,
  so a `va_list` parameter decays to a pointer and `&ap` became a
  pointer-to-pointer (wrong type for the `va_list*`-taking `fetch_*` helpers);
  i386's scalar `va_list` masked it.  Fix: `va_copy` into a genuine local array
  and format off that.  Verified on i386 **and** x86_64: full levelled boot log
  via `dmesg`, `dmesg -l warn` shows only the WARN line (NOTICE/INFO filtered),
  `cat /proc/kmsg` renders the ring, no boot regression.  See §4.18.

- **2026-07-10 — M21 Phase M: USB (xHCI + USB HID) on ARM64 — full x86 feature
  parity.**  QEMU `virt` exposes USB as an xHCI controller on its PCIe host
  bridge, so this needed a PCI layer first.  New `kernel/hal/aarch64/pci.c`:
  ECAM config access (config space is MMIO at 0x40_1000_0000, not the x86
  0xCF8/0xCFC ports) + `pci_scan` with **BAR assignment** — booting raw via
  `-kernel` there is no firmware to program the BARs, so we size each memory BAR
  and assign it from the board's 32-bit MMIO window (0x1000_0000, already
  Device-mapped by mmu.c) + enable memory/bus-master.  Exposes the same pci.h
  API x86 pci.c does, so the stock `xhci.c` + `usb_hid.c` link and run unchanged
  (xHCI is MMIO + POLLED — `xhci_poll()` runs from the generic-timer ISR on the
  BSP, the ARM analogue of the x86 PIT tick; no MSI/INTx wiring).  A
  `vmm_map_4mib` no-op (the BAR window is pre-mapped) + `mmu_map_device_1gib`
  (reach the ECAM window) round it out.  Verified: `-device qemu-xhci -device
  usb-kbd` → `xhci: HID kbd … ready` and `help` typed on the USB keyboard drives
  the shell.  **aarch64 now has full parity with x86** (boot → SMP → storage →
  framebuffer → GUI → EL0 userspace → USB), all three arches build clean.  See
  §4.17.

- **2026-07-10 — M21 Phase J + K: the framebuffer shell.c + the M22 GUI on
  ARM64 (M22 arch parity).**  The *same* full `shell.c` the x86 ports run now
  runs on a virtual console on aarch64, and the `gui` command brings up the M22
  desktop — compositor, taskbar, clock, windows — driven by virtio-input
  keyboard + mouse and the virtio-gpu framebuffer.  The entire GUI+shell bundle
  (gfx/gui/widget/apps, shell.c, vc.c, core/basic.c, config/keymap/layouts) now
  compiles + links on aarch64; portability shims: `arch_ringtest()` (moved the
  x86 ring-3 demo out of shell.c → `kernel/hal/x86/ringtest.c`), `hal_shutdown`/
  `hal_reboot` via PSCI, a **PL031 RTC** (`pl031_rtc.c`) for the taskbar clock,
  `fb_flip_*` stubs + `fb_present_flush()` in the compositor's single-buffer
  present path (virtio-gpu scanout push), and a `virtio_input.c` driver
  (evdev→HID→keymap→vc_kbd_push; REL/BTN→mouse listener).  **Scheduler bug
  fixed:** pid 0's aarch64 idle loop didn't re-enable IRQs, so if DAIF ever left
  IRQs masked the CPU stopped taking timer ticks → stopped scheduling → every
  task homed on it (e.g. the input poll task) was starved; `for(;;){
  hal_intr_enable(); hal_cpu_halt(); }` (matching cpu_idle_entry) fixes it on
  both -smp 1 and -smp 2.  Verified via QEMU screendump: `help` shows the full
  shell command set; `gui` shows the desktop + Start + live clock + mouse
  cursor.  i386 + x86_64 GUI/ringtest re-verified regression-free.  See §4.17.

- **2026-07-10 — M21 Phase L: EL0 userspace substrate on ARM64 + all-arch M25
  readiness.**  Brings aarch64 to the x86 M6/M20.5 baseline so the userland
  milestone (M25) can begin on all three arches.  New `kernel/hal/aarch64/`:
  `vmm.c` (per-process TTBR0 address spaces — private L1 with the kernel's
  low-4-GiB identity blocks copied in — + page-granular EL0 mappings, AP=01 +
  PXN/UXN, + TTBR0 switch), `usermode.S` (`aarch64_enter_user` `eret` to EL0 +
  the SYS_EXIT teleport + a PC-relative user stub), `syscall.c` (SVC dispatcher:
  x8=number, x0..x5=args, shared `syscall.h` numbers; SYS_PRINT/SYS_EXIT + the
  `usertest` self-test).  `exceptions.c` decodes ESR_EL1.EC==0x15 (SVC) on the
  EL0 sync vector; `hal_syscall_exit_to_kernel` now delegates to the teleport.
  Also: separate per-arch convenience scripts `scripts/build-{i386,x86_64,
  aarch64}.sh` + `run-{i386,x86_64,aarch64}.sh` (thin wrappers over the ARCH=
  generic scripts).  Verified: aarch64 `usertest` prints `hello from EL0` via
  `svc` and teleports back; i386 + x86_64 `ringtest` re-verified identical.  All
  three arches now run a user program + service a syscall.  See §4.17.

- **2026-07-09 — M21 Phase I: virtio-gpu framebuffer on ARM64 (the SAME
  fb_terminal renderer).**  QEMU `virt` has no VGA/Bochs-VBE and no linear-VRAM
  BAR, so the display is a virtio-gpu device on a virtio-MMIO slot.  New
  `kernel/hal/aarch64/virtio_gpu.c` runs the modern-transport handshake (reused
  from the Phase-F blk driver), stands up a 1280×800 B8G8R8X8 2D scanout backed
  by a contiguous RAM framebuffer, and hands it to the PORTABLE `fb_terminal.c`.
  To reuse that 8×8-font console rather than fork it, its one x86-only part (the
  Bochs-VBE port I/O + vmm identity map) was hoisted behind a new `fb_present.h`
  backend: `fb_present_map` (x86: 4 MiB PSE map; ARM: no-op) + `fb_present_flush`
  (x86: no-op; ARM: virtio-gpu `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` of the
  dirty rect), with the M22.6 double-buffer page flip moved verbatim from
  `fb_terminal.c` to `kernel/hal/x86/fb_present.c` (gui.c unchanged).  Net: the
  ARM boot log + interactive shell now render graphically, on the same renderer
  x86 uses.  Verified via QEMU screendump on aarch64 (boot log + help/ls/meminfo
  at 1280×800) and re-verified the i386 GUI compositor page-flip regression-free.
  See §4.17.

- **2026-07-07 — M21 Phase H: device-tree (FDT/DTB) discovery on ARM64.**
  The kernel now discovers the machine instead of hard-coding it.  `dtb.c` is
  a minimal big-endian FDT parser: it locates the DTB (x0 → a fixed load
  address → RAM scan) and extracts the `/memory` reg (RAM base + size) + a
  `/cpus/cpu@*` count.  `aarch64_boot_meminfo_init` (stubs.c) then sizes the
  PMM map to the discovered RAM instead of the baked-in 256 MiB.  QEMU's
  direct-ELF `-kernel` entry passes no DTB, so run_qemu.sh loads one at
  0x48000000 (`-device loader`, generated per config via `-machine dumpdtb`).
  Verified: with `-m 512M -smp 4` + DTB, `dtb: found ... RAM 512 MiB ... 4
  CPU(s)` and the PMM comes up with 509 MiB free; without a DTB it falls back
  to 253 MiB.  See §4.17.

- **2026-07-07 — M21 Phase G: exFAT persistent storage on ARM64.**  The stock
  `block_cache.c` + `fs/exfat.c` link + run unchanged on aarch64 (both are
  arch-independent — exfat.c even carries its own mem helpers), so mounting a
  real filesystem on the virtio-blk disk needed no new arch code: `main_entry`
  calls `bcache_init()` + `vfs_mount("exfat", "/mnt", "vda")`.  The serial
  shell's ls/cat/write/rm then operate on real disk under /mnt.  Verified:
  `exfat: mounted dev=vda clusters=7680`; `write /mnt/hello.txt` → `ls /mnt`
  shows it → `cat` reads it; and on a FRESH boot with the same disk image
  `cat /mnt/hello.txt` still returns the content — the write persisted across
  a reboot.  Full chain: virtio-MMIO → block cache → exFAT → VFS → shell.  Test
  images are mkfs.exfat'd in the x86 build image (exfatprogs).  See §4.17.

- **2026-07-07 — M21 Phase F: virtio-MMIO block device on ARM64.**  A real
  disk on `/dev/vda`.  The PCI `virtio_blk.c` speaks virtio over port I/O
  (meaningless on ARM), so `virtio_mmio_blk.c` is a fresh driver for QEMU
  `virt`'s virtio-MMIO transport: scan the 32 slots (0x0a00_0000) for a block
  device, run the modern version-2 init handshake, set up one split virtqueue
  (Desc/Driver/Device Low/High registers), and do polled 512-byte sector
  read/write (3-descriptor requests).  Registers with the stock block layer so
  vfs/exfat/the shell see a normal device.  `main_entry` runs a write→read
  self-test; the serial shell gains `blk [lba]` (sector hexdump).  Needs
  `-global virtio-mmio.force-legacy=false` (QEMU `virt` defaults its
  virtio-mmio to legacy/v1) + `-device virtio-blk-device` (wired into
  run_qemu.sh, disk optional).  Verified: `/dev/vda ready (8192 sectors)`,
  write→read self-test PASS on sector 100, and `blk 0` hexdumps the on-disk
  string — real DMA end-to-end.  See §4.17.

- **2026-07-07 — M21 Phase E: SMP via PSCI on ARM64.**  The stock per-CPU
  runqueue + load balancer + percpu.c now drive secondary cores on a third
  arch.  `smp.c` starts each secondary with a `PSCI_CPU_ON` HVC (QEMU's
  emulated PSCI) — no INIT-SIPI-SIPI, no low-memory trampoline; `smp_entry.S`
  is the MMU-off entry (per-CPU stack from `ap_sp[]` → `smp_secondary_main`),
  which turns the MMU on first (coherency), sets VBAR, brings up the per-CPU
  GIC interface + timer, and joins the scheduler as its core's idle task.
  `mmu.c`/`gic.c` gained per-CPU `mmu_enable_this_cpu`/`gic_cpu_init` helpers;
  percpu topology hooks map `lapic_id` → MPIDR.Aff0; cross-CPU kick is a GIC
  SGI.  Verified: `percpu: 2 CPUs known`, `secondary CPU 1 online`, and two
  never-yielding hog tasks run on CPU1 + CPU0 in parallel (`parallelism
  PASS`).  Configurable via `AARCH64_MAX_CPUS` + `-smp` (shipped at 2).
  See §4.17.

- **2026-07-07 — M21 Phase D: interactive serial shell + ramfs on ARM64.**
  An interactive REPL with a real filesystem now runs on aarch64.  The x86
  `shell.c` is welded to the framebuffer VC + GUI + block/USB + ring-3
  usermode (all x86-only or not-yet-ported), so a dedicated serial shell is
  brought up: `uart.c` gains a non-blocking PL011 RX (`uart_early_getchar`);
  `serial_shell.c` is a REPL on an ordinary scheduler task (poll + task_yield
  for input) driving the portable services — help/echo/clear, meminfo/free,
  uptime, ps, and the ramfs (ls/cat/mkdir/write/rm).  The stock
  `vfs.c`/`ramfs.c` (+ `block.c`/`module.c`) link unchanged; `vfs_init()` +
  `module_init_all()` mount ramfs at `/`.  Verified over the UART: `ls /`
  shows the ramfs skeleton, mkdir/write/ls/cat/rm round-trip a file, `ps`
  lists the tasks, `meminfo` reports 253 MiB free — no fault.  (Reaching the
  *same* framebuffer shell.c is Phase E+, gated on the VC/GUI/driver ports.)
  See §4.17.

- **2026-07-07 — M21 Phase C: preemptive scheduler + memory manager on
  ARM64.**  The kernel's heart comes up on aarch64.  Rather than porting
  the x86-coupled shared `kernel_main`, aarch64 runs its own bring-up
  (`main_entry.c`) calling the portable core directly.  New arch files:
  `switch.S` (context switch over x19–x30), `task_arch.c`
  (`hal_task_init_stack` + trampoline), `hal_arch.c` (full hal_api:
  DAIF interrupt masking, wfi/yield, identity-map hook), `stubs.c`
  (single-CPU lapic/acpi/smp stubs + a synthesised multiboot RAM map +
  PL011 console-sink registration), `lib.c` (freestanding mem* +
  `__getauxval`).  The stock `pmm.c`/`slab.c`/`kmalloc.c`/`task.c`/
  `percpu.c`/`lock.c`/`console.c`/`printf.c` now link + run unchanged;
  `BUDDY_MAX_FRAMES` gained the 4 GiB cap for aarch64 (RAM at pfn
  0x40000).  The timer ISR drives preemption (schedule_request →
  gic schedule_check → schedule → context_switch).  Build: aarch64
  CFLAGS gained `-mno-outline-atomics -fno-tree-loop-distribute-patterns`.
  Verified on serial: 253 MiB RAM managed, kmalloc reuse works, two
  never-yielding hog tasks BOTH progress (≈501M vs ≈509M) — timer
  preemption + context switch correct, PASS, no fault.  See §4.17.

- **2026-07-07 — M21 Phase B: GICv2 + ARM generic timer.**  The ARM
  interrupt path — analogue of the x86 IOAPIC + PIT.  `gic.c` drives the
  GICv2 distributor (0x08000000) + CPU interface (0x08010000):
  `gic_init` / `gic_enable_irq` / `gic_register_handler`, and the strong
  `aarch64_irq_dispatch` (override of the Phase-A weak stub) does the
  GICC_IAR→handler→GICC_EOIR handshake.  `timer.c` drives the NS EL1
  physical generic timer (CNTP_*, INTID 30 on `virt`): arm TVAL, enable
  CTL, re-arm per IRQ (no auto-reload), monotonic `tick_count` +
  `timer_ticks_ms()` + CNTPCT raw counter.  IRQs unmasked with `msr
  daifclr,#2`.  `run_qemu.sh` pins `-M virt,gic-version=2`.  Verified on
  serial: GIC init + timer arm, 1/2/3 s tick milestones, PASS after 300
  periodic IRQs — full path with no fault.  See §4.17.

- **2026-07-07 — M21 Phase A: ARM64 (AArch64) boot bring-up.**  The
  third architecture starts landing, phased like x86_64 (M20 → M20.6).
  Phase A is the foundation: raw-ELF boot on `qemu-system-aarch64 -M
  virt -cpu cortex-a72` (no GRUB / no multiboot — QEMU's `-kernel`
  loads the ELF at the `virt` RAM base 0x40000000, linked at
  0x40080000).  `boot.S` drops EL2→EL1 if needed, sets SP, zeroes
  `.bss`, calls C.  A PL011 UART (`uart.c`, MMIO 0x09000000) is the
  early console; the EL1 exception vector table (`vectors.S` +
  `exceptions.c`) goes into VBAR_EL1 (trapframe save + ESR/FAR dump on
  faults, weak IRQ hook for Phase B); `mmu.c` turns the MMU + caches on
  with a coarse 1 GiB-block identity map (device window + Normal RAM).
  Build: separate `Dockerfile.aarch64` (the cross toolchain conflicts
  with gcc-multilib), Makefile `ARCH=aarch64` branch (GNU-as `.S` via
  the cross gcc, no nasm), aarch64 paths in `scripts/{build,run_qemu}.sh`.
  Verified on serial: boots at EL1, installs VBAR_EL1, enables the MMU,
  post-MMU Normal-cached RAM read-back returns the sentinel.  Bug hit +
  fixed during bring-up: `exceptions.c` and the original `exceptions.S`
  both produced `exceptions.o` (mirror-path object tree) → the asm
  object was overwritten and `vector_table` went undefined at link; the
  assembler half is now `vectors.S`.  See §4.17.

- **2026-07-05 — M22.7: damage as a rect LIST (cursor-hitch fix).**  The
  compositor tracked damage as a single bounding box, so a Task Manager
  refresh in one corner and the cursor in another merged into their
  bounding box — the compositor re-blitted a huge diagonal region every
  refresh and the cursor visibly stuttered (~1 Hz).  Damage is now a LIST
  of up to 16 disjoint rects (`struct rect dmg_list[]`; new rects merge
  only into an overlapping one).  `compose()` snapshots the WM state once
  (`struct scene_snapshot`) and paints + presents EACH rect separately
  (`draw_scene_rect`), so two far-apart updates stay two small blits.
  The page flip's buffer-age-2 replay became a per-rect `prev_dmg` list.
  `gui stats` gained "avg KB blitted/frame".  Measured: with Task Manager
  refreshing + cursor moving, ~630 KB/frame vs the old union's
  ~2.4–5.3 MB/frame — the hitch is gone.  Verified i386 + x86_64 (scenes
  render correctly; window drag leaves no trail → flip replay correct;
  no fault).
- **2026-07-05 — M22.7: precise structural damage + listview-only refresh.**
  Two follow-ups to the damage list.  (1) A window click no longer
  `gui_damage_all()`s the screen for the focus/z change — `gui_mouse`
  damages only the two affected windows (old focus + raised/clicked one);
  resize-apply and maximize still take the full path (geometry grows the
  window).  (2) The Task Manager repaints only its listview via the new
  `gui_window_request_redraw_rect(win, cx, cy, cw, ch)` (widget-local
  rect → clipped redraw + damage of just that screen area), not the whole
  window each second.  Verified i386 + x86_64 (raising a covered window
  paints its focused title over the overlap correctly; TM list updates,
  chrome intact; no fault).
- **2026-07-05 — M22.7: GUI session root + clean-desktop start.**  The
  GUI is now its own SESSION: `gui_start` spawns the `desktop` task
  first and parents the compositor + windows UNDER it
  (`task_spawn_under`), so the whole GUI hangs off one session root
  instead of scattered under the shell that ran `gui` — a
  `kill_tree(desktop)` closes the session cleanly.  And no shells are
  auto-started: the GUI comes up as a bare desktop (wallpaper + taskbar,
  0 windows — a supported state, focus NULL until a window opens); the
  user opens terminals/apps from Start.  `ps` tree: `boot-shell →
  desktop → {compositor, launched windows/apps}`.  Verified i386 +
  x86_64 (empty desktop boots; Start-menu launch works).
- **2026-07-05 — M22.7: session vs detached GUI shells.**  A terminal
  launched from the taskbar used to orphan to init (its transient
  launcher app-host created the WIN_TERM then exited, since the host only
  manages WIN_APP windows).  Now the shell task is parented explicitly
  via the new `task_spawn_under(name, entry, ppid)`: **"New Shell"** =
  SESSION mode, child of the `desktop` task (a `kill_tree(desktop)` takes
  it with the session); **"Detached Shell"** = child of init, so it
  outlives the desktop session and its window stays while the compositor
  runs (the nohup/tmux-detach idea, in a GUI).  gui.c tracks `desktop_pid`
  and `gui_window_create`/`gui_window_create_detached` pick the parent;
  the initial two shells (created before the desktop exists) stay under
  the shell that ran `gui`.  Verified i386 + x86_64 (`ps`: session shell
  under `desktop`, detached shell under `init`).  See §4.15/§4.16.
- **2026-07-05 — M22.7 refinements: latency, parentage, panel memory.**
  Four follow-ups after the per-task cut.  (1) *Latency:* the compositor /
  desktop / app-host loops used to `hal_cpu_idle()` (halt a whole timer
  tick) every iteration, so with several always-runnable tasks the
  compositor's turn came around only every N ticks — visible cursor lag
  with the menu or Task Manager open.  Now each loop halts ONLY when it
  has nothing to do (`if (need_frame) compose(); else hal_cpu_idle();`),
  so it spins through the scheduler under load and still idles at rest.
  (2) *Menu lag:* vista_motion did a full-screen recompose per hover
  change (`gui_request_frame`); it now asks for a chrome-only repaint
  (`gui_panel_dirty`) — measured 2 full frames over 50 menu-hover motions
  vs one full recompose each before.  (3) *Parentage:* app launches moved
  from the compositor to the desktop task, so a launched app is a child
  of the desktop/session, not the display server (`ps` shows
  `app:File Manager` under `desktop`, not `compositor`).  (4) *Memory:*
  `panelsurf` is no longer a full-screen 9.2 MiB surface — only the bottom
  strip (taskbar reserve + `PANEL_POPUP_MAX`) is backed, addressed in
  screen coords via an offset `px` + clip (~5 MiB saved).  Also the
  `bare` shell reserves a thin strip so its hint line composites again.
  Verified i386 + x86_64.
- **2026-07-05 — M22.7 Stage B: desktop shell / taskbar as its own task.**
  The `desktop_shell` (taskbar, launcher, clock) now runs on a dedicated
  `desktop` task rendering into a full-screen `panelsurf`; the compositor
  composites only its opaque parts (taskbar strip always + launcher popup
  while open) on top of the windows, routes chrome input to the panel's
  `pevq`, and no longer calls the shell.  Completes the M22.7 goal: the
  compositor is now a pure surface-compositor + input router, and every
  UI surface (windows, apps, panel) is drawn by its own task — the M26
  Wayland shape with the internal API.  shell_vista is unchanged bar
  publishing its popup rect (`gui_panel_set_popup`).  Verified i386 +
  x86_64 (taskbar + Start menu render; a menu item launches an app as
  its own host; no fault).  See §4.16.
- **2026-07-05 — M22.7 Stage A: per-task GUI apps.**  Each WIN_APP window
  now runs on its own `app:<name>` task (`app_host_main` + the new
  `task_spawn_arg`) instead of on the compositor.  The compositor is now
  a surface-compositor + input router: it pushes input into a per-window
  ring (`win->aq`); the host does widget hit-test + dispatch + redraw and
  runs on_tick/on_layout off the compositor.  Two wins: apps are real
  processes (visible/killable in the Task Manager) and a slow app handler
  no longer freezes the whole GUI.  Teardown is a host↔compositor dance
  (host frees widgets + on_close + `host_released`; compositor disposes
  the struct + reaps the `reap_owned` host; sweeps catch no-window
  singletons + externally-killed hosts).  `window_alloc` now locks its
  slot claim (concurrent host creation); `launch` routes through
  `gui_queue_launch`.  Verified i386 + x86_64 (apps render off the
  compositor, close cleanly, no fault).  See §4.16.  Stage B (taskbar as
  its own panel task) pending — see PLAN §M22.7.
- **2026-07-04 — M27: process model — init, hierarchy, reaper, kill-tree.**
  Tasks gained `ppid` + `exit_code` + `reap_owned`.  An always-on
  **init** task (spawned by kernel_main before the shell) is the
  universal reaper: it sweeps DEAD, non-owned tasks at ~100 Hz, closing
  the old "exited kernel thread leaks as DEAD unless the Task Manager is
  open" gap; on reap it re-parents the victim's children to init.
  `task_kill_tree()` cooperatively takes a pid + its whole subtree down
  (the GUI window close uses it, so closing a shell window kills what it
  spawned); `task_spawn_detached()` parents to init for daemons.
  pid 0 (boot swapper) + init are reap-guarded.  `ps` / `/proc/tasks`
  grew a PPID column; the Task Manager shows a process tree.  The GUI's
  window-shell reaping now coordinates with init via the `reap_owned`
  flag (replacing the taskman's `vc_task_bound` reap gate).  Verified on
  i386 + x86_64 (init reaps a leaked self-test task; a `spawn`ed child
  under a GUI shell is killed + re-parented + reaped when its window
  closes; ps shows correct ppid; pid 0 survives; no fault).  See §4.15.
  The *upward* half (supervision on child death, freeze watchdog) is
  deferred to §M29 / §M31.
- **2026-07-04 — M22.6: tear-free presentation (page flip) + display scaling.**
  Chased down "the picture still wiggles on mouse move".  Root cause
  was two things wearing one coat.  (1) The visible whole-screen edge
  shimmer was host-side: `run_qemu.sh` scaled the guest with
  `-display cocoa,zoom-to-fit=on` (bilinear, non-integer on Retina), so
  every screen update re-sampled the frame and jittered static edges —
  switched to `zoom-to-fit=off` (crisp 1:1).  (2) The compositor's real
  tearing (direct blit into the live scanout) is now eliminated on the
  Bochs-VBE device via a hardware page flip: reserve a second VRAM
  frame (DISPI VIRT_HEIGHT = 2×H), compose into the hidden buffer, pan
  the scanout origin (DISPI Y_OFFSET) in one write.  Buffer-age-2
  ping-pong: each present copies `dirty_N ∪ prev_dmg` from the always-
  complete `backsurf`.  Graceful fallback to the single-buffer blit on
  non-Bochs displays.  New API `fb_flip_init`/`fb_flip_to`
  (fb_terminal.c); verified on i386 + x86_64 (log line "gui: page-flip
  present enabled", mouse-move stress, no fault).  Corrects the M22.4
  "not fixable on this device" note (DISPI panning IS a present
  boundary — virtio-gpu is no longer required for this).  Same session
  also: **1920×1200 desktop** (multiboot header both arches; needs
  `-device VGA,vgamem_mb=32` for the double buffer's ~18.4 MiB VRAM,
  `BUDDY_MAX_ORDER` 10→12 for the 9.2 MiB contiguous surfaces, and
  `-m 256M`), and **terminal windows auto-close when their hosted task
  dies** (End task / `kill` / natural exit) — flagged at TASK_DEAD so a
  merely-flagged-to-stop task keeps its window until it actually stops;
  the teardown reaps the task so it also leaves the Task Manager list.
  Verified on i386 + x86_64.  See §4.13.
- **2026-07-04 — M22.5: desktop apps — editor, Tiny-BASIC, file manager 2.0, maximize.**
  The desktop becomes a place to DO something: write BASIC on d-os,
  run it on d-os.  Input: PS/2 E0 cursor cluster decoded to HID
  usages; widget layer gained keycode events (`widget_ops.keycode`)
  + per-widget destructors; listviews got keyboard nav + Enter
  activation.  New: multiline editor widget (`w_editor.c` —
  selection, clipboard, viewport), kernel clipboard (`clipboard.c`),
  Editor app (open/save/Ctrl+S, retitle via gui_window_set_title,
  non-singleton), Tiny-BASIC interpreter (`core/basic.c`, kthread
  contract; GUI window via new gui_window_create_task + `run <path>`
  shell command), file manager 2.0 (editable path bar, size column +
  sorting, Ren/Copy/recursive-Del with two-step confirm, extension →
  app associations via GUI_APP_ASSOC/gui_app_for_path), VFS grew
  vfs_rename (same-dir; ramfs) / vfs_copy (self-copy guard) /
  vfs_unlink_recursive, maximize/restore (title-bar □ + dbl-click,
  work-area aware).  Verified in QEMU i386 with a scripted end-to-end
  story — type program in Editor → Ctrl+S to /mnt (exFAT!) → fileman
  keyboard-nav + Enter → BASIC window LOADs + RUNs it → maximize/
  restore → rename/copy/recursive-delete on ramfs — 7/7 checks green;
  x86_64 boots the same GUI (editor + select-all verified).
  Lessons: gui_app_def grew and STALE .o FILES kept the old struct
  size — mixed 8/16-byte registry entries broke the section walk
  (the no-header-deps pitfall, now with a concrete symptom); a
  formatted disk image carries a boot signature, so QEMU needs
  `-boot d` or SeaBIOS boots the empty disk instead of the CD.
- **2026-07-04 — M22.4: compositor smoothness + instant Task Manager.**
  Cursor flicker/ghosting fixed by moving cursor damage bookkeeping
  into compose() itself (`last_cur_x/y`; the mouse-IRQ glide path is a
  bare need_frame wake — lesson learned: the damage snapshot happens
  before the WM snapshot, so IRQ-side cursor rects can be stale).
  Window drags damage old∪new rect per motion instead of full-screen
  recompose (drag stays partial-frame dominated: 52:5 in the scripted
  test); tearing documented as std-VGA-inherent (no vblank; virtio-gpu
  flush is the post-M24 answer).  New `task_set_change_hook` fires on
  spawn/kill/exit/reap → the compositor runs window on_ticks
  immediately, so closed programs leave the Task Manager within one
  frame; taskman refresh opportunistically task_reap()s DEAD tasks not
  bound to a VC (new `vc_task_bound`), so DEAD rows no longer
  accumulate.  Verified in QEMU i386 (scripted glide + drag +
  `gui stats`); x86_64 builds clean.
- **2026-07-04 — M22.3: task manager + window lifecycle + damage rects.**
  Scheduler: task_kill/task_should_stop/task_reap (cooperative
  kthread_stop contract — spinlocks don't disable preemption here, so
  arbitrary-point kills are unsafe by design; kill lands at
  task_yield, CPU hogs must poll), per-task cpu_ms accounting at the
  switch boundary, `kill` command, `ps` CPUMS column.  vc_destroy
  frees offscreen VC slots.  GUI: minimize + close on every window
  (terminal close = kill→wait-DEAD→reap→vc_destroy state machine on
  the compositor), Windows-style taskbar button semantics, Alt-Tab
  via a raw-keycode hook dispatched pre-keymap from both keyboard
  drivers, per-window ~1 Hz tick hook, and dirty-rect composition
  (gfx clip boxes + damage accumulation; `gui stats` counters — 
  typing runs 20:1 partial:full).  New Task Manager app (list, CPU
  time, End task).  Verified in QEMU i386 (7-step scripted run) +
  x86_64 build.  Deferred: widget containers; forced kill (needs
  M25 user processes).
- **2026-07-04 — §S.1: command-shell provider registry.**
  Third registry after GUI_APP/DESKTOP_SHELL: `SHELL_PROVIDER(name,
  entry)` (shell_provider.h + `shell_providers` linker section).  The
  full shell registers as "d-os"; new `kernel/core/rescue_shell.c`
  ("rescue", 3 commands) proves the swap.  All three spawn sites
  (kernel.c boot shell, `pane split`, GUI terminal windows) resolve
  via `shell_provider_active()` — the `shell.provider` config key —
  instead of `extern shell_task_entry`.  Verified in QEMU i386:
  `setconf shell.provider rescue` + `gui` → both terminal windows run
  the rescue prompt, `help` answers.
- **2026-07-04 — M22.2: GUI modularity — swappable desktop shells + app registry + dev docs.**
  The desktop chrome and app launching moved out of the compositor
  core behind two linker-section registries (MODULE() pattern):
  `DESKTOP_SHELL()` (desktop.h; `shell_vista.c` extracted from gui.c
  as the default, new minimal `shell_bare.c` proves the swap — chosen
  via the `gui.shell` config key) and `GUI_APP()` (gui_app.h; the
  Start menu + new `launch [app]` shell command walk it).  Apps moved
  to kernel/gui/apps/ (fileman, about — extracted from gui.c,
  newshell — was a hardcoded menu action, hello — documented sample).
  gui.c now references no app or chrome by symbol; shells talk to the
  WM through gui_internal.h services with an explicit IRQ-vs-task
  calling convention.  New DOCS §4.14 (GUI development guide).
  Verified in QEMU i386: vista Start menu lists all 4 registry apps +
  power tail, Hello launches and its button counts clicks;
  `setconf gui.shell bare` + `gui` boots chromeless and `launch file`
  opens the file manager from a terminal.  x86_64 builds clean.
- **2026-07-04 — M22.1: widget toolkit + taskbar + file manager + resize fix.**
  PLAN §M22 stage 6 closed plus a Vista-shaped desktop shell.  New:
  `kernel/gui/widget.c` (label/button/listview/textinput; callbacks
  dispatched on the compositor task via IRQ→task event/key/action
  queues), APP window kind with close button + teardown, taskbar
  (Start menu, per-window buttons, RTC clock via new
  `kernel/drivers/rtc/cmos_rtc.c`), file manager
  (`kernel/gui/fileman.c`: browse / Up / MkDir / Touch / Del / View
  with read-only viewer), content-preserving resize (terminal char
  backing store re-rendered into the new surface; app windows
  re-layout), `vfs_unlink` + ramfs unlink (inode_ops.unlink signature
  gained the child inode), `vc_set_kbd_hook` keyboard intercept,
  1280×800 framebuffer (multiboot headers) + run_qemu.sh
  `-rtc base=localtime` and macOS `zoom-to-fit` so the QEMU window is
  usable on Retina.  Verified in QEMU i386 (9-step scripted run:
  taskbar focus, typing, content-preserving resize, Start menu, file
  create + delete, close via X) and x86_64 (Start menu → File
  Manager).  Deferred: minimize, Alt-Tab, per-window damage rects,
  terminal-window close (needs task kill).
- **2026-07-03 — M22: GUI infrastructure — compositor + windows + mouse.**
  New `kernel/gui/` subsystem (gfx primitives + surfaces, compositor,
  window manager, terminal windows) and a PS/2 aux-port mouse driver
  (IRQ12).  `gui` shell command starts a compositor task, two shell
  windows on a gradient wallpaper with drop shadows; mouse click
  focuses + raises, title-bar drag moves, grip drag resizes
  (wireframe + realloc on release); keyboard follows focus via the
  existing VC rings.  `struct vc` gained an `emit` hook
  (`vc_create_offscreen`) so windows host stock shell tasks with zero
  shell.c changes, and `vc_screen_suppress` keeps hidden panes from
  painting over the scene.  The Wayland-reuse evaluation ran first
  (see PLAN change log 2026-07-03): custom in-kernel protocol with
  Wayland-shaped objects now, wire protocol deferred to §M26 behind
  the §M25 userland substrate.  Verified on i386 AND x86_64 in QEMU
  via monitor-scripted sendkey/mouse_move + screendump (both archs:
  windows, focus click, drag; i386 additionally: typing into focused
  window, drag-move, rubber-band resize).  Deferred: widget toolkit
  (§M22 stage 6), window close, per-window damage rects.
- **2026-06-30 — Polish round 2: M18.6.5 + M19.5.1 + M19.5.3 + M20.6.2 + M20.6.3.**
  Five more polish sub-items shipped, leaving §M20.6.1 (SYSCALL/SYSRET —
  GDT slot reorg) as the lone outstanding item from the original 11.
  - **§M20.6.2 — xHCI 64-bit DMA audit + x86_64 enable.** Audit shows
    the i386 xHCI driver assumed `<4 GiB DMA via uint32_t phys fields;
    this is safe today because PMM only manages frames within the
    identity-mapped range (≤ 1 GiB).  Documented the assumption in
    xhci.c and re-enabled the driver on x86_64 (Makefile).  The
    `m20_stubs.c::xhci_poll` stub is gone.  Verified: x86_64 with
    `-device qemu-xhci -device usb-kbd` enumerates the HID keyboard
    end-to-end (slot assigned, HID interface configured, polling
    ready).
  - **§M20.6.3 — virtio-blk + exFAT 64-bit DMA audit + x86_64 enable.**
    Same audit + documentation pattern as xHCI.  `virtio_blk.c` and
    `exfat.c` now compile for x86_64.  Verified: `qemu-system-x86_64
    -drive if=virtio,...` registers `/dev/vda` and the bcache
    self-test round-trips through the driver.
  - **§M19.5.1 — HIGHMEM zone population (x86_64).**  Added
    `hal_extend_identity_map(end_phys)` to the HAL.  On x86_64 the
    impl installs 1 GiB pages in PDPT[1..] to cover all detected RAM
    up to the new `BUDDY_MAX_FRAMES` cap (4 GiB).  On i386 it's a
    no-op (kmap deferred; the identity map stays fixed at 256 MiB
    by vmm.c).  `pmm_init` calls it BEFORE the mmap walk so every
    frame the PMM marks is reachable through the kernel direct map.
    Per-arch `BUDDY_MAX_FRAMES` (i386 = 1 GiB, x86_64 = 4 GiB).
    Verified: `qemu-system-x86_64 -m 4G` boots and reports
    `pmm: identity map extended to 4096 MiB / NORMAL managed=782304
    (3069 MiB total free)`.
  - **§M18.6.5 — MSI/MSI-X discovery + vector allocator.**  Added
    `pci_find_cap(bus, slot, func, cap_id)` to walk the PCI
    capability list, and `pci_alloc_msi(bus, slot, func, handler)`
    that finds the MSI cap (0x05), allocates one of 4 reserved IDT
    vectors (0x50..0x53), installs the handler, and programs the
    device's MSI address (LAPIC base | apic_id << 12) + data
    (vector).  4 new ISR stubs in both archs; the dispatch lives in
    `isr_handler` next to the LAPIC-timer path.  No driver uses MSI
    yet — the framework ships so converting xHCI is a one-line
    change in its bring-up.  MSI-X is `cap_id=0x11`; identical
    discovery, table-based config is a follow-up.
  - **§M19.5.3 — ACPI SRAT → per-CPU NUMA node.**  Added an SRAT
    (System Resource Affinity Table) parser to `acpi.c` that maps
    each (enabled) processor entry to its proximity domain and each
    (enabled) memory range to its proximity domain.  `struct percpu`
    gained a `numa_node` field, populated at percpu_init_bsp time
    from `acpi_cpu_node(madt_slot)`.  `lscpu` now shows the node.
    Public getters: `acpi_numa_nodes()`, `acpi_cpu_node(i)`,
    `acpi_mem_affinity_count/get()`.  Verified: `qemu-system-x86_64
    -smp 4 -m 512M -object memory-backend-ram,... -numa
    node,nodeid=0,cpus=0-1,memdev=mem0 -numa
    node,nodeid=1,cpus=2-3,memdev=mem1` prints `ACPI: SRAT — 2
    node(s), 3 mem range(s)`.  PMM still has a single zone set (per-
    NUMA-node zones are a deeper refactor); the SRAT data is wired
    in for when that lands.
  Lessons learned:
  * On x86_64, extending the identity map via 1 GiB PDPT pages is
    cheap (one PDPT write per GiB) and needs no PD/PT allocations.
    But `BUDDY_MAX_FRAMES` is a compile-time cap on page_state[]'s
    size — we set it per-arch via `#ifdef __x86_64__` rather than via
    Makefile -D, since pmm.h is the natural place for it.
  * The capability-list walk in `pci_find_cap` MUST be bounded
    (we cap at 64 hops) — a malformed device could otherwise loop
    forever.  PCI 3.0 caps low 2 bits of next-pointer as reserved;
    we mask them off and reject offsets < 0x40 (= inside the standard
    header) as malformed.
  * SRAT entries reference processors by APIC ID, not by MADT slot
    index.  Our percpu uses slot indexing, so the SRAT parser
    translates via `apic_id_to_madt_slot()` — and it does so AFTER
    parse_madt has filled `g_cpu_apic_ids[]`.  Got the ordering
    wrong once; the fix is to defer SRAT parsing until the RSDT
    walk's second pass.

- **2026-06-29 — M18.6 (partial) + M19.5.2: SMP polish + empty-slab caching.**
  Half of the polish round shipped (5/11 sub-items):
  - **§M18.6.1 — Per-CPU runqueue + load balancer.** Replaced the global
    runqueue with a per-CPU one (intrusive doubly-linked list rooted at
    `percpu->rq_head`, threaded via `task->rq_next/rq_prev`).  Each
    CPU's schedule walks ONLY its own rq.  Master task list (for ps,
    iteration, find) is now separate, threaded via `task->next` and
    protected by a dedicated `master_lock`.  Load balancer runs from
    schedule's idle-fallback path: when local rq is empty, scan peers
    for the busiest queue and steal a task whose affinity allows
    running here.  Cleanest correctness win: scheduler lock acquire +
    release pair across context_switch (the "lock-handoff" pattern) is
    now safe under task migration — schedule()'s unlock re-reads
    `this_cpu()` so the lock released is whichever CPU we're on NOW,
    not the one we entered on.
  - **§M18.6.2 — Per-CPU `preempt_count`.** Was a single global
    (incorrect on SMP — disabling on CPU A also gated CPU B).  Now
    lives in `struct percpu`; accessors bracket the read-modify-write
    in `hal_intr_save`/`restore` so the local timer can't migrate us
    mid-increment.
  - **§M18.6.3 — Task affinity + `taskset`.** Each task carries a
    `cpu_mask` (default 0xFFFFFFFF = any CPU); scheduler and load-
    balancer-steal both filter by `(mask >> this_cpu_id) & 1`.
    `taskset <pid> <hex-mask>` rebinds.  `task_set_affinity` migrates
    the task if its current cpu_home is no longer in the new mask.
    `lscpu` now also prints per-CPU rq depth.
  - **§M18.6.4 — Cross-CPU preempt IPI sender.** New
    `lapic_send_ipi(target_apic_id, vector)` (fixed delivery, self-IPI
    no-op'd internally).  `smp_send_reschedule(cpu_index)` wraps it on
    vector 0x41 (handler already wired since M18.5).  `task_enqueue`
    fires it whenever a task lands on a CPU other than self — wakes
    the target's hlt'd idle so the task starts running without
    waiting up to ~10 ms for the next local LAPIC tick.
  - **§M19.5.2 — Empty-slab caching.** Slab caches keep up to
    EMPTY_SLAB_MAX (=4) fully empty slabs per cache instead of
    immediately releasing each to the buddy.  Refill prefers a cached
    empty slab over a fresh `page_alloc`.  `slabinfo` gained a
    `CACHED-EMPTY` column.  Reduces buddy thrash on bursty allocators
    without significant retention (4 × 4 KiB × 8 caches = 128 KiB max
    retained kernel-wide).
  Lock-protocol details: schedule() / schedule_locked split into
  acquire-then-pick-then-context_switch with the unlock conceptually
  paired across context_switches (see `kernel/core/task.c` block
  comment on schedule_locked).  Brand-new tasks use
  `task_finish_first_switch` to drop the rq_lock on their first run.
  Verified: `-smp 4` boots cleanly on both i386 and x86_64; preempt
  + parallel self-tests PASS; both tests show CPU-bound hogs running
  on multiple cores.
  Lessons learned:
  * The acquired rq_lock identity at schedule() entry is NOT the
    lock identity at schedule() exit if a context_switch led us here
    from another CPU's scheduler.  Re-read `this_cpu()->rq_lock` at
    release time.  Pairs across CPU boundaries: every acquire is
    matched by exactly one release SOMEWHERE in the chain of
    context_switches.
  * IPI on vector 0x41 must NOT also set need_resched on the
    receiver via the IDT handler — the IPI's PURPOSE is to wake the
    receiver from hlt; once the receiver returns to its idle loop's
    `task_yield`, the natural schedule() picks up the new work.
    Setting need_resched in the IPI handler would just create one
    extra schedule_check no-op.
  * Pre-decrement `c->slabs` BEFORE calling `slab_release`, then have
    `slab_release` re-increment if it kept the page cached.  Without
    this, "page count" diverges from "active page count" once the
    LIFO grows.
  Deferred to a follow-up polish session: §M18.6.5 (MSI/MSI-X
  discovery + vector allocator), §M19.5.1 (HIGHMEM zone population
  + kmap on i386 / identity-map extension on x86_64), §M19.5.3
  (ACPI SRAT → per-NUMA-node zones), §M20.6.1 (SYSCALL/SYSRET
  instruction path — needs GDT slot reorg), §M20.6.2/.3 (xHCI +
  virtio-blk 64-bit DMA audit + x86_64 enable).  All independent of
  each other and of M21+.

- **2026-06-29 — M20.5 Phase C: x86_64 ring-3 via `int 0x80`.**
  Ring-3 on x86_64 now works the same way it does on i386: shell
  `ringtest` allocates two frames, USER-maps them at 0x40000000 +
  0x40001000, hand-codes a tiny program that calls SYS_PRINT then
  SYS_EXIT via `int 0x80`, drops to ring 3 via `iretq`, and lands
  back in kernel mode via the SYS_EXIT teleport.  Verified end-to-
  end: `ringtest: dropping to ring 3... / hello from ring 3! /
  ringtest: back in ring 0`.
  New files: `kernel/hal/x86_64/usermode.s` (5-quadword iretq frame
  build + saved_rsp/saved_rip stash, exits via .return label on
  SYS_EXIT teleport), `kernel/hal/x86_64/syscall.c` (mirror of i386
  syscall dispatcher with rax/rbx field reads).  Removed `kernel/
  core/syscall.c` (was effectively i386-specific via the `eax`/`ebx`
  field reads); moved to `kernel/hal/x86/syscall.c` so both archs
  keep their dispatcher in their HAL tree.  This closes the M17
  deferred item "kernel/core/syscall.c arch split".  `kernel/hal/
  x86_64/hal_arch.c::hal_syscall_exit_to_kernel` got a real impl
  (movq saved_sp,%rsp ; jmpq *saved_pc) replacing the hard-halt
  stub.  `kernel/includes/usermode.h` prototype widened from
  uint32_t to uintptr_t — i386 callers passing 32-bit literals are
  source-compatible.  `m20_stubs.c` shrank to ONE stub:
  `xhci_poll` (deferred until the xHCI driver gets a 64-bit DMA
  audit).  Lesson learned: on x86_64 long mode, EVERY level of the
  4-level page walk checks the US bit — not just the leaf PT entry.
  boot.s builds PML4[0] / PDPT[0] / PD[i] with US=0; the first
  vmm_map of a user page would fault (err=5, P+U set) in ring 3
  because PML4[0] still had US=0 even though the PT entry was
  US=1.  Fix: in walk_to_pt, when traversing an existing
  intermediate entry whose US bit is 0 but the caller's flags
  request US, OR the bit in.  Permissions can only widen this
  way — safe under any caller mix.
  SYSCALL/SYSRET instruction path is deliberately NOT wired up in
  this phase: the SYSRET selector-arithmetic convention (user CS =
  STAR[63:48] + 16, user SS = STAR[63:48] + 8) doesn't fit our
  current GDT slot layout (user CS at 0x18, user DS at 0x20 — no
  STAR[63:48] satisfies both).  Deferred to a follow-up that
  reorganizes the GDT into the Linux-style layout (kernel CS/DS
  contiguous, user DS before user CS) — touching i386 + x86_64 +
  usermode.s + trampoline.  Phase C delivers full ring-3
  functionality via `int 0x80` either way.

- **2026-06-29 — M20.5 Phase B: x86_64 SMP AP bring-up.**
  x86_64 went from "BSP only, APs idle" to "all CPUs scheduling
  real work in parallel."  New `kernel/hal/x86_64/ap_trampoline.s`
  (flat-binary blob copied to physical 0x8000; 16-bit real → 32-bit
  protected → 64-bit long-mode chain with a self-contained
  trampoline GDT, then `lgdt` + far-ret into the kernel GDT and
  jmp to the C entry).  New `kernel/hal/x86_64/smp.c` (mirror of
  i386 smp.c with 64-bit ap_info fields; ap_main does the same
  per-CPU init as i386 — lapic_init_ap + percpu_init_ap + idt_load
  + task_install_ap_idle + lapic_timer_start_periodic + idle loop).
  smp_boot_aps / smp_set_lapic_timer_count dropped from
  m20_stubs.c.  Makefile gained the matching ap_trampoline.bin
  build rule (objcopy --output-target=elf64-x86-64
  --binary-architecture=i386:x86-64).  Verified on `qemu-system-
  x86_64 -m 256M -smp 4`: serial log shows `ap: cpu 1 (apic_id=1)
  online` for slots 1/2/3, then `smp: 3 AP(s) started (of 4 total
  CPU(s))`.  parallel self-test PASSes with hog ticks ~2-4×
  higher than UP — genuine multi-CPU execution.  i386 baseline
  unchanged.
  Why a self-contained trampoline GDT rather than reusing the
  kernel GDT (like the i386 trampoline does): `lgdt` in 16-bit
  real mode reads m16:24 (6-byte form), so it can't load the
  long-mode kernel GDT's 10-byte (m16:64) pointer directly.  The
  trampoline carries its own GDT with 32-bit + 64-bit code/data
  descriptors, gets the CPU into 64-bit, then re-`lgdt`s the
  kernel GDT (now reading m16:64) and far-rets to kernel CS.
  Between the `lgdt` and the far-ret, CS still references the
  trampoline GDT's slot 3 (now reinterpreted under the kernel
  GDT, which has user code descriptor at slot 3 with DPL=3) — but
  the CPU doesn't re-evaluate CS until something touches it, so
  as long as no instruction between lgdt and far-ret causes a
  segment recheck, the transition is safe.

- **2026-06-29 — M20.5 Phase A: x86_64 APIC bring-up + `printf %l`.**
  First slice of the x86_64 port closure.  LAPIC + IOAPIC now run on
  both archs from the same `kernel/hal/x86/lapic.c` / `ioapic.c`
  sources (they were always pure MMIO + MSR — no port I/O — so the
  same .c files compile under `-m32` and `-m64`).  Their public
  `phys` params widened from `uint32_t` to `uintptr_t` so MMIO above
  4 GiB is expressible without truncation (QEMU keeps it at
  0xFEC00000 / 0xFEE00000 on both archs, but the type is now right).
  `kernel.c` lost its `#if defined(__i386__)` guards around the APIC
  bring-up block + LAPIC-timer programming + `smp_boot_aps()` —
  the same flow runs on both archs.  Stubs in
  `kernel/hal/x86_64/m20_stubs.c` shrank: lapic_*/ioapic_* gone;
  remaining stubs are `smp_boot_aps` (returns 0 until Phase B's AP
  trampoline), `smp_set_lapic_timer_count` (no-op), `syscall_dispatch`
  + `enter_user_mode_wrap` (Phase C), `xhci_poll` (separate
  milestone).  `kprintf` gained length modifiers — `%l`, `%ll`, `%z`
  (so `%lx` prints 64-bit on x86_64, 32-bit on i386 transparently)
  — and `%p` now prints uintptr_t-width hex (8 digits on i386, 16
  on x86_64) so addresses line up regardless of arch.  Verified on
  `qemu-system-x86_64 -m 256M` with both `-smp 1` and `-smp 2`:
  serial log shows `lapic: BSP enabled at 0x00000000fee00000
  (id=0)`, `ioapic: 24 entries at 0x00000000fec00000`, `apic:
  routing live (bsp_apic_id=0), 8259 disabled`, `lapic: timer
  calibrated — ~79k ticks/ms, count=789320 for 100 Hz`, `percpu: N
  CPUs known, BSP at slot 0`, preempt self-test PASS (hog ticks
  ~100M in 500 ms — LAPIC timer is the preempt source on x86_64
  now, not the PIT).  parallel self-test reports PASS on `-smp 2`,
  but note the second hog is still round-robining on BSP — actual
  AP execution waits on Phase B.  i386 baseline unchanged.
  Lessons (added to source comments): `lapic.c`/`ioapic.c` are
  arch-family-shared, not x86-only — keep them under `kernel/hal/x86/`
  for now but list them in both arch source sets.

- **2026-06-29 — M20: x86_64 (long mode) port — UP, shell prompt up.**
  Second-arch shakedown of the M17 HAL boundary.  Multi-arch build
  matrix (`make ARCH=i386|x86_64`, default i386), separate output
  trees under `build/$(ARCH)/`.  New `kernel/hal/x86_64/`:
  `boot.s` (multiboot2 header + 32→64 long-mode entry per Intel SDM
  Vol 3A §9.8.5: CR4.PAE → CR3 → EFER.LME → CR0.PG → far-jmp into
  L=1 code segment), `vmm.c` (4-level paging behind the same vmm.h
  API as i386, inheriting boot.s's PML4/PDPT/PD), `gdt.c` (7-slot
  GDT including a 16-byte / 2-slot long-mode TSS descriptor),
  `idt.c` (16-byte gates, 64-bit offset split across 3 fields),
  `isr_stubs.s` (uniform 5-quadword CPU push + 15 GPR save, no
  segment-reg dance because long mode largely ignores ds/es/fs/gs),
  `switch.s` (System V x86_64 callee-saved set: rbx, rbp, r12-r15),
  `task_arch.c` (matching 64-bit first-switch frame), `tss.c`
  (packed 104-byte 64-bit TSS with RSP0 at offset 4), `hal_arch.c`,
  `io.c`, `mb2.c` (multiboot2 → mb1 tag-stream translator so
  pmm/fb_terminal/mboot_print etc. stay unchanged), `main_entry.c`
  (the bridge from boot.s long_mode_entry into kernel_main),
  `m20_stubs.c` (UP no-op returns for lapic_*/ioapic_*/smp_*/
  syscall_dispatch/enter_user_mode_wrap/xhci_poll — shrinks as
  M20.5 / Phase 7 land real impls).  Arch-conditionals: `struct
  int_frame` (in `idt.h`) is `#if defined(__x86_64__)`-gated;
  `vmm.h` API widened to `uintptr_t` so source-compatible on i386;
  `kernel_main` and `mboot_init` take `uintptr_t info_ptr`; APIC
  bring-up + LAPIC timer + `smp_boot_aps` blocks in `kernel.c`
  gated under `#if defined(__i386__)` (x86_64 stays on the 8259
  for UP IRQ delivery — PIT IRQ0 works fine via legacy path).
  Multiboot2 header includes the type-5 framebuffer-request tag
  (1024x768x32), without which GRUB doesn't deliver a runtime
  framebuffer tag and `fb_terminal` stays inert.  i386 `vmm.c`
  moved from `kernel/mem/vmm.c` to `kernel/hal/x86/vmm.c` to
  reflect its arch-specificity.  Verified UP on QEMU
  `qemu-system-x86_64 -m 256M`: shell prompt up, all M19+M18.5
  self-tests PASS (preempt_test ~52M hog ticks in 500 ms, vmm
  round-trip, kmalloc reuse, microbench 10 ms / 10k iterations,
  4-level paging confirmed via `vmm_print_status` MSR readback of
  EFER.LMA).  i386 baseline unchanged: same shell, same self-tests.
  Pitfalls codified: (1) long-mode code descriptor MUST have L=1
  AND D=0 — both set or D=1 #GPs on the far-jmp; (2) lgdt operand
  is 6 bytes in 32-bit and 10 bytes in 64-bit, but the 6-byte
  form's base is zero-extended on long-mode entry so the same
  pointer remains valid as long as the GDT lives in the low 4
  GiB; (3) mb2 framebuffer-request tag is mandatory for FB
  delivery (no GRUB-side default); (4) `objcopy --input-target=
  binary` symbol names depend on the input filename — keep
  `ap_trampoline.bin` at its source-relative path even when other
  build artefacts move into `build/$(ARCH)/`; (5) `kprintf` has
  no `%l` prefix and the `default:` case echoes `%l` verbatim
  without consuming a va_arg, so passing 64-bit args under `%lx`
  silently corrupts the subsequent arg slots; (6) x86_64 `rdmsr`
  can't use the `=A` GCC asm constraint (that means the
  edx:eax-as-64-bit-pair legacy form, not long-mode's
  zero-extended rax/rdx); use two `=a` / `=d` outputs and
  recombine in C.  Deferred to M20.5: SMP on x86_64 (AP
  trampoline 16→32→64), LAPIC/IOAPIC port, SYSCALL/SYSRET (`int
  0x80` retained as compatibility gate), USB host (xHCI DMA
  needs 64-bit revisit), virtio-blk + exFAT (block layer DMA
  same).

- **2026-06-28 — M18.5: APs scheduling (LAPIC timer per-CPU +
  per-CPU idle + scheduler idle-fallback policy).**  Closed the M18
  follow-up that left APs idling.  Added LAPIC timer driver
  (`lapic_timer_calibrate / _start_periodic / _stop`) — calibrated
  once on BSP against PIT, same count reused on every AP for a
  per-CPU 100 Hz preempt tick.  New IDT vector 0x40 (`isr64` stub),
  dispatched in `isr_handler` as the standard
  schedule_request + lapic_eoi + schedule_check sequence; 0x41
  reserved for a future cross-CPU preempt IPI.  Each AP now joins
  the scheduler in `ap_main`: `idt_load` (per-CPU lidt), then
  `task_install_ap_idle` to synthesize an idle task for the running
  context and splice into the global ring with `is_idle = 1`.
  Scheduler policy (`pick_next_locked` in task.c) is round-robin
  over RUNNABLE non-idle tasks, idle is a fallback only when no
  real work exists for this CPU.  BSP idle task is now synthesized
  separately at `task_init` time so kernel_main can `task_exit`
  cleanly after boot — without this, BSP would halt forever when
  the last non-idle task on it died, which also halts PIT delivery
  and freezes `timer_ticks_ms` on every other CPU.  New parallel
  self-test at boot: two CPU-bound hogs run concurrently for
  500 ms; verified PASS on `-smp 2` and `-smp 4` (both hogs make
  progress).  Pitfalls codified: (1) IDTR is a per-CPU register —
  each AP must `lidt` even though the IDT data is shared; (2) BSP
  needs its own idle from boot or task_exit becomes terminal for
  the whole system via PIT-starvation; (3) the schedule policy
  must NOT round-robin into idle when a worker is RUNNABLE on this
  CPU, otherwise CPUs constantly bounce between hog and idle.

- **2026-06-28 — M19: Memory at scale (buddy PMM + slab + per-CPU
  magazines).**  PMM rewritten as a per-zone binary buddy allocator;
  legacy `pmm_alloc_frame / pmm_alloc_contiguous / pmm_free_frame`
  retained as thin wrappers around the new `page_alloc(order,
  zone_hint) / page_free(addr, order)` API.  Zones: `ZONE_DMA`
  (pfn<4096, first 16 MiB), `ZONE_NORMAL` (bulk), `ZONE_HIGHMEM`
  (declared, not populated).  Per-zone spinlock (M18 cmpxchg) keeps
  allocator IRQ-safe and SMP-safe.  Free-list link stored inside the
  free page itself, no external link arrays.  New side table:
  `page_state[]` (1 byte/frame, 256 KiB for BUDDY_MAX_FRAMES = 1 GiB
  cap) encodes "head of free block at order N" or "allocated /
  doesn't exist".  New module `kernel/mem/slab.c` implements size-
  class slab caches (8 sizes from 16 B to 2048 B) with **per-CPU
  magazines** (32-deep array per CPU per cache, IRQ-off fast path,
  M18's percpu infrastructure paying off).  Cache lookup via slab
  page header (SLAB_MAGIC) — no per-object header.  `kfree` dispatch:
  page magic check (slab) → big-alloc side table (page-backed
  kmalloc>2048 B) → error.  Microbench at boot: 10000 × {alloc(64)+
  free} round-trips in 0–9 ms.  Shell additions: `slabinfo` (per-
  cache usage), `buddyinfo` (free-block counts per zone × order).
  Direct map: i386's 4 MiB PSE identity map from M5 already satisfies
  the "huge pages for the kernel" DoD; no VMM change needed.
  Verified end-to-end on QEMU `-smp 4` and UP: all self-tests PASS,
  exFAT mount/read/write still works, virtio-blk + xhci + ramfs
  unchanged.  Pitfalls: (1) `big_alloc_order[]` must be init'd to
  `0xFF` explicitly — 0x00 is a valid order (= one frame), so
  reliance on `.bss` zero-fill would misidentify every never-touched
  frame as a 1-page big-alloc.  (2) `kmalloc_init` runs in boot
  order after `pmm_init`, but BEFORE other subsystems that allocate
  — the side-table fill is on the critical path of every later
  `kfree` so it cannot be lazy.

- **2026-06-28 — M18: SMP support (APIC + AP boot + per-CPU + real
  spinlocks).**  Single-CPU UP became a multiprocessor.  ACPI MADT
  parsed for LAPIC + IOAPIC topology in `acpi_init`.  New x86 HAL
  files: `kernel/hal/x86/lapic.c` (MMIO + INIT/SIPI IPI), `ioapic.c`
  (redirection table programming, ACPI ISO honoring), `smp.c`
  (BSP-side bring-up), `ap_trampoline.s` (16-bit real-mode → 32-bit
  protected mode + paging, assembled as flat binary and linked via
  `objcopy --input-target=binary`).  `idt.c` gained `idt_use_apic`
  switching IRQ delivery from 8259 to IOAPIC+LAPIC, EOIs to LAPIC.
  New core files: `kernel/core/percpu.c` (per-CPU `struct percpu`
  array indexed by LAPIC-ID → dense map), `kernel/includes/atomic.h`
  (cmpxchg/fetch_add/fences via `__sync_*`/`__atomic_*` builtins),
  shared-runqueue spinlock in `task.c`.  `task->esp` now per-CPU
  via `this_cpu()->current`; `schedule_locked` walks the global
  runqueue skipping tasks `task_running_elsewhere`.  Lock-handoff
  trick: `task_finish_first_switch` releases the runqueue lock from
  the brand-new-task trampoline (which never ran a schedule frame
  of its own).  New shell command: `lscpu`.  Verified end-to-end
  on QEMU `-smp 4`: all 4 CPUs online, BSP preempt self-test PASS
  (107M hog ticks).  Pitfalls codified: (1) `percpu_init_bsp` must
  NOT zero existing slot state — `task_init` runs earlier and has
  already stamped the BSP's `current` pointer; wiping it leaves the
  scheduler with prev=NULL and dead-silent boots.  (2) AP trampoline
  has to be assembled as `-f bin` with `org 0x8000` so labels
  resolve at the physical run address; ELF + an org directive
  doesn't help because the trampoline lives at 0x8000 at run time
  but at a different offset in the kernel image.  (3) The
  `objcopy --input-target=binary` symbol names embed the input path
  (slashes → underscores), so the Makefile must NOT `cd` into the
  source directory before invoking objcopy or the C-side extern
  symbol names won't match.  Out of scope (M18 follow-ups): cross-
  CPU preemption IRQ (LAPIC timer per-CPU or BSP-broadcast IPI),
  per-CPU runqueues + load balancer, per-CPU `preempt_count`, task
  affinity / pinning, `vmm.c` HAL wrap-up.
- **2026-06-27 — M17: HAL portability cut.**  Introduced
  `kernel/includes/hal_api.h` — the arch-independent interface that
  `kernel/core/`, `kernel/mem/`, and `kernel/fs/` reach for CPU
  control, interrupt-flag manipulation, arch bring-up, and task
  stack setup.  x86 implementation in
  `kernel/hal/x86/hal_arch.c` (single-instruction wrappers + GDT/
  IDT/TSS delegation) and `kernel/hal/x86/task_arch.c` (the
  brand-new-task trampoline + stack-layout knowledge that used to
  inline in task.c).  Migrations: `task.c`, `lock.c`, `vc.c`,
  `kernel.c`, `syscall.c` lost all direct `__asm__` and their
  `gdt.h`/`idt.h`/`tss.h` includes; legacy PC drivers (`pit`, `ps2`)
  kept their port I/O (they're PC-only by definition) but switched
  their `sti; hlt` idle to the atomic `hal_cpu_idle`.  `struct
  task.esp` typed `uintptr_t` to be 32/64-bit-arch agnostic.  Boot
  test unchanged: vmm + kmalloc + exFAT + bcache + preempt (104M
  hog ticks) + VC + shell all pass.  Deliberately deferred to a
  later milestone (best done with x64 paging): walling
  `kernel/mem/vmm.c`'s CR0/CR3/CR4/invlpg behind a `hal_map`/
  `hal_unmap` interface, and splitting `kernel/core/syscall.c`
  along the arch-specific `struct int_frame` boundary.  Pitfalls
  codified: `sti; hlt` is an atomic CPU-guaranteed pair (Intel SDM
  Vol 2: `sti` blocks IRQ recognition for ONE instruction boundary)
  — split it into `hal_intr_enable()` + `hal_cpu_halt()` and you
  reintroduce a race against IRQs posted between the two; that's
  why `hal_cpu_idle()` exists as its own primitive.
- **2026-06-27 — M16: Keyboard layout abstraction.**  Introduced a
  shared keyboard pipeline (`kernel/core/keymap.c`,
  `kernel/core/layouts.c`, `kernel/includes/keymap.h`): input drivers
  produce (universal keycode, modifier-mask), the keymap layer
  resolves it to ASCII via the active `struct kbd_layout`.  The
  universal keycode IS the USB HID Usage ID (Page 0x07), so USB HID
  driver is now a zero-translation pass-through.  PS/2 driver gained
  a `sc1_to_hid[]` table and per-modifier bit-tracking (LShift,
  RShift, LCtrl, RCtrl, LAlt, RAlt, plus the 0xE0 extended-byte
  state machine so RAlt = AltGr is recognized).  Layouts: `us`
  (formerly hardcoded inside ps2_keyboard.c + usb_hid.c, now the
  single source of truth) and `hu` (Magyar 102-key QWERTZ — Z↔Y
  swap, magyar shifted number row, AltGr column with ASCII-only
  symbols; accented vowels left blank until the font grows).
  New `keyboard.layout` config default (`"us"`); `keymap_init()`
  consults it after `config_init` and falls back to `us` on an
  unknown name.  Shell commands: `lslayout`, `setlayout <name>`.
  Verified end-to-end in QEMU: under `us`, `echo yz` → `yz`; under
  `hu`, `echo yz` → `zy`, AND the very next attempted `lslayout`
  comes through as `lslazout` because the 'y' keypress now produces
  'z' — live proof the new pipeline is doing the work.  Pitfalls
  codified: PS/2 modifier tracking must handle both LAlt (intercepted
  for VC pane-switch) and RAlt (= AltGr, feeds the layout's altgr
  column); the 0xE0 prefix is a one-shot state flag, not a sticky
  mode; the active-layout pointer is read from IRQ context so the
  shell-task is the only writer (pointer-sized atomic on x86).
- **2026-06-27 — M15: USB host stack (xHCI) + HID boot keyboard.**
  Brought up a full USB pipeline: PCI-discovered xHCI controller with
  DCBAA, Command Ring, Event Ring (1 segment + ERST), root-port
  enumeration, Enable Slot + Address Device + Configure Endpoint
  commands, and a single Interrupt-IN endpoint feeding an HID class
  driver.  Files: `kernel/drivers/usb/xhci.c` (controller),
  `kernel/drivers/usb/usb_hid.c` (boot-keyboard decode + Shift/Alt
  handling), `kernel/includes/usb.h` (constants, descriptor structs).
  TRB rings use the Producer Cycle State trick: 256-TRB frames with
  the last slot a Link TRB (TC=1) so cycle bits flip on wrap.  No
  MSI/MSI-X yet — we drain the Event Ring from the PIT IRQ every 10 ms
  (`USB_POLL_TICKS`).  HID class driver diffs successive 8-byte
  reports for fresh key-down events, translates USB Usage IDs to
  ASCII via `usb_hid_kbd_lower`/`upper`, and pushes through
  `vc_kbd_push` — the same routing path as PS/2, so USB Alt-N pane
  switching just works.  Boot tested with `-device qemu-xhci -device
  usb-kbd`; serial log confirms enumeration succeeds and HID reports
  arrive (verified during bring-up with a temporary kprintf in the
  HID handler).  Pitfalls codified: HID handler runs in IRQ context
  so it must only touch SPSC-safe state; Address Device requires
  Slot Context's Speed AND Root Port Number fields, both extracted
  from PORTSC; ERDP write must include the Event Handler Busy bit
  (bit 3) to clear it.  Out of scope for now (PLAN §M15 follow-ups):
  hubs, multiple devices, MSI/MSI-X, bulk/iso, full HID report-desc
  parsing, 64-byte device contexts, scratchpad buffers.
- **2026-06-27 — M14: Multi-session shell with FB pane splitting.**
  Built a virtual-console subsystem on top of the framebuffer.  The
  screen is now partitioned by a binary split tree (`vc_node`); each
  leaf is a `struct vc` with its own rect, cursor, SPSC input ring,
  and bound shell task.  Added `kernel/core/vc.c` +
  `kernel/includes/vc.h` with `vc_init`, `vc_split(v, dir)`,
  `vc_focus_by_id(n)`, `vc_putchar`, `vc_getchar`, and `vc_kbd_push`.
  Extended `fb_terminal.c` with rect-aware primitives
  (`fb_clear_cells`, `fb_draw_glyph_at`, `fb_scroll_cells_up`,
  `fb_sink_disable`) — the legacy whole-screen `fb_term_putchar` still
  works for the boot log but is deactivated once vc_init runs.  Added
  `console_set_per_task_emit(fn)` in console.c plus a `void*
  out_console` slot in `struct task`; `console_putchar` now broadcasts
  to active sinks AND, when the running task has a bound console,
  delivers to a per-task hook (vc_init wires it to vc_putchar).  This
  is how each shell task's `kprintf` lands inside its own pane
  without the shell touching VC APIs.  Reworked the PS/2 keyboard to
  track the Alt modifier (scancode 0x38/0xB8): Alt+1..9 calls
  `vc_focus_by_id` instead of producing a character; any other
  character is pushed into the focused VC's ring (with a legacy ring
  fallback for early-boot `keyboard_getchar` callers).  Refactored
  `shell_run` to take a `struct vc*` parameter; added a
  `shell_task_entry` thunk that reads its VC out of
  `task_current()->out_console` so `task_spawn(name,
  shell_task_entry)` works.  `kernel_main` now spawns the first
  shell as a task bound to the root VC and turns itself into the idle
  task (hlt + yield).  New shell commands: `pane` (list),
  `pane split horizontal`, `pane split vertical`.  Verified in QEMU:
  three concurrent shells (H-split then V-split in the bottom pane),
  Alt-1/Alt-2/Alt-3 focus switching, and `ps` correctly identifying
  the running shell task per-pane.  Pitfalls codified: the spawner
  must `preempt_disable` around `task_spawn` + `task_set_out_console`
  so the new task's first kprintf already routes correctly; split
  must mutate the existing node in place (the parent's child
  pointer must not be invalidated).
- **2026-06-27 — M13: Preemptive scheduling.**  Turned the
  cooperative round-robin scheduler into a preemptive one.  Added
  `kernel/core/lock.c` + `kernel/includes/lock.h` with
  `spinlock_t` (UP-stub today, SMP-shaped API) and a `preempt_count`
  pair (`preempt_disable` / `preempt_enable`).  The PIT IRQ (1 kHz)
  now bumps a quantum counter every tick and calls
  `schedule_request()` every 50 ticks (50 ms quantum), which sets a
  deferred `need_resched` flag.  The IDT's `isr_handler` consults
  that flag in `schedule_check()` AFTER `pic_eoi` and, if
  `preempt_count == 0`, context-switches into the next RUNNABLE task
  right from IRQ context.  The deferred-flag pattern is load-bearing:
  switching tasks directly from `pit_irq` would leave IRQ0
  in-service from the PIC's perspective and stop further timer
  ticks.  Brand-new tasks have never been through `schedule()`, so
  `task_trampoline` now explicitly `sti`s before calling the entry —
  otherwise they would inherit IF=0 and could never be preempted.
  Refactored `task.c` so `task_yield()` is a thin wrapper around the
  shared `schedule()`; runqueue mutators (`task_init`, `task_spawn`)
  bracket their work in local cli/sti.  Added a `loop` shell command
  (spawns a tight-loop CPU hog — interactive proof preemption keeps
  the prompt alive) and a boot self-test (spawn hog, sleep kernel on
  `hlt` for 500 ms, assert the kprintf runs AND hog counter > 0).
  Pitfall codified: schedule from IRQ MUST come after pic_eoi, never
  before.
- **2026-06-27 — M12: exFAT + multi-fs VFS refactor + block cache.**
  Reshaped the VFS to host real filesystems: `inode.size` and
  `dirent.size` are now `uint64_t`; `file_ops.read/write` take an
  explicit `off` argument with `f->pos` owned by the VFS layer;
  `fs_type.mount` receives a `struct block_device*` and `vfs_mount`
  gained a third `dev_name` arg; `struct inode_ops { lookup, create,
  mkdir, unlink }` replaces the old `extern ramfs_create_in` escape
  hatch and powers lazy lookup for backed filesystems.  ramfs, devfs,
  procfs, and config were ported to the new shape; the latter two
  required no behavior changes thanks to the VFS-owned `f->pos`.
  Added `kernel/core/block_cache.c` — refcounted write-back LRU
  buffer cache (64 PMM-frame slots, one sector each) with a `bctest`
  shell command and a boot-time self-test.  Added
  `kernel/fs/exfat.c` implementing mount, readdir, read, create,
  write, and persistence-on-close for ASCII names ≤30 chars.
  Boot-time self-test writes `/mnt/dos-marker.txt = "wrote-from-dos"`
  on first boot, reads it back on second boot; Linux `fsck.exfat`
  declares the resulting image clean.  Added `mount` shell command
  for ad-hoc mounts (`mount exfat /mnt vda`).  `mkfs.exfat` (from
  exfatprogs) added to the Dockerfile so the build image can format
  test images.  Pitfalls codified in comments: SeaBIOS boots from
  the unbootable raw exFAT image first if `-boot d` isn't passed —
  symptom is a blank serial log; bcache writes are issued through
  `dev->write` for whole sectors only, which exFAT respects by going
  one sector at a time even for sub-sector dir-entry updates.
- **2026-05-12 — M11: Block layer + virtio-blk.**  Added abstract
  `struct block_device` registry (`kernel/core/block.c`), PCI
  configuration-space access (`kernel/hal/x86/pci.c`), and a
  legacy-transport virtio-blk driver registered via DRIVER().  The
  disk exposes itself as `/dev/vda` through devfs.  `blktest` shell
  command writes a 512-byte 0xA5/0x5A pattern to sector 1, reads
  back, verifies — passes round-trip and the change persists in the
  disk image.  Two pitfalls hit during bring-up + codified in
  comments: legacy virtio QUEUE_SIZE is read-only (must match
  device's reported value, 256 on QEMU); descriptor `addr` is
  physical, not virtual, so heap-backed buffers need `vmm_translate`.
  Added `pmm_alloc_contiguous` for the multi-frame queue allocation
  and `inw`/`inl`/`outl` to the HAL.
- **2026-05-02 — M10: procfs — kernel state as files under /proc.**
  Added `kernel/fs/procfs.c` + `kernel/includes/procfs.h` with a
  growing-string `procfs_writer` and lazy content generation.  Eight
  built-in nodes: version, uptime, meminfo, modules, drivers, console,
  tasks, config.  Added small iterator helpers (`console_for_each`,
  `task_for_each`, `config_for_each`) so procfs can render registries
  without poking internal state.  ramfs now pre-creates `/proc`
  alongside `/etc /dev /tmp`.  Verified: `cat /proc/uptime` returns
  different values across calls (lazy regen working); `cat
  /proc/modules` shows all 6 registered modules.
- **2026-05-02 — M9: devfs — drivers as files under /dev.**  Added
  `kernel/fs/devfs.c` + `kernel/includes/devfs.h`.  Built-ins
  `/dev/null`, `/dev/zero`; driver-registered `/dev/com1` (serial
  write), `/dev/keyboard` (blocking read).  Pre-init registrations
  queue and are flushed when `devfs_init` runs after the FS is up.
  Verified: `ls /dev` lists all four; `cat /dev/null` returns
  empty; `write /dev/com1 hi` puts "hi" on serial.
- **2026-05-02 — M8: Driver lifecycle scaffold (`DRIVER()`).**  Added
  `kernel/core/driver.c` + `kernel/includes/driver.h` with a richer
  registry sibling to `MODULE()`.  `struct driver_ops` carries
  probe / init / shutdown.  Linker.ld got a `.drivers` section; the
  walker tracks per-driver runtime state via a kmalloc'd parallel
  byte array.  First user: `kernel/drivers/null/null.c` — placeholder
  for `/dev/null` (devfs M9 will plug in read/write adapter).  New
  shell command: `lsdrv`.
- **2026-04-26 — M7 (post-roadmap): Process struct + scheduler.**
  Added `kernel/core/task.c` (run-queue, spawn/yield/exit/list) and
  `kernel/hal/x86/switch.s` (context_switch).  Cooperative round-robin
  over a circular linked list; the keyboard idle loop yields after
  every IRQ wake so a parallel ticker task gets CPU while the shell is
  at the prompt.  New shell commands: `ps`, `spawn`, `yield`.  Demo:
  `spawn` creates a ticker that prints `[tick N]` 6 times in parallel
  with the shell, then exits cleanly.
- **2026-04-26 — M6 (post-roadmap): TSS + ring 3 user-mode round trip.**
  Extended GDT with user code/data DPL=3 descriptors plus a TSS
  descriptor; loaded TR.  Added per-CPU TSS with a dedicated 4 KiB
  syscall stack via `tss.esp0`.  `enter_user_mode_wrap` builds an
  iret frame and drops to ring 3.  IDT vector 0x80 installed at
  DPL=3 routes through `syscall_dispatch`; SYS_PRINT / SYS_EXIT
  implemented.  SYS_EXIT teleports back to the kernel caller via a
  saved-ESP / saved-EIP trick instead of iret.  `ringtest` shell
  command verifies end-to-end: allocates user pages, hand-codes a
  ring-3 program, runs it, returns.
- **2026-04-26 — M5 (post-roadmap): Config store on VFS.** Added
  `kernel/core/config.c` with `config_get/set/save/load/dump`.
  Backing file `/etc/d-os.conf` parsed at boot if present, defaults
  populated either way.  Added VFS_TRUNC support to vfs_open.  Shell
  commands: `config`, `getconf`, `setconf`, `saveconf`.  Shell prompt
  now read from `shell.prompt` config key on every iteration —
  immediate `setconf` take-effect.
- **2026-04-26 — M4 (post-roadmap): VFS skeleton + ramfs.** Added
  `kernel/fs/vfs.c` (root dentry, fs registry, path resolution,
  open/read/write/readdir/mkdir/create) and `kernel/fs/ramfs.c`
  (in-memory inodes + grow-on-write file buffers).  ramfs registered
  as a `fs` class module, mounted at `/` with pre-created `/etc`,
  `/dev`, `/tmp`.  New shell commands: `ls`, `cat`, `mkdir`, `touch`,
  `write`.
- **2026-04-25 — M3 (post-roadmap): PIT timer + ms tick.**
  Added `kernel/drivers/timer/pit.c` registered as a `timer` module.
  IRQ0 hits at 1 kHz; `timer_ticks_ms` and `timer_msleep` available.
  New `uptime` shell command formats h:mm:ss.mmm.  Linked libgcc to
  resolve 64-bit math helpers (`__udivdi3`, `__umoddi3`).
- **2026-04-25 — M2 (post-roadmap): driver registry framework.**
  Added `kernel/core/module.c` (linker-section based `MODULE()`
  registration) and `kernel/core/console.c` (output sink registry).
  Migrated serial, ps2 keyboard, fb_terminal, vga_terminal to
  self-register.  Removed the old `terminal.c` dispatcher and
  `terminal.h` (callers now use `console_*`).  Mutually-exclusive
  `screen` category lets VGA defer to FB cleanly.  New shell
  commands: `lsmod`, `lsconsole`.  Adding a new driver no longer
  requires touching `kernel_main`.
- **2026-04-25 — M1 (post-roadmap): kmalloc heap.** Added a 4 MiB
  K&R-style block free-list heap at virtual `0xD0000000`, backed by
  PMM frames mapped through `vmm_map`.  `kmalloc` / `kcalloc` / `kfree`
  + `kmalloc_stats`.  `meminfo` shell command extended with heap
  utilization.  Self-test in `kernel_main` proves alloc → free →
  reuse round trip works.
- **2026-04-24 — M6: VBE framebuffer + bitmap font.** Modified the
  multiboot header to request 1024×768×32 graphics, added serial debug
  output (COM1) tee'd into `kprintf`, introduced `vmm_map_4mib` for
  cheap MMIO-style mappings, and wrote a framebuffer terminal driver
  with an embedded 8×8 CGA-derived bitmap font covering ASCII
  0x20..0x7E.  Terminal backend is now chosen at runtime via an ops
  table in `terminal.c`; FB is preferred, VGA text stays as a
  fallback.  Kernel init reordered so memory management runs before
  terminal init (FB needs the VMM to map 0xFD000000).
- **2026-04-24 — M5: Paging + VMM.** Enabled paging with a 256 MiB
  PSE identity map so all pre-paging pointers stay valid.  Added
  `kernel/mem/vmm.c` with `vmm_map` / `vmm_unmap` / `vmm_translate` for
  4 KiB-granular virtual mappings above the identity region; the
  mapping path allocates page tables on demand from the PMM.  A boot
  self-test maps a fresh frame at virt 0xE0000000, writes/reads
  0xDEADBEEF, and tears down — confirmed working under QEMU.
- **2026-04-24 — M4: Physical Memory Manager.** Added bitmap-based PMM
  at `kernel/mem/pmm.c`.  Pulls AVAILABLE regions from the multiboot
  mmap, reserves low memory + kernel image + multiboot info, and
  exposes `pmm_alloc_frame` / `pmm_free_frame` at 4 KiB granularity.
  Linker script now exports `kernel_start` / `kernel_end` symbols.
  `meminfo` extended with a PMM summary.
- **2026-04-24 — M3: Multiboot memory map + `meminfo`.** Added
  `kernel/core/multiboot.c` that validates the loader magic, caches the
  info pointer, and walks the memory-map list.  Shell grew a `meminfo`
  command that prints legacy mem_lower/upper plus every mmap entry with
  base, length, and type name.
- **2026-04-24 — M2: IDT + PIC + IRQ-driven keyboard.** Added IDT with
  48 real vector gates (exceptions 0..31, IRQ 32..47), remapped the
  8259 PIC away from the exception range, wrote per-vector asm stubs
  plus a common C dispatcher, and rewrote the keyboard driver to be
  IRQ-driven with a 64-byte ring buffer.  The main context now
  `sti; hlt`s while idle instead of spinning.
- **2026-04-24 — M1: Own GDT installed.** Replaced GRUB's GDT with our
  own 3-entry flat table (null + kernel code + kernel data) in
  `kernel/hal/x86/gdt.c`.  Loaded from `kernel_main` before any other init
  that would care about segment selectors.
- **2026-04-24 — ACPI shutdown wired.** Implemented RSDP/RSDT/FADT/DSDT
  walk and minimal `_S5_` parser. `hal_shutdown` now prefers ACPI and falls
  back to emulator hacks. Added `reboot` command using the 8042 reset pulse.
- **2026-04-23 — Initial bring-up.** Multiboot1 header, linker script,
  Makefile+Dockerfile build chain, VGA terminal (clear/scroll/backspace),
  polled PS/2 keyboard, shell with `help/clear/echo/about`.
