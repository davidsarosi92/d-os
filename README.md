# d-os

A small from-scratch operating system kernel.  Built as a teaching /
learning project — the goals are clarity, modularity, and clean
architectural boundaries, not raw performance.

Today it boots a 32-bit i386 kernel from GRUB, brings up its own GDT
/ IDT / paging, talks to a 1024×768 framebuffer with an embedded
8×8 bitmap font, services keyboard input via IRQ-driven PS/2,
exposes a ramfs-backed filesystem, and runs an interactive shell with
ring-3 user-mode and cooperative kernel-task scheduling.

## Quickstart

Requirements: Docker (for the cross-toolchain) and optionally
`qemu-system-i386` on the host (`brew install qemu` on macOS) for a
graphical window.

```sh
./scripts/build.sh        # docker build + make iso → build/d-os.iso
./scripts/run_qemu.sh     # boot the ISO; prefers host qemu, falls back to docker
```

In the shell, `help` lists every command.  Try:

```
help
meminfo
ls /
write /etc/msg hello
cat /etc/msg
spawn          # spawns a parallel kernel task
ps             # see both tasks
ringtest       # demo: drop to ring 3, syscall, return
shutdown       # ACPI soft-off
```

## Where to read more

- **[DOCS.md](DOCS.md)** — current state.  Every component that
  exists, with its API and quirks.  Read this to understand how the
  running system is put together.
- **[PLAN.md](PLAN.md)** — forward-looking roadmap.  Every milestone
  has a status, a design sketch, and definition-of-done.  Read this
  to understand what's coming and why.

## Architecture

Today: **i386** (32-bit x86) only, booted via GRUB / multiboot1,
hosted in QEMU.

Coming: **x86_64** and **aarch64** ports.  The codebase is being
shaped so the next two arches require new files under
`kernel/hal/<arch>/` rather than edits to core code.  See PLAN.md
§P (portability) and §M17–§M21 for the plan.

```
kernel/
├── core/        # arch-independent: scheduler, vfs, console, config, ...
├── mem/         # pmm, vmm, kmalloc
├── fs/          # vfs + concrete filesystems (ramfs today)
├── drivers/     # portable drivers (organized by class)
└── hal/<arch>/  # arch-specific: GDT, IDT, paging, port I/O, boot stub
```

## Licensing + contributing

License: [MIT](LICENSE).  Copyright (c) 2026 David Sarosi.

Contributions and pull requests are welcome once the repo lands on a
public host.  In the meantime: please keep code style consistent
with the existing files (heavy English comments, conversation in
Hungarian per the maintainer's preference) and update DOCS.md /
PLAN.md when a milestone advances.
