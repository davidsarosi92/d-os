# d-os

A small from-scratch operating system kernel.  Built as a teaching /
learning project — the goals are clarity, modularity, and clean
architectural boundaries, not raw performance.

It boots on **i386, x86_64 and aarch64** from one source tree: its own
GDT / IDT / paging (or EL1 / MMU on ARM), a preemptive SMP scheduler
with per-CPU runqueues and a load balancer, ramfs + devfs + procfs +
exFAT on virtio-blk, USB via xHCI, a compositing GUI with windows and
a taskbar, a TCP/IP stack, audio, and ring-3 userland running
**unmodified static and dynamic musl binaries** through a Linux
syscall-ABI personality — coreutils, `sh`, TLS, a Wayland server with
upstream libwayland clients, Mesa/EGL on softpipe, and the NetSurf
browser fetching real pages over HTTPS.

It is a hobby kernel, not a product: expect sharp edges, and read the
docs below before trusting anything to it.

## Quickstart

Requirements: Docker (for the cross-toolchains) and optionally
`qemu-system-*` on the host (`brew install qemu` on macOS) for a
graphical window.

```sh
./scripts/build.sh                 # ARCH=i386 by default → build/i386/d-os.iso
./scripts/run_qemu.sh              # boot it (4 CPUs, a NIC, 1 GiB RAM)

ARCH=x86_64  ./scripts/build.sh    # → build/x86_64/d-os.iso
ARCH=aarch64 ./scripts/build.sh    # → build/aarch64/kernel.bin (raw ELF, -M virt)
```

In the shell, `help` lists every command.  A tour:

```
meminfo lscpu           # memory and CPU topology
ls / ; cat /etc/msg     # filesystem
gui                     # start the desktop
loop 8 ; sched          # eight CPU hogs, then see how they spread
nice <pid> -10          # scheduling priority
wqtest                  # deferred-work pool, across cores
ping 10.0.2.2 ; wget    # networking
pkgrun sh -c "ls /store"  # musl shell + coreutils from the package store
netsurf                 # the browser
shutdown                # ACPI soft-off
```

## Where to read more

- **[DOCS.md](DOCS.md)** — current state.  Every component that
  exists, with its API and quirks, plus the bugs found bringing it up
  and why they happened.  There is a table of contents at the top with
  line numbers — jump, don't read it end to end.
- **[PLAN.md](PLAN.md)** — forward-looking roadmap.  Every milestone
  has a status, a design sketch, and a definition of done.
- **[PLAN_AARCH64.md](PLAN_AARCH64.md)** — the ARM64 port's catch-up
  plan, organised by gap rather than by milestone.

## Architecture

Three architectures from one tree.  Everything arch-specific lives
under `kernel/hal/<arch>/`; core code does no `__asm__`, no port I/O,
and makes no assumptions about page-table layout.  Drivers
self-register through a `DRIVER()` / `MODULE()` registry rather than
being wired into boot code.

```
kernel/
├── core/        # arch-independent: scheduler, vfs, net, workqueue, shell, ...
├── mem/         # pmm (buddy + zones), vmm, kmalloc (slab)
├── fs/          # vfs + ramfs, devfs, procfs, exFAT
├── gui/         # compositor, window manager, widgets, apps, Wayland
├── drivers/     # portable drivers, organised by class
└── hal/<arch>/  # i386 / x86_64 / aarch64: descriptor tables, paging, boot
user/            # in-tree libc programs + ported musl binaries
```

## Licensing + contributing

License: [MIT](LICENSE).  Copyright (c) 2026 David Sarosi.

Contributions and pull requests are welcome.  Please keep code style
consistent with the existing files (heavy English comments — including
*why*, not just *what*) and update DOCS.md / PLAN.md when a milestone
advances.
