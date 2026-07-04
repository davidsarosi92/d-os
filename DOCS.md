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
| 4.13 | GUI — compositor + windows + mouse (M22) | ~1530 |
| 4.14 | GUI development — apps, desktop shells (M22.2) | ~1610 |
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

### 4.13 GUI — compositor, WM, taskbar, widgets, file manager (M22 + M22.1)

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
- **Known limits (deferred):** no window minimize; no per-window
  damage rects (full recompose per frame); no Alt-Tab; no widget
  nesting/containers; terminal windows have no close button (their
  shell task would leak — needs task kill support); cursor is
  IRQ-latency bound (one tick worst case).

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
