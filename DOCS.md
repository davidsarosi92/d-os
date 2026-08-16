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
| 1 | Project layout | 98 |
| 2 | Boot flow | 164 |
| 3 | Memory layout | 181 |
| 4 | Components | 203 |
| 4.0 | Module framework + console registry | 205 |
| 4.1 | Terminal drivers | 278 |
| 4.2 | Keyboard | 301 |
| 4.3 | Shell | 314 |
| 4.4 | HAL x86 | 350 |
| 4.5 | GDT | 360 |
| 4.6 | IDT + PIC + IRQ dispatch | 380 |
| 4.7 | Multiboot info | 404 |
| 4.8 | Physical Memory Manager — buddy allocator (M19) | 420 |
| 4.9 | Virtual Memory Manager | 480 |
| 4.X | Tasks + scheduler | 507 |
| 4.X | SMP — APIC, AP boot, per-CPU, real spinlocks (M18) | 568 |
| 4.X | HAL — arch-independent interface | 696 |
| 4.X | Keyboard layouts | 753 |
| 4.X | USB host stack — xHCI + HID boot keyboard | 828 |
| 4.X | Virtual consoles / pane split | 928 |
| 4.X | Ring 3 / user mode | 1013 |
| 4.X | Block layer + virtio-blk | 1056 |
| 4.X | procfs — kernel state as files under /proc | 1108 |
| 4.X | devfs — drivers as files under /dev | 1140 |
| 4.X | Configuration store | 1163 |
| 4.X | Filesystem layer | 1179 |
| 4.X | Block cache | 1239 |
| 4.X | exFAT | 1268 |
| 4.X | Timer | 1313 |
| 4.10 | Kernel heap — slab + page_alloc (M19) | 1331 |
| 4.11 | Serial debug | 1394 |
| 4.X | Supported architectures — i386 + x86_64 (M20) | 1408 |
| 4.12 | ACPI | 1549 |
| 4.13 | GUI — compositor, WM, widgets, apps (M22 – M22.5) | 1570 |
| 4.14 | GUI development — writing apps and desktop shells (M22.2) | 1846 |
| 4.15 | Process model — init, hierarchy, reaper, kill-tree (M27) | 1921 |
| 4.16 | Per-task GUI apps (M22.7 Stage A) | 1979 |
| 4.17 | ARM64 (AArch64) port — Phases A–M (M21, full x86 parity) | 2058 |
| 4.18 | System log — klog ring buffer + dmesg | 2399 |
| 4.19 | Userland foundation — processes, fds, IPC, libc (M25) | 2463 |
| 4.20 | Blocking primitives — wait-queue, task_wait, blocking IPC (... | 2540 |
| 4.21 | Services & the service bus (M29) | 2597 |
| 4.22 | Watchdog — freeze detection (M31) | 2658 |
| 4.23 | cron — time-based task scheduling (M30) | 2699 |
| 4.24 | Concurrent user processes + full-arch libc (Tier B — M25 tail) | 2732 |
| 4.25 | Networking — virtio-net + TCP/IP stack (M24, i386) | 2783 |
| 4.26 | Audio — AC97 codec + PCM output (M23, i386) | 2877 |
| 4.27 | POSIX process model — fork/exec/wait/pipe/signals (M34, i386) | 2925 |
| 4.28 | Threads + futex (M35, i386) | 3010 |
| 4.29 | Package manager — content-addressed store (M35.5, first slice) | 3078 |
| 4.30 | POSIX syscall breadth + libc growth (M36 stage 1, i386) | 3119 |
| 4.31 | Linux i386 syscall-ABI compat layer (M36 stage 2 / §M41, i386) | 3153 |
| 4.32 | Wayland display server (M26, stage 1 — wire protocol + hand... | 3304 |
| 4.33 | Dynamic linking — ld.so / .so / dlopen (M37, i386) | 3410 |
| 4.34 | C++ runtime — libstdc++ + exceptions (M38, i386) | 3477 |
| 4.35 | Crypto, entropy & TLS (M39, i386) | 3508 |
| 4.36 | On-device C compiler — TinyCC (M43 slice, i386) | 3604 |
| 4.37 | Resilience & the ring-3 boundary (M46 + kernel audit) | 3644 |
| 4.38 | Crash records & reporting (M47) | 3753 |
| 4.38.1 | Closing a window is not a crash (M47.1) | 3822 |
| 4.39 | x86_64 userland parity (M47.5) | 3868 |
| 4.40 | Upstream libwayland-client (M40 stage 1) | 3966 |
| 4.40.1 | EGL + GLES2 on Mesa softpipe (M40, DoD complete) | 4313 |
| 4.41 | The ring-3 pointer gate was never armed for the Linux ABI (... | 4372 |
| 4.42 | The memory ceiling, discovered rather than compiled in (M48) | 4405 |
| 4.43 | NetSurf: input it can act on, and a fetcher (M48) | 4508 |
| 4.44 | Mesa/EGL on i386 (M48) | 4588 |
| 4.45 | The mmap cursor belonged to the address space (M48) | 4618 |
| 4.46 | Load distribution across CPUs, measured (M49) | 4649 |
| 4.47 | AArch64 grows a POSIX process model (A1) | 4958 |
| 4.48 | One translation engine instead of one per architecture (M50) | 5044 |
| 4.49 | AArch64 runs unmodified musl (A2), and the engine proves it... | 5126 |
| 4.50 | AArch64 A3 — a shell that forks and execs, and the regist... | 5256 |
| 4.51 | The broadcast x86 does not have (M51 — TLB shootdown) | 5419 |
| 4.52 | The note that outlived its premise (M52 — per-CPU SYSCALL) | 5523 |
| 4.53 | Time, in nanoseconds (M53 stages 1–2) | 5608 |
| 4.54 | A task the scheduler was still standing on (M54) | 5720 |
| 4.55 | A deadline you can wait on (M53 stage 3) | 5900 |
| 4.56 | Waiting for the network without spending a CPU on it (M55) | 6066 |
| 4.57 | A wait that is really a wait (M56 — poll timeouts + epoll) | 6258 |
| 4.58 | A task is not dead while it is still running (M57 — the §M54 residual) | 6517 |
| 4.59 | A network that can hold more than one conversation (M24, second half) | 6706 |
| 4.60 | A window that resizes its contents too (§M42 follow-up) | 6960 |
| 5 | Build & run | 5203 |
| 6 | Compiler flags | 5230 |
| 7 | Roadmap / open milestones | 5256 |
| 8 | Change log | 5278 |

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

**Open:** ~~x86_64~~ — **dynamic linking works on x86_64 too** (verified
2026-08-01: `solibtest` and `dlopentest` both green there, and NetSurf is itself
a dynamic musl binary on that arch); aarch64 is still open.  A real `brk` heap
(mallocng falls back to mmap today); mmap reclaim on munmap; pthreads under the
Linux ABI (needs `clone` wired in — the TLS *relocation* model is proven, but
multi-thread `__thread` awaits §M44's musl pthread path); lazy PLT resolution
(BIND_NOW-style works today).

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

**Stage 3b — REAL HTTPS over the network (`netmusl`, `httpstest`).**  An
unmodified musl binary now does ring-3 networking.  musl on i386 funnels every
BSD-socket op through the Linux `socketcall` multiplexer (syscall 102) and
*prefers* the direct socket syscalls (359 socket … 373 shutdown, falling back
to socketcall on ENOSYS); `kernel/hal/x86/linux_abi.c` handles **both**, routing
them through one `linux_socketcall()` translator onto the M24 BSD-socket API
(`sys_socket`/`connect`/`sendto`/`recvfrom`; TCP payload rides `sys_write`/
`sys_read`).  A single `sockaddr_in` ⇄ host-order (ip,port) conversion site
keeps all `ntoh`/`hton` in one place (byte-order is a Linux-ABI concern, not an
M24-stack one).

- `netmusl` — the musl counterpart of `httptest.c`: DNS-resolve example.com over
  a UDP socket, then TCP `connect`/`write`/`read` an HTTP GET → `HTTP/1.1 200 OK`.
- `httpstest` — **real HTTPS with genuine certificate verification**: DNS → TCP
  `:443` → an mbedTLS handshake with the BIO wired to the live socket → the
  Mozilla CA bundle (`third_party/cacert.pem`, 128 roots) provisioned to
  `/etc/ssl/cert.pem` as the trust store → `MBEDTLS_SSL_VERIFY_REQUIRED` with SNI
  + hostname check → an HTTP GET over TLS.  Boot-tested over QEMU SLIRP:

  ```
  https: example.com = 172.66.147.243
  https: TCP connected to :443
  https: CA bundle parsed (0 certs rejected)
  https: handshake OK — TLSv1.3 / TLS1-3-CHACHA20-POLY1305-SHA256
  https: cert verify flags = 0x0 (0 = chain + hostname trusted)
  https: HTTP status over TLS: HTTP/1.1 200 OK
  https: HTTPS PASS
  ```

The CA bundle + `/etc/resolv.conf` (`nameserver 10.0.2.3`, the SLIRP resolver)
are provisioned from `pkg_init()` — deliberately the *late* boot phase where the
store already does large ramfs writes (the ~750 KiB musl `libc.so`); doing the
333 KiB write from an early `kernel_main` self-test tripped a latent large-order
buddy corruption (PLAN.md §M39 "Lesson learned"; diagnostic: `pmm_validate()` +
the `memcheck` shell command).  QEMU needs `-netdev user -device virtio-net`.

**Stage 3c — musl `getaddrinfo` + a real `wget`.**  musl's OWN resolver now runs
on d-os: getaddrinfo() reads `/etc/resolv.conf`, sends its A query via
sendto()/recvmsg() and waits on poll() — three Linux-ABI ops that were missing
(`recvmsg`/`sendmsg` sub-calls + direct syscalls 372/370, and `poll` = 168) are
now translated in `linux_abi.c` onto the M24 socket API.  The recvmsg path is
the subtle one: musl DROPS any DNS reply whose source sockaddr doesn't match a
nameserver it queried, so `linux_recvmsg` fills `msg_name` from the datagram's
source (via the same host-order⇄sockaddr_in site).  `httpstest` dropped its
hand-rolled DNS for a plain `getaddrinfo("example.com","443",…)`.  On top of that
a userland **`wget`** (`user/wget.c`, musl + mbedTLS) fetches `http://` *and*
`https://` URLs from argv — real TLS + CA verify for https — streaming the body
(headers stripped) to stdout or a file; the shell `wget <url> [outfile]` command
execs it under the Linux personality (kernel HTTP-only path kept as a fallback).
Boot-tested: `wget https://example.com/` → getaddrinfo → TLS handshake → the
Example Domain HTML (571 body bytes), and the same over plain http.

**Open:** DHCP-populated `resolv.conf`; `/etc/hosts`; IPv6/AAAA; x86_64/aarch64
(no mbedTLS/M24 there yet).

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
it with tcc, runs the result, and shows the program's **captured stdout in an
"Output" window** (`devtools.h`: `dos_tcc_compile`/`dos_run_elf_cap` — the shared
engine).  Capture is a per-task buffer (`task->cap_buf`): `sys_write(1/2)` also
appends to it when set (bounded, NUL-terminated) while still echoing to the
console.  The shell `exec` reports `[captured N bytes]`.

**Needed (all help every musl program):** Linux-ABI `_llseek`(140) (tcc seeks
in `.o` files — else "invalid object file"), `lseek`(19), `unlink`(10).

**Open:** a bigger compiler (gcc/clang) for full self-hosting; capture compile
diagnostics too (only run output is captured today); more Linux ABI breadth;
x86_64/aarch64.

---

### 4.37 Resilience & the ring-3 boundary (M46 + kernel audit)

The design rule this milestone enforces: **nothing a user program does may take
the machine down.**  A crash, a wedge, a bad pointer, a runaway loop — each must
cost exactly one process.  The work came out of a full `kernel/` audit
(`AUDIT_FINDINGS.md`), whose findings are cross-referenced below as §x.y.

**1. A ring-3 fault kills the process, not the box** (`hal/x86/idt.c`,
`hal/x86_64/idt.c`, `hal/aarch64/exceptions.c`).  Any CPU exception taken with
`CS & 3 == 3` (x86) / `SPSR_EL1.M[3:0] == 0` (EL0) terminates the current task
with `128 + signal` (`#DE`→SIGFPE, `#UD`→SIGILL, `#GP`/`#PF`→SIGSEGV) and
reschedules; init reaps it and frees its address space.  Previously ANY fault
fell through to `cli; hlt`, which killed the timer, scheduler, cursor and the
watchdog itself on that CPU (§1.2 on ARM).  A ring-0 fault applies
`kernel.fault_policy`: `halt` (default) / `reboot` / `kill` (terminate just the
faulting kernel thread — best-effort, an M29 service is then restarted).

**2. Force-kill of a WEDGED ring-3 task.**  A program spinning in userland never
yields, so the cooperative `task_kill` can never land.  `task_force_kill(pid)`
sets a forced flag that `task_force_kill_point()` acts on at the IRQ/timer exit
path — where the interrupted task provably holds no kernel locks.  Wired on all
three arches (i386, x86_64, aarch64).  Shell: `fkill <pid>`, `wedge` spawns a
test hog.  Opt-in automatic reclaim: `task_set_auto_fkill(pid, ms)` +
`package[.<name>].auto_fkill_ms` (off by default) — the watchdog force-kills a
task that burned that much CPU with no voluntary yield (`last_yield_ms` /
`cpu_ms_at_yield` distinguish a real hog from a merely starved task).

**3. Hard-lockup detection (§M31 L3).**  `-device ib700` +
`-action watchdog=inject-nmi`: the watchdog task pets the chip every sweep; if
the kernel stops petting (spinning or halted with IRQs OFF — the one hang the
task-based layers cannot see) the chip raises an **NMI**, which is delivered
even with IRQs masked (LVT LINT1 = NMI mode, `lapic.c`).  The handler logs the
stuck EIP/RIP lock-free to COM1, then recovers: a ring-3 lockup is force-killed
in place and IRQs re-enabled on iret; a ring-0 or repeat lockup reboots.
`spin_lock` also reports a probable deadlock (lock + caller address) to COM1
after an absurd spin count, and `scripts/dos-dump.sh` dumps every vCPU's
registers from the QEMU monitor of a frozen guest.

**4. The chrome works when the app is frozen.**  Ctrl+Alt+Del (Task Manager) and
Ctrl+Alt+X (close top app) are trapped in the raw keyboard IRQ (`ps2_keyboard.c`
`sak_try`) BEFORE any window routing, and acted on by the compositor — never by
the possibly-wedged app.  A package window's X button force-kills a client that
does not respond (`gui.c` `apply_pending`), so the button always closes the
window.  The Task Manager gained a **Force kill** button next to End task.

**5. The user-pointer boundary (§1.1) — two layers.**
   - *Gate*: while a task is inside a syscall entered from ring 3
     (`task->in_user_syscall`, set by each arch dispatcher) every pointer
     argument is checked with `vmm_user_access_ok()` (range + present + U/S +
     R/W in the ACTIVE page tables).  The flag matters: the `sys_*` handlers are
     dual-use — in-kernel callers (the shell self-tests) pass KERNEL buffers and
     must not be gated.  Kernel-pointer arguments from a dispatcher that also
     handles user pointers use the explicit `sys_*_k` cores.
   - *Fault fixup*: the copies run through `uaccess_copy_in/out/str_in`
     (`hal/<arch>/uaccess.c`), whose user-touching instructions are registered in
     the `.ex_table` linker section.  A kernel-mode fault at such an instruction
     resumes at its fixup (`uaccess_fixup_lookup`), so even a range that becomes
     invalid BETWEEN the check and the copy returns `-EFAULT` instead of
     panicking.  `faulttest` proves both layers (it calls the primitives on
     deliberately unmapped memory).
   Everything reachable from ring 3 was routed through this: `SYS_PRINT` (all
   three arches — it used to walk the raw pointer), the whole `linux_abi`
   surface (stat64/fstat/clock_gettime/gettimeofday/time, iovecs, socketcall
   argument arrays, sockaddr, msghdr, set_thread_area, `_llseek`, waitpid
   status, nanosleep, unlink + execve paths, the dosgui window title).

**6. Cross-process integrity in the display bridge** (§2.1/§2.2, `dosgui.c`).  A
handle now records its owner pid and every call rejects a handle it does not own
(no blitting into, polling or destroying another package's window).
`dosgui_present` clamps `w`/`h`/`stride` and validates the whole source range in
the caller's address space before the compositor reads it.

**7. Signals (§1.3/§1.4, `hal/x86/signal.c`).**  `sigreturn` validates the saved
context on the untrusted user stack and restores **only** the user-settable
EFLAGS bits (`0xCD5`: CF PF AF ZF SF DF OF) — IF/IOPL/NT/TF stay the kernel's.
Delivery validates the user stack frame first and SIGSEGVs the process instead
of faulting the kernel on a smashed `%esp`.  `sys_kill` (§2.3) now only lets a
ring-3 caller signal itself or its descendants — never a kernel thread, pid 0 or
init.

**8. Process-model rule.**  A dying task takes its subtree with it (children are
killed — ring-3 children forcibly, kernel threads cooperatively), unless a child
set `survives_parent` (a detached daemon), in which case it is re-parented to
init.  This is why a GUI-launched app dies with the desktop while a "Detached
Shell" outlives it.  pid 0 and idle are excluded (pid 0's exit is just the boot
task becoming idle — taking its subtree down would kill init).

**9. Arch parity.**  x86_64 gained the force-kill safe point, the NMI handler and
**real copy-on-write** (`vmm_space_clone` marks writable pages COW + refcounts
them; `vmm_cow_fault` privatises on write) — it used to eager-copy every page on
fork.  aarch64 gained the EL0-fault-kill, the `kernel.fault_policy` knob and the
force-kill safe point.

**10. Boot with more than the identity window** (`acpi/acpi.c`).  Firmware puts
the ACPI tables at the top of low RAM, which on i386 with `-m 512M` is ABOVE the
256 MiB identity map — the first table read was an unmapped kernel access, i.e.
the box died during boot with no diagnostics.  `acpi_reach()` now identity-maps
each table's pages on demand before they are dereferenced.

**Shell:** `fkill`, `wedge`, `faulttest`; `/proc/watchdog` grew `runaway_events`.
**Config:** `kernel.fault_policy` (halt|reboot|kill), `kernel.hw_watchdog`,
`package.auto_fkill_ms`, `package.<name>.auto_fkill_ms`.

**Open:** a hardware watchdog on aarch64 (no ib700 on `virt`); credentials-based
`sys_kill` once §M32 lands.  (Bounce buffers for the bulk payloads — the third
layer — landed 2026-08-01, see the change-log entry below.)

---

### 4.38 Crash records & reporting (M47)

M46 made sure a faulty program cannot take the machine down.  M47 answers the
other half of the same requirement: **when something does go wrong, the system
must say so** — recorded now, surfaced to the user later or immediately, through
whatever mechanism happens to be armed.

**Two phases, on purpose** (`core/crash.c`, `includes/crash.h`).  *Capture*
(`crash_report`) runs in the worst context in the system — inside an exception
or NMI handler, IRQs off, possibly on a broken stack — so it does the absolute
minimum: copy a fixed-size `struct crash_record` into a static 32-entry ring and
bump a counter.  No allocation, no locks, no formatting, no I/O; a lock there
could deadlock against the very fault being recorded.  *Delivery*
(`crash_drain`, called from the watchdog sweep) runs on an ordinary task, where
a sink may allocate, block, open a file or draw a window.  That split is the
whole design.

**Sinks** register with `CRASH_SINK(name, fn)` (linker section, same pattern as
`DRIVER()`/`SERVICE()`).  Two ship today:

| sink | where it lives | what it does |
|------|----------------|--------------|
| `klog` | `core/crash.c` | always present — the baseline guarantee that nothing goes unrecorded |
| `gui-report` | `gui/apps/crashapp.c` | queues the "Crash Reports" window onto the compositor |

Adding "report over the network" or "append to a file" later is one more file
and one more `CRASH_SINK()` — **no fault path is ever touched again.**

**Surfaces.**  A record has one representation and several views: `crash`
(console), **`/proc/crash`** (machine-readable — the header lists `captured`,
`ring` and every registered sink, then one line per record newest-first), the
system log, and under a GUI the **Crash Reports** window (Start menu, or opened
by itself when a record is delivered).  The window is a *view*, never the
storage: turning it off leaves the record fully captured everywhere else.

Kinds: `user-fault`, `kernel-fault`, `hard-lockup`, `task-hang`, `deadlock`,
`forced-kill`, `unclean-boot`.  Wired into the ring-3 fault path, the ring-0
fault path (captured BEFORE the policy runs — halt and reboot never return), the
NMI watchdog, the L1 task-hang detector, the spinlock deadlock detector and the
forced-kill safe point, on all three arches.

**The failure nothing in the guest can log.**  A triple fault, a hardware reset
or a power loss resets the CPU with no handler running.  That case is covered
from the other side: `crash_boot_begin` arms a marker in battery-backed CMOS
NVRAM (HAL contract `hal_nvram_read/write`) and `crash_boot_clean` disarms it on
an orderly shutdown, so finding it still armed means the previous boot died
where nothing could report it.  Alongside the marker sits a **40-byte checksummed
breadcrumb** of the most recent record (kind, cpu, pid, pc, addr, code, uptime,
comm), rewritten on every capture — so the next boot reports not just *"the last
boot died"* but *"…after a forced-kill in 'wedge' (pid 25) at pc=…, 44 s into
that boot"*, which is usually the whole diagnosis.  NVRAM rather than a file on
purpose: it must survive exactly the events during which no filesystem write can
be trusted.  aarch64 on QEMU `virt` has no such storage and says so out loud
rather than silently concluding every boot was clean.

**Config:** `crash.report` (on|off, default on) — gates the GUI report window
only, read on every record so it can be armed or silenced at any time without a
reboot.

**Desktop chrome, same session:** the taskbar clock grew the ISO date and the
active keyboard layout code (`2026-08-02  20:18:16  US`), and the wallpaper
label carries the architecture next to the milestone (`d-os M47  x32` /
`x64` / `arm64`) — so a screenshot in a bug report identifies its own build.

**Open:** persisting records across a *clean* reboot (a store file), a network
sink, and per-kind filtering of what opens the window.

---

### 4.38.1 Closing a window is not a crash (M47.1)

M46 promised that the title-bar X **always** closes a window, even when the app
behind it is frozen, and implemented that by force-killing the client.  The
implementation force-killed on the FIRST compositor pass that saw `want_close` —
before the client could possibly have noticed the close event.  So closing a
perfectly healthy NetSurf was reported as *"unresponsive task reclaimed by
force"*, wrote a crash record, and (once §M47 stage 2 landed) popped the Crash
Reports window as though the browser had crashed.

The kill is the **fallback for a wedged client, not the close path**, and the
escalation belongs to the USER — the familiar desktop contract:

| click | meaning |
|-------|---------|
| 1st X | *ask* the client to close.  A healthy one sees the close event, calls `DOSGUI_DESTROY`, and the window is disposed with **no kill and no crash record**. |
| 2nd X | the window is still there, so it clearly is not going to close on its own — force it, **immediately**. |

`gui.close_grace_ms` (default 10000 ms) survives only as the **unattended
backstop**, for when nobody is there to click a second time; it is deliberately
generous so it never pre-empts a client that is merely slow to shut down.
M46's guarantee is unchanged — the X still always closes the window.

**`wedgewin` — the test this guarantee never had.**  `wedge` proved a wedged
ring-3 task can be reclaimed; nothing proved the thing a user actually
experiences, which is why a regression in the opposite direction went unnoticed.
`user/wedgewin.c` opens a real window through the dosgui bridge, presents one
frame, then spins forever without ever polling — so the close event can never be
observed by it.  Verified on x86_64:

| case | result |
|------|--------|
| NetSurf (healthy) — one X click | window closes, **no** force-kill, **no** crash record |
| `wedgewin` (frozen) — first X click | window **stays open** (the request was made and ignored) |
| `wedgewin` (frozen) — second X click | `gui: second close click on 'Wedged App' → force-killing client pid 18`, window disposed, forced-kill recorded, Crash Reports opens |

The last row is the correct outcome: that program really *was* unresponsive.

**Lesson (test-shape, worth keeping):** the pointer harness that drives these
tests must step the mouse in ≤100 px hops — QEMU's PS/2 packet carries a signed
byte delta, so one big `mouse_move` is silently clamped and the click lands
somewhere else entirely.  The first attempt at this repro clicked the middle of
the page and concluded, wrongly, that the bug did not exist.

---

### 4.39 x86_64 userland parity (M47.5)

Until this point x86_64 ran a visibly smaller userland than i386 — no coreutils,
no `sh`, no sockets from ring 3, no on-device compiler.  **None of that was
missing kernel support.**  It was three pieces of duplication, each of which had
quietly fallen behind its i386 twin.

**1. The blob symbol carried the architecture.**  Every in-tree ring-3 program is
embedded from `user/<name>_<arch>.elf`, and objcopy mints its symbols from that
filename — so the kernel spelled out `_binary_user_forktest_i386_elf_start` and
every program was i386-only *by construction*: the blob linked fine on x86_64 and
the shell still reported "not embedded for this arch".  One pattern rule now
embeds them all, and `--redefine-sym` strips the arch back out, so the kernel
refers to *the embedded forktest program* and the build decides which
architecture that is.  Sixteen identical rules collapsed into one.

**2. The program lists were written twice.**  `X86_USER_BLOBS`,
`MUSL_COREUTILS`, `MUSL_PROG_BLOBS`, `MUSL_DYN_BLOBS`, `CXX_RUNTIME_BLOBS` and
`MBEDTLS_PROG_BLOBS` are now defined once above the arch branches, so parity is a
property of the build rather than a promise.  (`linuxhello` stays i386-only on
purpose — it hard-codes the 32-bit Linux syscall numbers, which is the point of
that program.)

**3. The x86_64 syscall dispatcher stopped at M25.**  `fork`, `waitpid`,
`execve`, `pipe`, `dup2`, `kill`, `sigaction`, `sigreturn`, `socket`, `bind`,
`connect`, `sendto`, `recvfrom`, `clone`, `futex`, `set_tls`, `stat`, `fstat`,
`getdents`, `uname`, `clock_gettime`, `nanosleep` and `getrandom` all call the
SAME portable cores i386 calls; only reading the arguments out of the frame is
arch-specific.  New: `hal/x86_64/signal.c` (delivery + sigreturn) — amd64 passes
the signal number in RDI, so its sigreturn frame layout differs from i386's, and
getting that wrong would be silent (the handler runs, the restore brings back
garbage).  The Linux-ABI translator gained `poll` and the amd64 BSD-socket
numbers (41–55), including an amd64 `struct msghdr` whose 64-bit `msg_iovlen`
and post-`msg_namelen` padding are spelled out rather than copied from i386.

Four bugs surfaced, all the same shape — code that assumed 32 bits:

| where | what | how it showed up |
|-------|------|------------------|
| `crt0_x86_64.s` | called `main()` without reading argc/argv | every argv-using program read a pointer as argc |
| `libc.c thread_create` | pushed the thread argument on the stack (i386 cdecl); amd64 passes it in RDI | threads ran with a garbage argument — no crash, wrong data |
| `libc.h tls_load4` | hard-coded `%gs` *and* a literal field offset of 4 | x86_64's thread pointer is FS.base, and `self` is a pointer so the id sits at 8 |
| Makefile | virtio-net + AC97 absent from the x86_64 source list | the network stack linked with no NIC under it |

**Toolchains made arch-aware:** `make mbedtls` and `make tcc` now build into
`third_party/mbedtls-<arch>` / `third_party/tinycc-<arch>`, driven by per-arch
`MUSL_HDR_DIR` / `MUSL_CRT_DIR` / `TCC_CPU` / `TCC_CC` / `TCC_TOOLCHAIN`, and the
four copy-pasted Mbed TLS link rules became one canned recipe per arch.

**Also fixed (pre-existing, i386):** `linux_socketcall` validated its argument
array as a *user* pointer, but the direct socket syscalls (359+) repack CPU
registers into a *kernel* array — so every direct call returned -EFAULT, and musl
only falls back to socketcall on -ENOSYS, meaning `socket()` simply failed.  Split
into `linux_socketcall_k` (kernel array) + a gated wrapper, the same lesson as
the `sys_*_k` cores: **the user-pointer check belongs where the pointer's ORIGIN
is known, not in a shared core.**

**Verified on x86_64:** runargs / forkexec / pipetest / sigtest / threadtest /
tlstest / posixtest / faulttest / fputest / archtest green; `lsnic` finds eth0;
dnstest resolves and httptest fetches `HTTP/1.1 200 OK`; crypttest + ssltest
(TLSv1.3) + httpstest (HTTPS with CA verification) + `wget http://example.com`
all pass; `pkgrun sh -c "echo hi; ls /store"` forks children that execve real
musl coreutils; `tcc /hello.c -o /myprog` compiles and the result runs.  i386
regression re-run green; aarch64 builds clean.

**musl `getaddrinfo` — fixed (was broken on i386, pre-existing).**  Two
independent defects, both pre-existing, both found by tracing what musl's
resolver actually did rather than by reading the code:

1. **`SOCK_NONBLOCK` was discarded.**  musl opens its resolver socket
   non-blocking and drains it with `while (recvmsg(...) >= 0)` — it relies on
   the *second* call failing with EAGAIN to know the burst is over.  Our sockets
   were always blocking, so `sys_recvfrom_k` sat in its 40-million-iteration
   bounded spin: on emulated i386 that is minutes, indistinguishable from a
   hang, while on x86_64 it happened to finish inside musl's own timeout — which
   is precisely why the bug looked arch-specific.  `struct netsock` now carries
   a `nonblock` flag (`sys_socket_setnonblock` / `sys_socket_getnonblock`), the
   datagram and stream receive paths return `-SOCK_EAGAIN`, and both Linux-ABI
   layers honour `SOCK_NONBLOCK` on `socket()` **and** `fcntl(F_SETFL/F_GETFL)`.

2. **`hostorder_to_sockaddr` validated a KERNEL word as a ring-3 pointer.**
   Its `addrlen` argument is a client `socklen_t` when called from `recvfrom`,
   but `msg_namelen` lifted out of an already-validated `msghdr` when called
   from `recvmsg`.  The user-pointer check made the recvmsg path return without
   writing anything, so `msg_name` stayed zeroed — and musl *drops any DNS reply
   whose source address does not match a nameserver it queried*.  Every answer
   was silently discarded; the query just retried until it gave up.  The helper
   now takes a kernel in/out word and the recvfrom call sites validate the
   client's `socklen_t` themselves.  **Third instance of the same lesson** (after
   the `sys_*_k` cores and `linux_sendmsg`): the user-pointer check belongs where
   the pointer's ORIGIN is known, never inside a shared helper.

Verified on **both** arches: `httpstest` completes a real TLSv1.3 handshake with
CA verification, `wget http://example.com` downloads 571 body bytes, and
`netmusl` fetches over plain HTTP.

---

### 4.40 Upstream libwayland-client (M40 stage 1)

§M26 built d-os's own Wayland **server** plus a hand-written mini client library
(`user/libwl`).  That proved the wire protocol.  §M40 is the other half, and the
one that matters for running unmodified applications: **the real
libwayland-client** — the same library a GTK/Qt/SDL program links against on
Linux — talking to our server.  Nothing is forked; the vendored tree is pristine.

**Three pieces, and only the first is unusual.**

| piece | where it runs | why |
|-------|---------------|-----|
| `wayland-scanner` | the HOST (apt's, in the build image) | turns the protocol XML into C.  Nothing generated is committed — it is regenerated on every build. |
| `libffi` | cross-built for the target musl | libwayland dispatches an incoming event by building an argument list at runtime and calling the listener through `ffi_call`.  There is no way around it short of forking the library, which is what this milestone exists NOT to do. |
| the library | cross-built for the target musl | four C files (`connection`, `wayland-client`, `wayland-os`, `wayland-util`) + the generated protocol tables. |

`make [ARCH=…] wayland` produces `third_party/{libffi,wayland}-<arch>/`.  The
build happens in a scratch copy of the tree because wayland's sources do
`#include "../config.h"`, which meson would normally generate at the tree root —
writing it into the vendored tree would stop that tree being pristine.

**How the client connects.**  d-os has no named UNIX sockets, so there is no
`$XDG_RUNTIME_DIR/wayland-0` to open.  It does not need one: libwayland honours
**`WAYLAND_SOCKET`**, an already-connected file-descriptor number — the same
documented mechanism a real compositor uses to launch its own clients — which is
exactly the shape d-os already had (`run_wayland_client` hands the client fd 3
and runs the server on the other end).  Passing it needed one new kernel seam:
`proc_set_exec_env()` (`task.exec_extra_env`), one `KEY=VALUE` handed to the next
exec on the calling task and consumed by it, so it can never leak into a later
one.  That is the seam a full per-exec `environ` will grow from.

**One real bug this surfaced.**  Both Linux-ABI layers implemented `recvmsg` /
`sendmsg` for AF_INET sockets only.  A Wayland display connection is a UNIX
socket, so libwayland's very first read returned a bare `-1`, which musl turned
into **EPERM** — a spectacularly misleading errno for "wrong fd type", and the
reason the first run reported `wl_display_get_error=1` with no protocol error.
Both now route by `sys_fd_kind()`: a UNIX socket takes the ordinary read/write
path, only an AF_INET socket goes through recvfrom/sendto.

**Verified on i386 and x86_64** — `wayupstream`:

```
wlupstream: connected, display fd = 3
wayland: get_registry(id=2) -> advertising 4 globals
wayland: sync(callback=3) -> done + delete_id
wlupstream:   global 1: wl_compositor v4
wlupstream:   global 2: wl_shm v1
wlupstream:   global 3: xdg_wm_base v2
wlupstream:   global 4: wl_seat v5
wlupstream: PASS — UPSTREAM libwayland-client spoke to the d-os server
```

Every global came back through libwayland's own closure machinery, which means
the generated protocol tables, the connection layer and `ffi_call` are all
working.  The §M26 native `waytest` (handshake + shm buffer + xdg_shell) still
passes unchanged.

#### Stage 2 — a real window, driven by upstream libwayland

The client now runs the ordinary application sequence: bind
`wl_compositor`/`wl_shm`/`xdg_wm_base`, `memfd_create` + `ftruncate` + `mmap` a
pixel buffer, `wl_shm_create_pool` → `create_buffer`, `create_surface` →
`get_xdg_surface` → `get_toplevel` → `set_title` → commit, wait for
`xdg_surface.configure`, `ack_configure`, then attach + damage + commit.  The
server logs every step and the configure handshake completes:

```
wlupstream: shm buffer 160x120 ready (fd 4)
wayland: get_toplevel -> object 10; sending configure
wayland: xdg_toplevel set_title("Upstream Wayland")
wlupstream: xdg_surface.configure seen = 1
wayland: xdg_surface ack_configure(serial=2)
wayland: surface 8 attach buffer 7
```

Three kernel gaps had to be closed for that:

| gap | fix |
|-----|-----|
| no `memfd_create` / `ftruncate` in either Linux ABI | mapped to `sys_memfd` + the new `sys_memfd_resize` / `shm_grow` (Linux hands back a zero-length object and sizes it afterwards) |
| `sys_mmap_full` knew only file-backed and anonymous mappings | a memfd (`FD_SHM`) is a third case: it maps the object's EXISTING frames, shared, which is the whole point.  The native `sys_mmap` had this; the Linux-facing entry point did not, so the client's `mmap` of its own pool failed |
| no SCM_RIGHTS in the ABI control path | `lnx_cmsg_take_fd` / `lnx_cmsg_put_fd` around the existing `usock` descriptor passing (`sys_send_k`'s `passfd`), declared per-arch so the 12-byte/4-aligned and 16-byte/8-aligned `cmsghdr` layouts both come out right |

**Verified end to end on i386 and x86_64.**  The server reads the client's
pixels out of shared memory:

```
wayland: create_pool(id=3,size=76800) shm-fd=received
wayland: COMMIT surface 8: 160x120 buffer, top-left=ff102040 checksum=e4484200
wlupstream: PASS — UPSTREAM libwayland-client drove a real xdg_toplevel + shm buffer
```

`0xff102040` is exactly the colour the client wrote at (0,0).

**The pool descriptor arrived as 0 at first, and the reason is worth keeping.**
libwayland DUPLICATES every descriptor it sends
(`wl_closure_marshal` → `wl_os_dupfd_cloexec` → `fcntl(F_DUPFD_CLOEXEC)`), and
our `fcntl` "succeeded" by returning 0 for every command it did not implement.
**0 is a valid descriptor**, so libwayland believed the dup and sent descriptor
zero — no error anywhere, at any layer.  Two fixes: `F_DUPFD`/`F_DUPFD_CLOEXEC`
now really duplicate (`sys_dupfd`), and any unimplemented command that is
supposed to yield a descriptor fails loudly instead.  The first attempt at
`sys_dupfd` then handed back 0 again, because descriptors 0–2 are reserved for
the console and deliberately absent from the table (`fd_lookup` rejects them,
`fd_install` starts at 3) — a dup must obey the same convention or it returns
something that looks valid and can never be looked up again.

#### Stage 3 — the surface IS a desktop window

`wayupstream win` runs the connection in the server-per-surface `wm_mode` the
§M26 `waycomp` demo established, so the upstream client's `xdg_toplevel` becomes
a real desktop window: title bar, taskbar button, and the client's pixels as its
contents.  Verified by screenshot on both arches.

Two things had to move for it to look like an application rather than a demo:

- **The window is created at the FIRST COMMIT WITH CONTENT, not at
  `get_toplevel`.**  Our configure says 0×0 — "you pick a size" — so at
  `get_toplevel` the size is genuinely unknown; creating the window there meant
  a fixed placeholder rectangle with the client's pixels in one corner.  New
  `gui_window_outer_for_content()` turns the buffer size into the outer window
  size, since the decoration thickness can no longer stay private to `gui.c`.
- **The title is remembered until then.**  A client titles its toplevel *before*
  it has any content, so `wl_conn.title` holds it and the window is created with
  it (and retitled if it changes later).  Without this every application's
  window said "Wayland client".

#### Stage 4 — real input reaches the client's `wl_seat`

The client binds `wl_seat`, takes a `wl_pointer` and a `wl_keyboard`, installs
upstream listeners, and receives genuine desktop input: moving the QEMU pointer
over the window and pressing a key produce

```
wlupstream: pointer motion -> (78,62)
wlupstream: key 4 pressed
```

on both arches — dispatched by libwayland's own machinery into the client's
callbacks.

**Three gaps had to close, and each had been hidden by the same thing: §M26's
demo synthesised input directly instead of letting it come from the desktop.**

1. **Nobody drained a hook-backed window's event queue.**  Queued input is
   consumed by the window's app-host (`app_host_main`) — but a Wayland window is
   created by the server task, which then blocks reading its client's socket,
   and a dosgui window (NetSurf) is client-managed with `host_task` cleared
   outright.  Neither is anyone's app-host, so the events simply piled up.  The
   compositor now pumps any window that has an `input_hook`: it is an ordinary
   task (the hook does a socket send, which must not happen in the mouse IRQ)
   and such a window has no widgets for anyone else to dispatch to.
   **This also fixes NetSurf's input, which had the same defect.**
2. **Pointer motion never reached a window.**  Widget windows deliberately get
   motion only on click — a widget hit-test per mouse packet would be pointless
   work — but `wl_pointer.motion` is how an application tracks the cursor at
   all.  Motion is now delivered to hook-backed windows.
3. **Only nav and Ctrl-letter keycodes were forwarded.**  Again correct for
   widgets, wrong for a client: `wl_keyboard.key` carries raw keycodes and the
   application does its own interpretation, so a hook-backed window gets every
   key.

Note the listener structs in the client are filled completely, stubs included:
libwayland calls whatever the compositor sends, and a NULL slot for an event
that does arrive is a jump to address zero.

#### Stage 5 — the two globals a real toolkit refuses to start without

Before attempting a toolkit port it is worth knowing what a toolkit actually
demands, because both of these are hard requirements rather than niceties:

- **`wl_output`.**  SDL, GTK and Qt all walk the registry looking for an output
  and will not open a window without one — they need the size and scale before
  they can lay anything out.  A compositor that advertises none simply looks
  broken to them.  The server now exports `wl_output` v2 and answers a bind with
  the full property burst: `geometry` (position, physical size, make/model),
  `mode` (current + preferred, the real framebuffer size at 60 Hz), `scale`, and
  the `done` that tells the client the burst is complete.
- **`wl_surface.frame`.**  A render loop asks for a frame callback and then
  *blocks until it fires*.  Ignoring the request does not merely lose throttling
  — the application stops drawing entirely.  The server now records the callback
  and answers it on the next commit (the moment that frame became visible),
  then deletes the object, as the protocol requires for a single-shot
  `wl_callback`.

Verified on both arches with the upstream client:

```
wlupstream:   global 5: wl_output v2
wlupstream: output make=d-os model=virtual
wlupstream: output mode 1920x1200 @60000 mHz (preferred)
```

and the client's frame loop is genuinely self-sustaining — each callback
requests the next and commits — producing ~130 commits over the run instead of
the two a one-shot test would show.

#### Stage 6 — an UNMODIFIED upstream application runs

`weston-simple-shm` — weston's own reference client — runs on d-os, **compiled
exactly as it sits in the weston tree**.  Not a d-os test program: everything
around it is what weston's build system would have provided (the generated
protocol code, `shared/os-compatibility.c`, a `config.h`), and not a line of
`clients/simple-shm.c` is patched.

It binds every global, creates an `xdg_toplevel` titled "simple-shm", allocates
a 250×250 shm pool whose descriptor arrives over SCM_RIGHTS, and then **animates
continuously** — each commit carries a different checksum, driven by its own
frame-callback loop — into a real d-os window with a title bar and a taskbar
button.  716 frames in one i386 run.

`config.h` defines `HAVE_MEMFD_CREATE`, which sends `os_create_anonymous_file`
down the memfd path.  That is not a shortcut: d-os has no writable
`XDG_RUNTIME_DIR` for the `mkostemp` fallback to use, so the memfd path (§M40
stage 2) is the one that can actually work.

Two requests it makes had to be accepted:

- `xdg_toplevel.set_app_id` — the desktop-file identity.  We have no application
  database to look it up in, but every real toolkit sends it and an unanswered
  request is a protocol error to a strict client.
- `wl_shm_pool.destroy` — the client is done with the POOL, but buffers carved
  out of it stay valid, so the frames must **not** be released.  simple-shm
  destroys its pool immediately after creating its buffers and then draws from
  them for the rest of its life; releasing there would pull the pixels out from
  under a live window.

One build trap worth recording: `-I<weston>/shared` must **not** be on the
include path.  weston has its own `shared/signal.h`, so that directory makes
`#include <signal.h>` resolve to weston's server-side header instead of the C
library's.  The sources include it as `"shared/os-compatibility.h"`, so
`-I<weston>` is what they actually need.

#### Stage 7 — focus, the events a toolkit waits for

`wl_pointer.motion` and `wl_keyboard.key` are ignored by a real client until the
surface has been told it has **focus**: SDL, GTK and Qt all drop input that
arrives without a preceding `enter`, because on a real compositor that is what
"the pointer is over someone else's window" looks like.  §M26 got away without
them only because its demo client had no such logic.

The server now sends `wl_pointer.enter` and `wl_keyboard.enter` (plus the
`modifiers` that must follow it, and `wl_pointer.frame`, which a v5 client waits
for before applying what it has received) the first time it has both a surface
and the device object.  There is one surface per connection and it owns the
window, so focus follows the window and is never revoked.

**libwayland caught a bug our own client never would have.**  The first
`modifiers` event was sized 24 bytes; its signature is five uints, so it is 28.
Upstream answered with `message too short, object (15), message modifiers(uuuuu)`
and dropped the connection.  That strictness is the whole value of running a
real client library: a hand-written test client had happily ignored the
malformed event for two milestones.

#### Stage 8 — the keymap, generated from d-os's own layout

`wl_keyboard.key` carries a raw keycode and nothing else.  A toolkit turns that
into a character by feeding it to xkbcommon **together with the keymap the
compositor handed it** — so without one a client receives keystrokes it cannot
interpret at all.

The keymap is **generated from the live d-os layout** (`keymap_active()`), not
embedded as a fixed file: d-os already has layouts (us + hu, selected by
`keyboard.layout`, shown in the taskbar), and shipping a second copy would mean
two sources of truth that disagree the moment someone switches layout.

It travels the way the protocol requires — as a **descriptor**, not bytes:
`wl_keymap_make()` builds the text into a memfd and `wl_keyboard.keymap(XKB_V1,
fd, size)` passes it over SCM_RIGHTS, which is precisely why the shm and
fd-passing work had to come first.  The size includes the terminating NUL, which
xkbcommon requires.  Keycodes are d-os scancodes + 8 (the compositor defines the
keymap it hands out, so the two only have to agree with each other — one table
instead of an evdev translation layer).  The text is self-contained: no
`include` directives, since those make xkbcommon look for files d-os does not
have.

**Verified with a real xkb compiler, not by eyeballing the string.**  `waykeymap`
dumps the keymap; fed to `xkbcli compile-keymap --from-xkb` it produces 297
lines of compiled keymap with **zero diagnostics**, where a deliberately broken
input produces a syntax error and no output.  (Note the exit codes are inverted
in xkbcli 1.4.0 — success exits 1 — so the output/stderr are the signal, which a
control run against a known-bad keymap establishes.)  The mapping is consistent
end to end:

```
d-os scancode 4  →  wl_keyboard.key 4  →  xkb keycode 12  →  keysym a / A
```

#### Stage 9 — real musl pthreads (`clone`)

Every toolkit, and Mesa itself, is built on POSIX threads — so "threads work"
has to mean *musl's* threads, not d-os's own `thread_create`.  `clone` was the
last hard `-ENOSYS` in the Linux ABI and it blocked all of them.

`proc_clone_thread()` is `proc_fork` with the one difference that matters: the
child **shares** the address space rather than getting a copy, resumes on a
caller-supplied stack, and installs a caller-supplied thread pointer.  Linux's
clone resumes the child at the *same instruction* with the return value 0 —
musl's `__clone` relies on exactly that, having pre-laid the start function and
its argument on the new stack — which is why this reuses the fork register
snapshot instead of taking an entry point like the native `proc_clone`.

**`pthread_join` needs kernel help.**  musl parks on a futex at the child's tid
address and the contract is that the *kernel* zeroes it and wakes the waiters
when the thread dies (`CLONE_CHILD_CLEARTID`).  `task_exit_code` now does that
before marking the task DEAD, while the shared address space is still current.
Without it a program prints all its output and then hangs in join — which is
exactly what "it works but never exits" looks like.

**Two i386-only traps.**  The argument order is `(flags, stack, ptid, TLS,
ctid)` — TLS *before* ctid, the opposite of amd64 — so getting it backwards
hands the kernel a thread pointer where it expects a futex address.  And i386
TLS is a per-CPU GDT descriptor: the thread must be pinned to the CPU whose
descriptor is programmed **and** entered with the TLS selector in `%gs`, which
is not part of the register snapshot.  Missing that, musl's first thread-pointer
read faults at `%gs:0x10`.  (musl also passes a `struct user_desc*` here, not a
raw base — the base has to be read out of it.)

Verified on both arches with `pthreadtest`, a real `pthread_create` /
`pthread_mutex` / `pthread_join` program: 4 threads, 20000 locked increments,
all four joined, counter exact.

#### Where the EGL/GL half stands (probed, not guessed)

The milestone's remaining DoD item is *"an EGL+GLES2 program clears + draws a
triangle via a software rasteriser, presented through a Wayland buffer"*.  Rather
than estimate it, the Mesa build was actually attempted so the next session
starts from a measured position:

- The build image now carries Mesa's toolchain (meson, ninja, python3-mako,
  libexpat, pkg-config) and `third_party/mesa-cross.txt` is a working meson
  cross file for the musl x86_64 toolchain.
- `meson setup` with `-Dgallium-drivers=swrast -Dllvm=disabled -Dgbm=disabled
  -Dplatforms=wayland` gets **most of the way through configuration** — it
  builds the bundled expat subproject and resolves everything else — and then
  stops at `Run-time dependency libdrm found: NO`.  Note the driver is spelled
  `swrast`, not `softpipe`, in Mesa 23's option list.

So the next steps are concrete: cross-build **libdrm** (small, also meson), give
meson a cross `pkg-config`, and then face the runtime questions — Mesa `dlopen`s
its DRI module, and its EGL Wayland platform has to be pushed onto the
`wl_shm`-based swrast path rather than looking for `/dev/dri`.

**That deferral is now closed — see §4.40.1 below: EGL + GLES2 run.**

---

### 4.40.1 EGL + GLES2 on Mesa softpipe (M40, DoD complete)

The milestone's remaining half: *an EGL/GLES2 program renders through a software
rasteriser and is presented as a Wayland buffer.*  It runs — `egltri win` opens a
desktop window holding a **spinning orange triangle** drawn by GLES2 shaders,
`GL_RENDERER` = `softpipe`, `GL_VERSION` = `OpenGL ES 3.1 Mesa 23.1.9`.

The stack, bottom-up, all of it upstream and unmodified:

    d-os Wayland server (§M26)
    libwayland-client (upstream, §4.40)
    libwayland-egl            — wl_surface → EGLNativeWindow
    Mesa EGL + GLES2, gallium swrast/softpipe
    musl pthreads (§4.39) + dynamic linking (§M37) + shm/SCM_RIGHTS (§M26)

Mesa is cross-built for musl against **our** wayland headers and libdrm
(`third_party/mesa-cross.txt` + a cross `pkg-config` shim); `make mesa` drives
it, and the resulting `libEGL`/`libGLESv2`/`libglapi`/`libexpat`/`libdrm`/
`swrast_dri.so` are embedded as blobs that `pkg.c` lays into `/lib` at boot.
`egltri` is the first client linked DYNAMICALLY on purpose: libEGL is a shared
object that `dlopen()`s the DRI driver, so there is no static option.

Two bugs stood between "it builds" and "it draws", and both are worth keeping:

**1. Two copies of libwayland in one process.**  libEGL statically absorbed
`libwayland-client.a`, so the application and Mesa each had their own copy of the
protocol object tables.  A proxy created by one was unrecognisable to the other,
and `wl_connection_demarshal` crashed on the first event.  Fixed by building
libwayland as a real **shared** object and having Mesa link against it — which
also required switching the core protocol from `wayland-scanner private-code` to
`public-code`, because `private-code` hides the very interface symbols another
library must share.  *Lesson: a static library is fine until two consumers in one
address space must agree on its state — then it must be shared, and its symbols
must be visible.*

**2. `mincore()` was stubbed as "succeed and ignore".**  It had been grouped with
`madvise` as advisory.  It is not: its RETURN VALUE is the answer.  Mesa's
`_eglPointerIsDereferencable()` asks `mincore` whether an address is mapped, in
order to tell a version-3 `wl_egl_window` (whose first word is the literal 3)
from an ancient one (whose first word is a `wl_surface *`).  Answering "yes,
mapped" for every address made Mesa dereference **address 3** — a null fault deep
inside `wl_proxy_create_wrapper`, three libraries away from the cause.  Now
implemented truthfully on both arches: `EINVAL` if unaligned, `ENOMEM` if the
range holds unmapped pages, otherwise the residency vector.
*Lesson: stubbing a syscall as "succeed and ignore" is only safe when the CALLER
ignores the result too.  A **query** must answer truthfully or fail — a
convenient lie propagates into a fault with no trace back to the lie.*

Diagnosing #2 needed the fault address turned into a name.  The route that
worked, and is worth reusing: log every file-backed `mmap` the loader makes, find
which mapping contains the faulting PC, subtract its base, and `nm` that offset
in the *unstripped* library.  It named `wl_proxy_create_wrapper` in one pass
after two rounds of guessing had failed.

Scope note: this is x86_64 only, matching the milestone's stated target (i386 is
out of scope for the heavy ports).

---

### 4.41 The ring-3 pointer gate was never armed for the Linux ABI (M47.2)

Found while chasing the Wayland descriptor above, and more serious than the bug
that led to it.

§M46 closed the ring-3 pointer boundary in three layers, the first being a
**per-syscall gate**: `task->in_user_syscall` is set for the duration of a
syscall entered from ring 3, and the `sys_*` wrappers validate their pointer
arguments only while it is set — which is what lets the same handlers serve
in-kernel callers that legitimately pass kernel buffers.

The native dispatchers (`hal/x86/syscall.c`, `hal/x86_64/syscall.c`) set it.
**`linux_syscall_dispatch` never did, on either arch.**  So for every program
running under the Linux personality — every musl coreutil, the shell, the TLS
stack, NetSurf, the Wayland clients, i.e. essentially the entire userland — the
gated wrappers fell through to their ungated `_k` cores and ring-3 pointers were
dereferenced without validation.  Layer 1 was off for the code that needed it
most, and nothing failed visibly, which is exactly why it went unnoticed for two
milestones.

Both dispatchers now arm it around the body, like the native path.  The
discipline that follows is the one already established: anything in
`linux_abi.c` that legitimately hands a KERNEL buffer to a `sys_*` must call the
`_k` core (`linux_sendmsg`'s gather buffer) or the `_u` split
(`sys_recvfrom_u`, and the new `sys_recv_u` for recvmsg's payload-to-ring-3 /
descriptor-to-kernel shape).

Verified with the gate armed, both arches: `musltest`, `pkgrun sh -c`, HTTPS
with CA verification, `tcc` compiling and running a program, `wayupstream`, and
NetSurf launching and closing cleanly with no crash record.

---

### 4.42 The memory ceiling, discovered rather than compiled in (M48)

The physical-memory ceiling used to be `BUDDY_MAX_FRAMES`, a per-arch `#define`
sizing a static `page_state[]` in `.bss`.  That construction cannot scale:
metadata is one byte per frame, so supporting 128 GiB means 33 MiB of `.bss` on
every machine — including the 256 MiB one that has to load it before it can run.
"Supports big machines" and "boots on small ones" were in direct opposition for
as long as the number was baked in.

`pmm_init` now reads the firmware memory map, asks the HAL how far memory
reaches, and sizes its metadata to the RAM *this* machine has, out of a boot
arena reserved before any free list exists (`pmm_bootmem_alloc`).  kmalloc's
big-alloc side table and the COW refcount table follow the same rule.  One
kernel image boots on 128 MiB and on 128 GiB and pays for neither on the other.

Physical addresses widened to arch width (`pmm_phys_t`): the API returned
`uint32_t`, which is a 4 GiB ceiling no amount of metadata would have lifted.

**Seeding** no longer releases every usable frame individually at order 0 and
lets coalescing rebuild the higher orders.  Correct at 65k frames, ruinous at
33.8 million: it turned a 128 GiB boot into minutes of bookkeeping to
reconstruct a structure we can emit directly.  It now covers each run of usable
frames with the largest *aligned* power-of-two blocks that fit — the shape
coalescing would have converged on, without the intermediate work.

**`ZONE_DMA32`** is new and not cosmetic.  While the ceiling was 4 GiB, "any
frame" and "a frame a 32-bit device can reach" were the same thing, so no driver
had to say which it meant.  Past 4 GiB they diverge, and a device handed an
address it can only store 32 bits of does not fail loudly — it DMAs into
whatever the truncation lands on.  Device buffers now name DMA32 explicitly
(`pmm_alloc_frame_dma32`); 64-bit DMA is an opt-in a driver has to earn.

#### The kernel direct map, and why user space could not move instead

Raising the ceiling exposed a defect far larger than the one being fixed:
**x86_64 userland was broken on any machine with more than 1 GiB of RAM.**

The identity map grew by adding 1 GiB pages at virtual == physical.  User space
starts at `vmm_user_base()` = 1 GiB, so the first extension landed exactly on
top of it; `walk_to_pt_root` refuses to build a mapping under a large page, and
every `exec` returned `ELF_ENOMEM`.  The port only ever looked healthy because
it was always tested with `-m 1024M`.

User programs cannot move out of the way: they are compiled with the small code
model, which requires every symbol below 2 GiB.  So the *kernel's* physical
window moved instead — all of physical memory is mapped once in the canonical
upper half at `KERNEL_DIRECT_MAP_BASE` (PML4[256]), reached through
`phys_to_virt`, and low addresses belong entirely to user space whatever the RAM
size.  `phys_to_virt`/`virt_to_phys` are portable and compile to nothing where
the base is 0, so i386 and aarch64 are unaffected.

ACPI needed the same treatment.  It identity-mapped its tables on the stated
reasoning that they sit "far below the user base" — true on i386, false on
x86_64, where firmware puts them at the top of low RAM (~2 GiB on a 2 GiB
machine, inside the user region).  Every process inherited that mapping and
`fork()` came back reading ACPI memory.  It escaped notice because with much
more RAM the tables sit above the user region again.

#### i386: the ceiling was self-imposed, then it is real

The i386 identity map stopped at 256 MiB "because we have no pressure for more",
while `vmm_user_base()` is 1 GiB — everything below that is the kernel's to map
and nothing can collide with it.  Three quarters of the reachable window sat
unused, which is why a 512 MiB box managed 234 MiB and why Mesa ran out of
memory on a machine that had plenty.  The map now runs to 1 GiB (256 PSE PDEs,
a quarter of the page directory; user mappings start at PDE 256): 473 MiB usable
on a 512 MiB box.

Past that the limit is real.  32-bit paging holds 32-bit physical addresses —
4 GiB, full stop — and reaching it needs kmap, since 4 GiB of virtual space
cannot linearly map 4 GiB of RAM alongside user space.  **64 GiB on i386 is
exactly the PAE maximum, and PAE is a different page-table format, not a bigger
constant.**

#### Four latent bugs this surfaced, all silent

- **slab's `page_of`** masked with `~(4096u - 1)`, a 32-bit value that
  zero-extends and erases every bit above 4 GiB, pointing the lookup at an
  unrelated low page.  `kfree` then rejected valid objects.
- **The COW refcount table** covered only the first 1 GiB, documented as costing
  "a little memory, never correctness".  True for the fault path, false for
  clone/free: `fork` shares an untracked page in both spaces while
  `free_subtree`, finding no refcount, releases it from each.  A double free —
  the buddy handed one frame to two owners and the second's page table
  overwrote the first's.  The i386 twin had the same shape and was safe only
  because its window happened to equal its identity cap; both are now sized from
  `pmm_nr_frames`.
- **`send()`/`recv()` on a connected TCP socket** do not work in ring 3: the
  Linux-ABI layer wires `connect`/`read`/`write`, and the send/recv entry points
  serve the datagram paths (see §4.43).
- **i386 ring 3 could not execute SSE.**  `CR4.OSFXSR` was deliberately left
  clear, documented in `fpu.c` as "ring-3 code here is x87-only".  That held
  only while every user binary came from our own toolchain; a ported library
  does not ask, and Mesa's i386 build uses SSE for its math and memory paths.
  Enabling it needed no change to `fpu.c` — the eager FXSAVE path was written to
  cover the XMM half the moment OSFXSR is set.

**Verified:** x86_64 boots and runs userland at 1G / 2G / 3G / 4G / 8G / 128G
(130 891 MiB managed at 128 GiB, zero faults, zero `kfree` errors); i386
regression-free at 256M / 512M; aarch64 builds.

---

### 4.43 NetSurf: input it can act on, and a fetcher (M48)

Two independent reasons the browser was inert.

**Nothing could be clicked.**  `enum gui_input_type` was `{ KEY, MOTION }`.  A
window that forwards input to a client — NetSurf via dosgui, any Wayland surface
— could learn that the pointer *moved* but never that a button was pressed: a
click reached the compositor, raised the window, and was then delivered as an
indistinguishable motion event.  No link, form field or scrollbar could ever be
activated.  `GUI_INPUT_BUTTON` now carries press and release for both buttons
through `evq` → `aq` → `app_dispatch_event` → the client, in content
coordinates, with the motion pushed immediately before the press so the client's
pointer is where the click happened.

**Typing produced noise.**  `gui_raw_key` CONSUMED every key for a hook-backed
window, so the keymap never ran and only the raw scancode was forwarded.
libnsfb reads its key codes as SDL-shaped values where printable characters ARE
their ASCII, so NetSurf rendered scancodes as characters.  `gui_input` now
carries the cooked character alongside the scancode — both are needed and
neither substitutes for the other, since Wayland builds characters from
scancodes with its own xkb keymap while NetSurf has no keymap at all.  Return
needed mapping too: the keymap yields `'\n'` (10), `NSFB_KEY_RETURN` is 13, so
Enter in the URL bar inserted a character instead of navigating.

**No fetcher.**  The curated source list carried `data:`, `resource:` and
`file:` only, with the curl fetcher compiled out — so typing an address or
clicking a link did nothing, and that was never an input bug: no code in the
binary could speak HTTP.  `user/netsurf/fetch_dos.c` implements NetSurf's
`fetcher_operation_table` over what `wget` already proves on this stack — ring-3
sockets, musl `getaddrinfo`, Mbed TLS — and attaches through `fetcher_init`'s
existing `WITH_CURL` hook by DEFINING `fetch_curl_register`, so the vendored tree
stays untouched.  (`WITH_CURL` is set for `content/fetch.c` alone; globally it
also switches `utils/time.c` to `curl_getdate`, a symbol we do not have.)
Certificates are verified against `/etc/ssl/cert.pem` with
`MBEDTLS_SSL_VERIFY_REQUIRED` and the hostname is checked.

Three transport findings, each of which cost a run to isolate:

- **`send`/`recv` do not work** for a connected TCP socket here; the Linux-ABI
  layer wires `connect`/`read`/`write`.  The fetch connected, appeared to send,
  and received nothing for 30 seconds while `wget` fetched the same URL.
- **Never wait for end-of-stream.**  This transport does not surface the peer's
  FIN as `read() == 0`, so "read until EOF" hangs forever.  The length comes
  from the protocol instead: headers, then `Content-Length`, then stop.
- **The read must stay BLOCKING.**  Network RX is polled from the calling task,
  not from an interrupt, so the blocking read is what drives the NIC; a
  non-blocking one returns `EAGAIN` forever and nothing ever arrives.

Chunked bodies are decoded, because chunk sizes are not content: before that,
example.com rendered with a stray `22f` above the heading and a `0` below it.

**And the guest had no network card.**  `run_qemu.sh` attached none, so the
browser could not open a site however well the fetcher worked.  Every network
test passed its own `-netdev` on the command line — which is exactly how a gap
like this survives: the automated path and the path a person uses were not the
same path.

**Verified by screenshot** on both x86 arches: `http://example.com` and
`https://example.com` render, and `http://info.cern.ch` renders with its
favicon fetched as a subresource.

**The fetcher is non-blocking**, but not the way it looks like it should be.
An incremental `poll` cannot work here: RX is polled from the calling task, so
the blocking read is what drives the NIC — a non-blocking socket returns
`EAGAIN` forever and nothing arrives.  The blocking cannot be removed, only
moved, so each transfer runs on its own pthread and `poll` became a check for
finished work.  The split is strict: the worker touches only sockets, TLS and
its own context; every NetSurf callback happens on the main thread, because
NetSurf's core is not thread-safe.

Getting there required a kernel fix, and the symptom pointed at the wrong
place entirely.  Adding one worker killed the browser immediately after a
SUCCESSFUL fetch, at an address just below a page boundary — which reads like a
stack overflow or a thread-exit problem, and is neither.  `thrdyn` (a new
reproducer: threads in a DYNAMIC musl binary, in three phases) localised it —
a thread that returns at once is fine, a thread that calls `malloc` is not —
and the fix is in §4.45.

---

### 4.44 Mesa/EGL on i386 (M48)

`egltri win` renders the rotating GLES2 triangle in a d-os window on i386 as
well as x86_64 (`softpipe`, `OpenGL ES 3.1 Mesa 23.1.9`).

The Mesa blob block sat inside the Makefile's x86_64 branch, so "Mesa is
x86_64-only" was a property of where ten lines lived rather than of the code —
every path in them is already parameterised by `$(ARCH)`.  Hoisted out and
guarded on the built artifact; `scripts/build-mesa.sh` builds the tree for
either arch, so the second one is a parameter rather than a re-derivation.

Two blockers, both covered in §4.42: ring 3 could not execute SSE, and the
identity map's self-imposed 256 MiB cap starved Mesa's allocations.

The diagnosis is worth keeping because the symptom was so uninformative.
`eglCreateContext` returned `EGL_BAD_ALLOC`, which the DRI layer emits for every
context failure from a missing driver to a real allocation error.  Asking the
allocator directly settled it: a 32 MiB `malloc` failed IN THE CLIENT while the
kernel reported 139 MiB free — d-os commits `mmap` eagerly, one frame per page,
so Mesa's arenas were charged in full against a 234 MiB ceiling.

Diagnostics kept rather than removed: `egltri d` sets `EGL_LOG_LEVEL`/
`MESA_DEBUG` so Mesa narrates its own driver loading, `EGL_CLIENT_APIS` is
printed (it distinguishes a missing GLES frontend from a runtime failure), and
`eglCreateContext`/`eglCreateWindowSurface` are checked separately — testing
both and asking `eglGetError()` once reports `EGL_SUCCESS` whenever the first
failed and the second succeeded, which is exactly what this bring-up hit.

---

### 4.45 The mmap cursor belonged to the address space (M48)

`sys_mmap` bump-allocates user addresses from a cursor that lived on
`struct task`.  A cloned thread therefore started at zero, `sys_mmap_full` reset
it to the region base, and the thread handed out addresses **on top of the
mappings its own process was already using**.

i386's clone path did not even copy the parent's cursor (the x86_64 twin did),
which is why the browser died there first.  Copying is not the fix either: two
threads then bump independent copies toward the same addresses and collide
slightly later.  One address space, one cursor — it now lives in
`struct vmm_space` behind `vmm_space_{,set_}mmap_cursor`, shared by every task
that shares an mm, and `fork` inherits the parent's value because the child
inherits the parent's mappings.

Two traps on the way, both worth remembering.  `vmm_space_create` uses
`kmalloc`, which does not zero, so an uninitialised cursor is handed straight
back to the program as an mmap address — a NULL+8 fault inside ld.so before
anything printed.  And the field lives in a header, so the first rebuild kept
stale objects and booted into nonsense: the no-header-dependencies pitfall
CLAUDE.md documents, hit twice in one session.

Why it hid for so long: `pthreadtest` passes because a static binary's threads
in that test never allocate.  "Threads work" and "threads can allocate" were
different claims, and only the first had ever been tested.

**Verified** on i386 and x86_64: `thrdyn` PASS (all three phases),
`pthreadtest` PASS, `forktest`, `solibtest`; aarch64 builds.

---

### 4.46 Load distribution across CPUs, measured (M49)

§M18.6.1 shipped "per-CPU runqueue + load balancer" and the row has been ✅ ever
since.  Both halves were real; the second was narrower than its own design said.

#### What was actually there

`kernel/core/task.c` balanced **only when a CPU's runqueue ran empty**, on the
way into the idle loop.  That is a work-stealing rule — "am I out of work?" —
not a load-distribution rule — "is the work spread fairly?".  With every queue
holding at least one task, an arbitrarily bad split never corrected itself.

The file's own header comment described a periodic pass running every
`LOAD_BALANCE_INTERVAL_MS`, and named a constant **that did not exist anywhere
in the tree**.  The comment had documented the design, not the code, for two
milestones.

#### Nothing measured it, because nothing ran it

`scripts/run_qemu.sh` passed no `-smp` at all.  The everyday i386/x86_64 run was
uniprocessor, so the load balancer **never executed on the path a person uses**;
every test that ever exercised SMP supplied its own `-smp`.  This is the same
shape as §M48's missing NIC — the measured path and the used path were two
different paths — and it is why the gap survived so long.  The script now
defaults to `-smp 4` (`SMP=1` reproduces uniprocessor).

#### The measurement

New `sched [ms]` command: it samples twice and reports the delta, because
since-boot totals average away exactly what matters.  Per CPU it shows queue
depth, summed load, busy%, context switches and migrations; per task, the
balancer's view (`DEM`) beside what the task actually got (`CPU%`).

Pin five hogs onto CPU0 while CPU1–3 keep one each (`loop 8` + `taskset`), on a
4-CPU i386 guest:

```
CPU  RQ  BUSY%  SWITCH  MIGR
0    6   100      441       0      <- five hogs, 15-20% of a core each
2    2   100      418       0      <- one hog, 66%
```

Identical tasks, a 3.3x difference, stable indefinitely.  `MIGR 0` everywhere:
the balancer genuinely never ran.

**Note what does not show it.**  Every CPU reads 100% busy.  Aggregate
utilisation is blind to this class of problem, which is why `sched` reports
queue depth and per-task share as well — a dashboard that only had busy% would
have declared the machine perfectly balanced.

#### Three fixes

**1. A periodic pass.**  Every 100 ms each CPU also checks whether a peer is
meaningfully busier, and pulls if so.  Two constants carry the policy.
`MIN_DELTA` exists because a migration swings the difference by *twice* the
moved task: at a threshold of 1 a 3-vs-2 split would move a task to make it
2-vs-3 and move it back next window, a permanent ping-pong that costs cache
locality and buys nothing.  `MAX_STEAL` lets one pass correct a large imbalance
(5-vs-1 → 4-vs-2 → 3-vs-3, then the threshold stops it) instead of trickling one
task per window.

**2. Load means demand, not queue length.**  Queue depth scores four hogs and
four sleepers identically.  After fix 1 the depths equalised at 4/3/3/3 and the
hogs *still* ran at 25% versus 49%, because three of those slots were near-idle
tasks.  So each task now carries a `demand`: the share of wall-clock it spends
**runnable** — running *or waiting in a queue* — EWMA-smoothed.  `cpu_ms`
cannot serve here; on a saturated core it measures the competition rather than
the task, and four hogs sharing a CPU each read a modest 25%.  A runqueue's
load is the sum of its tasks' demand, maintained under `rq_lock` and published
for peers to read locklessly, so balancing never needs two runqueue locks.

Demand starts at maximum for a new task deliberately: starting at zero would be
worse than having no metric, because a burst of fresh tasks would all look free
and land on one core before the first window closed.  Guessing high is
self-correcting.

*Which* task moves matters as much as whether one does — migrating a sleeper off
an overloaded CPU changes the queue length and none of the load.  The periodic
caller therefore asks for a task of roughly **half** the imbalance (a move of
exactly half leaves the two CPUs level) and refuses anything larger than the
imbalance itself, so a correction can never overshoot into a mirror image.  The
idle caller asks for the heaviest task available: holding nothing, it cannot
overshoot.

**3. `task_msleep` really sleeps.**  It was a spin-yield loop — `hlt`, yield,
re-read the clock — leaving the task RUNNABLE and queued for the whole "sleep".
That cost twice: a scheduling slot on every round, and the `hlt` **halted the
CPU while other runnable tasks sat on that same runqueue**, so a sleeping cron
job could idle a core with work queued behind it.

It was found through fix 2.  Demand is measured as time spent runnable, so a
task that never leaves the queue measures as wanting a full CPU: `cron`,
`watchdog` and `heartbeat` all reported demand 100, indistinguishable from a
genuine hog.  **A metric is only as honest as the state it observes.**  Sleepers
now leave every runqueue and a tick sweep wakes them; `ps` shows them `SLP`.

That change has a consequence worth recording: while the sleep was a poll, a
`kill` landed within a tick because the task re-tested `task_should_stop()` on
every pass.  A blocked task tests nothing, and both the GUI window teardown and
§M46's kill-tree wait for tasks to reach a kill point — so `task_kill` and
`task_force_kill` now wake a timed sleeper explicitly.  Cheap to do correctly: a
timed sleeper is on no waitq, so there is no queue to unlink it from.

#### 4. Priority

Every task had equal weight, which is fine until the machine is busy: eight
background hogs and the desktop competed on identical terms, and nothing could
say otherwise.  `nice <pid> <-20..19>` now sets a priority, mapped through a
coarse eight-step table to a **weight** — a table rather than a formula so the
ratios are inspectable instead of implied.

Weight means two things.  It is the task's quantum budget per round
(`deficit`, spent one tick at a time by the tick handler, replenished on
rotation), and it scales the task's contribution to runqueue load, so placement
packs cheap tasks more densely than expensive ones — a nice-19 hog wants a full
core just as much as a nice-0 one, but it will not get one, and reserving a core
for work the scheduler has already decided to starve would be wrong.

Weights *below* baseline cannot buy fewer than one quantum, so they buy fewer
**turns** instead: the replenish converts leftover debt into `skips_left`, which
the pick honours by passing the task over.  The pick therefore runs two passes —
the second ignores skips, so a runqueue holding nothing but niced-down tasks
still runs instead of falling through to idle.

Four hogs pinned to one CPU, one at nice -10 and one at nice +10:

```
PID  NI  CPU%
16  -10   65      weight 350
18    0   16      weight 100
17    0   13      weight 100
19  +10    4      weight  14
```

The measured shares track the weights (350:100:100:14 predicts 62:18:18:2).

At the default nice 0 every one of these paths degenerates to the previous
behaviour — one quantum per tick, never skipped — so the feature costs nothing
when unused.

**Lesson learned.**  The first boot with weights took a **divide error before
the scheduler had run once**.  `struct task` is constructed in FOUR places and
only one is `spawn_common`: pid 0, the BSP idle task and each AP's idle task are
synthesised by hand, so they kept kcalloc's zero weight, and a zero weight is a
division by zero in the replenish.  Fixed with one `task_sched_defaults()` that
every site calls, plus a guard at the division itself — the scheduler tick is
the last place in a kernel that can afford to fault.  Adding a field to a struct
whose construction is spread over four functions is a change to four functions.

#### 5. The console read blocks too — and an idle machine goes quiet

`vc_getchar` was the same `hlt` + `task_yield` poll as the old `task_msleep`, so
a shell waiting at its prompt stayed RUNNABLE forever: one runqueue slot per
open shell, a core halted on every turn through the queue, and — once demand
was measured as time spent runnable — a reading of 100, identical to a CPU hog.
`init`'s reaper loop had the same shape, under a comment claiming it "cost
nothing when the system is quiet".

Both now block.  Readers park on a per-VC `waitq` that `vc_kbd_push` wakes from
the keyboard IRQ; the ring write stays outside the lock (still single-producer,
single-consumer) but the **wake** takes it, which is what closes the
lost-wakeup window — a reader tests the ring and parks while holding that same
lock, so the wake can never land between its test and its park.

The kill contract needed the other half.  `task.h` and the GUI window teardown
both rely on a killed shell dying "at its next `vc_getchar` yield", and a
blocked task polls nothing — so `task_kill` now wakes a task parked on a waitq
as well as a timed sleeper.  That required a `task->wq` back-pointer: `wq_next`
alone says "somewhere in some queue", which is not enough to get a task out
again.  The wake is deliberately spurious — the condition is still false — and
that is safe precisely because waitq's contract already makes callers loop.

Measured on an otherwise idle 4-CPU i386 guest, before and after: **one core at
100% → all four at 0-2%.**

#### 6. An SMP race the timing change exposed

The first boot with a blocking console printed `shell_task_entry: no VC bound —
exiting`.  The boot shell was spawned and its VC bound immediately afterwards,
inside `preempt_disable()` — but **that counter has been per-CPU since
§M18.6.2**, while `task_enqueue` places a new task on the least-loaded core and
IPIs it.  The new task could therefore reach its entry point on another CPU
before the binding landed.

Pre-existing, latent, and invisible while the everyday run was uniprocessor.
Four call sites had it (boot shell, `pane split`, GUI window shells, the
aarch64 boot shell), all with a comment explaining why `preempt_disable` made
them safe.

`start_arg` already had the right treatment — set inside `spawn_common`, before
the task can be enqueued, with a comment saying exactly why.  The console just
never got the same.  New `task_spawn_console()` threads it through the same
path, and all four sites use it.

#### 7. A deferred-work pool

New `kernel/core/workqueue.c`: `work_submit()` from a restricted context
(interrupt handler, spinlock-held region), callback runs later on an ordinary
task that may block, allocate or draw.  One worker per CPU, parented to init.

It is built on the scheduler rather than beside it, which is what makes it
worth having here: the workers are plain tasks, so several items submitted at
once genuinely run on several cores and §M49's balancer spreads them without
the workqueue knowing anything about CPUs.  And they block on a waitq, so an
idle pool costs nothing — a property that only became true once §M49 fixed the
poll loops above.

The pending list and the workers' waitq share **one** lock: waitq's contract
already says its lock serialises the guarded condition, the condition here *is*
the list, and sharing closes the lost-wakeup window for free.  Submission wakes
exactly one worker — waking the pool for a single item would have every worker
take the lock, find nothing and park again.  `work_flush` waits on an
`inflight` count that is decremented when a callback **returns**, not when its
item is dequeued; counting the queue alone would let a flush return while a
callback was still touching the submitter's data, which is the bug flush exists
to prevent.

Deliberately absent: submission from NMI.  `work_submit` takes a spinlock, and
an NMI interrupting a CPU that holds it deadlocks.  §M47's crash capture is
exactly such a caller and keeps its own lock-free ring; an NMI-safe path needs
Linux's `irq_work` shape (per-CPU list + self-IPI), a different mechanism
rather than a flag on this one.

`wqtest [n]` measures it: 16 items across 4 CPUs in **26 ms against ~80 ms
serial**, four items on four cores in 5 ms against 20.

**Lesson learned.**  `wqtest` first reported FAIL on duplicate runs — and the
test was wrong, not the queue.  Re-submitting an item that is still *waiting*
collapses into one run; an item a worker has already picked up is legitimately
queued again, which is precisely how a driver says "more arrived while you were
draining".  The assertion contradicted the contract written in the same commit.
It now checks what is actually guaranteed: every item ran, nothing pending
after the flush, and the completion counter agreeing with the runs observed.

#### 8. Its first production consumer: the xHCI event ring

`xhci_poll()` is called from the **timer IRQ** (x86 `pit.c`, aarch64
`timer.c`) every 10 ms, and it used to drain the whole Event Ring right
there: MMIO reads across the ring, HID report decoding, and — since §M49 made
the console read blocking — a `vc_kbd_push` that takes a waitq lock, makes a
task RUNNABLE and may IPI another core.  That is a great deal of work to do
with interrupts off, on every tick, with every other interrupt on the machine
queued behind it.

The ISR now only submits; the drain runs on a worker.  One change in `xhci.c`
covers both arches, because both timers call the same function.

It also **fixed a latent bug**.  `evt_drain` is not reentrant and has always
had a second caller: `cmd_submit_wait`, which drains the ring in task context
during enumeration.  The timer ISR could interrupt it mid-drain on the same
CPU — corrupting the dequeue cursor and, worse, `evt_drain(NULL)` from the ISR
**swallows the command completion the enumerating task is waiting for**,
turning a successful command into a 200 ms timeout.  Rare because enumeration
is short, and real.  A new `spin_trylock` gives the deferred drain the right
semantics (if someone else is draining, a second drainer has nothing useful to
do and waiting would only tie up a worker), while the enumeration path holds
the lock across its whole bounded wait so it can never lose its event.

**Verified** with `-device qemu-xhci -device usb-kbd`: enumeration completes,
and the guest prints `usb-hid: first key delivered over USB` — a one-shot
marker added for exactly this, because a PC target always has a PS/2
controller too and "typing still works" is not evidence that the USB path
works.  `wqtest` reports the drain alongside its own items: `+1 completions
from other submitters`.

NIC RX remains the next consumer, and it is a bigger job than it looks: the
stack is single-task by construction (`net.c` says so — "everything therefore
runs in one task context → no locking"), every blocking helper spins calling
`dev->poll`, and moving RX to a worker means locking the stack and converting
those spins to waits.  That is §M24's documented follow-up, not a loose end of
this one.

#### Result

Same experiment, after:

```
CPU  RQ  LOAD  BUSY%  MIGR
0    3   299   100      0
1    3   300   100      0
2    2   200   100      0
3    2   200   100      0
```

Queue-depth spread 2..6 → 2..3.  The residual is arithmetic, not a defect: nine
tasks do not divide evenly across four CPUs, and the balancer correctly stops
rather than ping-ponging.  On x86_64 with eight hogs, seven of the eight settle
at 49-50% of a core — the ideal split.

**Verified** on i386 and x86_64 at `-smp 4`: the imbalance experiment above,
plus `forktest`, `pthreadtest` (20000/20000), `solibtest`, prompt reaping after
`loopstop`, and services still firing on schedule; aarch64 builds.

#### Still open

`keyboard_getchar` (`/dev/keyboard`) and the GUI compositor / app-host loops are
still `hlt`+`yield` polls.  The keyboard one has no in-kernel caller worth the
change; the GUI loops do real work per iteration and converting them means
giving the compositor's event queues waitqs — worth doing, but a compositor
change rather than a scheduler one, and not something to fold into the same
work that rewrote the blocking primitives.

The workqueue's first consumer is the xHCI drain; NIC RX is next and needs
`net.c` locked first (see above).

Priority is per-task only.  There is no group or per-user fairness (a process
that spawns ten threads gets ten shares), which is fine until §M32 makes users
real.

### 4.47 AArch64 grows a POSIX process model (A1)

M21 declared "full x86 parity" in 2026-07-10 and it was true then; §M34's
`fork`/signals landed on x86 afterwards and were never carried across.
`PLAN_AARCH64.md` scoped the catch-up as "mirror `hal/x86_64/fork.c`".
Measured, that file was about a quarter of it.

#### What was actually missing

`struct user_regs` had no aarch64 branch, so the port silently used the **i386**
definition — `eax`, `ebx`, `esi` at 64-bit width.  There was no
`enter_user_mode_regs` to resume EL0 from a full register set, and no decode of
a data abort into a copy-on-write resolution.  The absence of `fork.c` was the
symptom the plan noticed; these three were the work.

#### Copy-on-write

`vmm_space_clone` walks the parent's user region (L1[4..511] → L2 → L3, since
`vmm_user_base()` is 4 GiB), marks every leaf read-only in **both** spaces and
tags it `PTE_SW_COW` — bit 56, software-reserved in a stage-1 descriptor, next
to the existing `PTE_SW_SHARED`.  Read-only in both is the part that is easy to
get wrong: leave the parent writable and it silently edits the child's memory.

Borrowed pages (`PTE_SW_SHARED`, i.e. memfd and shared mappings) stay shared and
writable — privatising one on first write would hand the child a copy nobody
else can see, which is the opposite of what the mapping is for.

The refcount table is sized from `pmm_nr_frames`, per §M48: a fixed window
leaves any frame above it untracked, and an untracked shared frame is a double
free the moment both address spaces exit.  Teardown releases COW pages through
the count instead of freeing them outright.

A 2 MiB block leaf in the user region would be shared writably by a plain
descriptor copy.  Nothing creates one today (`aarch64_vmm_map_user` is 4 KiB
granular), so the clone **refuses loudly** rather than guessing — if that ever
changes, the failure is a message, not silent shared memory.

#### Three things the x86 code cannot be transliterated into

**SP_EL0 is not in the trapframe.**  Taking an exception from EL0 switches the
CPU to SP_EL1 and leaves SP_EL0 banked, so `vectors.S` never had a reason to
save it.  A forked child resuming in its own address space needs it, and so does
signal delivery; both read and write it directly with `mrs`/`msr`.

**The return address is a register.**  x86 pushes the signal trampoline so the
handler's `ret` consumes it.  AArch64's `ret` branches to x30, so the trampoline
goes in x30 and nothing is pushed — which is also why, on entry to
`SYS_SIGRETURN`, the user SP points *exactly* at the saved context, with no
return address above it to skip.

**COW is checked before the uaccess fixup.**  When the kernel writes to a user
page on a task's behalf (a `uaccess` copy into a freshly forked child's buffer),
the right answer is to resolve the copy-on-write and continue — not to unwind
the copy with `-EFAULT`.  Reversing the two makes fork-then-write-through-a-
syscall fail in a way that looks like a bad pointer.

One smaller thing: `enter_user_mode_regs` restores the register holding the
frame pointer **last, through itself** (`ldr x9, [x9, #72]`).  Restore it any
earlier and every remaining load reads from whatever the child's value happened
to be.

#### Where the proof runs

The ARM serial console is `serial_shell.c`, a small REPL of its own — the full
`shell.c` only comes up on a VC behind virtio-input, which a headless boot has
no way to drive.  That is why `PLAN_AARCH64` scopes A1's definition of done to
the serial shell, and why the three self-tests were added there.

**Verified** on `qemu-system-aarch64 -M virt`: `forktest` reports
`parent fork()=18, waitpid=18 status=7, secret still=111` — the child's write to
`secret` did not reach the parent, so the COW isolation is real and not merely
asserted; `pipetest` and `sigtest` pass (`handler ran in ring 3, caught signal
10`, then sigreturn restores the interrupted context).  i386 and x86_64 rebuild
unchanged.

Also fixed on the way: the teardown path cast physical addresses to `uint32_t`
before freeing them, which frees a *different* frame on any machine with RAM
above 4 GiB.  Pre-existing, and exactly the §M48 shape.

#### Still open on this arch

`proc_clone_thread` (A4 — TLS is `TPIDR_EL0`, much simpler than i386's per-CPU
GDT descriptor), and the whole of A2: without the Linux-ABI personality no musl
binary runs, which is why the port still embeds three in-tree-libc programs
where x86 embeds sixty.

### 4.48 One translation engine instead of one per architecture (M50)

`hal/x86/linux_abi.c` and `hal/x86_64/linux_abi.c` are 2275 lines and ~160
`case` labels between them, and they are two copies of one idea.  The aarch64
port (PLAN_AARCH64 A2) would have been a third.  Linux numbers its syscalls
differently on every architecture — `read` is 3 on i386, 0 on amd64, 63 on
arm64 — but it is the same `read`.  The difference between the ports is
numbering, and numbering is data.

#### The pipeline

```
guest trap ──► arch shim ──► number map ──► canonical op ──► handler
  (regs)      (frame→args)  (per guest ABI)  (arch-neutral)   (shared)
```

Each stage is replaceable alone.  A new **architecture** is a shim — six lines
saying which registers hold the number, the arguments and the result.  A new
**guest ABI** is a table.  A new **syscall** is one handler, and every
architecture and every guest that names it gets it at once.

`kernel/includes/abi.h` is the contract, `kernel/core/abi_engine.c` the
handlers, `kernel/core/abi_linux.c` the three Linux number spaces as data.

#### Two decisions worth keeping

**The vocabulary is named after meanings, not after Linux.**  `ABI_SEEK`, not
`ABI_LSEEK`.  Tying the interlingua to one system's spelling would quietly make
that system the only guest that ever fits — and the point of an interlingua is
the guests that are not written yet.

**The engine may decline.**  `abi_dispatch` returns "not handled" for a number
the map does not name, and the hand-written switch stays behind it as the
fallback.  That is what makes migration incremental: 2275 lines move one
operation at a time, with the old path beside the new one for comparison,
instead of in a single unverifiable jump.  A handler that does not exist yet is
a normal state, not an error.

#### Where the truth is recorded rather than dressed up

There is no `sys_munmap` — user `mmap` is a bump allocator that does not
reclaim, so unmapping succeeds and leaks a bounded amount.  Both x86 layers
have always done this.  The canonical handler says so in one place, so the gap
stays visible instead of being rediscovered per architecture.

#### Proof

Both x86 architectures now route `read`, `write`, `close`, `seek`, `mprotect`,
`munmap`, `getpid` and `getppid` through the shared engine, and their existing
musl userland is unchanged by it: `musltest` (an unmodified **static** musl
binary), `solibtest` (a **dynamic** one, so ld.so's own `read`/`seek`/`mprotect`
traffic goes through the engine) and `crypttest` (Mbed TLS) all pass on i386
and x86_64.  aarch64 builds; its shim is A2.

`abi` prints the tables — one row per meaning, one column per platform:

```
MEANING     linux/i386   linux/amd64   linux/arm64
op 1         3            0            63            (read)
op 2         4            1            64            (write)
op 3         6            3            57            (close)
op 4         19           8            62            (seek)
op 7         20           39           172           (getpid)
```

The arm64 column is filled in although no aarch64 shim consumes it yet —
deliberately, because writing the numbering beside its siblings is where it is
easiest to get right, and it makes "a new arch is a table" checkable rather
than aspirational.

#### On reaching further than Linux

Whether this can host a Windows guest is analysed in PLAN.md §M50.  The short
version: the pipeline generalises but the **cut point** does not.  Linux's
syscall numbers are a documented contract, which is why a number map works;
Windows' NT syscall numbers are an internal detail that changes between builds,
which is why Wine cuts at the DLL boundary instead and why WSL1's kernel-level
approach was eventually replaced by a real kernel.  The canonical vocabulary is
the shared asset either way — but the hard part is never the numbers, it is the
semantics: HANDLEs versus file descriptors, `CreateProcess` versus fork/exec,
SEH versus signals, reserve/commit versus `mmap`.

### 4.49 AArch64 runs unmodified musl (A2), and the engine proves itself

`hal/aarch64/linux_abi.c` is **~80 lines**.  Its siblings are 1211
(`hal/x86/linux_abi.c`) and 1064 (`hal/x86_64/linux_abi.c`).  The difference is
not capability — the ARM port runs the same unmodified static musl binary the
x86 ports do — it is that §M50 moved the translation into a shared engine
before this port was written.

What remains in the file is the only architecture-specific thing about a
syscall translation: **x8 = number, x0..x5 = arguments, result in x0.**
Everything else is `kernel/core/abi_linux.c` (the arm64 numbering, as data) and
`abi_engine.c` (handlers shared with both x86 arches).  The §M47.2 pointer gate
is armed from the first line, since that is precisely the thing both x86 layers
left unset for two milestones without anything failing visibly.

**`musltest` passes with ZERO unhandled syscalls.**  The vocabulary grown for
x86 was already sufficient for an ARM musl startup — the best available evidence
that the engine's split between "what a number means" and "what the meaning
does" falls in the right place.

#### The trap that A6 predicted, hit four stages early

The first attempt faulted at EL0 with `FAR_EL1 = 0`, which reads exactly like a
null dereference and is nothing of the kind.  Disassembling the faulting address
gave the answer in one step:

```
00000001000021a0 <memset>:
   1000021a0:   4e010c20   dup  v0.16b, w1
```

musl's `memset` opens with NEON, so libc startup trapped on its first string
operation — an FP/SIMD access trap, not a bad pointer.  Reasoning from `FAR = 0`
would have cost an evening; **when a fault address looks impossible, disassemble
the faulting instruction before theorising about the address.**

`kernel/hal/aarch64/fpu.c` had described this failure, and both halves of its
fix, before it happened: enable `CPACR_EL1.FPEN` **and** save/restore the vector
registers, with an explicit warning that doing the first without the second is
worse than neither, because FP would start working and silently corrupt across
task switches.  Both are now implemented:

- `hal_fpu_enable_this_cpu()` sets `CPACR_EL1.FPEN = 0b11`, called from **both**
  the BSP and the AP bring-up paths.  It is a per-CPU system register: enable it
  on one core only and FP works there and traps on the other — the nastiest
  possible shape of this bug.
- `hal_fpu_save`/`restore` move Q0..Q31 + FPCR + FPSR (528 bytes, aligned inside
  the oversized blob the core hands over).

Two small encoding notes: `stp` of 64-bit registers has a scaled 7-bit immediate
topping out at 504, so the control words at offset 512 need plain `str`; and a
zeroed image is a valid starting state here, unlike x86's FXSAVE, where an
all-zero MXCSR unmasks every SIMD exception and faults on the first instruction.

This also unblocks A5/A6 early — the FP unit is what every ported library needs.

#### Verified

On `qemu-system-aarch64 -M virt`: `musltest` prints `hello from REAL musl on
d-os (unmodified, static libc)` and returns 0, and the A1 self-tests
(`forktest`, `sigtest`, `pipetest`) all still pass with FP live on the
context-switch path.  i386 and x86_64 rebuild, and their `musltest`,
`solibtest` and `crypttest` are unchanged.

#### Provisioning, which the plan had in the wrong order

A2's proof needs a musl for this architecture — the toolchain half of A3 — so
the two stages were not independent.  It was cheap:
`scripts/fetch-musl-cross-prebuilt.sh` was already arch-parametric, so an
aarch64 musl.cc toolchain is a download rather than the ~10 h from-source gcc
build the i386 path once needed.  The `%.muslelf` rule now keys off "does this
arch have a musl cross-sysroot" instead of naming x86_64, and the musl blob rule
uses `$(USER_OBJCOPY)` rather than a bare `objcopy` that does not exist in the
ARM container.

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

### 4.50 AArch64 A3 — a shell that forks and execs, and the register nobody saved

`pkgrun sh -c "echo A; echo B; echo C"` prints `A B C` on ARM: a musl shell
forking musl coreutils out of the content-addressed store, on the third
architecture.  Verified on `-smp 1` and `-smp 4`; `forktest`, `pipetest`,
`sigtest` and `musltest` all still pass.

#### What actually cost the stage

The predicted work — an `aarch64-linux-musl` cross toolchain, arch-parametric
`pkg` recipes — was an afternoon.  The stage was held up instead by **per-task
state kept in a place nothing saves**, twice.

**TPIDR_EL0** (fixed one commit earlier).  A forked musl child faulted at a
small negative address because its thread pointer was zero.  On x86 a task's
thread pointer can only change through the kernel (a GDT descriptor on i386,
the `FS.base` MSR on x86_64), so the scheduler knows about it and restores it
from `task->tls_base`.  AArch64 lets EL0 write `TPIDR_EL0` **directly** —
musl's aarch64 `__set_thread_area` is one `msr`, no syscall — so
`task->has_tls` stays 0 while the register very much holds a live pointer.
musl computes its `struct pthread` as `TP - 0xc8` and stores through it
immediately after the `clone`, so a child starting with TP = 0 dies on its
first instruction back in user mode.

**SP_EL0** — the one that took the day.  A1 had already recorded that SP_EL0 is
banked and absent from the trapframe, and applied that to `fork` and to signal
delivery.  The conclusion was right; the scope was one step too small.  The
kernel writes SP_EL0 exactly once, at the `eret` into EL0, and from then on it
belongs to the user program, which moves it on every call — and
`context_switch` never saved it.  So when task A blocked at EL0 and task B ran
at EL0, B's stack pointer stayed in the register, and A's trapframe restored
its registers, its PC and its PSTATE, then `eret`ed **with B's stack**.  A read
its locals and return addresses from whatever lived at those offsets in its own
stack and returned into nothing.

x86 cannot have this bug: the user SP is a field in the interrupt frame the CPU
itself pushes.  That is why nothing in the port's design review caught it.

The symptom fit the cause exactly once the cause was known: `sh -c "echo one"`
printed `one` and *then* the shell died at a wild PC — because a shell's first
schedule-out at EL0 is its `waitpid`, so the damage always landed after the
child had already run.

**The generalisation, for A4 and for any future architecture:** enumerate the
registers the kernel writes once and the user owns thereafter. Every one of
them belongs in the context switch. On AArch64 that set is `TPIDR_EL0`,
`SP_EL0` and the FP/SIMD file — all three are now saved, and all three were
found the same way, one crash at a time.

#### How it was found — narrowing, not guessing

Five theories, each killed by a measurement instead of an argument:

| Theory | The measurement | Verdict |
|---|---|---|
| COW double-free in the child's `execve` teardown | made `cow_release` never free at all | unchanged → not it |
| Stale TLB across cores (`tlbi vmalle1` is CPU-local) | re-ran on `-smp 1` | **still failed, and deterministically** → not it |
| Trapframe overwritten while the parent blocked | magic word in the frame's pad slot, checked before every `eret` | **never fired** — the frame was intact, which killed every remaining theory about the return path |
| Kernel executing at EL0 | `ESR` added to the fault print: `EC=0x24`, a data abort from a *lower* EL | confirmed EL0 |
| The user stack itself is wrong | dumped 32 words at `SP_EL0` on the fault | kernel frame data at user addresses → **named the answer** |

The canary was the turning point. Proving the frame was *good* is what redirected
the search from the return path to the register the return path never touches.

The fault print now carries `ESR`, `SP_EL0` and the task name permanently. `ESR`
is the difference between "jumped to a bad address" (`EC 0x20/0x21`, `FAR` = the
PC) and "dereferenced one" (`EC 0x24/0x25`, `FAR` = the operand); printing only
`ELR`/`FAR` leaves that ambiguous, and guessing wrong sends the investigation to
the wrong half of the program. `SP_EL0` is there because it is banked out of the
trapframe — nothing else in a dump reveals it.

#### Three pre-existing defects fixed on the way

- **`cow_release` leaked the last reference.**  `if (*rc > 0) { (*rc)--; return; }`
  decrements the final holder's count and returns **without freeing** — every
  page a fork ever shared leaked.  The x86_64 twin has it right (`> 1`); the ARM
  copy did not.
- **`tlbi vmalle1` in the COW paths is CPU-local.**  `vmm_space_clone` makes the
  parent's pages read-only and `vmm_cow_fault` replaces a mapping; a sibling
  core sharing the mm keeps its stale writable entry and writes straight through
  the copy-on-write.  Both now use the inner-shareable broadcast `vmalle1is`.
- **`ABI_WAIT` returned the raw exit code.**  A guest reads the status word
  through `WIFEXITED`/`WEXITSTATUS`, which look for the code in bits 8..15 — so
  the shared handler now returns `(code & 0xFF) << 8`, and validates the guest's
  status pointer before writing it (where the pointer's origin is known, per
  §M46's thrice-learned rule).  Raw-code-in-status happens to work for 0, which
  is exactly why it survived.

#### The build cache was lying about architecture

`scripts/build.sh` parks each arch's `user/*.muslelf` etc. in
`build/.userartifacts/<arch>/` across an ARCH flip.  It filed them by
`build/.last_arch` — a **hint**, and wrong whenever `make` runs directly or a
build is interrupted.  An AArch64 `sh.muslelf` ended up in the x86_64 slot, was
restored, and was linked into the x86_64 kernel, where it surfaced only at
runtime as `pkgrun: 'sh' returned rc=-7` (`ELF_EBADARCH`) — a build accident
wearing a kernel bug's clothes.  An audit found **27 of the cached artifacts
mis-filed**.

Every artifact states its own architecture in its ELF header, so the cache now
reads it: stash, restore and refresh all key on `e_machine`, and an entry that
does not match its slot is re-filed rather than trusted.  **Ask the file, not
the stamp.**

#### Open

- `ls` needs `openat` (56) and `getdents64` (61).  Both belong in the §M50
  engine, and both need something the pipeline does not have yet: a place for
  per-guest FLAG translation (Linux `O_*` → the VFS's).  A number map maps
  numbers; flags are a second table, and adding one is a design step, not an
  entry.
- **`sh -c` with more than one command fails on BOTH x86 arches, and only under
  SMP.**  Narrowed, not root-caused — the state of the hunt is below so the next
  session starts where this one stopped rather than from the symptom.

  **Reproduction.**  `pkgrun sh -c "echo A; echo B"`, i386, `-smp 2`: fails
  roughly two runs in three.  The **same binary at `-smp 1` passes every time**,
  and so does `-smp 2` with a `kprintf` on the syscall path — i.e. it is a race,
  not a logic error.  x86_64 behaves the same way (a #PF then a #GP).  It was
  invisible until now because the x86_64 `sh` blob was the AArch64 binary; i386
  fails too, so §M36's "a multi-command `sh` works" needs **re-establishing, not
  assuming**.

  **What the failure looks like.**  `A` prints, the *parent* (`pid 13 'shell'`,
  running `sh` as an excursion) takes a user fault, and `B` still prints — so the
  second child was forked and ran.  The parent's `esp`/`ebp` at the fault are
  **identical across runs** (`esp=0x45fffc60`, `ebp=0x45fffdb8`, inside
  `run_command`'s frame, its local `argv[]` visible on the stack and correct),
  while the faulting `eip` is **different every time** and always garbage —
  `0x45ffff00`, `0x45ffff59` (both inside its own argv strings), `0xfff`.  So the
  parent reaches a fixed point in its own code and transfers control to a value
  that varies: a corrupted code pointer or return address, not a corrupted stack
  pointer.  That distinguishes it from the AArch64 bug above, which was the stack
  pointer itself.

  **Ruled out by measurement, not by argument:**

  | Hypothesis | How it was killed |
  |---|---|
  | The artifact cache shipping a foreign-arch binary | fixed; i386 was never affected and still fails |
  | `task_reap` freeing a task still linked in a runqueue | a check before `kfree` never fired in any run |
  | Stale cross-CPU TLB after a COW resolution or migration | forced an unconditional `CR3` reload on every switch — **still fails** |
  | `signal_deliver` rewriting a ring-0 frame | it guards on `(cs & 3) == 3`; correct |

  **Still standing, and the thing to look at first:** *there is no TLB shootdown
  IPI anywhere in the tree.*  Every `invlpg` in `hal/x86/vmm.c` and
  `hal/x86_64/vmm.c` is CPU-local, and `lapic_send_ipi` is used only for the
  preempt IPI.  Forcing a full `CR3` reload on every switch closes the
  *migration* window but **not** the window where two tasks share one address
  space and run concurrently on two cores — which is exactly what a `fork` in
  flight looks like before the child's `execve` completes.  Proving or excluding
  that needs a real shootdown, which is a milestone-sized change touching both
  x86 arches.

- An intermittent SMP kernel fault in `load_steal_one` (the §M49 balancer) — a
  `#GP` on x86_64 and a `#PF` at `cr2=0xf0010123` on i386, each taking the
  runqueue lock down with it and hanging the box in `schedule`.  Seen twice in
  roughly a dozen `-smp 2` boots.  Likely the same underlying corruption as the
  item above rather than a separate bug — a walk of the victim's ring is exactly
  what notices a scribbled pointer first — but recorded separately until that is
  shown rather than assumed.

### 4.51 The broadcast x86 does not have (M51 — TLB shootdown)

`pkgrun sh -c "echo A; echo B; ls /store"` now runs to completion on i386 at
`-smp 1`, `-smp 2` and `-smp 4`, with no faults.  Before this it failed roughly
two runs in three at `-smp 2` and passed every time at `-smp 1` — the signature
of a race, and the race was a missing TLB broadcast.

#### What was wrong

x86 does not broadcast TLB invalidation.  `invlpg` and a CR3 reload affect the
CPU that executes them and nothing else, so a page-table edit made on one core
leaves every other core translating the OLD entry.  Every `invlpg` in
`hal/x86/vmm.c` and `hal/x86_64/vmm.c` was CPU-local, and nothing anywhere sent
an invalidation IPI — `lapic_send_ipi` existed but served only the preempt IPI.

Copy-on-write is a page-table edit whose entire safety argument is "the next
write faults": `fork()` marks the parent's pages read-only so the parent's own
next write traps and gets a private copy.  If the parent is also runnable on a
second core — which is exactly the window between `fork` and the child's
`execve` — that core still holds a WRITABLE entry for the pages just protected.
Its next write does not fault.  It goes into the frame the child is now sharing,
and the two processes scribble on each other with no fault and no log.

AArch64 never had this hole: `tlbi ...is` is a hardware broadcast across the
inner-shareable domain, which is why the ARM port's COW works and why
`hal_tlb_shootdown` is empty there.  The difference in *how* is exactly what a
HAL is for, so the entry point lives in `hal_api.h` and portable VMM code says
"tell every CPU" once.

#### The protocol (`kernel/hal/x86/tlb.c`, shared by both x86 arches)

A ticket pair per CPU (`percpu.tlb_req` / `tlb_ack`) and nothing else — no lock,
no shared request slot, no message:

    sender   flush locally, then for every other online CPU take a ticket
             (atomic ++tlb_req), send IPI vector 0x42, and wait for that
             CPU's tlb_ack to reach the ticket.
    target   flush everything, then publish tlb_ack = tlb_req.

Three properties, each load-bearing:

1. **The request carries no address.**  The remote action is always a full
   flush, so overlapping requests from different senders cannot clobber each
   other — which is what removes the need for a lock.  A per-VA `invlpg` would
   be cheaper and would need a request slot, a lock, and an answer to "what if
   two CPUs shoot down at once"; on a path that already costs an IPI round trip
   that trade is not worth making.
2. **`tlb_ack` is published after the flush**, never before.  The counter is
   the promise.
3. **The wait loop services its own slot.**  A shootdown can be issued from a
   page-fault handler, i.e. with interrupts disabled, so a waiting CPU cannot
   take the IPI another waiting CPU is waiting on.  Checking our own ticket in
   the spin loop means a peer's request is honoured whether or not we can
   currently take an interrupt, and the cycle cannot form.

Uniprocessor is free: with one CPU online there is no remote work at all.

#### Only weakening edits pay for it

A remap over a PRESENT entry, an unmap, an mprotect and a COW resolution all
broadcast.  A *fresh* map does not — a CPU with nothing cached will walk the
table and find the new entry.  That distinction is not an optimisation, it is
the difference between working and not: `map_in_pd` runs once per page of every
ELF load and every mmap.

`vmm_space_clone` is the other half of the same lesson.  It rewrites the
parent's entire user space one page at a time, so the first version — which
broadcast per page — turned a fork into thousands of IPI round trips and never
finished.  The clone now suppresses the per-page broadcast (`map_in_pd_ex(...,
notify=0)` on i386, a plain local `invlpg` on x86_64) and issues **one
whole-space shootdown at the end**, which reaches the same end state.

#### Lesson: the harness lied for an hour

Most of the time on this went into a phantom.  After adding fields to
`struct percpu` the shootdown appeared to hang: the target CPU was demonstrably
alive (its tick and switch counters advanced) yet never took vector 0x42, and
its `apic_id` read back as 3 on a two-CPU box whose boot log said 1.

**This project has no header dependencies** — CLAUDE.md says so, and says to run
`make clean ARCH=<arch>` after editing a shared header.  I had not.  Half the
tree was compiled against the old `struct percpu` layout and half against the
new one, so every per-CPU field was read at the wrong offset.  Every measurement
taken in that window was fiction, and the "stuck IPI" never existed: after
`make clean` the very first run passed.

Two things to carry: a documented build convention is a *correctness*
convention, not a style note; and **an impossible measurement — a field holding
a value it cannot hold — is evidence about the build, not about the code.**

#### Open

- **x86_64 still fails at `-smp` ≥ 2, and it is NOT this bug.**  `-smp 1` runs
  `sh -c "echo A; echo B"` cleanly; `-smp 2` takes kernel faults, one of them a
  `#GP` inside `copy_str` while `execve` marshals argv.  Verified independently
  of this change: with the shootdown wiring removed, `-smp 2` fails the same way
  (a `#GP` at a garbage `rip`), so M51 neither caused nor fixed it.  i386 and
  x86_64 share the VMM shape but not the syscall entry path, and the fault being
  in a user-pointer copy points there.
- The shootdown is a full remote flush.  Per-VA `invlpg` on the remote side
  would need the request slot and lock described above; worth doing only if
  measurement shows the flush costs something real.

### 4.52 The note that outlived its premise (M52 — per-CPU SYSCALL entry, x86_64)

x86_64 now runs `pkgrun sh -c "echo A; echo B; ls /store"` cleanly at `-smp 1`,
`-smp 2` and `-smp 4`.  Before this it was reliable only on one CPU; on two it
produced a child executing its own argv strings, kernel `#GP`s at garbage
addresses, and — memorably — the kernel returning to address 3.

#### The bug was documented before it was written

`syscall_entry.s` had said this in its own header since §M20.6.1:

> This uses a GLOBAL scratch pair, so it is UP-correct only — which matches
> x86_64's current single (non-per-CPU) TSS: ring-3 tasks only run on the BSP
> today.  When x86_64 grows a per-CPU TSS (like i386's M35), this must move to
> a swapgs + %gs:per-cpu-slot scheme.  **Noted, not built.**

Every word was true when written.  §M35 then gave x86_64 a per-CPU TSS and
ring-3 tasks began running on APs — and nothing went back to the note.  The
premise it depended on became false silently, because a comment cannot fail a
test.

What was left was two globals:

    syscall_kernel_rsp   the kernel stack to switch to
    scratch_user_rsp     where the caller's rsp was stashed

Two CPUs inside `syscall` at the same time therefore **stashed over each
other's user rsp and ran the kernel on the same stack**.  Each then returned to
ring 3 with the other's stack pointer.

The reason this needs a trick at all: `syscall` hands the kernel no stack —
`rsp` is still the *user* stack on entry — and leaves no register free to find
one with.  `rcx` and `r11` are already clobbered by the instruction itself and
everything else holds a syscall argument.

`swapgs` exists for exactly this.  It swaps `IA32_KERNEL_GS_BASE` into `GS.base`
atomically, so the stub can address a per-CPU slot without computing an address:

    [gs:0]  this CPU's ring-0 stack top   (written by hal_set_kernel_stack)
    [gs:8]  this CPU's user-rsp stash

GS is free for it — x86_64 musl keeps thread-local storage in FS, which is also
why `arch_prctl(ARCH_SET_GS)` was already answered with `-ENOSYS`.  The stub
`swapgs`es back **before** entering the shared `isr_common` tail, so the rest of
the kernel and the `iretq` back to ring 3 see exactly the state they always did
and need to know nothing about any of this.  Nothing can interrupt the window
between the two: `IA32_FMASK` cleared IF, and both accesses touch already-mapped
kernel pages.

`syscall_init_64` already ran per CPU, so the `KERNEL_GS_BASE` write goes there;
`hal_set_kernel_stack` writes the slot of the CPU it is running on, which is the
CPU doing the context switch.

#### Why it hid for so long

- **i386 was immune.** It reaches the kernel through `int 0x80`, and an
  interrupt gate switches stacks using the TSS — which has been per-CPU since
  §M35.  Only the x86_64 fast-syscall path had to find a stack by hand.
- **Native d-os programs were immune.** `forktest`, `forkexec` and `pipetest`
  link the in-tree libc and use `int 0x80`; all three passed at `-smp 2`
  throughout.  Only *musl* binaries issue `syscall`, because musl's
  `syscall_arch.h` hard-codes the instruction — and not patching musl is the
  entire point of the Linux-ABI personality.
- **One musl process was immune.** `musltest` passed at `-smp 2`: a single
  process rarely has a second thread of its own inside a syscall at the same
  instant.  It took *two* musl processes — a shell and the coreutil it forks —
  to collide.

That is three separate reasons why every test in the suite could pass while the
flagship capability was broken, and it is the general lesson: **a deferred note
is a dependency on a premise, and nothing in the build checks that the premise
still holds.**  The two milestones that invalidated it (§M35's per-CPU TSS,
ring-3 on APs) were both green.

#### Found by elimination

`-smp 1` clean, `-smp 2` broken said "race".  The in-tree fork/exec tests
passing at `-smp 2` while the musl shell failed said "not fork, not COW" — and
pointed straight at the one path the two groups do not share.  The first fault
captured (`pid 'forked'`, executing inside its own argv strings, with a kernel
address sitting in `rdi`) was consistent with a return to ring 3 through someone
else's frame.  §M51's TLB shootdown, landed just before this, was verified
*not* to be the cause by removing it and reproducing the failure unchanged.

### 4.53 Time, in nanoseconds (M53 stages 1–2)

Two things landed: a monotonic nanosecond clock, and deadline timers built on
it.  Together they replace "poll a millisecond counter on every tick" with
"tell me when this instant arrives".

#### Stage 1 — the clock

`timer_ticks_ms` counts interrupts.  Every timestamp it produced was a multiple
of one millisecond, so two reads taken microseconds apart compared EQUAL and any
duration measured across it carried ±1 ms.  Fine for a watchdog deadline;
useless for anything a program times.

`timer_now_ns()` is one monotonic nanosecond clock from whatever the machine
has, and callers never learn which:

| arch | source | measured |
|---|---|---|
| aarch64 | `CNTPCT_EL0`, rate from `CNTFRQ_EL0` | 62.5 MHz, 16 ns |
| i386 / x86_64 | TSC, calibrated against the PIT | ~1.2 GHz, 1 ns |
| fallback | the 1 ms tick, scaled | 1 ms |

The ARM side needs no calibration and no capability check — the architecture
*defines* the counter's rate and hands it over in a register.  The x86 side is
eighty lines establishing the same two facts: that the counter is constant-rate
(`CPUID.80000007:EDX[8]`, or `CPUID.1:ECX[31]` for a hypervisor, which
virtualises the TSC and so cannot track a guest core's frequency scaling), and
what its rate is.  Neither → keep the tick, because a coarse clock that is right
beats a fine one that is wrong.

Two implementation details that are load-bearing rather than incidental:

- **The conversion splits into seconds plus a remainder.**  The obvious
  `delta * 1000000000 / hz` overflows 64 bits after about nine seconds at 2 GHz
  — long enough to look correct in a boot test and then wrap in front of a user.
- **The clock never returns less than it last returned.**  The counter is read
  without a lock and on x86 it is per-CPU; a task migrating between two slightly
  skewed CPUs would otherwise see time step backward, which breaks callers in
  ways that are very hard to trace back to the clock.

**What it found in its first minutes.**  `ktime` reported a 100 ms sleep on
aarch64 as 57 ms.  `timer_ticks_ms` computed `tick_count * 1000 / hz`, and
`tick_count` is a single global that EVERY CPU's timer ISR increments while `hz`
is the per-CPU rate — so on an N-CPU machine the millisecond clock ran N times
too fast.  Every timeout, watchdog deadline and sleep on that architecture was
wrong by the CPU count, silently, because nothing had a second opinion to check
it against.  It now reads `CNTPCT` directly: exact, and indifferent to how many
CPUs take interrupts.  Measured after: 100.0 ms at `-smp 1`, 105 ms at `-smp 2`,
106 ms at `-smp 4`.  x86 was never affected — the PIT delivers to the BSP only.

#### Stage 2 — deadline timers

`ktimer_arm(t, deadline_ns, fn, arg)` fires a callback at an absolute
nanosecond deadline; `ktimer_cancel` returns whether it actually removed a
pending timer, which is the only race-free way to know a callback will not run.
A single sorted list under one lock — deliberately not a hierarchical wheel,
which earns its complexity at thousands of pending timers where the honest
number here is a handful.  The interface hides the list, so that can change
later without touching a caller.

Callbacks run in interrupt context with the lock NOT held, so a callback may arm
another timer; anything heavier belongs on the §M49 workqueue.

`task_sleep_until_ns()` is the first consumer, and `sys_clock_nanosleep_ns`
exposes both POSIX forms.  The absolute form is not a convenience: a relative
sleep restarted after a signal drifts, so every periodic loop that must not
drift is written against the absolute one.

#### The accuracy floor is measured, not assumed

`ktimer` sleeps for a spread of intervals and reports the error, because a timer
list is easy to believe in and the number that matters is *lateness*.

The first version hooked expiry into `schedule_check`, which looks like the tick
and is not — it runs at the QUANTUM rate (every `SCHED_QUANTUM_TICKS`, 100 Hz).
Every timer was up to 10 ms late regardless of its deadline, and a 500 µs sleep
measured 9.7 ms.  Moving expiry to the actual tick ISR fixed it:

| | before | after |
|---|---|---|
| worst lateness, i386 | 9037 µs | **840–953 µs** |

That ~1 ms is the tick period — the deadline is exact, the moment we *notice* it
is not.  aarch64 ticks at 100 Hz, so its floor is 10 ms (measured: 9193 µs) and
its `ktimer` output says so.  Removing the floor entirely means driving the
timer hardware as a one-shot deadline instead of a periodic source, which is a
change to the timer service alone — nothing above it moves.  That decision now
has a number behind it instead of an intuition.

#### Open

- One-shot hardware deadlines (TSC-deadline / LAPIC one-shot; `CNTP_CVAL` on
  ARM) to remove the tick floor.
- aarch64 still ticks at 100 Hz.  Raising it needs a quantum divider like
  x86's `SCHED_QUANTUM_TICKS`, or the scheduler would preempt ten times more
  often as a side effect.
- `timerfd`, `timer_create`/`setitimer` — the remaining POSIX surface, and the
  piece `epoll`-shaped event loops need.
- A clock read currently costs a 64-bit division (~2–4 µs measured under
  emulation on i386).  Linux precomputes a multiply-and-shift; worth doing if a
  caller ever reads the clock in a hot loop.

---

### 4.54 A task the scheduler was still standing on (M54)

**The report was three sentences: open NetSurf, a crash report appears, close
it, open it again — the machine dies.**  The rule this violates is the oldest
one in the project: *nothing a user program does may take the box down.*  A
browser crashing is a browser problem; the machine going with it is a kernel
problem.

#### The evidence, which was already on disk

The guest had rebooted, and §M47's NVRAM breadcrumb had already written down
what killed the previous boot:

```
crash: PREVIOUS BOOT ENDED UNCLEANLY — last recorded event was kernel-fault
       in 'idle-3' (pid 4) at pc=0x0000000000120ab5 code=11
```

```
$ ./scripts/dos-sym.sh 120ab5 x86_64
0x120ab5 is in pick_next_local_locked (+0x54)
  120ab5: 8b 80 d8 01 00 00     movl 0x1d8(%rax), %eax    # t->state
```

The idle task of CPU 3, walking its own runqueue, read `state` out of a task
that was not a task any more.  So the crash had nothing to do with the browser
except its timing: the browser's DEATH left something behind in the scheduler,
and the next scheduling decision stepped on it.

#### The root cause: `current` is not "running"

`schedule_locked` publishes the incoming task as the CPU's `current` **before**
it swaps stacks:

```c
me->current = next;
...                          /* FPU save/restore, address-space switch —      */
                             /* all of it still executing on PREV's stack     */
context_switch(&prev->esp, next->esp);
```

Between those two points the outgoing task is `current` **nowhere**, while a
CPU is still executing on its kernel stack and has not yet written back its
saved stack pointer.  Two independent pieces of the kernel asked the wrong
question about that state:

- `task_running_elsewhere` (the guard that stops two CPUs picking one task)
  scanned only `current` — so a second CPU concluded the task was free, picked
  it, and resumed it **from a stale `esp`**.  One task, two CPUs, one stack.
- `task_reap` refused to free a task that was `current` somewhere — so it
  freed one that was merely being switched away from, handing its kernel stack
  to the next allocation while a live CPU was still writing to it.

Everything downstream is corruption with no attribution: a wild return address
(the machine was observed jumping to `0x3`, and to a heap page in the direct
map), a runqueue ring with a freed task in it, a fault in the idle task on a
third CPU.  **The recorded crash was three or four hops away from its cause.**

The fix is one bit that answers the question actually being asked — *is a CPU
still standing on this task?*

```c
volatile int on_cpu;    /* struct task */
```

Set when a task is switched to; cleared by **the task that takes the CPU over**,
because the outgoing task cannot do it (by the time it would be safe, it is no
longer running).  The incoming task finds its predecessor in a per-CPU
`g_leaving` slot.  There are exactly two places an incoming task can arrive —
the tail of `schedule_locked` for an established task, and
`task_finish_first_switch` for a brand-new one — and **missing the second one
was a bug in the first version of this fix**: a task switched away from in
favour of a freshly spawned task kept `on_cpu` set forever and became
unreapable.

`task_running_elsewhere`, `load_steal_one` and `task_reap` now all consult
`on_cpu`.  The `current` scan is kept underneath it as a second opinion.

#### Four more defects on the same path

1. **A DEAD task could be enqueued.**  `wake_timed_sleeper` and
   `wake_waitq_sleeper` decide "this task was asleep, wake it" and then call
   `task_enqueue` *after* dropping the lock they decided under.  In that window
   the task can be woken for real by somebody else, run, and exit.  Nothing
   takes a DEAD task out of a runqueue again — its own exit path has already
   run — so it stays linked until it is reaped and freed.  The test now lives
   inside `rq_insert_tail_locked`, under the destination queue's lock, one
   instruction before the insert.

2. **The exit path removed the task from the wrong queue — or from none.**
   Four places read

   ```c
   if (self->cpu_home == this_cpu_id()) rq_remove_locked(me, self);
   ```

   which is not a removal but a removal *attempt*: when the premise is false the
   task silently stays queued, and the premise is false for a real window (a
   waker may re-home a task onto another CPU's queue while it is still
   `current` here — `waitq_block`'s own comment describes that window).  Now
   `rq_purge_all` sweeps every queue, which is airtight *because* it runs after
   DEAD is published: an enqueue is then either already complete (and the sweep
   finds it) or still waiting for the lock (and refuses when it gets it).

3. **A sleeper count that could drift low, which is not a slow kernel but a
   stopped one.**  A timed sleep is ended by two independent parties — the tick
   sweep when the deadline passes, and a kill that will not wait for it — and
   both tested `sleep_until_ms` with no lock between them.  Both could conclude
   the sleep was theirs to end, and both then decremented the global sleeper
   count for ONE sleep; the plain `++`/`--` could also simply lose an update
   across CPUs.  The sweep **skips itself entirely when that count reads zero**,
   so an undercount means a real sleeper is never woken: the task blocks
   forever, with no fault, no log and no way in.  Ending a timed sleep is now a
   CLAIM — an atomic exchange of `task->timed_sleep` 1 → 0 — and whoever wins it
   owns the wake, exactly once, and is the only one that touches the count.
   (This is also what made aarch64's `forktest` report `status=130` instead of
   `status=7` under load: the child's exit was being raced.)

4. **"I marked myself asleep, then took myself off the queue — but by then
   someone had already woken me."**  `task_msleep` and `waitq_block` set
   `state = SLEEPING` and dequeue as two steps.  A waker fitting between them
   flips the state back to RUNNABLE and finds the task still queued, so its
   enqueue is a no-op — and then the sleeper dequeues itself anyway.  The result
   is a task that is **awake, ready, and on no runqueue**: never picked again,
   by anything, ever.  That is what "the shell just stopped" looked like, and it
   leaves nothing behind at all.  Both now re-check after dequeuing and put
   themselves back rather than sleeping through a wake meant for them.

5. **The runqueue walks trusted the ring absolutely.**  One bad link and the
   kernel dereferenced it, in the scheduler, holding the queue lock — the box
   died and took its own diagnostics with it.  `pick_next_local_locked`,
   `load_steal_one` and `rq_refresh_local_load` are now bounded walks that
   repair a broken ring (closing it at the last reachable node) and report once,
   lock-free, over the serial port.

#### The reproducer, in the tree

None of this was reachable by clicking.  `killstorm [rounds] [tasks]` (both
shells) spawns N tasks that park in `task_msleep`, kills them all, and repeats
— so the three-party operation at the centre of the bug (a killer wakes a
sleeper, another CPU runs it and it exits, the reaper frees it) happens
hundreds of times a second on every core at once.

It killed the machine within seconds on the first run.  After the fixes:

```
killstorm: done — 480 spawned, 480 killed, 0 still alive     (x86_64, -smp 4)
killstorm: done — 480 spawned, 480 killed, 0 still alive
killstorm: done — 480 spawned, 480 killed, 0 still alive
killstorm: done — 300 spawned, 300 killed, 0 still alive     (aarch64, -smp 4)
```

**A bug that needs a browser, a crash and a reboot to reproduce is a bug nobody
can work on.  Same bug, in a shell command, in two seconds.**

#### The GUI half: a handle whose lifetime was inferred

Separately — and this is what the user actually saw first — a crashed NetSurf
could not be reopened after four crashes.  `dosgui` released a bridge handle
only in `dosgui_destroy`, which is a call a *crashed* client never makes.  So
every crash burned one of four handles permanently and left `win` pointing at a
window struct the compositor had already recycled.

The fix is a disposal notification (`gui_window_set_dispose_cb`) fired from
`destroy_window` on **every** teardown route, not just the ones the bridge
caused itself.  *A handle whose lifetime is inferred is a handle that leaks:
the owner of a handle has to be told when the object behind it dies.*

#### Diagnostics that were missing at the moment they were needed

- The x86_64 kernel-fault record hard-coded **0** for the fault address.  CR2 is
  the most informative number a page fault has, the box halts, and the record is
  the only thing that survives to the next boot.  Now recorded (i386 and aarch64
  already did).
- Ring-0 fault dumps did not say **which task** faulted.  "EXCEPTION 6 at rip=3"
  names an address and nothing else.  Now: task name, pid, CPU.
- Two CPUs faulting at once interleaved their dumps character by character,
  exactly when the output most needs to be readable.  `crash_dump_begin/end` is
  a bounded, lock-free gate (never a real lock — this runs in fault context)
  around the dump on all three arches.
- `crash_report` claimed its ring slot with a plain `seq++`.  Two CPUs faulting
  together claimed the *same* slot and blended their strings into it — so the
  one record describing the failure was a mixture of two.  Now an atomic
  increment, which is still lock-free.

#### Lessons

- **`current` is not "running".**  A CPU is standing on a task's stack from
  before that task is published as current until after it is unpublished.  Any
  question of the form "is anyone using this task?" has to be asked of a flag
  that spans the whole switch, not of a pointer that changes in the middle of
  it.
- **A guard that silently skips when its premise fails is not a guard.**
  `if (cpu_home == this_cpu_id()) remove()` reads like a removal and behaves
  like a coin flip.
- **The crash you record is not the crash that happened.**  Every symptom here
  (fault in the idle task, jump to 0x3, #GP in the crash reporter itself) was
  several hops downstream of one shared-state bug.  What made it findable was
  narrowing with a reproducer, not reading the faults.
- **Make it reproducible before making it right.**  The first two "fixes" in
  this milestone were wrong and the stress test said so within a minute — one
  left a task unreapable, one hung tasks outright.  Neither would have been
  caught by clicking on a browser.

#### Where it stands

Six back-to-back storms per arch (≈2900 spawn+kill cycles each, `-smp 4`),
i386 / x86_64 / aarch64: **no fault, no stall, no lost task, every victim
reaped.**  `killstorm` itself now runs the storm on its own task and the shell
watches it, so a stall is REPORTED with the task table rather than hanging the
test along with the thing it is testing — the first two versions hung silently
and cost two runs each.

#### Open

- The `task_reap` sweep still reports a queued task roughly 8 times in 2900
  kills.  It is caught, repaired and logged, and the box is unaffected.  The
  instrumented message narrows it sharply: **every single instance was
  `cpu N (cpu_home N)`** — a *completed*, self-consistent `task_enqueue` that
  landed after the exit path had already swept every queue while DEAD.  Since
  both the DEAD test and the insert happen under that queue's own lock, that
  should be impossible; one of the orderings in this file is still not what it
  reads like, and the message says so rather than repairing in silence.
- `load_steal_one` sets `cpu_home` to the stealing CPU while the task is
  briefly on no queue at all.  Setting it to -1 there is more honest and
  **hangs tasks** — the block paths use `cpu_home` to find the queue to detach
  from, and a -1 they cannot act on leaves a task queued and asleep at once.
  The right fix is a single owner for that transition, not a more honest
  transient.

---

### 4.55 A deadline you can wait on (M53 stage 3)

Stage 2 gave the kernel deadline timers and `clock_nanosleep` gave a program a
way to wait for one — but only by doing nothing else while it waits.  A real
event loop cannot afford that: it is already blocked in `poll` on sockets and
pipes, and "wake me in 20 ms" has to arrive through the SAME wait, or the loop
has to choose between being responsive to I/O and being punctual.

Stage 3 closes both halves of the POSIX timing surface:

- **`timerfd`** — the timer becomes a DESCRIPTOR, so a timeout is just another
  readable fd and the loop keeps one blocking point instead of two.  This is
  exactly what an `epoll`-shaped loop needs, which is why it had to land before
  the async work rather than after it.
- **`setitimer`** — for the program that wants to be INTERRUPTED by time rather
  than to wait for it (a watchdog around a blocking call, a timeout on
  something with no descriptor to poll).  Delivers SIGALRM.

Both sit on stage 2's `ktimer`; only the delivery differs.

#### The count is the point

A read of a timerfd yields the number of EXPIRATIONS since the last read and
resets it — Linux's semantics, kept deliberately.  A timer that quietly dropped
the ticks nobody collected would let a program drift with no way to notice; the
count is what makes a missed tick observable instead of invisible.  Same idea,
one layer up, as the §M53 stage 1 finding that a millisecond clock can be N
times too fast and nothing says so.

#### Re-arm from the DEADLINE, never from now

A periodic timer that re-arms from `now` adds each expiry's lateness to every
subsequent period.  The lateness is bounded by the tick, so the drift is not:
over a minute it accumulates without limit.  Stepping the stored deadline
forward instead keeps the phase.  If the machine was busy longer than a whole
period, whole periods are skipped and ADDED TO THE COUNT rather than fired as a
catch-up burst — the reader learns it fell behind, which is the honest report.

`timerfdtest` measures this the only way that can catch it: the error is
reported against the ORIGINAL start, not against the previous tick.  A drifting
timer looks perfect tick-to-tick.

```
x86_64, 50 ms period, waited on via poll(2):
  tick 1: drift +953 us     tick 4: drift +782 us
  tick 2: drift +1001 us    tick 5: drift +397 us
  tick 3: drift +1066 us
aarch64, same test:  +11293, +10386, +9248, +7269, +8424 us
```

The error oscillates around the tick period and does not accumulate.  ARM's
floor is ~10 ms because it ticks at 100 Hz — the same number §M53 stage 2
measured, now visible from a second direction.

#### The `word_bytes` the map was missing

`struct itimerspec` is four `long`s: 16 bytes on a 32-bit guest and 32 on a
64-bit one.  The §M50 engine's handlers are shared across every architecture
and deliberately never learn which one they are on — so the width had to come
from somewhere else, and the right somewhere was the GUEST DESCRIPTION that
already existed: `struct abi_map` gained `word_bytes`.  A handler that had
inferred it from the host's own `sizeof(long)` would be right only by
coincidence, and would break silently the first time a 32-bit guest ran on a
64-bit kernel.

That is the §M50 payoff arriving as advertised: **four canonical operations,
four handlers, four rows per guest table — and `timerfd` exists on all three
architectures at once.**  `it_interval` comes FIRST in `itimerspec`; getting
that backwards produces a timer that works exactly once and then never again,
which reads like a different bug entirely.  `setitimer`'s `itimerval` is
MICROseconds, not nanoseconds — the one place POSIX uses a different unit for
the same idea, and a silent factor of 1000 if it is missed.

#### Delivery is a bit, not a call

The itimer callback runs in interrupt context, so it does the one thing that is
safe there: set the pending-signal bit atomically.  The task notices it on its
own return-to-user path, where every other signal is delivered — so this adds a
SOURCE of signals, not a second delivery mechanism.  `sys_kill` would have been
the tidier call and is the wrong one: it takes the scheduler's lock and applies
a ring-3 credential rule, neither of which belongs on a timer interrupt.

Interval timers live in a table keyed by pid rather than in `struct task`.  A
timer embedded in the task would have to be cancelled at exactly the right
point in teardown, and the cost of getting that wrong is a callback firing into
freed memory — §M54's failure, one layer up.  `task_exit_code` cancels the slot;
a pid that is gone is simply a miss.

#### Interfaces

| Native | Linux (i386 / amd64 / arm64) |
|---|---|
| `SYS_TIMERFD_CREATE` 37 | 322 / 283 / 85 |
| `SYS_TIMERFD_SETTIME` 38 | 325 / 286 / 86 |
| `SYS_TIMERFD_GETTIME` 39 | 326 / 287 / 87 |
| `SYS_SETITIMER` 40 | 104 / 38 / 103 |

The native calls pass times as a two-element `uint64_t` array rather than as
register arguments: a nanosecond value does not fit one register on i386, and
packing it into a register PAIR would make the native ABI's shape depend on the
word size — the arch-specific detail every other syscall here has been kept
free of.

Shell: `timerfdtest [ms]`, `alarmtest [ms]` (both shells, all three arches).

#### Open

- `timer_create`/`timer_settime` (POSIX per-process timer IDs, `sigev_notify`)
  are not implemented; `setitimer` covers ITIMER_REAL, which is what almost
  every program that wants an alarm actually uses.  ITIMER_VIRTUAL and
  ITIMER_PROF need per-task CPU-time accounting hooks and are declined rather
  than faked.
- `poll(2)` still treats a positive timeout as a snapshot.  Now that a deadline
  is a first-class object, a finite `poll` timeout is a timerfd internally —
  the natural next step, and the one the async work wants.

---

### 4.56 Waiting for the network without spending a CPU on it (M55)

**Files:** `kernel/core/net.c`, `kernel/includes/net.h`,
`kernel/core/usyscall.c` (socket RX), `kernel/core/shell.c` (`netstorm`,
`lsnic`).  **Verified:** i386 + x86_64, `-smp 4`.

#### What was wrong

§M24 drove receive by *polling from the calling task*: whoever wanted a reply
called `dev->poll()` in a bounded spin loop until it arrived.  The file said so
in its own header — *"Everything therefore runs in one task context → no
locking"* — and that was true when written.  Three defects follow from it, and
they compound:

1. **The waiter burned a CPU.**  A task waiting for a DNS answer spun with
   `hal_cpu_pause()` and never left the runqueue, so *waiting for the network
   cost exactly as much CPU as computing flat out*.  §M49 removed the last such
   polls from the console and the init reaper for precisely this reason; the
   network stack was the one that got away.

2. **N waiters meant N pollers.**  `dev->poll()` is not reentrant — it advances
   the driver's `last_used_idx` and recycles RX buffers — so two tasks each
   "waiting for their own packet" were two tasks mutating one ring.  It survived
   only because nothing ever waited on two sockets at once, which is a statement
   about the workload, not about the code.

3. **A spin count is not a timeout.**  `20000000u` iterations means a different
   amount of time on every machine, every arch and every host CPU under
   emulation.  That is not a theoretical complaint: it is how musl's resolver
   came to hang "for minutes" on emulated i386 (§M39).  The bound was doing its
   job; it just could not say how long its job took.

#### The model now

One poller task (`netd`) is the only caller of `dev->poll`.  Everyone else
BLOCKS on a wait queue until the state they care about changes or a real
deadline — §M53's nanosecond clock — passes.

`g_netwq`'s lock is THE stack lock.  By waitq's own contract the queue's lock
also serialises the guarded condition, so the ARP cache, the ping and DNS reply
flags, the TCP connection state and the per-socket RX rings are all mutated and
checked under it; `net_rx()` and everything below it run with it HELD.

**netd runs exactly while somebody is waiting, and is fully blocked otherwise.**
A poller with nobody waiting for it is pure waste, and an idle box must not pay
for a network stack it is not using.  On a machine that has never touched the
network the task does not even exist: `lsnic` reports `netd: not started`.

#### The consequence that reshaped the file

The RX path may never block, so **it may never resolve an ARP entry**.  Every
reply generated while handling a frame therefore goes back to the MAC that frame
CAME FROM — which is the correct next hop by construction, on-link or via a
router, and is cheaper besides: *a TCP ACK has no business doing an address
lookup*.  That is what the `via_mac` argument on the emit path is for, and why
the send helpers now come in two flavours:

- `*_locked` — assembly and transmit only, caller already holds the lock and has
  already decided the next hop.  Used by the RX path.
- unlocked — resolve the route (which may block), then take the lock and emit.
  Used from task context, and never called with the lock held.

A TCP connection resolves its peer's MAC **once**, at connect time, and caches
it in `g_tcp.peer_mac`: a connection has a fixed route for its lifetime, so this
is not merely an optimisation — it is what makes `tcp_input` able to ACK at all.

#### Timeouts

Every spin budget became a duration: ARP 3 × 1000 ms, ping 1000 ms, DNS 3000 ms,
TCP connect 5000 ms, TCP recv 10000 ms, HTTP "gone quiet" 2000 ms, datagram recv
5000 ms.  Each wait arms a `ktimer` for its own deadline; the callback takes the
queue lock and wakes, which is what closes the window against a task that has
checked its deadline but not yet parked.

#### `netstorm` — the proof, because a model change that cannot be measured is a claim

New shell command `netstorm [n]`: n tasks each ARP for a *different* address
nothing will answer for, so every one of them really has to wait (a cache hit
would prove nothing).  Each probe takes 3 × 1000 ms; the measurement is the
elapsed time, and it is falsifiable — parallel waiting is ~3 s, serialised
waiting would be 3 s × n.  The shell watches rather than participating, so it can
still report a wedge (the `killstorm` lesson: a test that hangs along with the
thing it tests reports nothing at all).

```
netstorm: 8/8  finished in 3296 ms, peak waiters  8, 2% of 4 CPUs busy, 4047 pumps
netstorm: 16/16 finished in 3467 ms, peak waiters 16, 3% of 4 CPUs busy
```

Doubling the waiters changed neither the elapsed time nor the CPU cost.  Under
the old model those same 8 tasks would have been 8 cores' worth of spinning.

#### A number that did not add up, and the bug behind it

The first `netstorm` on a freshly booted box reported **143 115 pumps**; the
second, doing identical work, reported **4 463**.  Same load, same duration,
35× the polling — one of the two had to be wrong.

It was the first.  `net_wait_cond` has a fallback for the case where the poller
is not up yet: pump in line so the wait can still make progress.  That fallback
looped **with the stack lock held and interrupts off**, so a CPU that entered it
could not be preempted — and what it was waiting for was the scheduler starting
`netd`.  Eight probes entering it at once starved the very task whose arrival
would end it, turning a startup window into 138 000 wasted pumps.

The fix is one line of discipline: drop the lock and `task_yield()` each round.
First-use cost fell to **4047 pumps, of which 2 were inline** — and the inline
counter is now reported separately by `lsnic`, because *a rising count there
means the poller is not doing its job, and that should be visible rather than
inferred from a total*.

**Two process lessons from the same episode.**  The measurement that exposed
this was one I had almost not taken — the number simply looked large.  And the
build that produced the first "explanation" had **failed**: my error filter let
`error:` through unnoticed and I read a stale ISO.  §M51's rule again, from the
other direction: *check the build's exit status, not its output.*

#### Part 2 — the NIC interrupt

With one poller and no spinning waiters, the interrupt is finally safe to wire:
before part 1 it would have been *two* pollers on one virtqueue, the ISR and
every spinning waiter, which is exactly why it came second.

**The ISR does two things and no third.**  It reads the legacy virtio ISR
status register — the read is what deasserts the device's level-triggered line,
so skipping it is an interrupt storm, not a missed packet, and a zero read means
the interrupt was not ours (the line can be shared) and must not be acked.  Then
it calls `net_rx_irq()`, which wakes the poller.  It does **not** drain the ring:
draining runs `net_rx`, which can generate a TCP ACK, which spins on the TX
virtqueue.  That is §M49's xHCI lesson, word for word.

**The stack learns that interrupts work by receiving one.**  `net_rx_irq` sets
the flag; until then netd stays in the timed fallback.  A driver that wires an
interrupt which never fires therefore degrades to polling instead of blocking
forever on a promise — no config key, and no way to claim a capability the
hardware is not delivering.

**netd never blocks indefinitely.**  Each wait arms a 10 ms backstop, so a
missed or misrouted interrupt costs latency, not liveness.  The sequence
counter (`g_irq_seq`), sampled *before* the pump and re-compared under the queue
lock, is what makes an interrupt that lands *during* that pump impossible to
miss — a bare "is there work" flag would lose exactly those.

TX completion interrupts are actively suppressed
(`VRING_AVAIL_F_NO_INTERRUPT` on the transmitq): `vnet_transmit` waits for the
used ring synchronously, so a TX interrupt could only ever wake a task that
finds nothing to do.  And `vnet_poll` now notifies the device only when it
actually recycled a buffer — telling a device that fresh buffers are available
when none are is a VM exit with no meaning, and on an empty pump that was one
wasted exit per round.

#### Measured (i386 / x86_64, `-smp 4`)

| | before part 1 | after part 1 | after part 2 |
|---|---|---|---|
| `nettest` pumps | 4982 | 4982 | **26 / 39** |
| `netstorm 8` pumps | — | 4047 | **286 / 285** |
| `netstorm 8` CPU | 8 cores spinning | 2–3% of 4 | **0% of 4** |
| `netstorm 8` elapsed | — | 3296 ms | 3033 / 2951 ms |

`missed 0` on every run: no interrupt was ever late enough for the backstop to
find frames already waiting.  `netstorm`'s ~290 backstops are not a defect and
the counters say so separately — with nothing coming, the timer is the only
thing that *can* wake the poller, and 3 s ÷ 10 ms = 300 is the number the design
predicts.

#### One more time on the same rake

The first run of part 2 reported `peak waiters 0` and a `netstorm` FAIL — while
8 probes demonstrably finished in parallel in 2912 ms.  I had added a field to a
struct in `net.h` and rebuilt **without `make clean`**.  This project has no
header dependencies; `net.c` and `shell.c` compiled against two different
layouts of the same struct, so the reported field was not the one being read.

That is §M51's lesson exactly, and CLAUDE.md states the rule outright.  Worth
recording that it caught me anyway, and what saved it: the numbers *disagreed
with each other* — a run cannot both serialise and finish 8 probes in 3 s.
**A self-contradictory measurement is evidence about the measuring apparatus.**

#### Open

- The stack is still single-instance above the transport: one in-flight ping,
  one DNS query, one TCP connection.  §M55 makes concurrency *safe*, it does not
  make the stack multi-connection — and `netstorm` is careful to test only what
  is claimed.
- `dev->transmit` now waits for its virtqueue completion with interrupts masked.
  Under QEMU that is microseconds; the driver's own bounded spin is what keeps a
  wedged device from becoming a hard lockup.

---

### 4.57 A wait that is really a wait (M56 — poll timeouts + epoll)

**Files:** `kernel/core/epoll.c` + `kernel/includes/epoll.h` (new),
`kernel/core/usyscall.c` (readiness), `kernel/core/vc.c` (stdin readiness),
`kernel/core/net.c` (stream readiness), `kernel/core/abi_engine.c` +
`abi_linux.c` + `includes/abi.h` (guest marshalling), `user/epollmusl.c`.
**Verified:** i386, x86_64, aarch64.

#### The defect that mattered most was already documented

`poll(2)` with `timeout > 0` was treated as a snapshot.  The comment said so
plainly — *"a finite millisecond wait is not honoured yet … treated as a
snapshot so it never blocks past the caller's intent"* — and the reasoning
sounds conservative.  It is not.  A program asking to wait 200 ms got an
immediate `0`, so **every correct event loop written against it became a busy
loop**: the caller does exactly what it should, sees nothing ready, and asks
again immediately, forever.

*A timeout that returns early is not a safe approximation of one that waits; it
is a different function.*  §M53 gave the kernel deadline timers and §M55 built
three separate waits on them — there was no reason left for this one to be a
lie.  A finite `poll` now arms a `ktimer` and blocks, exactly like the others.

#### One definition of "ready"

`fd_readiness()` (fd.h) is now the single answer to "would this descriptor
block", shared by `poll` and `epoll`.  Extracting it immediately surfaced two
bugs that had been sitting in `poll_snapshot`'s fall-through:

- **`FD_NETSOCK` was reported permanently ready.**  It fell through to the
  "VFS/shm: ready" default, so any loop polling an AF_INET socket span at full
  speed.  Nothing had noticed because nothing polled a network socket — until
  epoll made that the obvious thing to do.
- **stdin was reported never ready**, which is safe but useless: an interactive
  event loop could not exist.

stdin is now readable *when a whole LINE is buffered*, not when a byte is —
because `stdin_read_line` is cooked and blocks until Enter.  Reporting readable
on the first keystroke would hand a poller a descriptor whose `read` then blocks
anyway, and **a poll that lies about which reads will not block is the one thing
a poll must never do**.  `vc_kbd_push` therefore signals readiness on the
newline only; waking every poller in the system on every keystroke would be a
cost with no matching readiness.

#### epoll, and what it is honestly not

`epoll_create`/`epoll_ctl`/`epoll_wait`, level-triggered, with the set living in
the kernel and a caller cookie handed back verbatim.

**It is not O(ready).**  Our `epoll_wait` scans the registered set, so the
asymptotics are still poll's.  The win here is the *interface*: a program
written against epoll runs unmodified, and the watch list stops crossing the
syscall boundary on every iteration.  Making the wait genuinely proportional to
what became ready needs per-fd wait queues with callback registration — a change
to every fd kind — and bundling that in silently would be claiming an efficiency
the code does not have.

**`EPOLLET` is refused, not downgraded.**  Serving edge-triggered registrations
level-triggered would "work": level is a superset.  But a program written for
`EPOLLET` drains each fd once per report, so it would be handed the same fd
forever and spin while appearing correct.  A loud `-EINVAL` points at the one
line that needs changing.

Both `poll` and `epoll_wait` share `fd_readiness_wait()` — one blocking loop,
one deadline, one check-then-park discipline.  Two copies would have been two
chances to get the lost-wakeup rule wrong, and poll already had the first copy.

#### The ABI trap, which is the sharpest one yet

`struct epoll_event` is `{ u32 events; u64 data; }`, and its size is:

| guest | size | `data` offset | why |
|---|---|---|---|
| Linux/i386 | 12 | 4 | u64 aligns to 4 on i386 |
| Linux/amd64 | **12** | 4 | Linux **packs it on x86_64 specifically** so the 32- and 64-bit layouts agree |
| Linux/arm64 | 16 | 8 | not packed; u64 aligns to 8 |

So the size **does not follow the word size**, and a handler deriving one from
the other would pass on i386, pass on amd64, and fail exactly on arm64.  The map
carries it: `abi_map.epoll_event_bytes` (12/12/16), a sibling of §M53's
`word_bytes` and a reminder that *the guest's layout is a property of the guest,
never something the host may infer*.

The x86 layout also puts `data` at offset 4 — **unaligned for a `u64` on a
64-bit host** — so the handler reads and writes it bytewise rather than through
a `uint64_t*`.  x86 tolerates the misaligned access; writing the kernel so it
only works on forgiving hardware is how an arch port later fails for no visible
reason.

The native syscalls therefore carry a flat `u64` pair per event and never see
`struct epoll_event` at all, the same rule as `SYS_TIMERFD_SETTIME`'s `u64[2]`.

#### Measured

`epolltest` (kernel side) and `epollmusltest` (an unmodified musl binary, so the
translation is under test rather than the mechanism):

```
epoll: empty set, 200 ms timeout -> n=0 after 200657 us    (was: instant 0)
epoll: timerfd -> n=1 events=1 data=0abcdef01 after 80987 us
epoll: pipe    -> n=1 data=2222        (and NOT ready before the write)
epoll: EPOLLET refused with -22 (correct)

epollmusl (i386)   : sizeof(struct epoll_event) = 12
epollmusl (x86_64) : sizeof(struct epoll_event) = 12
epollmusl (aarch64): sizeof(struct epoll_event) = 16
epollmusl: epoll_wait -> n=1 events=0x1 data=0x1122334455667788   [all three]
```

The cookie carries bits in **both halves** of the u64 on purpose: an offset
error of four bytes would still look plausible with a small integer and would be
invisible with zero.  Three architectures, two struct sizes, one cookie
surviving all three — the `epoll_event_bytes` design demonstrated rather than
asserted.

#### Completing it (§M56.1, same day)

Four of the five open items above turned out to be things an event loop cannot
work without, so they were finished rather than deferred.

**Hangup is now visible without reading.**  `fd_readiness` returns a POLL* mask
instead of two 0/1 flags, and reports `POLLERR`/`POLLHUP`/`POLLNVAL`
**unrequested** — POSIX requires it, and for a good reason: the only other way
to discover EOF is to attempt the read the event loop exists to avoid.
`POLLRDHUP` (Linux's, so request-gated) is reported separately from `POLLHUP`,
and the distinction matters:

```
epoll: after writer close -> events=2001   (POLLRDHUP|POLLIN — data still there)
epoll: after drain        -> events=2010   (POLLRDHUP|POLLHUP — now it is over)
```

Collapsing the two would throw away the tail of every conversation whose writer
closed promptly, which is most of them.

**`O_NONBLOCK` is generic.**  It used to live inside `struct netsock`, so
setting it on a pipe did nothing and said nothing — the worst failure an event
loop can meet, since every descriptor it drains is one it must not block on.
The flag now lives on `struct ofile`, where POSIX says it belongs (a property of
the open file description: shared by dup, not by a second open).  A
non-blocking empty pipe with a live writer returns `-EAGAIN`, **not 0**: zero
means end of file, and a drain loop told "EOF" by a live pipe stops for good.

**An epoll set is itself pollable**, so loops can nest.  That immediately
created a way to hang the kernel — set A watching set B watching set A recurses
until the stack is gone, holding a lock on every frame — so the readiness walk
carries a bounded depth (`task->epoll_depth`).  Refusing to descend reports "not
ready", which is the safe answer: a too-deep chain never fires instead of
killing the machine that built it.

**`sigprocmask` is real, and `epoll_pwait`'s mask with it.**  `h_sigprocmask`
was `return 0` — accept and forget.  A blocked signal now stays *pending* and is
delivered when unblocked; dropping it would make sigprocmask a way to lose
signals rather than defer them.  `SIGKILL` remains unblockable, which on this
kernel is not politeness but the thing §M46 guarantees.  `rt_sigpending` was
added because without it a program can block a signal and never learn one
arrived — making "defer" and "discard" indistinguishable from the inside, and
leaving the property untestable.

`epoll_pwait` can now swap the mask around the wait, which is the entire reason
the call exists: unblock-then-wait as two steps loses a signal that lands
between them.

#### Two bugs the new tests found, both worth keeping

**The engine's fallback silently shadowed the real handler.**  `ABI_SIGPROCMASK`
had a working handler and was registered in the arm64 number map only.  §M50's
engine *declines* numbers absent from a guest's map and lets the old per-arch
`switch` answer — where a stub still returned "success, did nothing".  So the
implementation worked on arm64 and was unreachable on both x86 guests, with no
error anywhere.  **A fallback is only a fallback while nothing better exists;
when something better arrives, the fallback must be removed in the same
change.**  (§M52's lesson, arriving through the mechanism §M50 built.)

**A `sigset_t` numbers its signals from zero.**  Linux stores signal N at bit
N-1; this kernel's `sig_pending`/`sig_blocked` store it at bit N, because they
are built with `1u << sig` and nothing had ever needed to disagree.  Both are
self-consistent — copying the word across without shifting is what is wrong, and
it fails *silently*: SIGALRM (14) blocked by the guest lands on bit 14 here,
which is SIGCHLD's slot, so the mask looks set and simply never matches.  The
symptom was one number in one test (`pending=0`) with nothing else wrong.
`abi_sigset_to_kernel`/`to_guest` are the whole fix, and exist as named
functions so the next signal-shaped operation has something obvious to call.

The first test run also "found" a POLLRDHUP bug that was the test's own: it
closed a pipe fd without `EPOLL_CTL_DEL`, the next pipe reused the number, the
`ADD` failed `-EEXIST`, and the stale entry's narrower mask hid the bit.  The
hazard is real — a set watches descriptor *numbers* — it just belonged to the
caller, which is exactly what `epoll_close`'s comment already said.

#### `epoll_wait`: the cache that was built, measured, and removed

The remaining item was "stop scanning the whole set".  It was implemented: each
item remembered `(description, generation, answer)` and a scan re-evaluated only
descriptors whose description had bumped a global generation counter.  The
lifetime problem was solved cleanly — the fd table is consulted every time, so a
remembered pointer is only ever *compared*, never dereferenced, and generations
come from one global sequence so a new object at a reused address cannot present
a number the cache has seen.

It was still wrong, and the test written alongside it said so on the first run:

```
epoll: memo check — 20/20 ready, 0/20 idle transitions seen
epoll: FAIL (the readiness cache hid a transition)
```

A cache like this is correct only if **every** state change that affects
readiness bumps the generation — and the sites are not where intuition puts
them.  A pipe's *readability* changes when its owner reads; its *writability*
changes when its **peer** reads.  A producer-side bump is not enough, the
enumeration has to be exhaustive, and two sites were missing on the first
attempt.  *A cache whose invalidation must be remembered at every mutation site
is a bug generator* — and the bug it generates is an event that never arrives,
surfacing long after the change that caused it.

It also bought nothing: **15.5 µs versus 16.4 µs** for 26 registered
descriptors, inside the noise.  The per-item cost is the descriptor lookup and
the loop, not the readiness evaluation the cache was skipping.

So it was removed, and replaced with the one change that is plainly redundant
work: `fd_readiness_of()` takes the `ofile` the scan has already resolved
instead of looking it up a second time.  Honest about that too — at this scale
the benchmark is noise-dominated (16–25 µs across runs) and does **not**
demonstrate a speedup.  It is the right shape, not a proven win.

The standing measurement: ~16 µs for 26 descriptors, so `EPOLL_MAX_ITEMS = 64`
bounds a scan at tens of microseconds **by construction**, and a realistic loop
here (under ten descriptors) pays a few.  Genuine per-fd wakeups need per-object
wait queues with callback registration and the lifetime coupling that comes with
them — §M54's defect class — for a saving this workload cannot notice.  *The
trigger for revisiting is measured:* a program whose scan cost is visible next
to its work.

#### `EPOLLERR` has a producer now

A TCP `RST` is not a `FIN`.  Both end the connection, but a FIN is the peer
saying "I am done sending" and an RST is "this connection is broken" — the first
is an orderly EOF, the second is an error.  Without the distinction a refused or
dropped connection looks exactly like a server that answered with nothing.
`g_tcp.reset` now surfaces as `POLLERR`, which (like `POLLHUP`) is reported
whether or not it was requested.

#### Signal masks are read and written at their real width

A guest `sigset_t` is passed with a `sigsetsize`, and every libc passes 8.  This
kernel has 32 signals and no real-time signals, so bits 32–63 can never be
pending here — writing **zero** for them is the truth, not a loss, which is why
the full eight bytes are now handled rather than the first four.  Anything the
guest keeps beyond `sigsetsize` is its own business and is left untouched.

#### Still open

- Per-fd wakeups — declined on the measurement above, with the reason and the
  trigger both written down.
- `EPOLLONESHOT` disarms on report; `EPOLLEXCLUSIVE` and edge-triggered mode are
  not implemented (the latter deliberately refused, see above).


### 4.58 A task is not dead while it is still running (M57 — the §M54 residual)

§M54 rebuilt the scheduler's lifetime rules and closed five real defects, but
its own notes ended with three things it could not finish.  The first was
written down like this:

> the reap sweep still reports a queued task roughly once in several hundred
> kills (caught, repaired, logged — box unaffected)

That sentence is a confession.  "Roughly once in several hundred" is not a
measurement, it is a shrug — and the reason it stayed a shrug for two
milestones is that the only evidence was a log line arriving long after the
event, from a subsystem that was merely the first to notice.  §M57 turns each
of the three into something that can be reproduced in seconds, then fixes it.

#### The invariant, stated in code

Every defect in this family is one thing: **a disagreement between where a task
IS and where the kernel believes it is.**  None announces itself; the symptom
is a fault in the scheduler, a shell that stops, or a line at reap time — each
arbitrarily far from its cause.

So `task_rq_audit` (task.h) states the rule and checks it directly:

1. a task linked in CPU N's ring has `cpu_home == N`;
2. a task linked in a ring never has `cpu_home == -1`;
3. `rq_count` equals what the ring contains;
4. every ring is circular within its bound;
5. no RUNNABLE non-idle task sits on no queue while running nowhere;
6. `rq_load` equals the summed contribution of the ring's members.

**Rules 1–4 are structural and hold at every instant.  Rules 5 and 6 do not,
and saying so is the difference between a checker people trust and one they
learn to ignore.**  Rule 5 has a legitimate transient (a task really is off
every queue for a moment while it is re-homed).  Rule 6 measures a published
*estimate* — `rq_load` is read locklessly by peers by design, and self-heals at
each balance tick; a transient disagreement is the design, a persistent one is
a bug.  Reporting all six together and demanding zero would have made "one
estimate briefly stale" indistinguishable from "the runqueue is corrupt".

Two commands drive it: `rqcheck` (one snapshot) and `schedstorm [rounds]`
(spawn/kill churn plus **four concurrent tasks** hammering `taskset` and `nice`
on a shared set of hogs, auditing every round).

#### cpu_home was a hint being used as a fact

`task.h` had documented `cpu_home` as "which CPU's rq this task lives on (or
-1)" since M18.6.1.  It was an aspiration: callers assigned it at moments when
they merely *intended* to place a task, under whichever lock they happened to
hold.  So it could name a queue that did not hold the task — and every
operation acting on "the queue `cpu_home` names" then mutated a ring while
holding the wrong CPU's lock.  Four sites did exactly that:

- **`rq_rotate_to_tail_locked`** tested `!t->rq_next || !t->rq_prev`, which
  answers "is it on *a* queue", not "is it on *this* one".
- **`task_set_affinity`** (reachable from the `taskset` command) read
  `cpu_home` unlocked and then called `rq_remove_locked` on that queue —
  a helper that does not verify membership, so it spliced the task out of a
  *different* CPU's ring and charged the removal to the wrong queue's counters.
- **`task_set_nice`** did the same for `rq_load`, subtracting and adding on a
  queue that need not hold the task.
- **`load_steal_one`** left the task on no queue while `cpu_home` still named
  the destination — a lie in the direction that hurts, because the block paths
  use `cpu_home` to find the queue to detach from.

`cpu_home` is now an **ownership token**: claimed by CAS from -1 inside
`rq_insert_tail_locked`, released by a store back to -1 at the *end* of
`rq_remove_locked`, both under the owning queue's lock.  The rule is one
sentence — *holding queue N's lock while `cpu_home == N` is exclusive
permission to touch that task's `rq_next`/`rq_prev`* — and it makes the value
checkable rather than merely well-intentioned.  §M54 had tried clearing it to
-1 in the steal and that HUNG TASKS; the reason is visible now, and it is
instructive: the matching *claim* was missing, so a queued task carried -1
forever and every detach silently did nothing.  **Half of this fix is worse
than neither half.**

The migration itself became one transition under **both** rq locks, taken in
ascending `cpu_index` order — the only place in the file that nests two, and
the ordering rule is stated there once instead of being reasoned about at every
reader.

#### The residual itself: DEAD was published too early

With the audit in place the reap report finally said something useful.  The
first version printed the task's state *after* the sweep had removed it, which
is the same for every cause and therefore says nothing — a measurement artifact
that cost one round of wrong theories.  Capturing it under the lock, before the
removal, gave the answer in one line:

```
reap: pid 25 'ks-victim' STILL QUEUED on cpu 3
      (home 3, next=set prev=set head=self state=2) — removed
```

The task is **properly linked**, `cpu_home` **agrees**, and `state=2` is
`TASK_DEAD`.  So this was never a corrupted ring at all.

`task_exit_code` marked itself DEAD at the top and then did a great deal of
preemptible work — waking joiners, tearing down the subtree, cancelling
timers — before sweeping the runqueues.  But `pick_next_local_locked` picks
only RUNNABLE tasks, so **from the instant a task calls itself DEAD it is
unschedulable, while it is still executing with interrupts on.**  One timer
preemption in that window and the task is switched away and never picked again:
it never reaches its own `rq_purge_all`, so it stays linked as a corpse — and
`task_reap` frees the struct and the kernel stack out from under a suspended
stack frame.

*A task is not dead while it is still running.*  Everything before the end is
work the task does while ALIVE, so it now stays RUNNABLE for all of it and
remains an ordinary preemptible task.  DEAD, the joiner wake, the queue sweep
and the final switch happen together with interrupts off, as one indivisible
step whose every part is bounded.  `me = this_cpu()` is re-read there too: the
value read at function entry may name a CPU this task has since migrated off,
and scheduling on another CPU's runqueue puts two CPUs on one stack.

Nothing regresses from publishing DEAD later — a parent in `task_wait` now
wakes when the child is genuinely finished rather than when it has merely begun
to finish, which is what the call always meant.

#### Measured

| | before | after |
|---|---|---|
| `killstorm`, 2880 kills, i386 -smp 4 | ~1 `STILL QUEUED` per 200–300 kills | **0** |
| `schedstorm`, ~420k churn ops | **hard-locks the box** (NMI, several CPUs) | **0 structural violations** |

The falsification matters more than the pass.  The first `schedstorm` drove
affinity from a single task and reported `ok` even against the pre-§M57 code —
because the only thing it could race with was the periodic balancer, which
fires every `LOAD_BALANCE_INTERVAL_MS`, so a 240 ms run offered about two
chances to hit a window measured in instructions.  **A test that cannot fail is
not evidence.**  With four concurrent churners it takes the pre-fix kernel down
with an NMI hard-lockup within a minute, and passes on the fixed one.

#### The log that shredded, and why it is a correctness bug

`printf.c` carried this line: *"Not reentrant; fine because the kernel is
single-threaded today."*  True when written, false since §M18 brought up SMP —
the §M52 shape exactly, a deferred note outliving its premise, because **a
comment cannot fail a test**.

The cost is not cosmetic.  Two CPUs formatting at once interleave character by
character, so a completed self-test reads as

```
kstorm-vict:im 20/2r0oun ds,id 240 spa0)wned, 240 killed, 0 alive
```

and every automated check in this project is a grep over the serial log.  One
`killstorm` run was read as a frozen shell on exactly this evidence, and the
run had passed.  *A harness that silently loses output is worse than none,
because it is trusted.*

`console_out_begin`/`console_out_end` now bracket a whole `kprintf` or
`console_write`.  Two properties carry the design:

- **Preemption is disabled, not interrupts.**  A message can be slow — a
  framebuffer scroll moves megabytes — and holding it with interrupts off drops
  timer ticks and can trip the softlockup watchdog.  A logging path that makes
  the machine look wedged is a poor trade for tidy output.
- **Re-entry is detected, not waited on.**  If this CPU already holds it (an
  ISR, an NMI, or a sink that itself prints), the message goes out unlocked.
  The owner test comes *before* the acquire, because `spin_lock` blocks: asking
  "do I already hold this?" after waiting for it is a question whose only
  answer is a hung machine.  Fault-context paths keep their existing lock-free
  channel (§M47).

#### Also fixed

- **`usock_set_owner` had no prototype** — added in §M56.2 and called across a
  translation unit, so the compiler assumed `int usock_set_owner()`.  It
  happens to pass two pointers correctly on every arch we build today, which is
  exactly the kind of luck an arch port later runs out of.
- Source comments labelled **§M57** that belonged to §M56.1, relabelled: one
  label meaning two unrelated bodies of work is a comment that misleads later.
- Two warnings cleared so the build is silent again and a real one cannot hide
  in the noise.

#### Still open

- `AARCH64_MAX_CPUS` ships at 2, so the ARM verification is genuine SMP but at
  two cores rather than four.
- `load_balance_pull`'s `mine->migrations++` counts an attempt whose insert can
  still be refused (the task went DEAD under us) — the figure is diagnostic
  only, and is now the only place in the file where a count can exceed the
  event it names.

---

### 4.59 A network that can hold more than one conversation (M24, second half)

**Shipped 2026-08-15, all three architectures.**  §M24 stages 1–3 gave this
kernel a NIC, ARP/IPv4/ICMP/UDP/TCP and a socket API in 2026-07; §M55 made
waiting for the network free; §M56 gave it poll and epoll.  What none of them
changed is that the transport underneath held **exactly one TCP connection, in
one file-scope struct**, could not accept an incoming one at all, and forgot
every byte the moment it left.  This is the rest of §M24.

#### What was actually wrong

- **One connection.**  `g_tcp` was a single `static struct`.  A second
  `connect()` silently destroyed the first.  §M56 had just built the machinery
  for watching many descriptors at once, over a transport that could hold one
  conversation.
- **No server role.**  There was no LISTEN, no accept queue, and therefore no
  way to test the half of the socket API every network program is written
  around — not merely untested, untestable.
- **A receive buffer that only grew.**  16 KiB of response and the connection
  stalled for good: nothing was ever reclaimed, and the advertised window was a
  constant that had stopped being true.
- **No send buffer.**  `net_tcp_send` put the caller's bytes on the wire and
  forgot them, so one dropped segment ended the conversation.
- **One device, hard-coded.**  Address, mask, gateway and nameserver were
  compile-time constants matching QEMU's SLIRP.
- **Two copies of the socket ABI** (one per x86 arch), and aarch64 built the
  whole portable stack with **no NIC driver at all** — every network feature
  this project shipped was untested on a third of its targets.

#### What is there now

**A loopback device (`kernel/drivers/net/loopback.c`).**  A real device, not a
shortcut: a frame goes through the full IPv4/TCP output path, sits in a queue,
and comes back through `net_rx` exactly as a NIC's would.  The queue is what
breaks the recursion — `transmit` runs with the stack lock held, and calling
`net_rx` directly would re-enter the whole stack on one C stack with a lock on
every frame.  It makes both endpoints ours: deterministic, no host network, and
available on an arch with no NIC.  `lo drop <permille>` makes it lose frames on
purpose, which is what turns retransmission from an untestable feature into a
tested one.

**Routing.**  `net_route(dst)` picks the device: 127.0.0.0/8 to the loopback,
otherwise a matching subnet, otherwise the default route.  `net_primary()` was
redefined as *the first non-loopback device* — with the old "first registered"
reading, adding `lo` would have silently handed every existing caller (ping,
DNS, wget, NetSurf's fetcher) a device that reaches nothing, on registration
order alone.

**A TCP connection table** (`TCP_MAX_CONNS = 32`, static): four-tuple
demultiplexing with a LISTEN entry as the fallback match — and the ORDER is
load-bearing, since a listener and its accepted connections share a local port.
Per connection: a receive RING (so the free space it implies *is* the window we
advertise), a linear send buffer with segmentation, real sequence arithmetic on
signed differences, and the RFC 793 state set through TIME_WAIT.  The table is
static because connections are created on the RX path, under the stack lock
with interrupts off, where an allocator call would nest the heap's lock inside
the network's on the one path that must not fail.

*Over the loopback every connection occupies TWO entries*, because both
endpoints live in this table — which is why the first `tcptest 8` came back 7/8
with one RST sent, and the stack was telling the truth about a limit.

**A server role.**  `listen`/`accept` with a bounded backlog, `SYN_RCVD`
children owned by their listener until accepted, and an RST for a segment that
belongs to no connection — that last one is why connecting to a closed port now
fails immediately instead of at the timeout, and is what gives poll/epoll the
POLLERR §M56.2 built.  A listening socket reports POLLIN when a connection is
waiting, which is the convention every event loop is written against.

**Reliability.**  One sweeper timer for the whole table (a per-connection
`ktimer` would be §M53's warning: an embedded timer must be cancelled at
exactly the right point in teardown), 200 ms RTO with exponential backoff,
Go-Back-N retransmission, zero-window probing, and a close that keeps working:
a connection whose owner has closed becomes an ORPHAN that keeps sending until
its queue drains and its FIN is acknowledged, up to a 10 s cap.

**The socket ABI became six canonical operations plus a table per guest**
(§M50): `ABI_SOCKET/BIND/CONNECT/LISTEN/ACCEPT/ACCEPT4/GETSOCKNAME/
GETPEERNAME/SEND/SENDTO/RECV/RECVFROM/SHUTDOWN/SET-GETSOCKOPT`, with ONE
`sockaddr_in` marshaller for all three architectures.  i386's `socketcall(102)`
became a pure demultiplexer into those same ops; the per-arch cases were
deleted rather than left as a shadow (§M56.1's rule).  `shutdown` stopped being
`return 0` and became a real half-close.

**DHCP** (`kernel/core/dhcp.c`): DISCOVER/OFFER/REQUEST/ACK, address, mask,
router and nameserver applied to the device, lease recorded, and T1 renewal
armed on a `ktimer` that hands the actual conversation to a §M49 worker —
because a DHCP exchange sends and then waits, and neither is legal in a timer
callback.  Off by default (`net.dhcp`), because SLIRP hands out exactly the
address the driver already hard-codes and every boot would pay for arriving at
the same four numbers.

**`/proc/net/{dev,arp,route,tcp,stat}`**, for which procfs learned one-level
subdirectories.  **AArch64 got a NIC**: `virtio_mmio_net.c`, the sibling of the
port's block driver, ~250 lines of transport after which ARM runs the same
stack the x86 arches do.


#### The wallpaper label learned to say two things

Milestones do not always finish in numerical order, and this one did not: §M24
was completed after §M57 shipped.  Neither obvious label was honest — `M57`
hides a milestone's worth of work, `M24` reads as a regression to anyone who
saw the wallpaper yesterday — so the label carries both and the relationship
between them: **`d-os M57 (updated M24)  x32`**.  `DOS_MILESTONE_NOTE` (empty
when there is nothing to say) is cleared when the next numbered milestone
ships, at which point the number is the newer news.

Verified the only way a drawn thing can be: a `screendump` of the running
desktop.  The first one still read `d-os M57  x32` — this project has no header
dependencies, so `version.h` changed and `gui.o` did not.  *A screenshot is the
test for anything that is drawn, and it caught a stale object file that every
source file in the tree contradicted.*

#### The bug this milestone found in the two before it

`wget http://example.com/` reported **`cannot resolve`**, and so did NetSurf —
while `nettest` said DNS was fine.  Both statements were true, and the gap
between them is the bug:

- **§M55** made the network poller (`netd`) run *only while somebody is waiting
  for the network*, which is what makes an idle box cost nothing.
- **§M56** made a finite `poll()` timeout a *real* wait instead of a snapshot,
  which is what makes an event loop stop spinning.

Neither knew about the other.  A task blocked in `poll()` on a socket **is**
waiting for the network, but nothing said so — so the poller stayed parked, no
frame was ever collected, and the wait ran to its deadline with the answer
sitting unread in the NIC.  musl's resolver sends its query and then polls; it
therefore never resolved anything again, and every ring-3 program that looks up
a name — `wget`, NetSurf's fetcher — stopped working.

**Nothing in the build could see it.**  `nettest` resolves through the KERNEL
resolver, which waits with `net_wait_cond` and thus counts itself.  The
regression lived entirely on the ring-3 path, between two changes that were
each correct and each verified.

The fix is two lines of contract: a readiness wait that touched a socket
registers as a network waiter for its duration (`net_waiter_enter/leave`), and
a pump that delivered frames wakes the readiness queue as well as its own —
*after* dropping the stack lock, because a poll waiter holds the readiness
queue while its scan takes the stack lock, and signalling from inside the stack
lock would be the opposite order.  Verified end to end: `wget` over HTTP and
HTTPS (TLS 1.3), and NetSurf reporting `HTTP 200, 571 body bytes` for
example.com.

*The general lesson: **two subsystems can each be right and compose into a bug,
and the test that would catch it is the one that uses the path a program uses,
not the path the kernel uses.***

#### Measured

| check | i386 | x86_64 | aarch64 |
|---|---|---|---|
| `tcptest 8` (server + 8 concurrent clients over `lo`) | PASS, 363 ms | PASS, 369 ms | PASS (4 clients), 404 ms |
| `tcploss 100 32` (32 KiB through a 10 % loss link) | PASS, 891 ms | PASS, 857 ms | PASS, 1113 ms |
| `nettest` (SLIRP: ICMP + DNS + HTTP) | PASS | PASS | ping + DHCP PASS |
| `netmuslserv` (bind/listen/accept via unmodified musl) | PASS | PASS | PASS |
| `dhcp` (against SLIRP) | 10.0.2.15/24, lease 86400 s | same | same |

#### Lessons

- **A test that cannot fail is not evidence — so build the way to make it
  fail first.**  The loopback learned to drop frames *before* the retransmit
  timer was written.  `tcptest` was checked against a deliberately shrunken
  connection table (2 entries): 0/4 clients connected, FAIL.  Without that, a
  passing run says nothing about a stack that can hold several connections.

- **Three bugs surfaced in the order a slow transfer is usually
  misdiagnosed**, and every one of them looked like "the network is slow":

  1. A **zero-window deadlock** — an ACK that only WIDENS the window carries
     the same acknowledgement number, so reacting to it only inside the
     "new data acknowledged" branch left the sender stopped.
  2. A **window probe that dug its own hole** — advancing `snd_nxt` for the
     probe byte means that when the receiver (which by definition has no room)
     drops it, everything after it is out of order, and only the RTO repairs
     the gap.
  3. **A FIN whose sequence number was inferred from `snd_nxt`** — a
     retransmission rolls `snd_nxt` back, after which an ACK covering only the
     data satisfied "have they acknowledged my FIN?".  The sender moved to
     FIN_WAIT_2 believing the conversation was over while the peer sat in
     ESTABLISHED; every byte arrived and the reader still blocked until its
     timeout.

  What found all three was **instrumentation, not inspection**: splitting the
  test's wall clock into "time in recv", "time asleep" and "time in send" turned
  8.5 s of mystery into "the 33rd read waited 8 s", and printing the connection
  table AT THE MOMENT OF THE STALL named the third bug in one line — after two
  wrong theories that a measurement had already ruled out.

- **A counter that lives on an object cannot measure a period longer than the
  object.**  The loss test summed per-connection retransmit counts and reported
  zero while seven timeouts had fired — the connections had ended.  The
  assertion uses a cumulative global now.

- **Syscall numbers are data, and data must be copied, not recalled.**  The
  i386 socket numbers are not sequential by name (360 is `socketpair`, between
  `socket` and `bind`), and reciting them from memory put `ABI_CONNECT` on
  bind's number.  The symptom was a `bind` that failed with ECONNREFUSED — the
  one errno that names the handler which actually ran.

- **A doubled struct field is a doubled struct layout.**  Adding `flags` to
  `struct net_device` and rebuilding without `make clean` produced
  `net: registered eth0 ... ip=255.255.255.0` — this project has no header
  dependencies and CLAUDE.md says so; the log caught it in one line.

- **The path the tests take and the path a person takes must be the same
  path.**  The ARM run script had no NIC; both it and the new harness attach one
  now (§M48's shape, one arch over).

#### Still open

- **No reassembly queue**: an out-of-order segment is dropped and duplicate-
  ACKed rather than held.  Correct, and it costs throughput on a lossy link.
- **No congestion control** (no cwnd, no slow start): the peer's advertised
  window is the only limit.  Honest on a loopback and on SLIRP; wrong on a real
  path with a bottleneck.
- **RTO is fixed at 200 ms**, not RTT-estimated — on links whose round trip is
  microseconds, an estimator would be measuring the emulator.
- The aarch64 NIC is **polled**, not interrupt-driven; the stack degrades to
  its timed fallback and `lsnic` says `rx-irq: none seen (polled)`.
- `sendmsg`/`recvmsg` are still per-arch: they carry control messages and file
  descriptors (SCM_RIGHTS — what Wayland runs on), whose `struct msghdr` is a
  row of guest-width words rather than a fixed 16-byte address.
- IPv6, multicast, a firewall, and zero-copy RX remain out of scope, as
  §M24 said from the start.

---

### 4.60 A window that resizes its contents too (§M42 follow-up)

Reported from use: *"I enlarge the NetSurf window, the window grows nicely, but
its content does not — it stays small."*  Every other window kind was fine.

**Why only that one.**  A resize allocates a new, larger content surface and
then has to tell somebody to fill it.  A terminal window re-renders its cell
grid; an app window's host task gets `layout_pending` and re-lays-out its
widgets.  A **client-managed** window — the dosgui bridge NetSurf renders
through — has neither: `host_task` is cleared by design (§M54: the compositor
must never reap a client init owns), so nothing consumed `layout_pending`, and
the bridge had no event that could carry a size.  The compositor grew the
window, the client kept presenting its original 800×600 image, and it landed in
the corner of a bigger surface.

**The fix is one event and one `realloc`.**

- `dosgui_event` gains **type 4 = RESIZE**, carrying the new content size.  It
  is reported the way the close event already is — by comparing window state
  inside `dosgui_poll`, not by queueing — because a resize is a LEVEL, not an
  edge: what a client needs is the size the window is NOW, and a drag that
  produced fifty events would hand it forty-nine stale ones.
- `user/netsurf/libnsfb_dos.c` turns it into `NSFB_EVENT_RESIZE`.  Everything
  downstream already existed upstream: fbtk's event loop calls `gui_resize()`,
  which reallocates through `nsfb_set_geometry` and re-lays-out the toolbar and
  browser widgets.  The vendored tree needed no patch — a framebuffer frontend
  normally runs on a screen, which does not change size, so nothing had ever
  sent it the event.
- **And `dos_set_geometry` had to start reallocating**, like the ram surface it
  was modelled on.  It changed the dimensions and left the buffer alone, which
  was harmless only because nothing could change them; the first real resize
  would have had NetSurf plot past the end of its heap block.  *A latent bug is
  a bug whose trigger has not shipped yet.*

**Verified by driving the mouse**, not by reasoning about it: `scripts/dos-shell-
test.py` grew a `--monitor-cmd` option, and the test homes the pointer, walks it
to the grip in ≤90-pixel steps (the PS/2 delta is a signed byte — §M48), presses,
drags and releases, then screendumps.  The first attempt missed the grip by 23
pixels and proved nothing; measuring the window's real rectangle out of the
"before" screenshot rather than deriving it from constants is what made the
test actually press the thing it was aiming at.  After: the window spans
1089×793 instead of 795×579, and the page text REFLOWS to the new width — a
scaled-up image would not.

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

This list stops at M6 and has not been the roadmap for a long time —
**PLAN.md is**, with a status table covering M1 through M49 plus the
design sketches and definitions of done.  `PLAN_AARCH64.md` carries the
ARM64 port's catch-up plan separately, organised by gap rather than by
milestone.  Kept here only so a reader who lands in this section is not
misled into thinking M6 is where the work ends.

---

## 8. Change log

- **2026-08-15 — §M24 SECOND HALF: a network that can hold more than one
  conversation (DOCS §4.59, all three arches).**  §M24's first stages shipped a
  NIC and a TCP/IP stack in July; what they left was a transport holding
  **exactly one connection in one file-scope struct**, with no server role, a
  receive buffer that only grew, and no memory of a byte once it was sent.
  §M56 had just built poll and epoll on top of it — machinery for watching many
  descriptors at once, over something that could hold one conversation.  Now:
  a bounded **connection table** with four-tuple demultiplexing, per-connection
  receive ring and send buffer, **listen/accept**, retransmission with
  zero-window probing, and a close that keeps working until its data and FIN are
  delivered.  A **loopback device** makes both endpoints ours — deterministic,
  no host network, available on an arch with no NIC — and `lo drop <permille>`
  makes it lose frames on purpose, which is what turned retransmission from an
  untestable feature into a tested one.  The **socket ABI became canonical
  operations plus one table per guest** (§M50), with a single `sockaddr_in`
  marshaller: i386's `socketcall` is now a demultiplexer into the same handlers
  amd64 and arm64 reach directly, the per-arch copies are deleted, and
  `shutdown` stopped being `return 0`.  Plus **DHCP** (address, mask, router,
  nameserver, T1 renewal through a §M49 worker), **/proc/net/**, and a
  **virtio-mmio NIC for aarch64** — after which ARM runs the same stack the x86
  arches do, closing an open item that had let every network feature ship
  untested on a third of the targets.  **Measured:** 8 concurrent connections on
  each arch; 32 KiB through a 10 % loss link intact, in order, on each arch;
  bind/listen/accept/getpeername/shutdown through an **unmodified musl binary**
  on each arch.  **Three real bugs, each of which looked like "the network is
  slow":** an ACK that only widened the window did not restart the sender; a
  zero-window probe that occupied a sequence number dug a hole only the RTO
  could fill; and a FIN whose sequence number was inferred from `snd_nxt` was
  declared acknowledged after a retransmission rolled `snd_nxt` back — sender in
  FIN_WAIT_2, receiver still ESTABLISHED, every byte delivered and the reader
  blocked until timeout.  **What found them was instrumentation, not
  inspection:** splitting the test's wall clock into recv/asleep/send turned 8.5
  s of mystery into "the 33rd read waited 8 s", and dumping the connection table
  AT THE MOMENT OF THE STALL named the last one in a line, after two wrong
  theories the measurements had already excluded.  Also: a per-connection
  counter cannot measure a period longer than the connection (the loss test read
  zero retransmits while seven timeouts had fired); i386's socket syscall
  numbers are not sequential by name and reciting them put `connect`'s handler
  on `bind`'s number; and adding a field to `struct net_device` without
  `make clean` produced `eth0 ... ip=255.255.255.0` — two layouts of one struct
  in one build, caught by the log in one line.  Open: no reassembly queue, no
  congestion control, a fixed RTO, a polled ARM NIC, and sendmsg/recvmsg still
  per-arch.

- **2026-08-12 — §M57: a task is not dead while it is still running (DOCS
  §4.58).**  §M54's own notes ended with *"the reap sweep still reports a queued
  task roughly once in several hundred kills"* — a confession, not a
  measurement, and it stayed one for two milestones because the only evidence
  was a log line arriving long after the event from the subsystem that merely
  noticed first.  **The invariant is now stated in code** (`task_rq_audit`, six
  rules) and driven by `rqcheck` + `schedstorm`, with rules 5 and 6 counted
  SEPARATELY because they have legitimate transients — folding a briefly stale
  estimate into "the runqueue is corrupt" is how a checker gets ignored.
  **`cpu_home` was a hint being used as a fact:** documented since M18.6.1 as
  "which CPU's rq this task lives on", it was assigned by callers at moments
  when they merely INTENDED to place a task, so it could name a queue that did
  not hold it — and four sites then mutated a ring while holding the wrong
  CPU's lock, including `taskset` and `nice`, both reachable from the shell.  It
  is an **ownership token** now: claimed by CAS from -1 inside
  `rq_insert_tail_locked`, released at the END of `rq_remove_locked`, both under
  the owning queue's lock, with one sentence covering every reader — *holding
  queue N's lock while cpu_home == N is exclusive permission to touch that
  task's links*.  §M54 had tried the release half alone and it HUNG TASKS;
  **half of this fix is worse than neither half.**  Migration became one
  transition under BOTH rq locks, ascending cpu_index.  **THE RESIDUAL ITSELF
  WAS NOT A CORRUPTED RING AT ALL:** with the diagnostic captured under the lock
  and BEFORE the removal (the first version printed the state the removal had
  just created — the same for every cause, and one round of wrong theories), the
  report read `home 3, next=set prev=set head=self state=2` — properly linked,
  cpu_home agreeing, `state=2` = DEAD.  `task_exit_code` marked itself DEAD at
  the top and then did a great deal of PREEMPTIBLE work, but
  `pick_next_local_locked` picks only RUNNABLE tasks, so **from that store the
  task was unschedulable while still executing with interrupts on** — one timer
  preemption and it was switched away forever, never reaching its own
  `rq_purge_all`, left in a runqueue as a corpse, and freed by the reaper with a
  live frame on its stack.  DEAD, the joiner wake, the sweep and the final
  switch are now one indivisible step with interrupts off, and `this_cpu()` is
  re-read there (the value from function entry can name a CPU the task has since
  migrated off — scheduling on another CPU's runqueue puts two CPUs on one
  stack).  **Measured: 2880 kills, 0 `STILL QUEUED` (was ~1 per 200–300);
  ~420k churn ops, 0 structural violations.**  **THE FALSIFICATION MATTERS MORE
  THAN THE PASS:** the first `schedstorm` drove affinity from ONE task and
  reported `ok` even against the pre-fix code, because the only thing it could
  race was the 100 ms balancer — *a test that cannot fail is not evidence*; with
  four concurrent churners it takes the pre-fix kernel down with an NMI
  hard-lockup and passes on the fixed one.  **AND THE LOG ITSELF WAS BROKEN:**
  `printf.c` said *"Not reentrant; fine because the kernel is single-threaded
  today"* — true when written, false since §M18, the §M52 shape exactly, because
  a comment cannot fail a test.  Two CPUs interleave character by character, and
  every check in this project is a grep over the serial log, so **one passing
  `killstorm` was read as a frozen shell on that evidence alone**.  Output is
  serialised now with PREEMPTION disabled rather than interrupts (a framebuffer
  scroll moves megabytes; holding it IRQ-off drops ticks and trips the
  softlockup watchdog) and with same-CPU re-entry detected BEFORE the acquire
  rather than waited on.  Also: `usock_set_owner` had no prototype since §M56.2
  (the compiler assumed `int f()`, correct by luck on today's arches); stale
  §M57 comment labels belonging to §M56.1 relabelled; two warnings cleared so
  the build is silent and a real one cannot hide.  Verified i386 + x86_64 at
  -smp 4 and aarch64 at -smp 2, full regression green on both x86 arches.

- **2026-08-11 — §M56: a wait that is really a wait (DOCS §4.57).**  `poll(2)`
  with a positive timeout was treated as a snapshot — documented as such, which
  made it sound conservative.  It is not: a program asking to wait 200 ms got an
  immediate 0, so **every correct event loop written against it became a busy
  loop**.  A timeout that returns early is not a safe approximation of one that
  waits; it is a different function.  It now arms a ktimer and blocks, like
  every other wait §M53 and §M55 built.  **One definition of readiness**
  (`fd_readiness`, shared by poll and epoll) replaced two, and extracting it
  immediately exposed that `FD_NETSOCK` had been falling through to "always
  ready" — so any loop polling an AF_INET socket spun — and that stdin was never
  ready at all.  stdin is now readable when a whole LINE is buffered, because
  cooked reads block until Enter and *a poll that lies about which reads will
  not block is the one thing a poll must never do.*  **New: epoll**
  (create/ctl/wait, level-triggered, kernel-resident set, caller cookie).  Said
  plainly in its own header: our `epoll_wait` still SCANS, so the asymptotics
  are poll's — the win is the interface, and claiming otherwise would be
  claiming an efficiency the code does not have.  `EPOLLET` is REFUSED rather
  than silently served level-triggered, which would "work" and spin.  poll and
  epoll share one blocking loop (`fd_readiness_wait`); two copies would be two
  chances to get the lost-wakeup rule wrong.  **The sharpest ABI trap yet:**
  `struct epoll_event` is 12 bytes on i386, **12 on amd64** (Linux packs it on
  x86_64 specifically so the 32- and 64-bit layouts agree) and 16 on arm64 — so
  its size does NOT follow the word size, and deriving one from the other passes
  on two arches and fails on the third.  `abi_map.epoll_event_bytes` carries it;
  the x86 layout also leaves `data` unaligned for a u64, so it is marshalled
  bytewise rather than relying on x86's tolerance.  Proven with an unmodified
  musl binary on all three arches: sizes 12/12/16, cookie
  `0x1122334455667788` intact everywhere (bits in both halves on purpose — a
  four-byte offset error would look plausible with a small integer).  Open:
  per-fd wakeups instead of a scan; generic `O_NONBLOCK` on VFS/pipe fds;
  `epoll_pwait`'s mask is ignored (the wait is real, the mask is not).

- **2026-08-11 — §M56.1: finishing it (DOCS §4.57).**  Four of §M56's five open
  items were things an event loop cannot work without, so they were completed
  rather than deferred.  **Hangup is visible without reading**: readiness is now
  a POLL* mask, with `POLLERR`/`POLLHUP`/`POLLNVAL` reported unrequested (POSIX,
  and the only other way to find EOF is the read the loop exists to avoid) and
  `POLLRDHUP` kept SEPARATE from `POLLHUP` — a closed writer with data still
  buffered reports `RDHUP|IN`, and only once drained `RDHUP|HUP`, so a reader
  finishes the tail instead of discarding it.  **`O_NONBLOCK` is generic**: it
  lived inside `struct netsock`, so setting it on a pipe did nothing and said
  nothing; it now lives on `struct ofile` where POSIX puts it, and an empty pipe
  with a live writer returns EAGAIN rather than 0 (zero means EOF, and a drain
  loop told EOF by a live pipe stops for good).  **An epoll set is pollable**
  so loops nest — which created a way to hang the kernel (two sets watching each
  other recurse with a lock held on every frame), closed with a bounded
  `task->epoll_depth`.  **`sigprocmask` is real** (it was `return 0`): a blocked
  signal stays PENDING and is delivered when unblocked, SIGKILL stays
  unblockable, `rt_sigpending` was added so "defer" and "discard" are
  distinguishable from inside the process, and `epoll_pwait` swaps the mask
  around the wait — the entire reason that call exists.  **Two bugs the new
  tests found:** (1) `ABI_SIGPROCMASK` had a working handler registered in the
  arm64 map ONLY, and §M50's engine declines unknown numbers and lets the old
  per-arch switch answer — where a stub returned "success, did nothing".  The
  implementation worked on arm64 and was unreachable on both x86 guests, with no
  error anywhere: *a fallback is only a fallback while nothing better exists;
  when something better arrives it must be removed in the same change.*  (2) A
  Linux `sigset_t` stores signal N at bit N-1 while this kernel stores it at
  bit N — both self-consistent, and copying the word across without shifting
  fails SILENTLY (SIGALRM lands on SIGCHLD's slot, so the mask looks set and
  never matches).  **The one item not built:** `epoll_wait` still scans, and
  `epolltest` now measures it — ~16 µs for 26 registered fds, ≈620 ns each under
  emulation, so `EPOLL_MAX_ITEMS = 64` bounds the worst case at ~40 µs by
  construction and a realistic loop pays ~6 µs.  Per-fd wakeups would buy none
  of that back at this scale and would cost a new lifetime relationship between
  epoll items and open file descriptions — §M54's defect class.  A measured
  decision, with a measured trigger for revisiting.

- **2026-08-12 — §M56.2: no bugs left behind (DOCS §4.57).**  `EPOLLERR` has a
  producer: a TCP **RST is not a FIN** — both end the connection, but one is an
  orderly EOF and the other is a broken connection, and without the distinction
  a refused or dropped connection looks exactly like a server that answered with
  nothing.  **Signal masks are read and written at their real width** (8 bytes,
  the `sigsetsize` every libc passes): this kernel has 32 signals and no
  real-time signals, so writing ZERO for bits 32–63 is the truth rather than a
  loss, and what the guest keeps beyond `sigsetsize` is left untouched.  **And
  `epoll_wait`'s scan was finished — by building the cache, measuring it, and
  removing it.**  Each item remembered (description, generation, answer); the
  lifetime problem was solved cleanly (the fd table is consulted every time, so
  a remembered pointer is only ever COMPARED, and generations come from one
  global sequence so a reused address cannot present a seen number).  It was
  still wrong, and the test written alongside it said so on the first run —
  `memo check: 20/20 ready, 0/20 idle`.  A cache like this is correct only if
  EVERY readiness-affecting state change bumps the generation, and the sites are
  not where intuition puts them: a pipe's readability changes when its OWNER
  reads, its writability when its PEER reads.  Two sites were missing on the
  first attempt.  *A cache whose invalidation must be remembered at every
  mutation site is a bug generator*, and the bug is an event that never arrives.
  It also bought nothing — 15.5 µs vs 16.4 µs for 26 descriptors, inside the
  noise, because the per-item cost is the lookup and the loop, not the readiness
  evaluation it skipped.  Replaced by `fd_readiness_of()`, which takes the ofile
  the scan already resolved instead of looking it up twice — plainly redundant
  work removed, and honestly labelled as the right shape rather than a proven
  win, because at this scale the benchmark is noise-dominated (16–25 µs).

- **2026-08-11 — §M55: waiting for the network without spending a CPU on it
  (DOCS §4.56).**  The network stack drove RX by polling from the calling task,
  so waiting for a packet cost as much CPU as computing flat out, N waiters were
  N tasks mutating one non-reentrant RX ring, and every timeout was a spin count
  that meant a different duration on every machine.  Now ONE poller task
  (`netd`) is the only caller of `dev->poll` and everyone else BLOCKS on the
  stack's wait queue until their condition holds or a real millisecond deadline
  passes.  netd runs exactly while somebody is waiting and is fully blocked
  otherwise — on a box that has never touched the network the task does not even
  exist.  The wait queue's lock is the stack lock, so `net_rx` and everything
  below it run with it held, which means **the RX path may never resolve an ARP
  entry**: replies now go back to the MAC the frame came from (the correct next
  hop by construction), and a TCP connection caches its peer's MAC once at
  connect time.  New `netstorm [n]`: **8 and 16 concurrent waiters both finish
  in ~3.4 s at 2–3% of 4 CPUs** — doubling the waiters changed neither figure,
  where the old model would have spun one core per waiter.  **A number that did
  not add up found a real bug:** the first storm on a fresh boot reported 143 115
  pumps and the second, doing identical work, 4 463 — `net_wait_cond`'s
  poller-not-up-yet fallback looped with the stack lock held and interrupts off,
  starving the very task whose arrival would end it; dropping the lock and
  yielding each round cut first-use cost to 4047 pumps, 2 of them inline.  The
  inline count is now reported separately by `lsnic`, because a rising figure
  there means the poller is not doing its job and that should be visible rather
  than inferred.  Two process lessons: the measurement that exposed this was one
  I nearly skipped because the number merely looked large, and the build behind
  the first explanation had FAILED — an `error:` slipped past my output filter
  and I read a stale ISO.  Check the exit status, not the output.
  **Part 2 — the NIC interrupt**, safe to wire only now (before part 1 it would
  have been the ISR plus every spinning waiter on one virtqueue).  The ISR acks
  the device — reading the legacy ISR status register is what deasserts the
  level-triggered line — and wakes the poller; it does NOT drain the ring,
  because draining runs `net_rx`, which can generate a TCP ACK, which spins on
  the TX virtqueue (§M49's xHCI lesson verbatim).  The stack learns interrupts
  work by RECEIVING one, so a driver that wires an interrupt which never fires
  degrades to polling instead of blocking forever on a promise; and netd never
  blocks indefinitely — a 10 ms backstop makes a missed interrupt cost latency,
  not liveness, while a sequence counter sampled before the pump and re-compared
  under the queue lock makes an interrupt arriving DURING that pump impossible
  to lose.  TX completion interrupts are suppressed (we wait for them
  synchronously) and `vnet_poll` notifies only when it recycled a buffer.
  Result: `nettest` 4982 → **26/39 pumps**, `netstorm 8` 4047 → **286 pumps at
  0% of 4 CPUs**, `missed 0` throughout.  **And the rake got stepped on again:**
  the first part-2 run reported `peak waiters 0` and a FAIL while 8 probes
  demonstrably finished in parallel in 2912 ms — I had added a struct field to
  `net.h` and rebuilt WITHOUT `make clean`, so net.c and shell.c used two
  layouts of one struct.  §M51's lesson, which CLAUDE.md states outright.  What
  caught it: the numbers contradicted each other, and *a self-contradictory
  measurement is evidence about the measuring apparatus.*  Open: the stack is
  still single-instance above the transport (one ping, one DNS query, one TCP
  connection); `epoll` and non-blocking file I/O next.

- **2026-08-10 — §M53 stage 3: a deadline you can wait on (DOCS §4.55).**
  `timerfd` (create/settime/gettime, read, poll-integrated) and `setitimer`
  (SIGALRM), on all three arches.  A timer behind a descriptor is what lets an
  event loop wait for time and for I/O in ONE place instead of choosing between
  being responsive and being punctual — the piece an `epoll`-shaped loop needs,
  which is why it landed before the async work.  A read yields the EXPIRATION
  COUNT and resets it, so a loop that fell behind learns how far; a timer that
  dropped uncollected ticks would let a program drift with nothing to notice.
  Periodic timers re-arm from the stored DEADLINE, never from `now`: re-arming
  from now adds each expiry's lateness to every later period, and since lateness
  is bounded by the tick but drift is not, it accumulates without limit.
  `timerfdtest` measures the error against the ORIGINAL start rather than the
  previous tick — a drifting timer looks perfect tick-to-tick — and shows it
  oscillating around the tick floor (~1 ms x86, ~10 ms aarch64 at 100 Hz)
  instead of growing.  The §M50 engine paid off as advertised: four canonical
  ops, four handlers and four table rows per guest gave all three arches
  `timerfd` at once — with one addition, `abi_map.word_bytes`, because
  `struct itimerspec` is four `long`s (16 bytes on a 32-bit guest, 32 on a
  64-bit one) and the width is a property of the GUEST, not something a shared
  handler may infer from the host.  Interval timers live in a pid-keyed table
  rather than in `struct task`: an embedded timer would have to be cancelled at
  exactly the right point in teardown, and getting that wrong fires a callback
  into freed memory — §M54's failure one layer up.  Delivery is an atomic
  pending-signal bit set from interrupt context, not a `sys_kill` (which takes
  the scheduler lock and applies a ring-3 credential rule).

- **2026-08-10 — §M54: a task the scheduler was still standing on (DOCS §4.54).**
  Reported as "open NetSurf, it crashes, reopen it, the machine dies"; the
  actual fault was in `pick_next_local_locked`, in the idle task of another
  CPU, several hops downstream.  Root cause: `schedule_locked` publishes the
  incoming task as `current` BEFORE swapping stacks, so between those points
  the outgoing task is current nowhere while a CPU is still executing on its
  stack — and both the "is this task running elsewhere" guard and the reaper's
  "is it current anywhere" check asked exactly that question.  One CPU resumed
  a task from a stale `esp` while another was still on it; the reaper freed a
  kernel stack that was in use.  Fixed with an `on_cpu` flag that spans the
  whole switch, released by the task that takes the CPU over (both arrival
  points — missing the brand-new-task trampoline was a bug in the first version
  of the fix).  Four more defects on the same path: a DEAD task could be
  enqueued (the check now lives under the destination queue's lock, one
  instruction before the insert); the exit path removed the task from the wrong
  queue or from none (`rq_purge_all` sweeps every queue, airtight because it
  runs after DEAD is published); `task_msleep`/`waitq_block` could dequeue
  themselves *after* a waker had already woken them, leaving a task awake,
  ready and on no runqueue — never scheduled again, with no trace; and the
  runqueue walks are now bounded and repair a broken ring instead of faulting
  in the scheduler.  New **`killstorm`** command on both shells reproduces the
  whole family in seconds (480 spawn+kill cycles across 4 CPUs, three runs
  clean on x86_64, 300 clean on aarch64) — a bug that needed a browser, a crash
  and a reboot is now a two-second shell command.  GUI half: a crashed dosgui
  client leaked its bridge handle permanently, so NetSurf could not be reopened
  after four crashes; disposal is now NOTIFIED (`gui_window_set_dispose_cb`)
  from every teardown route.  Diagnostics: x86_64 kernel-fault records now
  carry CR2 (they hard-coded 0), ring-0 dumps name the faulting task/pid/CPU,
  simultaneous dumps no longer interleave, and `crash_report` claims its ring
  slot atomically.

- **2026-08-09 — §M53 stages 1–2: time, in nanoseconds (DOCS §4.53).**  A
  monotonic nanosecond clock (`timer_now_ns`) and deadline timers built on it
  (`ktimer_arm`/`ktimer_cancel`, `task_sleep_until_ns`,
  `sys_clock_nanosleep_ns`), replacing "poll a millisecond counter every tick"
  with "tell me when this instant arrives".  Sources: `CNTPCT_EL0` on aarch64
  (62.5 MHz, 16 ns — the architecture defines the rate, so nothing to
  calibrate), the TSC on x86 (~1.2 GHz, 1 ns) but only once established to be
  constant-rate via `CPUID.80000007:EDX[8]` or a hypervisor bit; neither → keep
  the tick, because a coarse clock that is right beats a fine one that is wrong.
  **The clock found a real bug in its first minutes:** `ktime` reported a 100 ms
  sleep on aarch64 as 57 ms, because `timer_ticks_ms` divided a tick counter
  that EVERY CPU increments by a per-CPU rate — so the millisecond clock ran N
  times too fast on an N-CPU machine, and every timeout, watchdog deadline and
  sleep on that arch was wrong by the CPU count, silently, for want of a second
  opinion.  Now read from `CNTPCT` directly (100.0/105/106 ms at -smp 1/2/4);
  x86 was never affected.  **And the accuracy floor is measured rather than
  assumed:** expiry was first hooked into `schedule_check`, which looks like the
  tick but runs at the QUANTUM rate — every timer up to 10 ms late regardless of
  its deadline, a 500 µs sleep measuring 9.7 ms.  Moved to the tick ISR: worst
  lateness 9037 µs → **840–953 µs on i386**, which is the tick period itself
  (aarch64's is 10 ms, since it ticks at 100 Hz, and its `ktimer` says so).  New
  `ktime` and `ktimer` commands on both shells report the source, its
  resolution, and the error on a spread of sleeps — a timer list is easy to
  believe in, and lateness is the number that decides whether one-shot hardware
  deadlines are worth building.  Verified on all three architectures with the
  fork/pipe/signal/musl suite green.

- **2026-08-09 — §M52: the note that outlived its premise (DOCS §4.52).**
  x86_64's SYSCALL entry stub kept the kernel stack and the stashed user `rsp`
  in two GLOBALS, so two CPUs inside `syscall` at once overwrote each other's
  stash **and ran the kernel on the same stack** — each then returned to ring 3
  with the other's stack pointer.  The file's own header had said so since
  §M20.6.1 ("UP-correct only ... ring-3 tasks only run on the BSP today ...
  *Noted, not built*"), and every word was true when written; §M35 then gave
  x86_64 a per-CPU TSS and ring-3 tasks began running on APs, and nothing went
  back to the note.  **A comment cannot fail a test.**  Fixed with `swapgs` —
  the instruction that exists for exactly this: it swaps `IA32_KERNEL_GS_BASE`
  into `GS.base` atomically, so the stub reaches a per-CPU slot (`[gs:0]` kernel
  rsp, `[gs:8]` user-rsp stash) without needing a spare register to compute an
  address, and `syscall` leaves none — `rcx`/`r11` are clobbered by the
  instruction and everything else holds an argument.  GS is free because
  x86_64 musl keeps TLS in FS.  The stub swaps back before the shared
  `isr_common` tail, so nothing else in the kernel needs to know.  **Verified:
  x86_64 clean at `-smp 1`, `-smp 2` and `-smp 4`, with `forktest`, `pipetest`,
  `musltest` and the multi-command shell all green; i386 unaffected and still
  green at `-smp 4`.**  **Why it hid: three independent immunities.**  i386
  enters through `int 0x80`, and an interrupt gate switches stacks via the TSS,
  per-CPU since §M35.  Native d-os programs (`forktest`/`forkexec`/`pipetest`)
  use `int 0x80` too and passed at `-smp 2` throughout — only *musl* binaries
  issue `syscall`, because musl hard-codes the instruction and not patching musl
  is the whole point of the personality.  And a single musl process rarely
  collides with itself, so `musltest` passed; it took TWO musl processes — a
  shell and the coreutil it forks — to break.  **The general lesson: a deferred
  note is a dependency on a premise, and nothing in the build checks that the
  premise still holds.**  Both milestones that invalidated it were green.

- **2026-08-08 — §M51: the broadcast x86 does not have (DOCS §4.51).**  x86 does
  not broadcast TLB invalidation — `invlpg` and a CR3 reload are strictly local
  — and nothing in the tree ever sent an invalidation IPI.  Copy-on-write's
  entire safety argument is "the next write faults", so a parent runnable on a
  second core during the window between `fork` and the child's `execve` kept a
  WRITABLE entry for pages `fork` had just protected: its next write did not
  fault, it landed in the frame the child was sharing, and the two processes
  corrupted each other with no fault and no log.  That is why
  `pkgrun sh -c "echo A; echo B"` failed ~2 runs in 3 at `-smp 2` and passed
  every time at `-smp 1`.  New `kernel/hal/x86/tlb.c` (shared by both x86
  arches) does the broadcast with IPI vector 0x42 and a per-CPU ticket pair —
  no lock, no request slot, because the remote action is always a FULL flush,
  which makes overlapping requests harmless; the ack is published only after
  the flush; and **the wait loop services its own slot**, without which two
  simultaneous shootdowns from interrupt-disabled contexts wait on each other
  forever.  AArch64 needs none of it (`tlbi ...is` is a hardware broadcast), so
  its `hal_tlb_shootdown` is an empty function with the reason written down.
  Only WEAKENING edits pay: a remap over a present entry, unmap, mprotect and
  the COW resolution broadcast; a fresh map does not, and that distinction is
  what keeps `map_in_pd` (once per page of every ELF load) affordable.
  `vmm_space_clone` suppresses the per-page broadcast entirely and issues ONE
  whole-space shootdown at the end — the first version broadcast per page and
  turned a fork into thousands of IPI round trips that never finished.
  **Verified: i386 clean at `-smp 1`, `-smp 2` and `-smp 4`.**  **Lesson: the
  harness lied for an hour.**  After adding fields to `struct percpu` the
  shootdown appeared to hang — the target CPU was alive (tick/switch counters
  advancing) yet never took the vector, and its `apic_id` read back as 3 on a
  box whose boot log said 1.  This project has no header dependencies and
  CLAUDE.md says to `make clean` after editing a shared header; I had not, so
  half the tree used the old `struct percpu` layout.  Every measurement in that
  window was fiction and the first run after `make clean` passed.  *A
  documented build convention is a correctness convention, and an impossible
  measurement is evidence about the build, not about the code.*  **Open:
  x86_64 still fails at `-smp` ≥ 2 and it is NOT this bug** — `-smp 1` is clean,
  `-smp 2` takes a kernel `#GP` inside `copy_str` while `execve` marshals argv,
  and removing the shootdown wiring reproduces the same failure, so M51 neither
  caused nor fixed it.

- **2026-08-08 — AArch64 A3: a shell that forks and execs, and the register
  nobody saved (DOCS §4.50).**  `pkgrun sh -c "echo A; echo B; echo C"` prints
  all three on ARM — a musl shell forking musl coreutils out of the store, on
  the third architecture (`-smp 1` and `-smp 4`).  The predicted work (a cross
  toolchain, arch-parametric recipes) was an afternoon; the stage was held up by
  **per-task state kept where nothing saves it**, twice: `TPIDR_EL0` (EL0 writes
  it directly with one `msr`, so `has_tls` is never set and a forked musl child
  dies at `TP - 0xc8`), and then **`SP_EL0`** — banked, absent from the
  trapframe, written by the kernel exactly once at the `eret` into EL0, and
  never saved by `context_switch`.  A task that blocked at EL0 resumed with
  whichever task ran there next, so `sh -c "echo one"` printed `one` and *then*
  the shell died at a wild PC — a shell's first schedule-out at EL0 is its
  `waitpid`.  x86 cannot have this bug: the user SP is a field in the frame the
  CPU pushes.  **The rule to carry to A4 and to any new arch: enumerate the
  registers the kernel writes once and the user owns thereafter; every one of
  them belongs in the switch.**  Found by narrowing, not guessing — a
  never-free experiment, a `-smp 1` run, a canary in the trapframe (which proved
  the frame INTACT, killing every theory about the return path at once), `ESR`
  in the fault print, and finally a dump of the user stack, which showed kernel
  frame data at user addresses and named the answer.  `ESR`, `SP_EL0` and the
  task name are now permanent in that print.  Three pre-existing defects fixed
  on the way: `cow_release` decremented the LAST reference without freeing
  (every fork-shared page leaked); `vmm_space_clone`/`vmm_cow_fault` used the
  CPU-local `tlbi vmalle1` where a sibling core keeps a stale writable entry
  (now `vmalle1is`); and the shared `ABI_WAIT` handler returned the raw exit
  code instead of the `(code & 0xFF) << 8` status word a guest reads through
  `WIFEXITED`, unvalidated.  **Build:** the per-arch artifact cache filed
  `user/*.muslelf` by `build/.last_arch` — a hint, wrong whenever `make` runs
  directly — and had parked an AArch64 `sh.muslelf` in the x86_64 slot, from
  where it was linked into the x86_64 kernel and surfaced as `rc=-7`
  (`ELF_EBADARCH`); 27 cached artifacts were mis-filed.  The cache now keys on
  the file's own `e_machine`: **ask the file, not the stamp.**  **Open and
  measured, not fixed: a multi-command `sh -c` fails on BOTH x86 arches** —
  invisible until now because the x86_64 blob was the wrong architecture, and
  i386 fails too, so §M36's "multi-command sh works" needs re-establishing.

- **2026-08-07 — AArch64 A2: unmodified musl runs on ARM, in ~80 lines
  (DOCS §4.49).**  `hal/aarch64/linux_abi.c` is ~80 lines against 1211 and 1064
  for its x86 siblings — not less capability, but §M50's engine landing first,
  so all that remains is x8 = number, x0..x5 = args, result in x0.  `musltest`
  passes with **zero unhandled syscalls**: the vocabulary grown for x86 was
  already enough for an ARM musl startup.  **The trap PLAN_AARCH64 predicted for
  A6 arrived at A2:** musl's `memset` opens with `dup v0.16b, w1`, so libc
  startup trapped on NEON — presenting as an EL0 fault with `FAR_EL1 = 0`, which
  reads exactly like a null dereference and is nothing of the kind.
  Disassembling the faulting address settled it in one step.  `fpu.c` had
  described this failure and both halves of the fix in advance; both are now
  implemented — `CPACR_EL1.FPEN` enabled **per CPU** (BSP and AP; enable it on
  one core only and FP works there and traps on the other) and Q0..Q31 + FPCR/
  FPSR saved on context switch.  A1's `forktest`/`sigtest`/`pipetest` still pass
  with FP live on the switch path.  Also: A2's proof needed the toolchain half of
  A3, so the plan's ordering was wrong — cheap to fix, since the prebuilt-fetch
  script was already arch-parametric.

- **2026-08-07 — §M50 started: one guest-ABI translation engine instead of one
  per architecture (DOCS §4.48).**  The two Linux personalities were 2275 lines
  and ~160 `case` labels of the same idea, and aarch64 would have been a third.
  Linux numbers `read` 3 on i386, 0 on amd64 and 63 on arm64 — same meaning,
  different data.  New pipeline: arch shim (frame → argument vector) → per-guest
  number map → canonical operation → shared handler.  A new architecture is six
  lines, a new guest ABI is a table, a new syscall is one handler that every
  arch gets at once.  The engine may DECLINE, with the hand-written switch as
  fallback, so 2275 lines migrate one operation at a time rather than in one
  unverifiable jump.  Both x86 arches now serve read/write/close/seek/mprotect/
  munmap/getpid/getppid through it with their musl userland unchanged
  (`musltest`, `solibtest`, `crypttest` pass on both).  New `abi` command prints
  the three number spaces side by side.  PLAN §M50 records the analysis of
  whether this can reach a Windows guest: the pipeline generalises but the cut
  point does not — NT syscall numbers are not a contract, which is why Wine cuts
  at the DLL boundary, and the hard part is semantics (HANDLEs vs fds,
  CreateProcess vs fork, SEH vs signals), not numbering.

- **2026-08-07 — AArch64 A1: a POSIX process model on ARM (DOCS §4.47).**
  M21's "full x86 parity" was true when written; §M34's fork and signals landed
  on x86 afterwards and were never carried across.  `PLAN_AARCH64` scoped the
  catch-up as "mirror `hal/x86_64/fork.c`" — measured, that file was about a
  quarter of it: the port also had no aarch64 `struct user_regs` (it silently
  used the **i386** one, `eax`/`ebx` at 64-bit width), no `enter_user_mode_regs`,
  and no decode of a data abort into a copy-on-write resolution.  Now:
  `vmm_space_clone` marks both sides read-only + `PTE_SW_COW`, `vmm_cow_fault`
  privatises on first write (refcount table sized from `pmm_nr_frames`, per
  §M48), `fork.c` + `signal.c`, and `SYS_FORK`/`WAITPID`/`EXECVE`/`PIPE`/`DUP2`/
  `KILL`/`SIGACTION`/`SIGRETURN` in the dispatcher.  Three ARM-specific traps
  worth remembering: **SP_EL0 is not in the trapframe** (EL0 exceptions switch
  to SP_EL1 and leave it banked, so fork and signal delivery read it with
  `mrs`); **the signal return address is a register**, not a stack slot, so the
  trampoline goes in x30 and sigreturn finds the saved context exactly at the
  user SP; and **COW must be resolved before the uaccess fixup**, or a kernel
  write into a forked child's buffer unwinds as `-EFAULT` instead of copying the
  page.  Verified over the ARM serial shell (`forktest` — `secret still=111`,
  i.e. real isolation — plus `pipetest` and `sigtest`).  Also fixed: the
  teardown path truncated 64-bit physical addresses to `uint32_t` before freeing
  them.  Open on this arch: `proc_clone_thread` (A4) and the Linux-ABI
  personality (A2), without which no musl binary runs at all.

- **2026-08-06 — §M49: load distribution across CPUs, measured (DOCS §4.46).**
  §M18.6.1's load balancer only ran when a CPU's runqueue went empty — a
  work-stealing rule, not a distribution rule — so with every queue non-empty an
  arbitrarily bad split never corrected itself.  The file's own header comment
  described a periodic pass and named a constant that existed nowhere in the
  tree.  **`run_qemu.sh` passed no `-smp` at all**, so the balancer never
  executed on the path a person actually uses; every SMP test supplied its own
  flag (the §M48 missing-NIC shape again).  New `sched [ms]` command samples the
  spread; on 4 CPUs, five hogs pinned to CPU0 got 15-20% of a core each while
  singletons got 66% — a 3.3x unfairness that aggregate busy% (100% everywhere)
  cannot see.  Fixed in three parts: a periodic threshold pass with an
  anti-ping-pong margin; load measured as **demand** (share of time spent
  RUNNABLE) instead of queue length, since four hogs and four sleepers are the
  same queue depth; and `task_msleep` made to really block — it was a spin-yield
  loop that kept every service task queued *and* halted the CPU with work behind
  it, which is why `cron` and `watchdog` measured as CPU hogs.  A metric is only
  as honest as the state it observes.  `task_kill` now wakes a timed sleeper, so
  kill latency stays what §M46 teardown assumes.  Queue spread 2..6 → 2..3; on
  x86_64 seven of eight hogs settle at the ideal 49-50%.  Also **priority**:
  `nice <pid> <-20..19>` → a weight that is both the task's quantum budget and
  its share of runqueue load (measured 65%/16%/13%/4% for nice -10/0/0/+10),
  degenerating to the old behaviour at the default.  Its first boot took a
  divide error — `struct task` is built in four places and only one is
  `spawn_common`, so three kept a zero weight; one shared initialiser plus a
  guard at the division.  **`vc_getchar` and init's reaper now block too**, so
  an idle 4-CPU box went from one core pegged at 100% to all four at 0-2%; that
  needed `task_kill` to wake waitq-parked tasks (new `task->wq` back-pointer),
  and it exposed a latent SMP race — the boot shell's VC was bound *after*
  spawn under `preempt_disable`, which is per-CPU, so another core could run the
  shell first and it exited with "no VC bound" (four sites; fixed with
  `task_spawn_console`, which binds it inside the spawn as `start_arg` already
  was).  Finally a **deferred-work pool** (`kernel/core/workqueue.c`,
  `work_submit`/`work_flush`, one worker per CPU, blocking so an idle pool costs
  nothing): `wqtest` runs 16 items across 4 CPUs in 26 ms against ~80 ms serial.
  Its first test asserted no duplicate runs and failed against the contract
  written beside it — re-queueing an item that is already RUNNING is the
  intended semantic.  **First production consumer: the xHCI event-ring drain**,
  which ran inside the timer IRQ (MMIO ring walk + HID decode + a task wake) and
  now only submits from there — one change covering both arches, and it closed a
  latent bug on the way: `evt_drain` is not reentrant and the ISR could swallow
  the command completion `cmd_submit_wait` was waiting for.  Verified with a USB
  keyboard attached (`usb-hid: first key delivered over USB`, a marker added
  because every PC target also has PS/2 and "typing works" proves nothing).
  NIC RX is next and needs `net.c` locked first — the stack is single-task by
  construction.

- **2026-08-04 — §M48: the memory ceiling is discovered, not compiled in; NetSurf
  becomes usable; Mesa reaches i386 (DOCS §4.42–§4.44).**  `pmm_init` sizes its
  metadata from the firmware map instead of a per-arch `#define`, so one image
  boots on 128 MiB and on 128 GiB — verified on x86_64 at 1G/2G/3G/4G/8G/128G
  with userland running and zero faults.  Physical addresses widened to arch
  width; seeding emits maximal aligned blocks instead of releasing 33.8 million
  frames one at a time; new `ZONE_DMA32` keeps device buffers under 4 GiB now
  that "any frame" and "a frame a 32-bit device can reach" have stopped being
  the same thing.  **x86_64 userland turned out to be broken on any machine with
  more than 1 GiB of RAM** — the identity map's 1 GiB page landed on top of the
  user region and every `exec` returned `ELF_ENOMEM`; the port only looked
  healthy because it was always tested with `-m 1024M`.  Fixed by moving the
  kernel's physical window to a direct map in the upper half.  Four more latent
  bugs surfaced and were fixed: slab's 32-bit page mask, the COW refcount
  table's 1 GiB window (a double free once exceeded), ACPI identity-mapping its
  tables into user space, and i386 ring 3 being unable to execute SSE.  i386's
  identity map now runs to 1 GiB (234 → 473 MiB usable on a 512 MiB box); past
  that the limit is real, and 64 GiB there is exactly the PAE maximum.  NetSurf
  gained mouse-button events (the compositor had none, so nothing was ever
  clickable), cooked characters (typing was raw scancodes rendered as text), and
  an http/https fetcher over d-os sockets + Mbed TLS with real certificate
  verification.  `run_qemu.sh` now attaches a network card — its absence is why
  no site would load however well the fetcher worked.  `egltri win` renders on
  i386 too.  New: `PLAN_AARCH64.md`, a measured catch-up roadmap for the ARM64
  port.

- **2026-08-03 — §M40 COMPLETE: EGL + GLES2 render through a Wayland buffer.**
  `egltri win` opens a d-os desktop window with a spinning GLES2 triangle drawn
  by Mesa's softpipe rasteriser (`OpenGL ES 3.1 Mesa 23.1.9`), presented over the
  §M26 `wl_shm` path.  Mesa cross-built for musl against our own wayland/libdrm
  (`make mesa`), libraries embedded and laid into `/lib` at boot.  Two fixes made
  it work, both in DOCS §4.40.1: libwayland had to become a real SHARED object
  (libEGL had statically absorbed it — two protocol object tables in one process
  crashed the first event dispatch, and `wayland-scanner private-code` hid the
  symbols that must be shared), and **`mincore` had to stop lying** — it was
  stubbed as advisory alongside `madvise`, but its return value IS the answer, and
  reporting "mapped" for every address made Mesa dereference address 3.  Now
  implemented truthfully on i386 + x86_64.  x86_64 only, as the milestone scopes.

- **2026-08-02 — FIX: a syscall now runs with interrupts ENABLED (the freeze +
  reboot when launching NetSurf from the desktop).**  Reported symptom: start
  the GUI, launch NetSurf from the Start menu, the window appears — then the
  machine freezes and reboots.  Reproduced by driving the Start menu from the
  QEMU monitor's mouse, and diagnosed to a single call chain:
  `linux_syscall_dispatch → dosgui_present → gui_window_blit → spin_lock` on
  `windows[0].lock`, spinning forever.
  **Root cause, and it was never really about the GUI.**  Both x86 syscall entry
  paths arrive with IF clear — i386 through an interrupt gate (0xEE), x86_64
  because `IA32_FMASK` clears IF on `SYSCALL` — and nothing re-enabled it.  So
  every system call ran non-preemptibly from start to finish.  That is not just
  a latency issue but a correctness one: a `spin_lock` taken inside a syscall
  can NEVER be resolved on a uniprocessor, because the task holding the lock
  cannot be scheduled while we spin with interrupts off.  Any contention on any
  lock, in any syscall, was a guaranteed hard lockup; NetSurf's present syscall
  contending with the compositor on a window lock is simply the first path that
  hit it in practice.  The ~4 s ib700 watchdog then fired an NMI, classified it
  as a kernel-mode lockup and rebooted — hence "freeze *and* restart".
  Fixed identically on all three arches (`hal_intr_enable()` before the syscall
  dispatch in `hal/x86/idt.c`, `hal/x86_64/idt.c` for both the `int 0x80` and
  the `SYSCALL` sentinel, and `hal/aarch64/exceptions.c` for `svc`) — the bug
  was arch-independent even though it surfaced on x86_64 first, so the treatment
  is deliberately the same everywhere rather than a targeted patch on the arch
  that happened to show it.  Safe because every user task has had its own kernel
  stack since Tier B (being preempted mid-syscall just parks that stack), and
  the force-kill safe point only fires for contexts interrupted in ring 3, so a
  task inside a syscall is never torn down half-way through one.
  **Why it stayed hidden, and the tooling fix that follows from it.**  The
  kernel already had a deadlock detector that prints the lock address and the
  caller — but its threshold (400M spins) was calibrated on i386 and did not
  fire within the ~4 s watchdog window on x86_64.  The system knew exactly what
  was wrong and never got to say it.  Lowered to 20M (measured to land well
  inside the window); a false positive costs only a log line, since the detector
  never changes behaviour.  With the watchdog removed from the QEMU command line
  the same run printed the diagnosis immediately — *if a diagnostic can lose a
  race with a recovery mechanism, it will.*
  Verified: the exact reproduction (GUI → Start → NetSurf) now goes from
  `NMI HARD-LOCKUP: 2, rebooting: 1, booted: 2` to all-zero and a single boot,
  on **both** i386 and x86_64, with the browser rendering.  Regression at
  `-smp 2`: `faulttest`, `fputest`, `archtest`, `fdtest`, `socktest`,
  `polltest`, `solibtest`, `forktest`, `threadtest`, `musltest`, `dnstest`,
  `httptest` (`HTTP/1.1 200 OK`) and `pkgrun sh -c` all green; aarch64 builds
  clean.

- **2026-08-03 — §M40: an UNMODIFIED upstream Wayland application runs on d-os
  (DOCS §4.40).**  `weston-simple-shm`, weston's own reference client, compiled
  exactly as it sits in the weston tree, animates continuously in a real d-os
  window (716 frames in one i386 run) — its shm pool descriptor arriving over
  SCM_RIGHTS and its frame-callback loop driven by the server.  Getting there
  needed stages 3–6: the surface mapped onto a `gui_window` sized to the client's
  buffer, real desktop input routed into the client's `wl_seat` (which also
  fixed NetSurf's input — a hook-backed window had no app-host draining its
  queue), `wl_output` + `wl_surface.frame` (the two globals a toolkit refuses to
  start without), and accepting `set_app_id` + `wl_shm_pool.destroy`.

- **2026-08-03 — §M40 stage 2 complete + the Linux-ABI pointer gate (DOCS §4.40,
  §4.41).**  The upstream libwayland client now drives a real `xdg_toplevel` with
  a real shm buffer on both arches, and the server reads its pixels out of
  shared memory (`top-left=ff102040`, the colour the client wrote).  Needed:
  SCM_RIGHTS in both ABI control paths, `memfd_create`/`ftruncate`, memfd
  mapping from the Linux `mmap` entry point, `wl_surface.damage`, and a real
  `F_DUPFD`/`F_DUPFD_CLOEXEC` — libwayland dups every descriptor it sends, and
  our `fcntl` "succeeded" by returning 0, which **is a valid descriptor**, so the
  pool silently arrived carrying fd 0.  `sys_dupfd` must also skip 0–2, which
  are reserved for the console and absent from the fd table.
  **Bigger find:** `linux_syscall_dispatch` never armed `task->in_user_syscall`,
  so §M46's first boundary layer was disabled for the ENTIRE musl userland.  Now
  armed on both arches, with the established `_k`/`_u` discipline for the places
  that legitimately pass kernel buffers (adds `sys_recv_u`).  Verified with the
  gate on: musltest, `pkgrun sh -c`, HTTPS, `tcc`, `wayupstream`, and NetSurf
  launch+close clean.

- **2026-08-03 — §M40 stage 1: UPSTREAM libwayland-client runs on d-os (DOCS
  §4.40).**  The real, unmodified libwayland — not our §M26 mini client library —
  completes `wl_display_connect` + `get_registry` + a listener +
  `wl_display_roundtrip` against the d-os Wayland server, on both i386 and
  x86_64.  `make [ARCH=…] wayland` cross-builds libffi (needed for libwayland's
  `ffi_call` event dispatch) and the client library, with `wayland-scanner`
  generating the protocol code on the host; the vendored tree stays pristine
  (the build runs in a scratch copy because wayland `#include "../config.h"`,
  which meson would normally generate at the tree root).  The client connects via
  `WAYLAND_SOCKET` — upstream's documented already-connected-fd mechanism — which
  needed one new kernel seam, `proc_set_exec_env()`: one `KEY=VALUE` handed to
  the next exec on the calling task and consumed by it.  **Bug found on the way:**
  both Linux-ABI layers implemented recvmsg/sendmsg for AF_INET only, so
  libwayland's first read on the UNIX display socket returned a bare -1 that musl
  turned into EPERM; both now route by `sys_fd_kind()`.

- **2026-08-03 — closing a window is not a crash (DOCS §4.38.1).**  Clicking the
  title-bar X on NetSurf force-killed the client immediately and therefore
  recorded a crash + popped the Crash Reports window, as though the browser had
  died.  M46's force-kill is the fallback for a WEDGED client, not the close
  path, and the escalation is the USER's: the FIRST X click asks the client to
  close, the SECOND forces it immediately.  `gui.close_grace_ms` (now 10000 ms)
  remains only as the unattended backstop.  Healthy clients close on one click
  with no record; M46's "the X always closes the window" guarantee is unchanged.  Adds `user/wedgewin.c` + the
  `wedgewin` command — a client that opens a real window and then freezes,
  which is the automated test that guarantee never had (and whose absence is why
  the regression went unnoticed).  Both cases verified on x86_64.

- **2026-08-03 — musl `getaddrinfo` fixed (DOCS §4.39).**  Two pre-existing
  defects, found by tracing what musl's resolver actually did.  (1) We discarded
  `SOCK_NONBLOCK`; musl drains its socket with `while (recvmsg(...) >= 0)` and
  needs the EAGAIN a non-blocking socket produces, so the blocking path sat in
  its 40M-iteration bounded spin — minutes on emulated i386, which is why the
  bug looked arch-specific.  Sockets now carry a `nonblock` flag honoured by the
  datagram + stream receive paths and by both Linux-ABI layers (`SOCK_NONBLOCK`
  and `fcntl(F_SETFL/F_GETFL)`).  (2) `hostorder_to_sockaddr` validated its
  `addrlen` as a ring-3 pointer, but the recvmsg caller passes a kernel word —
  so it returned without writing `msg_name`, and musl DROPS any DNS reply whose
  source address does not match a queried nameserver.  Every answer was
  discarded silently.  Third instance of the same lesson as the `sys_*_k` cores
  and `linux_sendmsg`.  Verified on both arches: TLSv1.3 + CA verification,
  `wget` downloading a real page, `netmusl` over plain HTTP.

- **2026-08-02 — x86_64 userland parity (DOCS §4.39).**  x86_64 now runs the
  same userland i386 does: musl coreutils + a real `sh`, ring-3 BSD sockets,
  threads/TLS/signals, Mbed TLS (crypto + TLSv1.3 + HTTPS with CA verification +
  `wget`), and the on-device TinyCC compiler.  The gap was never missing kernel
  support — it was duplication that had fallen behind: the objcopy blob symbols
  carried the arch in their NAME (so every in-tree program was i386-only by
  construction, linking fine on x86_64 while the shell said "not embedded for
  this arch"), the program lists were written out twice, and the x86_64 syscall
  dispatcher stopped at M25.  One pattern rule + `--redefine-sym` + shared lists
  + the missing dispatcher cases fix all three, and `hal/x86_64/signal.c` fills
  in delivery/sigreturn.  Four 32-bit assumptions surfaced and were fixed:
  crt0 never read argc/argv, `thread_create` passed its argument the cdecl way,
  `tls_load4` hard-coded both `%gs` and a 4-byte field offset, and virtio-net +
  AC97 were missing from the x86_64 source list.  `make mbedtls` / `make tcc`
  are now arch-aware.  **Also fixed, pre-existing on i386:** `linux_socketcall`
  validated its argument array as a user pointer while the direct socket
  syscalls hand it a kernel array — every direct call returned -EFAULT and musl
  does not fall back on -EFAULT, so `socket()` failed outright.  Split into a
  `_k` core + a gated wrapper (same lesson as the `sys_*_k` cores).  Verified by
  boot-testing both arches; the previous commit was rebuilt to confirm the
  socketcall break predated this work.

- **2026-08-02 — §M47 stage 2: `/proc/crash`, the GUI report window, a wider
  breadcrumb (DOCS §4.38).**  Stage 1 built the seam; this is the proof that
  adding a destination costs one file and zero fault-path edits.
  **`/proc/crash`** renders the ring machine-readably — `captured`, `ring`, one
  `sink` line per registered sink, then one line per record newest-first.  Read
  without a lock, exactly like the capture side and for the same reason: a
  reader able to block a fault handler would invert the priority this subsystem
  exists to protect.
  **The Crash Reports window** (`gui/apps/crashapp.c`) is a completely ordinary
  GUI app — listview, 1 Hz tick, click a row for the full detail — plus eight
  lines of `CRASH_SINK("gui-report", …)`.  The sink runs on the watchdog task,
  so it must not build a window itself; it pushes an app launch onto the
  compositor's lock-free queue and the compositor opens it on its own schedule,
  which also means a wedged compositor cannot be made worse by a crash report.
  Gated by `crash.report` (default on), read per-record so the window can be
  armed or silenced at any time without a reboot.  **Naming note:** it is a
  *report*, not a "popup" — a record has one representation and the surface is a
  detail; the window is a view, never the storage, and switching it off silences
  the interruption, never the recording.
  **The NVRAM breadcrumb grew 24 → 40 bytes**, adding the faulting address, the
  signal/exception code and the uptime at capture, so a boot that died where
  nothing could log it is reconstructed as a first-class record instead of a
  bare "something happened".
  Verified on i386 and x86_64: `cat /proc/crash` lists both sinks and the
  record; `wedge` + `fkill` inside a GUI terminal makes the Crash Reports window
  **open by itself** (screenshot, both arches — nobody clicked it); a
  `system_reset` from the QEMU monitor is reported on the next boot as
  *"…last recorded event was forced-kill in 'wedge' (pid 16) at pc=… code=137,
  44 s into that boot"*.  aarch64 builds clean.
  Same session, desktop chrome: the taskbar clock now shows the **ISO date and
  the active keyboard layout** (`2026-08-02  20:18:16  US`) and the wallpaper
  label carries the **architecture** next to the milestone (`d-os M47  x32` /
  `x64` / `arm64`), so a screenshot identifies its own build.  `DOS_MILESTONE`
  → M47, `DOS_VERSION` → 0.47.0.

- **2026-08-02 — §M47 stage 1: crash records + pluggable reporting sinks.**
  The standing requirement is that a faulty program must never take the machine
  down — and that when something *does* go wrong, the system says so instead of
  the event existing only as a line on a serial console nobody was watching.
  M46 delivered the first half wherever a fault handler still runs.  This adds
  the reporting half, built so that ANY notification mechanism can be armed
  later without touching a fault path again.
  **Two phases, deliberately separated,** because they have opposite
  constraints.  *Capture* (`crash_report`) runs in the worst context in the
  system — inside an exception or NMI handler, IRQs off, possibly on a broken
  stack — so it only copies a fixed-size record into a static ring and bumps a
  counter: no allocation, no locks, no formatting, no I/O.  A lock there could
  deadlock against the very fault being recorded, so a simultaneous double
  report may overwrite a slot; losing one record beats hanging.  *Delivery*
  (`crash_drain`, called from the watchdog sweep task) runs in ordinary context,
  where a sink may allocate, block, open a file or draw a window.  That split is
  the whole design: a GUI popup sink is ordinary GUI code precisely because it
  never runs in fault context.
  Sinks register with `CRASH_SINK()` (linker section, same pattern as
  `DRIVER()`/`SERVICE()`); the built-in klog sink is always present so nothing
  goes unrecorded even before any richer reporter exists.  Wired in: ring-3
  faults, ring-0 faults (captured BEFORE the policy runs, since halt and reboot
  both never return), NMI hard lockups, watchdog task hangs, spinlock deadlocks
  and forced kills — on i386, x86_64 and aarch64.  Surfaced by the new `crash`
  command.
  **The failure this cannot capture, and the answer to it.**  A triple fault, a
  hardware reset or a power loss resets the CPU with no handler running at all —
  by definition nothing in the guest can log it.  That case is covered from the
  other side: `crash_boot_begin` arms a marker in battery-backed CMOS NVRAM
  (new HAL contract `hal_nvram_read/write`) and `crash_boot_clean` disarms it on
  an orderly shutdown, so finding it still armed means the previous boot died
  where nothing could report it.  NVRAM rather than a file on purpose: the
  marker must survive exactly the events during which no filesystem write can be
  trusted.  aarch64 on QEMU `virt` has no such storage and says so out loud
  rather than silently concluding every boot was clean.
  Verified on i386 and x86_64 by causing both kinds of event: `wedge` + `fkill`
  produces a `forced-kill` record (klog line + `crash` listing), and a
  `system_reset` issued from the QEMU monitor — an event no in-guest code can
  log — is reported on the next boot as *"previous boot ended without a clean
  shutdown"*.  Regression: `faulttest`, `fputest`, `archtest`, `fdtest`,
  `solibtest`, `musltest` green on both arches at `-smp 2`.
  **Still open (stage 2):** `/proc/crash`; a GUI popup sink; persisting records
  to a file so they survive to the next boot (the NVRAM marker carries only the
  fact, not the detail); crash reasons for the reset path.

- **2026-08-01 — Architecture identity: the ELF loader, the store and `uname`
  all know what machine this is.**  Three related gaps, all of which showed up
  as confusing multi-arch failures rather than clear errors:
    1. **The ELF loader accepted any word size and never looked at
       `e_machine`.**  Nothing downstream fails on its own — a 32-bit image on
       a 64-bit kernel maps fine and the CPU is then handed an entry point full
       of foreign instruction encodings, so the failure surfaced far from the
       cause.  The other direction was worse: a 64-bit image loaded by the
       32-bit kernel had its `p_vaddr` fields truncated by header
       normalisation, so segments mapped at the WRONG addresses silently.
       `elf_load_ex` now asks the arch (`hal_elf_can_exec`) and returns
       `ELF_EBADARCH`.
    2. **The store had no notion of architecture.**  A package's identity was
       id+version+deps, so the same name/version on two arches hashes to the
       same `/store` path — fine while `/store` lives in ramfs and is rebuilt
       per boot, but wrong the moment it persists.  `pkg_recipe.arch` (NULL ⇒
       this kernel's) is now folded into the content hash and written as
       `<store>/<dn>/.arch`, so the two builds get different paths and neither
       shadows the other.  `pkg_run` reads it back and refuses a foreign package
       by DATA, naming both arches, instead of letting the loader fail later.
       Same data-driven seam as `.abi`.
    3. **`uname` hardcoded `"i386"`** — it lied on x86_64 and aarch64, and a
       libc or build script branching on it would have made the wrong choice.
  New HAL contract: `hal_arch_name()` (one spelling of an arch in the whole
  system — the store hash, `.arch`, and `uname -m` all use it) and
  `hal_elf_can_exec(cls, machine)`.  Each arch answers only for its own native
  pair; a 64-bit kernel *could* run 32-bit binaries, but that needs a compat
  syscall entry, 32-bit user segments and a 32-bit personality, so accepting
  them here would just relocate the failure.  When that lands,
  `hal_elf_can_exec` is the single place that changes.  Also removed the
  `#if defined(__x86_64__)` that built the `ld-musl-<arch>.so.1` interpreter
  path inside `pkg.c` — core code now derives it from `hal_arch_name()`
  (convention 3), so a third arch needs no edit there.
  New self-test `archtest`: two foreign header shapes must be rejected with
  `ELF_EBADARCH` **and** the native shape must get past the gate (observed as
  the next error along, `ELF_ENOLOAD`) — the second half is what proves the
  gate discriminates rather than refusing everything.
  Verified on i386 and x86_64, `-smp 2`: `archtest` PASS on both (reporting the
  correct `arch=` string), `fputest`, `faulttest`, `solibtest`, `musltest`,
  `pkgtest` and `pkgrun` from the re-hashed store all green, NetSurf renders on
  both.  aarch64 builds clean.

- **2026-08-01 — Per-task FPU/SIMD state (FXSAVE/FXRSTOR on context switch).**
  `context_switch` swapped the INTEGER context only, so the x87/MMX/XMM register
  file was simply whatever the previously-running task left in it.  Two tasks
  doing floating-point work silently corrupted each other's arithmetic — no
  fault, no log line, just wrong numbers — and on SMP a task migrated to another
  core resumed on THAT core's register file, making it timing-dependent and
  unreproducible.  This had been a known, commented gap since SSE was first
  enabled; it stopped being theoretical once x86_64 ring-3 started running on
  APs, because SSE2 is baseline in the AMD64 ABI (the compiler emits XMM for FP
  *and* for ordinary memory copies), so every musl binary is an FP user.
  New HAL contract in `hal_api.h`: `hal_fpu_init_state` / `hal_fpu_save` /
  `hal_fpu_restore` over an opaque `HAL_FPU_STATE_SIZE` blob carried in `struct
  task`.  The scheduler saves `prev` and restores `next` around every switch
  (eager, not lazy: at 100 Hz the cost is noise, and CR0.TS + #NM lazy switching
  is a classic source of cross-task — and on SMP cross-core — state leaks).
  `fork` snapshots the parent's LIVE state before copying it to the child, since
  the parent's blob only holds what it had at its last switch-out.
  **Two traps worth remembering.** (1) A zero-filled blob is NOT a valid x86 FPU
  image: `fxrstor` would load MXCSR = 0, i.e. every SIMD exception UNMASKED, and
  the task takes #XF on its first FP instruction — so `hal_fpu_init_state`
  writes the architectural reset values (FCW = 0x037F, MXCSR = 0x1F80) and is
  called at all four task-creation sites, not left to `kcalloc`.  (2) FXSAVE
  needs 16-byte alignment or it #GPs; `struct task` is `kcalloc`'d, so the blob
  is oversized and the HAL aligns *inside* it — the arch rule stays in the arch.
  aarch64 is a deliberate no-op with the reasoning written down: CPACR_EL1.FPEN
  is never set, and both the kernel and its user libc build
  `-mgeneral-regs-only`, so the vector registers are unreachable and there is no
  state to lose.  Enabling FP there later means doing BOTH halves (set FPEN per
  CPU *and* fill in the save/restore) — enabling only the first is the dangerous
  combination.
  New self-test `fputest`: two kernel tasks each hold a different pattern in a
  live FP register across 3000 yields.  It was validated by temporarily removing
  the fix — it reports `mismatches a=3000 b=0` without it and `a=0 b=0` with it,
  so it is a real regression test rather than a tautology.
  Verified on i386 and x86_64, `-smp 2`: `fputest` PASS, plus `faulttest`,
  `fdtest`, `socktest`, `polltest`, `forktest`, `threadtest`, `musltest` and
  NetSurf rendering on both.  aarch64 builds clean.

- **2026-08-01 — SMP: x86_64 ring-3 works on an AP (per-CPU TSS + SSE +
  SYSCALL MSRs).**  x86_64 `-smp 2` triple-faulted as soon as a ring-3 task was
  load-balanced onto the AP — reliably reproducible by launching NetSurf, which
  runs long enough to migrate.  Three pieces of x86_64 CPU state that are
  PER-CPU were being programmed only on the BSP:
    1. **TR** (`ltr`) — the AP ran with TR = 0, so a trap taken while it
       executed ring-3 code had no RSP0 to switch to → #DF → triple fault, with
       no chance for the kernel to report anything.  Worse, there was only ONE
       TSS descriptor in the GDT, so even LTR-ing it would have made both cores
       share one RSP0.
    2. **CR0.EM/MP + CR4.OSFXSR/OSXMMEXCPT** (`enable_sse`) — SSE2 is baseline
       on x86_64, so without it every musl binary #UDs on its first SSE
       instruction.
    3. **EFER.SCE + STAR/LSTAR/FMASK** (`syscall_init_64`) — without them the
       `syscall` instruction is not enabled on that core, so every libc syscall
       from a ring-3 task there raises #UD.
  Fixed by giving x86_64 the per-CPU TSS shape i386 got in M35 (an array in
  `tss.c`, two GDT slots per CPU since a long-mode TSS descriptor is 16 bytes,
  `gdt_load_cpu_tss()`), and by collecting all three into one
  `hal_arch_init_this_cpu()` that `ap_main` calls after `percpu_init_ap()`.
  **Lessons learned.** (1) The diagnostic ladder that found this in minutes:
  QEMU exiting under `-no-reboot` ⇒ triple fault ⇒ `-d cpu_reset,int` ⇒ the
  exception chain named it (#UD at CPL=3, then #SS delivering it).  (2) The
  register dump answered "why" without reading any source: `TR=0000` and
  `EFER=…500` on the faulting CPU versus `…501` on the BSP, `CR4=0x20` versus
  `0x620` — **diff the two CPUs' control registers; every difference is a
  per-CPU init you forgot.**  (3) The general rule this is the third instance
  of (after M35's i386 TSS and M18.5's per-CPU IDTR): *anything held in a
  control register or MSR is per-CPU by definition* — so the per-CPU bring-up
  list belongs in ONE function, which is what `hal_arch_init_this_cpu` now is.
  Verified: x86_64 `-smp 2` and `-smp 4` (all APs online, parallel self-test
  PASS), `faulttest`/`fdtest`/`socktest`/`polltest`/`musltest` green on 2 CPUs,
  NetSurf renders on `-smp 2`; UP unchanged and still green.
  **Known gap this exposes (NOT fixed here, pre-existing):** the x87/SSE
  register file is not saved across a context switch (see the comment on
  `enable_sse` in `hal/x86_64/hal_arch.c`).  On UP that already meant two
  concurrent SSE-using ring-3 tasks clobber each other; on SMP a task migrating
  between cores also resumes on a different XMM file.  A proper fix is
  per-task FXSAVE/FXRSTOR state (with a valid initial image, since `fxrstor` of
  a garbage MXCSR faults) — a follow-up in its own right.
- **2026-08-01 — §1.1 layer 3: bounce buffers close the last ring-3 pointer
  hole.**  M46 shipped two layers — a per-syscall gate and an exception table —
  but the BULK payloads (`read`/`write`/`send`/`recv`/`sendto`/`recvfrom`/
  `getdents`/`getdents64`/`poll`/`getrandom`) were still handed to the VFS,
  socket and console layers as raw ring-3 pointers.  Those layers dereference
  the buffer deep inside their own call chains, far from any instruction
  registered in `.ex_table`, so layer 2 could not cover them: a range that went
  bad mid-transfer (a concurrent `munmap`, a revoked COW page — genuinely
  reachable now that a second thread can run on another CPU) still took a ring-0
  #PF and, with the default `kernel.fault_policy = halt`, killed the box.  That
  contradicted M46's headline claim, so it is now fixed rather than documented.
  Each of those handlers is split into a `*_k` core that only ever sees kernel
  memory plus a gated wrapper that stages the payload through a kernel chunk
  (`struct bounce`: ≤192 B on the stack, otherwise a kmalloc'd chunk capped at
  4 KiB and looped, so asking to read 64 MiB does not allocate 64 MiB).  The
  staging copies themselves are layer-2 protected, so a range going bad becomes
  a short count or -EFAULT.  Semantics preserved deliberately: stream writes
  loop and honour short writes; reads loop only for a regular VFS file (a short
  read IS the right answer on a socket/pipe/stdin, and looping would block a
  caller that offered a big buffer — hence `read_may_loop`); a UDP datagram is
  atomic so `sendto` stages it whole or fails, bounded by `UDP_MAX_PAYLOAD`.
  `sys_recvfrom_u` is the split a foreign personality needs — ring-3 payload,
  kernel (ip, port) out-params.  `faulttest` grew a third case that proves both
  halves: a valid ring-3 payload still round-trips, and a copy straddling into
  an unmapped page fails AFTER partially succeeding (`partial=1`) — the exact
  TOCTOU shape a pre-check cannot catch.
  **Found while doing it:** `linux_abi`'s `sendmsg` gathers the client's iovecs
  into a KERNEL buffer and then called the gated `sys_write`/`sys_sendto` — which
  by M46's own rules must reject a kernel address while `in_user_syscall` is set.
  So musl's TCP-fallback DNS path had been silently broken since M46.  It now
  calls the `*_k` cores.  Same dual-use trap as the original M46 lesson, one
  layer up: introducing a gated wrapper means auditing every in-kernel caller of
  the function you just wrapped.
  Verified on i386 and x86_64: `faulttest` (all three cases PASS), `fdtest`,
  `socktest`, `polltest`, `solibtest`, `musltest`, `dnstest`, `httptest`
  (`HTTP/1.1 200 OK`), the musl coreutils from the store (`echo`/`ls`/`cat` and
  `sh -c`), and NetSurf rendering — i386 with `-smp 2`.
- **2026-08-01 — SMP: i386 `-smp N` boots again (AP trampoline GDTR
  truncation).**  Passing `-smp 2` to the i386 build killed the machine during
  AP bring-up.  It looked like a silent hang, but it was a **triple fault** —
  QEMU simply exits under `-no-reboot`, which is why nothing appeared on serial
  past `lapic: timer calibrated`.  Root cause: the AP trampoline runs in 16-bit
  real mode, and `lgdt m16&32` with a 16-bit operand size loads only the **low
  24 bits** of the GDT base (Intel SDM Vol 2A).  This was harmless for years
  because `gdt[]` lived below 16 MiB — but the kernel image grew past that
  (embedded font + NetSurf/freetype blobs), `gdt` moved to `0x014f61a0`, and the
  AP loaded GDTR base `0x004f61a0`.  The very next instruction that needs a
  descriptor — the far jump to selector `0x08` — took a #GP; with no IDT loaded
  yet that escalated #GP → #DF → triple fault.  Fix: an `o32` prefix on the
  trampoline's `lgdt` (`kernel/hal/x86/ap_trampoline.s`) so the full 32-bit base
  is loaded.  x86_64 was never affected: its trampoline first loads a small
  inline GDT that lives below 64 KiB, and only re-loads the kernel GDT once it
  is already in long mode, where `lgdt` takes the 10-byte form.
  **Lessons learned.** (1) A hang that leaves QEMU *dead* rather than spinning
  is a triple fault — reach for `-d cpu_reset,int` first; the exception log
  names the faulting instruction and error code in one shot.  (2) An AP that
  dies exactly at the protected-mode far jump means a bad **GDTR**, not a bad
  descriptor; compare the GDTR base in the QEMU dump against `nm kernel.bin |
  grep gdt` — here the two differed by exactly the truncated top byte.  (3) This
  is a class of bug where *growing the image* breaks boot code written when the
  image was small; any 16-bit-mode address handling deserves a comment saying
  what it silently assumes.
  Verified: i386 `-smp 2` and `-smp 4` (every AP online, parallel/preempt
  self-tests PASS), `faulttest`/`fdtest`/`socktest`/`threadtest`/`tlstest`/
  `forktest` green on two CPUs, GUI desktop and NetSurf both render; x86_64
  `-smp 2` unchanged and still green.
- **2026-08-01 — M46: resilience — a program can no longer freeze the box
  (DOCS §4.37).**  Ring-3 faults kill just the process on all three arches;
  force-kill reclaims a wedged ring-3 task at the timer preemption boundary
  (`fkill`, Task Manager "Force kill", opt-in `package.auto_fkill_ms` runaway
  policy); an ib700 hardware watchdog + NMI catches a hard lockup (IRQs-off
  spin/halt), logs the stuck EIP and recovers or reboots; Ctrl+Alt+Del /
  Ctrl+Alt+X are trapped in the keyboard IRQ so the chrome works while an app is
  frozen, and a package window's X force-kills an unresponsive client.  The
  ring-3 pointer boundary is closed in two layers — a per-syscall gate
  (`task->in_user_syscall` + `vmm_user_access_ok`) and a real **exception table**
  (`.ex_table` + `uaccess_*`, so a fault DURING a copy returns -EFAULT instead of
  panicking); `faulttest` proves both.  dosgui handles are owner-bound and its
  blits range-checked; sigreturn sanitises EFLAGS; `sys_kill` is restricted to
  the caller's subtree.  x86_64 gained real COW + the force-kill point, aarch64
  the EL0-fault-kill + fault policy.  ACPI tables above the identity map are now
  mapped on demand (i386 boots with `-m 512M` again).
  **Lesson learned (cost a full NetSurf debugging session):** the `sys_*` layer
  is DUAL-USE — the same functions serve ring-3 dispatchers and in-kernel
  callers.  Validating user pointers *inside* them broke every kernel caller:
  `linux_abi`'s `fstat(fd, &kernel_kstat)` started returning -1, so musl's ld.so
  failed `load_library()` for every `.so` ("No such file or directory") and
  NetSurf could not start on either x86 arch — with `fdtest`/`socktest`/
  `polltest` collateral.  A pointer check must live where the pointer's ORIGIN
  is known: the dispatcher flags the task, and kernel-destination calls use the
  explicit `*_k` cores.
- **2026-08-01 — aarch64: the ARM kernel links again + parity fixes.**  The
  third arch had drifted out of buildability while x86 features landed: the
  shared user libc did an unguarded `movw %gs` (i386-only) and its crt0 lacked
  the `__sig_trampoline`/`__thread_exit_tramp` symbols libc.c references; the
  kernel source list was missing `pkg.c`/`futex.c`/`net.c`/`audio.c`/`random.c`/
  `devfs.c` that shell.c now calls; `vmm_space_protect` (mprotect's arch half)
  was never implemented; and the new panic-class diagnostics call
  `serial_write`/`serial_putchar`, which had no aarch64 implementation.  Fixed
  by arch-guarding the TLS selector load, adding the AArch64 trampolines,
  completing the source list, implementing `vmm_space_protect` (AP[2]/UXN), and
  forwarding the emergency-serial contract to the PL011.  The hardware-watchdog
  entry points are now **weak no-ops** in the portable watchdog, so a board
  without one (QEMU `virt`) links and runs with L1/L2 only — no arch #ifdef in
  core code (convention 3).
- **2026-08-01 — build: per-arch user/ artifact cache
  (`build/.userartifacts/<arch>/`).**  Switching ARCH (or any `make clean`) used
  to wipe `user/*.so|dynelf|…` and re-compile the whole NetSurf + freetype stack
  (~25 min).  `scripts/build.sh` now parks the outgoing arch's artifacts and
  restores the incoming arch's, so a flip is instant; the artifact PATHS stay
  arch-agnostic (their objcopy blob symbol names derive from them).
  `DOS_ARTIFACT_CACHE=0` opts out.

- **2026-07-21 — §M42: NetSurf ported to i386 — renders in a desktop window on
  BOTH x86 arches.**  The whole browser stack was rebuilt for i386 with the
  musl-cross-i686 toolchain (`MUSL_ELF_CC` defined for i386; the support +
  component + runway libs and the ~147-TU binary link as ELF32; harfbuzz is
  skipped — freetype is built without it and NetSurf does not link it).  The
  arch-independent display bridge (`dosgui.c` + the libnsfb `dos` surface)
  needed no changes; the i386 linux-abi grew the syscalls NetSurf makes —
  `readlink`/`readlinkat`, `access`/`faccessat`, `madvise`, `stat64`/`lstat64`/
  `fstatat64`, `newuname`, `rt_sigaction`, and the `DOSGUI_CREATE`/`PRESENT`/
  `POLL` bridge calls via `int 0x80`.  `about:welcome` renders identically to
  x86_64 (screendump-verified).  Bring-up lessons: (1) the boot render-autorun
  must call `pkg_init()` itself — on i386 the shell provisions `/lib` only
  later, so the browser's DT_NEEDED store `.so`s must be planted first; (2)
  **`statx` must return `-ENOSYS`, not synthetic data** — musl's ld.so uses
  statx's dev/ino to dedup already-loaded libraries, and a hand-rolled statx
  struct made ld.so wrongly reject valid `.so`s ("No such file"), while the
  ENOSYS path (musl falls back to the correct `stat64`) works.  Open: the i386
  toolchain musl (1.2.3) vs runtime musl (1.2.5) split is forward-compatible but
  not yet unified.
- **2026-07-21 — §M42: the NetSurf browser BINARY compiles, links AND runs
  (x86_64).**  The whole browser builds via `scripts/build-netsurf.sh`
  (`make ARCH=x86_64 netsurf`): a curated ~147-TU set (core + framebuffer
  frontend + fbtk, JS = the `none` stubs, no curl/PDF/SVG/JPEG/WebP) links a
  915 KB musl dynamic PIE against the store `.so`s.  It is embedded + provisioned
  (binary blob run by the `netsurf [url]` shell command under the linux-abi
  personality; a `/res` archive — resources + English `Messages` + DejaVu TTFs —
  unpacked into the VFS at boot).  **It RUNS:** `netsurf about:blank` takes the
  browser through complete init (musl ld.so pulls the 14 store libs, `/res`
  resources + fonts load, freetype comes up, the browser window is created, the
  page is processed) and into the fbtk event loop with **zero unhandled
  syscalls** — confirmed both interactively and via a boot-time autorun
  (`x86_64.netsurf-test`, which shows the launch reaching the event loop with no
  fault/return).  Running it grew the linux-abi surface by three syscalls
  (`readlink`/`access`/`madvise`).  **AND IT IS VISIBLE:** a **display bridge**
  (`kernel/gui/dosgui.c` + a libnsfb `dos` surface backend, `user/netsurf/
  libnsfb_dos.c`) lets the ring-3 browser drive a real WM window — three custom
  linux-abi syscalls (`DOSGUI_CREATE`/`PRESENT`/`POLL`, 0xD050–52) create a
  `gui_app_window`, blit each rendered frame into it with `gui_window_blit` (the
  exact primitive the §M26 Wayland bridge uses), and hand input back from the
  window's hook.  `netsurf -f dos about:welcome` renders the Welcome page —
  heading, nav links, body text, a search box, link lists, DejaVu-freetype
  fonts, correct colours — into a titled desktop window with a URL bar; a
  **Start-menu `GUI_APP("NetSurf")`** launcher (`kernel/gui/apps/netsurf_app.c`)
  opens it, and the `netsurf` shell command brings the compositor up itself.
  Verified by a framebuffer screendump.  Left: a `dos` fetcher over M24+mbedTLS
  for `https://` pages, and input/scroll polish.  See PLAN.md §M42.
- **2026-07-21 — §M42: browser-runway libs (x86_64) — libnsutils / libnslog /
  libnspsl / libnsfb — store packages.**  The utility + framebuffer-surface deps
  that sit between the NetSurf parsing/DOM/decoder core and the browser *binary*.
  Ported pristine from git.netsurf-browser.org, cross-built vs musl into
  versioned store packages (soname'd into `/lib` for ld.so), provisioned by
  `pkg_init`.  **libnsutils** (base64/time/unistd — a hard NetSurf dep),
  **libnslog** (logging + a flex/bison filter language, generated into `src/` at
  build; used by NetSurf's `utils/log.c`), **libnspsl** (public-suffix list,
  pre-generated `psl.inc`), **libnsfb** (the framebuffer surface the fb frontend
  renders into — only the RAM surface + default plotters are built; the SDL/X/
  VNC/Wayland surfaces need host libs).  Two boot self-tests (dyn-musl, gated
  behind `x86_64.boot-selftest`): `nsutest` (base64 encode→decode round-trip,
  PASS) and `nsfbtest` (create a RAM surface, `nsfb_plot_rectangle_fill` a grey
  rect, read the pixel back = `0x00777777`, PASS — the exact render path the fb
  frontend uses).  The `netsurf` app source is now fetched (gitignored).  PLAN.md
  §M42 records the two now-DECIDED bring-up choices for the binary: frontend =
  libnsfb→framebuffer (blit the RAM surface into a `gui_window`), fetcher = a
  custom NetSurf fetcher over M24 + mbedTLS (reuse `user/wget.c`'s path, NOT a
  libcurl port).
- **2026-07-20 — §M42: libnsbmp (x86_64) — the BMP/ICO decoder — store package.**
  Completes the NetSurf image-decoder set alongside libnsgif.  Ported pristine
  from git.netsurf-browser.org, cross-built vs musl into a versioned store
  package (`/store/…-libnsbmp-1.0.0`, soname `libnsbmp.so.0`), provisioned by
  `pkg_init`.  Boot self-test `btest` (dyn-musl, gated behind
  `x86_64.boot-selftest`): `bmp_analyse` + `bmp_decode` a tiny embedded 1×1
  24-bit BMP → 1×1, `BMP_OK`.  Same session also fetched `libnsutils`.  With this
  the Tier-1 NetSurf core libs (wapcaplet/parserutils/hubbub/css/dom/nsgif/nsbmp)
  are all ported + running; PLAN.md §M42 has the concrete next steps toward the
  browser binary (frontend = libnsfb→framebuffer recommended).
- **2026-07-20 — x86_64 boots to an interactive shell (VC-bind confirmed).**  The
  arch-generic VC + shell spawn in `kernel_main` already binds the boot shell to
  the root framebuffer VC and takes keyboard input on x86_64 — verified by boot
  test (`vc: ready, root VC = 240x150 cells`, `d-os>` prompt, a typed `ps` runs).
  The stale "no VC bound at boot" comment is corrected, and the x86_64 musl/
  dynlink/C++/NetSurf-lib boot self-test suite is now gated behind
  `x86_64.boot-selftest` (config, default off) — so x86_64 boots straight to the
  prompt like i386 instead of dumping a wall of self-test output.  `pkg_init()`
  stays unconditional (userland provisioning).  `kernel/core/kernel.c`.
- **2026-07-20 — libdom (x86_64) rebuild fix.**  The libdom `.so` rule failed on
  a clean rebuild — `HUBBUB_OK` undeclared in `bindings/hubbub/parser.c` — because
  `-Ithird_party/libdom/bindings` preceded `-Ithird_party/libhubbub/include`, so
  `#include <hubbub/errors.h>` resolved to libdom's OWN binding header (which
  defines `DOM_HUBBUB_OK`, not `HUBBUB_OK`) instead of libhubbub's.  Reordered the
  library includes before `-I.../bindings`; libdom rebuilds clean and the x86_64
  ISO no longer needs the mtime workaround.  `Makefile`.
- **2026-07-20 — §M39 stage 3c: musl `getaddrinfo` + a real `wget` (i386).**
  musl's own DNS resolver now runs on d-os — the three Linux-ABI ops it needed
  are translated in `linux_abi.c`: `recvmsg`/`sendmsg` (socketcall 17/16 + direct
  372/370) and `poll` (168), onto the M24 socket API.  `linux_recvmsg` fills
  `msg_name` with the datagram source so musl doesn't drop the reply (it verifies
  the answer came from a queried nameserver).  `httpstest` now calls plain
  `getaddrinfo("example.com","443",…)` instead of hand-rolled DNS.  New userland
  **`wget`** (`user/wget.c`, musl + mbedTLS) fetches `http://`/`https://` URLs from
  argv (real TLS + CA verify for https), streaming the body to stdout or a file;
  shell `wget <url> [outfile]` execs it under the Linux personality (kernel
  HTTP-only path kept as fallback).  Boot-tested: `wget https://example.com/` →
  getaddrinfo → TLS 1.3 handshake → Example Domain HTML (571 body bytes), 0
  unhandled syscalls; same over plain http.  DOCS §4.35.
- **2026-07-20 — §M39 stage 3b: REAL HTTPS from an unmodified musl binary
  (i386).**  musl BSD sockets now work in ring 3: `linux_abi.c` gained the Linux
  `socketcall` multiplexer (102) + the direct socket syscalls (359–373), both
  routed through one translator onto the M24 socket API with a single
  `sockaddr_in` ⇄ host-order (ip,port) site.  `netmusl` fetches over plain TCP;
  **`httpstest` does real HTTPS** — DNS → TCP :443 → mbedTLS handshake on a live
  socket → the Mozilla CA bundle (`third_party/cacert.pem` → `/etc/ssl/cert.pem`)
  as the trust store → `VERIFY_REQUIRED`, verify flags 0x0 (chain + hostname
  trusted) → HTTP/1.1 200 OK over TLS 1.3.  CA bundle + `/etc/resolv.conf`
  provisioned from `pkg_init()`.  New: `scripts/fetch-cacert.sh`, `user/netmusl.c`,
  `user/httpstest.c`, shell `netmusl`/`httpstest`; a `pmm_validate()` free-list
  checker + `memcheck` command (diagnostic for a latent early large-order buddy
  corruption, documented in PLAN.md §M39 — worked around by provisioning large
  files from the late `pkg_init` phase).  DOCS §4.35.
- **2026-07-20 — §M42 NetSurf libraries: all six core libs run on x86_64.**  The
  browser's parsing/DOM/CSS/image foundation, each a versioned content-addressed
  STORE PACKAGE with a correct dependency closure, cross-built vs musl from
  git.netsurf-browser.org sources (`scripts/fetch-netsurf-libs.sh`), bypassing
  the netsurf-buildsystem (compile the `.c` set into a `.so`): libwapcaplet
  0.4.3, libparserutils 0.2.5 (perl-generated charset table), libhubbub 0.3.8
  →parserutils (perl entities + gperf element-type; Dockerfile gained gperf),
  libnsgif 1.0.0, libcss 0.9.2 →wapcaplet+parserutils (a host `gen_parser`
  builds 119 per-property parsers + a python select generator; `-fcommon`),
  libdom 0.4.2 →wapcaplet+hubbub+parserutils.  Self-tests (wctest/putest/hbbtest/
  gtest/csstest/domtest) parse real HTML, resolve charsets, decode a GIF, parse
  a stylesheet, and build DOM strings — sixteen x86_64 boot self-tests green.
  The heavy libs (libcss/libdom/…) are `make <lib>` targets, blob-guarded on
  the prebuilt `.so`.  Also this session (§4 refs pending): the x86_64 userland
  port, runtime musl + support libs as store packages, a modular pkg backend,
  and universal component versioning.

- **2026-07-19 — x86_64 userland port: unmodified musl runs on x86_64.**  Full
  parity with i386's musl userland (bar signal delivery).  Six x86_64 boot
  self-tests green: static hello, dynamic linking (ld.so), a DT_NEEDED `.so`,
  dlopen, C++ (STL + an exception thrown+caught across a `.so`, DWARF unwind via
  libgcc_s), and fork+execve+pipe.  Landed the long-deferred **§M20.6.1
  fast-syscall path**: x86_64 musl issues the `syscall` instruction, so
  `kernel/hal/x86_64/syscall_entry.s` is the `LSTAR` entry — and it RETURNS via
  `iretq` (fabricating an `int 0x80`-shaped frame + falling into the shared
  `isr_common` tail) rather than `SYSRET`, which sidesteps SYSRET's selector
  arithmetic so NO GDT reorg was needed (the reason it stayed deferred).
  `syscall_init_64()` arms EFER.SCE/STAR/LSTAR/FMASK; int_no sentinel `0x81`
  routes to a new x86_64 `linux_abi.c` (Linux `unistd_64` numbers, SysV arg
  regs, x86_64 `struct stat`, byte-offset `mmap`, `arch_prctl(ARCH_SET_FS)` →
  the FS.base MSR for TLS, `pread64`, fork/execve/wait4/pipe/dup2/kill/futex).
  Also: `enable_sse()` (SSE2 is baseline on x86_64 — musl emits it), `fork.c`
  (x86_64) + `enter_user_mode_regs` + an EAGER `vmm_space_clone` (COW is a
  follow-up).  **Runtime musl is now a versioned, swappable, pkg-managed store
  package** (`pkg_recipe.soname`/`is_libc`, `pkg_libc_use`) — uniform on i386
  (musl 1.2.5) + x86_64 (musl 1.2.2), aarch64 inherits it when its userland
  lands.  Open: signal delivery to musl handlers (needs the Linux `rt_sigframe`
  layout); x86_64 interactive shell (no VC bound at boot → the self-test runs
  from `kernel_main`); aarch64 userland.
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
