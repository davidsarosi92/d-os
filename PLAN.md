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
| §M23 | Audio subsystem — ◐ stage 1 + stage 2 (i386 + x86_64): AC97 PCM output, tone, WAV player (DOCS §4.26, §4.26.1) | ~1040 |
| §M24 | Network stack (Ethernet → TCP/IP → sockets) — ✅ stages 1–3 (i386): virtio-net + ARP/IPv4/ICMP/UDP/TCP + DNS + ping/wget (DOCS §4.25) | ~1080 |
| §M25 | Userland foundation (Wayland prerequisites) — ✅ stages 1–7 + Tier B tail (concurrent user processes + full-arch libc; DOCS §4.24) | ~1545 |
| §M26 | Wayland server (wire protocol on M22 + M25) — ✅ core + integration (i386+x86_64, DOCS §4.32): handshake + shm buffers (SCM_RIGHTS) + xdg_shell + framebuffer/gui_window bridge + wl_seat input + a real ring-3 client + **server-per-surface** (`waycomp`: a client's toplevel IS a desktop window, input routed to its wl_seat) + a **mini-libwayland client lib** (`user/libwl`, `wayapp`). Upstream libwayland + unmodified apps landed in §M40 | ~1615 |
| §M27 | Process model — init, hierarchy, reaper, kill-tree — ✅ shipped | ~1818 |
| §M28 | System log (klog ring buffer + dmesg) — ✅ shipped | ~1860 |
| Tier A | Blocking primitives — wait-queue + task_wait + blocking IPC — ✅ shipped (DOCS §4.20) | — |
| §M29 | Services / daemons — supervisor + SERVICE() registry + service bus (endpoint/contract/transport) — ✅ shipped (DOCS §4.21) | ~1895 |
| §M30 | Task scheduling — cron service — ✅ shipped (DOCS §4.23) | ~1935 |
| §M31 | Watchdog — heartbeat freeze detection (task / CPU / hw) — ✅ shipped L1+L2 (DOCS §4.22; L3 HW deferred) | ~1960 |
| §M32 | Multi-user — identity, login, file perms, isolation | ~2160 |
| §M33 | Execution domains — ✅ COMPLETE (DOCS §4.82): a driver runs in ring 3 from the same source, is supervised and restarted, and an IOMMU-confined device is refused outside its granted windows.  Remaining items all gated on named triggers | ~2265 |
| §M34 | POSIX process & signals — ✅ shipped (i386): fork(COW)/execve/waitpid/pipe/dup2/signals (DOCS §4.27) | — |
| §M35 | Threads & futex — ✅ shipped (i386, UP + SMP): clone/futex/thread_create + per-CPU TSS (DOCS §4.28) | — |
| §M35.5 | Package manager & isolation — ✅ store shipped (i386): content-addressed /store + profiles + GC (DOCS §4.29); gates every port | — |
| §M36 | POSIX syscall breadth + native libc — ◐ stage 1 (i386, DOCS §4.30) + stage 2 "two brothers": **Linux-ABI peer runs real musl + coreutils (`echo`/`cat`/`ls`/`env`) + a real `sh -c` (fork/execve/waitpid) FROM the store via `pkgrun`, data-driven `.abi` seam (DOCS §4.31)**; native musl-fork peer TODO (= 2nd ABI backend). Own-libc PARKED → `NATIVE_LIBC.md` | — |
| §M37 | Dynamic linking — ld.so / `.so` / dlopen — ✅ shipped (i386, DOCS §4.33): shared musl (libc.so=ld.so) + ET_DYN/PIE loader + PT_INTERP + full auxv + full mmap2/mprotect/fstat64; dynamic hello, separate .so (DT_NEEDED + .so __thread), dlopen all green | — |
| §M38 | C++ runtime + support libs — ◐ runtime shipped (i386, DOCS §4.34): musl-cross-make g++ 11.2.0 + libstdc++; `cpptest` throws+catches across a `.so` (DWARF unwind) + STL, dynamically linked.  Support libs (zlib/freetype/harfbuzz/ICU/Skia) still open | — |
| §M39 | Crypto + entropy + TLS + DNS — ◐ stages 1–3b shipped (i386, DOCS §4.35): ChaCha20 CSPRNG + /dev/urandom + getrandom (arch-generic); Mbed TLS v3.6.2 (`crypttest` SHA-256+AES-GCM); **verified TLS 1.3** handshake (`ssltest`); **stage 3b = REAL HTTPS** — musl `socketcall`→M24 sockets (`netmusl`), `httpstest` = DNS→TCP:443→TLS 1.3 handshake→Mozilla CA bundle at /etc/ssl/cert.pem→VERIFY_REQUIRED (flags 0x0)→HTTP 200.  **stage 3c** = musl `getaddrinfo` runs natively (recvmsg/sendmsg/poll in linux_abi → `httpstest` resolves via real musl resolver) + a userland `wget` (http+https over mbedTLS, argv URL).  Open: DHCP resolv.conf, x86_64/aarch64 | — |
| **§M40** | **Client graphics stack** — ✅ **SHIPPED** (DOCS §4.40 + §4.40.1). Wayland half: UPSTREAM libwayland-client cross-built for musl (+ libffi, `wayland-scanner` on the host, vendored tree pristine); connects via `WAYLAND_SOCKET`; real `xdg_toplevel` + shm buffers; the surface IS a desktop window; desktop input reaches its `wl_seat`; `wl_output` + `wl_surface.frame`.  **`weston-simple-shm` — an UNMODIFIED upstream application — animates in a d-os window.**  GL half: **Mesa EGL + GLES2 on gallium softpipe** — `egltri win` spins a shader-drawn triangle in a window (`OpenGL ES 3.1 Mesa 23.1.9`), presented through `wl_shm`.  Lessons: libwayland must be SHARED (libEGL had absorbed it statically → two protocol tables in one process), and `mincore` must answer truthfully (stubbed as advisory, it made Mesa dereference address 3).  x86_64, as scoped.  Open: a full toolkit (GTK/Qt) — needs the remaining §M38 support libs | ~1900 |
| §M41 | Linux syscall ABI shim — optional binary-compat accelerator | — |
| §M42 | Web browser bring-up — NetSurf → WebKit → Firefox/Chromium (north star) — ◐ IN PROGRESS (**x86_64 + i386**): Tier-1 NetSurf component libs (wapcaplet/parserutils/hubbub/css/dom/nsgif/nsbmp) + runway libs (nsutils/nslog/nspsl/**nsfb** framebuffer surface) all ported + running as store pkgs; **the NetSurf BINARY compiles + links + RUNS** — a 915 KB musl dynamic PIE (`make netsurf`); `netsurf [url]` shell cmd execs it under linux-abi, `/res` (resources+TTF+Messages) provisioned at boot; `netsurf` (0 unhandled syscalls) RENDERS a real page (`about:welcome` — text, links, fonts, layout) into a **WM-managed desktop window** via the display bridge (`kernel/gui/dosgui.c` + a libnsfb `dos` surface backend + `gui_window_blit`); a **Start-menu "NetSurf" launcher** (`GUI_APP`) opens it.  **Ported to i386 too** (same lib stack rebuilt with the musl-cross-i686 toolchain; linux_abi grew i386 readlink/access/madvise/stat64/statx/uname/rt_sigaction + the DOSGUI syscalls via int 0x80).  Verified by framebuffer screendump on BOTH arches.  i386 musl **unified to 1.2.5** (the musl-cross-i686 toolchain's musl rebuilt to match the 1.2.5 runtime the kernel provisions; `MUSL_VER=1.2.5` pinned in the Makefile).  Two i386 runtime bugs fixed: a triple-fault (heavy pkg_init ran on the shell task's small stack → moved to the boot task) and a GUI freeze (NetSurf busy-spun → added nanosleep/sched_yield + a yielding `dos_input`).  Left: network fetch (a `dos` fetcher over M24+mbedTLS) → `https://` pages; NetSurf window UX (close button, minimize/restore, click/type input — it bypasses the M22.7 app-host loop) | — |
| §M43 | Native developer toolchain (self-hosting) — ◐ first slice shipped (i386, DOCS §4.36): **TinyCC compiles + runs C ON d-os** (`tcc`/`exec` shell cmds + Editor "Run" button).  Full gcc/clang self-hosting + binutils/make still open | — |
| §M44 | Language ecosystems — Rust / C++ / .NET (NativeAOT→CoreCLR) / Java (JVM); run cross-built musl binaries, then per-runtime ports | — |
| §M45 | Package manager frontend + GUI installer — apt-like UX + wizard over the §M35.5 store; remote repo over §M39 TLS; driver/module hot-swap via §M33 | — |
| **§M46** | **Resilient control plane + freeze-freeness** — ✅ **SHIPPED** (i386 + x86_64, aarch64 parity; DOCS §4.37): ring-3 fault ⇒ kill just the process (all 3 arches); real force-kill of a WEDGED ring-3 task at the timer preemption boundary (`fkill`, Task Manager "Force kill", opt-in `package.auto_fkill_ms` runaway policy); ib700 HW watchdog + NMI hard-lockup recovery + deadlock reporting on COM1; Ctrl+Alt+Del / Ctrl+Alt+X trapped in the keyboard IRQ (work while an app is frozen), window X force-kills an unresponsive client; the ring-3 pointer boundary closed with a per-syscall gate **and** a real exception table (`.ex_table` + `uaccess_*`, `faulttest`); dosgui handles owner-bound + blits range-checked; sigreturn EFLAGS sanitised; `sys_kill` restricted to the caller's subtree; x86_64 real COW; ACPI tables above the identity map mapped on demand | — |
| **§M47** | **Crash records & reporting** — ✅ **SHIPPED** (i386 + x86_64, aarch64 parity; DOCS §4.38): fault-safe capture into a static ring (`crash_report`, no locks/alloc/IO) + deferred delivery on an ordinary task (`crash_drain`) + a `CRASH_SINK()` registry, so ANY reporting mechanism can be armed later WITHOUT touching a fault path.  Sinks: `klog` (always on) + `gui-report` (the Crash Reports window, gated by `crash.report`).  Surfaces: `crash`, `/proc/crash`, the system log, the GUI window.  A triple fault / power loss — the one event nothing in the guest can log — is reported on the NEXT boot from a CMOS NVRAM marker + a 40-byte checksummed breadcrumb of the last record (kind/pid/pc/addr/code/uptime/comm).  **Stage 2**: `/proc/crash` + the Crash
Reports GUI window (a sink, not a fault-path change) + a wider breadcrumb.
**§M47.1**: closing a window is not a crash — the X asks first, a SECOND X click
forces (`gui.close_grace_ms` is only the unattended backstop); `wedgewin` is the
automated test M46's "chrome works when the app is frozen" guarantee never had.
**§M47.2**: `linux_syscall_dispatch` never armed `task->in_user_syscall`, so
M46's per-syscall pointer gate was OFF for the ENTIRE musl userland — now armed
on both arches | — |
| **§M47.5** | **x86_64 userland parity** — ✅ **SHIPPED** (DOCS §4.39): musl
coreutils + `sh`, ring-3 sockets, threads/TLS/signals, Mbed TLS (TLSv1.3 + HTTPS
w/ CA verify + `wget`), on-device TinyCC.  The gap was DUPLICATION, not missing
kernel support: objcopy blob symbols carried the arch in their NAME, the program
lists were written twice, and the x86_64 dispatcher stopped at M25.  Four 32-bit
assumptions fixed (crt0 argv, thread-arg passing, TLS segment+offset, missing
virtio-net/AC97).  Also fixed musl `getaddrinfo` on both arches (SOCK_NONBLOCK
was discarded; `hostorder_to_sockaddr` validated a kernel word as a user
pointer) | — |
| **§M48** | **The memory ceiling, discovered rather than compiled in — and a
usable browser** — ✅ **SHIPPED** (DOCS §4.42–§4.44).  `pmm_init` sizes its
metadata from the firmware map instead of a per-arch `#define`, so ONE image
boots on 128 MiB and on 128 GiB (verified on x86_64 at 1G/2G/3G/4G/8G/128G with
userland running, zero faults).  Physical addresses widened to arch width; block
seeding emits maximal aligned runs instead of releasing 33.8 M frames one at a
time; new `ZONE_DMA32` because past 4 GiB "any frame" and "a frame a 32-bit
device can reach" stop being the same thing.  **Found: x86_64 userland was broken
on ANY machine with >1 GiB RAM** — the identity map's 1 GiB page landed on the
user region and every `exec` returned `ELF_ENOMEM`; it only looked healthy
because every test used `-m 1024M`.  Fixed with a kernel direct map in the upper
half (`phys_to_virt`), since user programs cannot move (small code model).  Four
more latent bugs: slab's 32-bit page mask, the COW refcount table's 1 GiB window
(a DOUBLE FREE once exceeded — same shape on i386), ACPI identity-mapping its
tables into user space, and i386 ring 3 unable to execute SSE (`CR4.OSFXSR`).
i386's identity map now runs to 1 GiB (234 → 473 MiB usable on a 512 MiB box);
past that the limit is real and 64 GiB there is exactly the PAE maximum.
**NetSurf**: mouse-button events (the compositor had NONE — nothing was ever
clickable, in any client), cooked characters (typing was raw scancodes rendered
as text), and an http/https fetcher over d-os sockets + Mbed TLS with real
certificate verification.  `run_qemu.sh` now attaches a NIC — its absence is why
no site loaded however well the fetcher worked.  **Mesa** runs on i386 too | i386
kmap / PAE; non-blocking fetcher `poll` |
| §M58 | **Text selection** — a pointer grab + press/motion/release for widgets, a selection MODEL (byte range for text, cell rectangle for the terminal grid), inverted rendering on damage rects, word/line on double/triple click.  Blocked today by the widget mouse callback having only click/double — a drag has no transport | — |
| §M59 | **Clipboard, system-wide** — the M22.5 kernel clipboard widened to typed offers + a ring-3 surface (§M50 ops + `/dev/clipboard`) + Wayland `wl_data_device` (ownership-based, fd hand-off) + a primary selection (select ⇒ middle-click paste).  Today nothing in ring 3 can touch the clipboard at all | — |
| §M60 | ✅ **Wallpaper (SHIPPED, DOCS §4.62)** — the background becomes a surface with a source (`gui.wallpaper`: gradient / path / solid) + a fit mode; ONE trivial in-kernel decoder, everything else decoded in ring 3 and handed over the dosgui bridge; unreadable image ⇒ gradient + a klog line; `wallpaper` cmd then a Settings app | — |
| §M61 | ✅ **Resolution switching (SHIPPED, DOCS §4.70 + §4.77 — aarch64 too)** — the mode is a constant in `boot.s` (and `FB_WIDTH` on ARM) today.  New `fb_mode_list`/`fb_mode_set` behind the existing `fb_present.h` seam (Bochs DISPI / virtio-gpu set_scanout), then the real work: resize the backbuffer + wallpaper + panel, clamp windows, re-send `wl_output`, deliver the §4.60 dosgui RESIZE; apply-confirm-or-revert, because a bad mode is a black screen | — |
| §M62 | ✅ **Boot splash (SHIPPED, DOCS §4.71)** — `boot.splash` off/on/quiet; an embedded image painted through `fb_present` before the GUI exists; the log is SUPPRESSED, never discarded (`dmesg` keeps it); Esc drops to the log; **any fault/panic/watchdog trip tears the splash down first** — a splash over a panic turns a diagnosable crash into "it froze at the logo" | — |
| §M63 | ✅ **Control Panel (SHIPPED, DOCS §4.65; stage 0 §4.63)** — a `SETTINGS_PANEL()` linker registry (so a setting ships next to the code it configures and `controlpanel.c` never changes), panels as their own §M22.7 windows, ONE Start-menu entry.  **Stage 0 is a prerequisite that does not exist: settings do not survive a reboot** — `/etc/d-os.conf` is on ramfs and `config_init` runs 125 lines before the exFAT mount.  Panels: Display (§M61), Personalisation (§M60), System (a dozen existing keys with no UI), Packages (§M35.5/§M45 as a panel, not an app), Region (= three features: live keymap switch + a `rtc_write()` that exists on NEITHER arch + a timezone/locale layer that does not exist at all) | — |
| §M64 | ✅ **Desktop shortcuts (SHIPPED, DOCS §4.64)** — icons on the wallpaper, shortcuts ONLY.  A shortcut is a FILE (`/desktop/*.lnk`) so the file manager, `ls` and `rm` already work on it; four target kinds behind one resolver; drawn by the desktop shell, damaged as small rects; drag-to-move needs §M58's press/motion/release.  **Real cost is the icons: there is no icon anywhere in this system and `GUI_APP` has no icon field.**  Never auto-populate | — |
| §M65 | ✅ **Widget toolkit with a seam (SHIPPED, DOCS §4.78)** — `WIDGET_CLASS()` registry (name-addressed), `struct ui_spec` as DATA, one event sink per window, two-pass layout, three CELL-based size classes, checkbox/radio/slider/combo/menubar, a window popup, a table view, and the same toolkit from RING 3 over the dosgui bridge | — |
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
| M12 | exFAT (with multi-FS abstraction for FAT/NTFS)  | Storage          | ✅ DOCS §4.X + §4.73 (mkdir/rmdir) + §4.78.1 (rename — the ops table has no NULLs left) |
| M13 | Preemptive scheduling (timer IRQ → schedule)    | Concurrency      | ✅ DOCS §4.X |
| M14 | Multi-session shell with FB pane splitting      | UX               | ✅ DOCS §4.X |
| M15 | USB host stack + USB HID keyboard              | Input            | ✅ DOCS §4.X |
| M16 | Keyboard layout abstraction (US, HU, DE, …)     | Input            | ✅ DOCS §4.X |
| M17 | Portability cut — extract `hal_api.h`           | Architecture     | ✅ DOCS §4.X (partial — see notes) |
| M18 | SMP support — APIC, AP boot, per-CPU, locking   | Concurrency      | ✅ DOCS §4.X |
| M19 | Memory at scale — slab, huge pages, near-NUMA   | Memory           | ✅ DOCS §4.8, §4.10 |
| M18.6 | SMP polish — per-CPU runqueue + load balancer ✅, preempt_count ✅, taskset ✅, cross-CPU IPI ✅, MSI/MSI-X ✅ | Concurrency | §M18.6 (balancer completed by §M49) |
| M49 | Load distribution — periodic balance, demand metric, blocking sleeps/console reads, priority/`nice`, deferred-work pool (first consumer: xHCI drain), `sched`/`wqtest` | Concurrency | ✅ DOCS §4.46 |
| M19.5 | Memory polish — HIGHMEM ✅ (x86_64), empty-slab caching ✅, SRAT/NUMA ✅ (parser) | Memory | §M19.5 |
| M20 | x64 (long mode) port (UP)                       | Architecture     | ✅ DOCS §4.X (closed by §M20.5) |
| M20.5 | x64 SMP + APIC + ring-3 (int 0x80) — Phase A/B/C | Architecture | ✅ §M20.5 |
| M20.6 | x86_64 closure — SYSCALL/SYSRET, xHCI + virtio-blk 64-bit DMA | Architecture | ✅ SYSCALL entry + USB/blk DMA shipped (SYSRET-out not used — we `iretq` back, so the GDT reorder was never needed) — §M20.6 |
| M21 | ARM (aarch64 generic / RPi) port                | Architecture     | ✅ Phase A–M — **full x86 parity**: boot + SMP + virtio-blk + exFAT + DTB + framebuffer + EL0 userspace + full shell.c + M22 GUI (virtio-input kbd/mouse, PL031 clock) + **USB (xHCI + HID over PCIe ECAM)** on ARM64 (DOCS §4.17) — §M21 |
| M22 | GUI infrastructure — compositor, windows, mouse, widgets, taskbar, file manager | UX | ✅ DOCS §4.13 |
| M22.2 | GUI modularity — swappable desktop shell + app registry + GUI dev docs | UX | ✅ DOCS §4.14 |
| M22.3 | Desktop polish — task manager, task_kill, term-window close, minimize, Alt-Tab, damage rects | UX | ✅ DOCS §4.13 |
| M22.4 | Compositor smoothness — cursor-damage race, rect-bounded drag, tearing mitigation | UX | ✅ DOCS §4.13 |
| M22.5 | Desktop apps — text editor, BASIC interpreter, file manager 2.0, maximize/restore | UX | ✅ DOCS §4.13 |
| M22.6 | Tear-free present — Bochs-VBE page flip + display-scaling fix | UX | ✅ DOCS §4.13 |
| M22.7 | Per-task GUI apps (each WIN_APP on its own task) + panel-as-task | UX | ✅ DOCS §4.16 |
| M23 | Audio subsystem (AC97 / HDA / I2S)              | Devices          | ◐ stage 1 (DOCS §4.26) + stage 2 WAV player, blocking playback, honest frame counts (DOCS §4.26.1).  **All 3 arches** (virtio-sound on ARM).  + stages 3–4: `/dev/dsp`, **mixer + master volume + taskbar indicator** (DOCS §4.26.1).  Open: HDA, the AC97 capture rate |
| M24 | Network stack (NIC → TCP/IP → sockets)          | Networking       | ✅ DOCS §4.25 + §4.59 (complete: connection table, server role, retransmit, DHCP, /proc/net, all 3 arches) |
| M25 | Userland foundation — per-process VMM, ELF, fd, unix sockets, mmap | Architecture | ✅ §M25 (stages 1–7) + Tier B (concurrent user processes + full-arch libc, DOCS §4.24) |
| M26 | Wayland server — wire protocol over M22 compositor + M25 substrate | UX | §M26 |
| M27 | Process model — init, parent/child hierarchy, always-on reaper, kill-tree | Concurrency | ✅ DOCS §4.15 |
| M28 | System log — klog ring buffer, severity levels, /proc/kmsg, dmesg | Observability | ✅ DOCS §4.18 |
| Tier A | Blocking primitives — wait-queue (block/wake), task_wait, blocking socket read + poll | Concurrency | ✅ DOCS §4.20 |
| M29 | Services / daemons — SERVICE() registry + supervisor (autostart, restart policy) + service bus (endpoint / contract / transport, location-independent binding) | Architecture | ✅ DOCS §4.21 |
| M30 | Task scheduling — cron service (crontab, timer loop, RTC-driven jobs) | Architecture | ✅ DOCS §4.23 |
| M31 | Watchdog — heartbeat freeze detection (per-task / per-CPU softlockup / hardware) | Reliability | ✅ DOCS §4.22 (L1+L2; L3 HW deferred) |
| M32 | Multi-user — credentials, user DB, login, file ownership/perms, per-user isolation | Security | §M32 |
| M33 | Execution domains — a service's run location as a declared capability + config choice; driver placement is the flagship case | Reliability | ✅ COMPLETE 2026-08-29 (DOCS §4.82): Tier 0/1/2, shared-controller arbitration, IOMMU stage 5, and per-driver DMA domains proven by a driver in ring 3 whose device is refused outside its own buffer.  OPEN, none of it gating the claim: the modern virtio transport (legacy has no feature bit 33 — a virtio-driver item), a REAL DMA driver ported to drvrt, richer state replay |
| **M46** | **Resilient control plane — SAK hotkeys + force-kill** — Ctrl+Alt+Del = always-live Task Manager, Ctrl+Alt+X = kill last/frozen app, window chrome (close/min/restore) works even when the app is wedged (close ⇒ force-kill), Task Manager force-quit; the enabler is a real force-kill of a wedged ring-3 process | Reliability / UX | ✅ DOCS §4.37 |
| M58 | Text selection — pointer grab + press/motion/release, selection model (text bytes / terminal cells), word + line selection | UX | §M58 |
| M59 | Clipboard, system-wide — typed offers, ring-3 ops + `/dev/clipboard`, Wayland `wl_data_device`, primary selection | UX | §M59 (wants §M58) |
| M60 | Wallpaper — image background, fit modes, config + `wallpaper` cmd, gradient fallback | UX | ✅ DOCS §4.62 (panel → §M63) |
| M61 | Resolution switching at runtime — `fb_mode_set`, scene resize, `mode` cmd, Display panel, confirm-or-revert dialog | UX / Devices | ✅ DOCS §4.70 (x86) + §4.77 (aarch64) |
| M62 | Boot splash, switchable — `boot.splash`, drawn splash, log suppressed not discarded, torn down by any fault | UX / Reliability | ✅ DOCS §4.71 |
| M63 | Control Panel — `SETTINGS_PANEL()` + `CONFIG_KEY()` registries, generic panel, one Start-menu entry, `conf` with validation | UX / Architecture | ✅ DOCS §4.65 (+ stage 0 §4.63) |
| M66 | **Driver agility** — orderly shutdown, device lifetimes, runtime stop/start/swap, PCI hot-plug with BAR assignment, fault quarantine | Architecture / Devices | ✅ DOCS §4.80 |
| M67 | **Loadable driver modules** — a driver from outside the kernel image | Architecture | ✅ shipped 2026-08-27 — see DOCS.md §4.81 |
| M68 | **Dynamic privilege model — an INVESTIGATION** — use only the isolation the machine actually offers, switchable; does the ring axis (0/3 vs 0/1/2/3) earn its keep? | Architecture / Security | §M68 — study, verdict required |
| M64 | Desktop shortcuts — icons on the wallpaper, shortcut files, one resolver, swappable grid/list view | UX | ✅ DOCS §4.64 + §4.79 (drag-to-move, keyboard, Send to desktop, and the ramfs-persistence bug it exposed).  All four target kinds resolve |

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
- ✅ **`mkdir` / `unlink` / `rmdir` on exFAT — shipped 2026-08-22, DOCS §4.73.**
  Asked for directly, and the piece §4.72 was blocked on: a package store is a
  directory per package.  A directory turned out to be a file with three
  differences (ATTR_DIRECTORY, a ZEROED first cluster — "end of directory" is a
  0x00 type byte, so an unzeroed cluster is a directory full of garbage — and a
  DataLength of one cluster, never zero); removal is not erasure but clearing
  **bit 7** of every entry in the set (the SecondaryCount matters: clearing only
  the File entry leaves orphan Stream/Name entries that `fsck` reports); freeing
  the clusters needs both allocation shapes (`NoFatChain` run vs a FAT walk),
  and getting that wrong leaks free space SILENTLY.  A non-empty directory is
  refused with -2 — `rm -r` is policy, and it already lives in the VFS.
  **The bug worth remembering: `exfat_make` did not check for an existing
  name**, so the second boot created a SECOND `/mnt/store` that the scan found
  first — every package rebuilt while `ls` showed a full store.  *A filesystem
  that can create the same name twice does not have a namespace.*  Verified by
  `fsck.exfat -n` = clean on i386 and x86_64, not by our reader agreeing with
  our writer.  Also added: `rm [-r]` in the x86 shell (there was none — nothing
  to delete before), and `--empty` / `--no-disk` on every run script.
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
- *(added 2026-08-02)*  **A syscall must run preemptibly.**  Both x86
  entry paths (interrupt gate on i386, FMASK on x86_64 SYSCALL) and the
  AArch64 SVC vector all arrive with interrupts masked, and nothing
  re-enabled them — so every system call was non-preemptible end to
  end.  On a uniprocessor that turns ANY spinlock contention inside a
  syscall into a guaranteed hard lockup: the holder can never be
  scheduled while the spinner spins with IRQs off.  It presented as a
  GUI bug (NetSurf's present syscall vs the compositor) but was
  arch-independent and had nothing to do with the GUI.  Generalisation:
  *whenever a context masks interrupts, ask what happens if code in it
  waits for another task.*
- *(added 2026-08-02)*  **If a diagnostic can lose a race with a
  recovery mechanism, it will.**  The deadlock detector's threshold was
  calibrated on i386 and did not fire inside the ~4 s hardware-watchdog
  window on x86_64 — so the kernel knew exactly which lock and which
  caller were stuck, and rebooted before it could say so.  Any
  detector that competes with a watchdog/reset must be budgeted against
  it explicitly, on the SLOWEST target.
- *(added 2026-08-02)*  **The guarantee "a bad program cannot take the
  box down" needs TWO things, and M46 only had one.**  M46 made every
  fault handler kill just the process — but it assumed the handler
  RUNS.  The x86_64 SMP crash proved otherwise: with TR = 0 on the AP
  there was no stack to deliver the trap on, so the machine triple-
  faulted with no message, no log line, nothing.  Two consequences now
  baked in: (1) per-CPU bring-up state is part of the safety story, not
  just of correctness (see the control-register lesson below); (2) a
  system that can die without saying anything needs an out-of-band
  marker — hence §M47's NVRAM unclean-shutdown flag, which is the only
  channel through which such an event can ever reach the user.
- *(added 2026-08-02)*  **Capture and delivery of a crash report have
  opposite constraints, so they must be separate phases.**  Capture
  happens in the worst context in the system (exception/NMI, IRQs off,
  possibly a broken stack) and may not allocate, lock or format —
  taking a lock there can deadlock against the very fault being
  recorded.  Delivery can do all of that, from an ordinary task.  Once
  split, adding a popup / file / network reporter is just registering
  a sink, and no fault path is ever touched again.
- *(added 2026-08-01)*  **A loader that accepts a foreign binary is not
  being permissive, it is deferring the error.**  `elf_load_ex` took
  any ELF class and ignored `e_machine`; a wrong-arch image mapped
  cleanly and then died somewhere unrelated (or, 64-bit-on-32-bit, had
  its `p_vaddr` truncated and mapped at the wrong addresses in
  silence).  Generalisation worth keeping: when a check is cheap and
  the failure without it is *displaced* rather than absent, do the
  check.  Related: an architecture should have exactly ONE spelling in
  the system — `hal_arch_name()` now feeds the store's package hash,
  the `.arch` metadata and `uname -m`, which had drifted to a
  hardcoded "i386".
- *(added 2026-08-01)*  **A context switch that only swaps the integer
  registers is not a context switch.**  The FP/SIMD register file was
  left shared between tasks — silent wrong answers rather than a crash,
  which is why it survived so long.  Fixed with per-task
  FXSAVE/FXRSTOR (`hal_fpu_*`).  Two traps for anyone re-treading it:
  a zero-filled FXSAVE image is INVALID (MXCSR = 0 unmasks every SIMD
  exception → #XF on the first FP op), and FXSAVE #GPs unless the area
  is 16-byte aligned — so the state blob is oversized and the HAL
  aligns inside it rather than the core depending on kmalloc.  Also:
  a self-test for silent corruption is only worth having if you have
  SEEN it fail — `fputest` was validated by temporarily removing the
  fix (`a=3000 b=0` without, `a=0 b=0` with).
- *(added 2026-08-01)*  **Anything in a control register or an MSR is
  PER-CPU by definition.**  This is now the third instance of the same
  omission (per-CPU IDTR in M18.5, per-CPU TSS on i386 in M35, and on
  x86_64 here): the AP was missing TR, the CR0/CR4 SSE enable bits AND
  the EFER.SCE/STAR/LSTAR MSRs, so x86_64 `-smp N` triple-faulted the
  instant a ring-3 task migrated onto it.  The per-CPU bring-up list
  therefore lives in ONE function (`hal_arch_init_this_cpu`) instead of
  being scattered across the BSP boot path.  Debugging shortcut worth
  keeping: **diff the two CPUs' control registers in the QEMU dump** —
  `TR=0000` vs a real selector, `EFER=…500` vs `…501`, `CR4=0x20` vs
  `0x620` named all three gaps before any source was read.
- *(added 2026-08-01, found long after M18.5 shipped)*  **A 16-bit
  `lgdt` truncates the GDT base to 24 bits.**  The AP trampoline runs
  in real mode, so `lgdt m16&32` at the default operand size drops
  bits 31:24 of the base; the `o32` prefix is mandatory, not
  cosmetic.  This lay dormant for months because `gdt[]` happened to
  live below 16 MiB — then the kernel image grew past that (embedded
  font + NetSurf/freetype blobs), `gdt` moved to `0x014f61a0`, the AP
  loaded `0x004f61a0`, and the far jump to selector 0x08 #GP'd → #DF
  → **triple fault on every `-smp N` boot**.  Two transferable
  lessons: (1) a "hang" where QEMU itself *exits* under `-no-reboot`
  is a triple fault — go straight to `-d cpu_reset,int`; (2) boot
  code written in 16-bit mode encodes silent assumptions about how
  big the image is, so every address it touches deserves a comment
  stating the assumption.  Fixed in `ap_trampoline.s`; x86_64 was
  immune (inline low-memory GDT first, kernel GDT only re-loaded in
  long mode).

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

**Lesson learned (§M49, 2026-08-06).**  This periodic pass was designed
here and then NOT built — what shipped balanced only when a runqueue ran
empty, and the source comment described the periodic version anyway,
naming a `LOAD_BALANCE_INTERVAL_MS` that existed nowhere.  A ✅ row and a
confident comment are not evidence that a design was implemented.

Worse, nothing could have caught it: `run_qemu.sh` passed no `-smp`, so
the balancer never ran on the everyday path at all.  **Ship the way to
measure a subsystem together with the subsystem** — the `sched` command
took an hour and turned a guess into 15-20% versus 66% on identical
tasks.

And the "cheap heuristic" above is the part that aged worst: queue length
scores four hogs and four sleepers identically.  See DOCS §4.46 for the
demand metric that replaced it, and for why `task_msleep`, `vc_getchar`
and init's reaper all had to start really sleeping before any metric could
be trusted — three separate `hlt`+`yield` loops, each with a comment
asserting it was cheap, together keeping a core busy on an idle machine.

**Second lesson (§M49).**  Making them block shifted the boot timing just
enough to expose a latent SMP race: four call sites bound a task's console
AFTER spawning it, guarded by `preempt_disable()` — which §M18.6.2 made
PER-CPU, while `task_enqueue` deliberately places the new task on another
core.  Every one of those sites carried a comment explaining why it was
safe.  A guard whose scope changed under it is worse than no guard,
because the comment keeps vouching for it.

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

> **✅ SHIPPED (the entry half).**  `kernel/hal/x86_64/syscall_entry.s` +
> `syscall_init_64()` arm EFER.SCE / STAR / LSTAR / FMASK, and x86_64 musl
> binaries reach the kernel through the `syscall` instruction today — that is
> the live path for every Linux-personality program on this arch.  The RETURN
> half deliberately stayed on `iretq`, which is why **the GDT reorganisation
> described below was never needed**: SYSRET is the instruction with the rigid
> selector arithmetic, and we do not use it.  Left open: SYSRET-out (a latency
> optimisation, not a capability) and `IA32_KERNEL_GS_BASE`/`swapgs` per-CPU
> data.  *Lesson: the requirement that made this milestone look expensive
> belonged to only one half of it.*
>
> Per-CPU footnote learned the hard way (2026-08-01): these MSRs are PER-CPU.
> An AP that never runs `syscall_init_64()` raises #UD on every libc syscall —
> see the SMP change-log entry in DOCS §8.

**Status quo (at planning time):** ring 3 reaches the kernel via `int 0x80`
only.  Modern x86_64 userspace prefers the `syscall` instruction (lower
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

## §M24 — Network stack (NIC → TCP/IP → sockets) — ✅ COMPLETE

**Status: COMPLETE (2026-08-15) — see DOCS.md §4.25 (stages 1–3) and §4.59
(the second half).**

Stages 1–3 + the socket API shipped 2026-07-11 on i386: virtio-net, the
portable Ethernet/ARP/IPv4/ICMP/UDP/TCP stack, a DNS stub resolver and
`FD_NETSOCK` sockets.  §M55 (DOCS §4.56) then made waiting for the network
free, and §M56 gave it poll/epoll.  **The second half landed 2026-08-15 on all
three architectures** and is what the design below actually asked for:

- a **loopback device** + a routing decision (`net_route`), so both endpoints
  of a test can be ours and 127.0.0.1 means something;
- a **TCP connection table** (32 entries, four-tuple demux, per-connection
  receive ring + send buffer, real sequence arithmetic, the RFC 793 states);
- a **server role** — listen/accept with a bounded backlog, and an RST for a
  segment belonging to no connection, so a closed port fails fast;
- **reliability** — segmentation, RTO retransmission with backoff, window
  advertisement and zero-window probing, and a close that keeps working until
  its data and FIN are delivered;
- the **socket ABI as canonical §M50 operations**, one `sockaddr_in`
  marshaller for all three arches, i386's `socketcall` reduced to a
  demultiplexer, the per-arch copies deleted, and a real `shutdown`;
- **DHCP** (stage 7) with lease renewal through a §M49 worker;
- **`/proc/net/{dev,arp,route,tcp,stat}`** (procfs learned subdirectories);
- **a virtio-mmio NIC for aarch64**, closing "x86_64/aarch64 ports".

**Definition of done, met:** 8 concurrent connections and a 32 KiB transfer
through a 10 % loss link (intact, in order) on i386, x86_64 and aarch64; and
bind/listen/accept/getpeername/shutdown through an UNMODIFIED musl binary on
each.  `tcptest`, `tcploss`, `netstat`, `lo drop`, `dhcp` in both shells.

**Lessons learned.**

- *Build the way to make a test fail before you build the thing it tests.*  The
  loopback learned to drop frames before the retransmit timer existed, and
  `tcptest` was checked against a deliberately shrunken connection table
  (0/4 clients connected, FAIL) before being believed about the real one.

- *Three separate bugs each presented as "the network is slow".*  An ACK that
  only WIDENS the window carries the same acknowledgement number, so reacting
  to it only where new data is acknowledged leaves the sender stopped; a
  zero-window probe that occupies a sequence number digs a hole only the RTO
  can fill; and a FIN whose sequence number is inferred from `snd_nxt` is
  declared acknowledged after a retransmission rolls `snd_nxt` back — leaving
  the sender in FIN_WAIT_2 and the receiver in ESTABLISHED, with every byte
  delivered and the reader blocked until its timeout.  **Record the FIN's own
  sequence number; do not derive a fact from a value that moves.**

- *Instrumentation found all three; inspection found none.*  Splitting the
  test's wall clock into time-in-recv / time-asleep / time-in-send turned 8.5
  seconds of mystery into "the 33rd read waited 8 s", and printing the
  connection table AT THE MOMENT OF THE STALL named the last bug in one line —
  after two theories the measurements had already ruled out.

- *A counter that lives on an object cannot measure a period longer than the
  object.*  The loss test summed per-connection retransmit counters and read
  zero while seven timeouts had fired: the connections had ended.

- *Syscall numbers are data and must be copied, not recalled.*  i386's socket
  numbers are not sequential by name — 360 is `socketpair`, between `socket`
  and `bind` — and reciting them put `connect`'s handler on `bind`'s number.
  The symptom was a bind failing with ECONNREFUSED: the one errno that names
  the handler that actually ran.

- *Two correct changes can compose into a bug that neither one's tests can
  see.*  §M55 made the network poller run only while somebody waits; §M56 made
  a finite `poll()` timeout a real wait.  A task polling a SOCKET is waiting
  for the network and had no way to say so, so the poller stayed parked and the
  wait ran to its deadline with the answer sitting in the NIC.  musl's resolver
  polls — so nothing in ring 3 could resolve a name, `wget` and NetSurf
  included, while `nettest` passed throughout because the KERNEL resolver waits
  through `net_wait_cond` and counts itself.  **The test that catches this is
  the one that uses the path a program uses, not the path the kernel uses.**

- *A test that needs the host's network reports the host's network.*  The
  server half cannot be reached through SLIRP without a hostfwd rule the
  automated runs do not have, which is why the loopback came first and why the
  whole suite runs on a machine with no NIC at all.

**Still open (deliberately, with the trigger written down):** no reassembly
queue for out-of-order segments (they are dropped and duplicate-ACKed — correct,
and it costs throughput on a lossy link); no congestion control (the peer's
window is the only limit — honest on a loopback and on SLIRP, wrong on a real
bottleneck); a fixed 200 ms RTO rather than an RTT estimate (on links whose
round trip is microseconds an estimator measures the emulator); the aarch64 NIC
is polled rather than interrupt-driven; `sendmsg`/`recvmsg` remain per-arch
because their `msghdr` is guest-width words rather than a fixed address.  IPv6,
multicast, IPsec, bridging/VLAN, netfilter and zero-copy RX were out of scope
from the start and stay there.

**IRQ-driven RX, re-scoped after §M49 (2026-08-06).**  §M49 built the missing
half of this — a deferred-work pool (`work_submit`, one worker per CPU) whose
first consumer is the xHCI event drain, exactly the "ISR hands work to task
context" shape RX needs.  What is NOT ready is `net.c` itself.  The file says so
in its own header: *"everything therefore runs in one task context → no
locking"*, and the whole stack is built on that — a static TX assembly buffer, a
single `g_tcp` connection state, an ARP cache and UDP bindings with no
serialisation, and every blocking helper (`arp_resolve`, `ping`, `tcp_connect`,
`tcp_recv`) spinning on `dev->poll(dev)` from the CALLING task.  §M48 recorded
the user-visible consequence: *the blocking read is what drives the NIC*.

So RX-on-a-worker is not a driver change, it is a stack change, and in this
order: (1) put the stack's state under one lock — the waitq's own, so the same
lock that guards the state is the one waiters park on; (2) make the worker the
SOLE caller of `dev->poll`, preserving today's single-consumer property rather
than adding reentrancy to a stack that has never had it; (3) convert the spin
loops to waitq waits woken by the RX path, which needs a timeout the current
`waitq_block` does not have; (4) only then wire the NIC's own interrupt (the
MSI/MSI-X allocator from §M18.6.5 is already there).  Doing (4) first is the
tempting mistake: two pollers on one virtqueue.

*Done when:* `ping`, `nslookup`, `wget` and NetSurf all still work, and a
blocking `recv` no longer needs to be the thing that pumps the ring.

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

## §M33 — Execution domains — ◐ Tier 0 shipped, Tier 1+ open

**Stages 1 and 2 shipped 2026-08-27 — see DOCS.md §4.82.**  The declared
`.domains` capability with its honesty gate (`user`/`isolated` REFUSED with the
reason, never accepted and quietly run in the kernel), `driver.<name>.domain`,
the `/proc/drivers` placement view, and **Tier 0**: a fault inside a driver
entry point unwinds out of that entry point instead of reaching a fault policy
that takes the machine.  Verified on all three arches with `drv crash`.

**What Tier 0 is not, kept here because the plan promised it:** not memory
isolation — the driver is in ring 0 and the wild write has already happened.  It
converts the trap-style failures from panic into restart.  Recovery is refused
when the driver held a lock (a deadlock is worse than a panic), and IRQ handlers
are not guarded (no caller to unwind to).

**Still open, in the order they unblock each other:**
  * ~~The driver-runtime API~~ — **shipped 2026-08-27** (stage 2, DOCS §4.82):
    handles not pointers, offsets not addresses, `drv_irq_wait` blocks, one
    context owns everything, DMA carries CPU and device addresses separately.
    `ps2_mouse` is ported to it and measured.
  * **Tier 1, first half — shipped 2026-08-27.**  Port grants really are in the
    TSS I/O bitmap, bounded by a kernel-side manifest; `drv_irq_wait` and input
    publication are syscalls.  Falsified by `drvtest`: a granted port reads from
    ring 3, an ungranted request is refused, and a raw `in` on an ungranted port
    is a #GP that kills only that process.
  * **Tier 1 — COMPLETE 2026-08-28 on x86.**  `driver.<name>.domain = user`
    launches the ring-3 image instead of calling init; `ps2_mouse` runs there
    from the same source and the pointer still moves.  `domain_enforceable()`
    says yes on x86 and refuses on aarch64 with the reason (no port space, no
    MMIO-into-driver mapping).
  * **Tier 2, first half — shipped 2026-08-28: RECONNECTION.**  A supervisor
    notices the driver process is gone (polling for DISAPPEARANCE — init may
    reap it first), hands the grants back BEFORE re-spawning (our own conflict
    detector would otherwise refuse the replacement), quiesces the driver's
    CLIENTS (a driver that dies mid-drag leaves a button held forever), backs
    off, and quarantines a crash loop through `driver.restart_max`.  `drv crash`
    is one verb for both placements.  Measured: 25 driven pointer movements →
    25 events through the process that REPLACED the killed one, both x86 arches;
    with `restart_max = 0` the same kill quarantines instead, which is the
    control that makes the positive result mean anything.
  * **Still missing from Tier 1/2:** MMIO mapped into a driver's own space (no
    placeable driver needs it yet; `drv_mmio_request` refuses from ring 3
    rather than faking it); state replay richer than "the driver runs its own
    bring-up again", which suffices for a mouse and will not for a device
    holding a session.
  * **ARBITRATION OF A SHARED CONTROLLER — found by placing a driver, and
    SHIPPED 2026-08-28.**  The 8042's answer to a config-byte read lands in the
    single output buffer with the AUX bit clear, so it raises IRQ1 and the
    KEYBOARD driver reads it; our mouse driver then timed out, robbed by a driver
    behaving correctly.  Fixed in two halves: `drv_ports_lock`/`unlock` is the
    generic one — the KERNEL holds off the competing line, because a ring-3
    driver must not be able to mask an interrupt and the kernel decides which
    line — and `0xAD`/`0xAE` is the device one, without which masking is not
    enough (a masked IRQ1 stops the keyboard driver being told about a byte, not
    the keyboard producing one, and then WE steal it).  The claim is bounded and
    reclaimed on a deadline or on resource release, so a crashed mouse driver
    cannot leave the keyboard dead.  Measured: six placement cycles under
    continuous typing → 0 bring-up failures on both x86 arches; recovery became
    deterministic at 1 restart.  *This is the shape §M33 predicted a process
    boundary would produce: a privilege that has to become an operation.*
  * **Stage 5, first half — shipped 2026-08-28: WHAT CAN THIS MACHINE ENFORCE?**
    `ADVISORY(!)` was the right answer and an ASSUMPTION — nothing had looked for
    an IOMMU.  `iommu_init` walks the DMAR and reads the unit's CAP/ECAP.  **The
    rule: finding one must NOT improve the verdict** — an IOMMU in passthrough
    restricts nothing, and reporting otherwise because the chipset is capable is
    the most convincing kind of isolation theatre.  What it buys is the REASON,
    kept in a separate call: "this machine cannot" vs "this machine can and we
    have not built it" leave a driver equally exposed and call for different
    decisions.  `--iommu` on the harness and on run_qemu.sh, deliberately not
    default (the ordinary machine has none, and that path must stay the tested
    one).  All three arches answer; ARM says NONE and names the SMMU as the
    analogue nothing looks for yet.
  * **Stage 5, second half — shipped 2026-08-28: TRANSLATION IS ON.**  Root and
    context tables, second-level page tables with 2 MiB leaves (CAP.SLLPS
    checked, never assumed), an identity domain over all RAM, translation
    enabled.  The identity domain first, because tables that do not cover what
    devices already do kill the machine with no way to say why.  Falsified by
    `iommu block`: the AC97 given a domain mapping nothing produced a refusal the
    UNIT recorded by device and address.  **THE FINDING: virtio devices bypass
    the IOMMU** (no `VIRTIO_F_ACCESS_PLATFORM`), so the disk and the network are
    not behind the boundary at all — `iommu` lists which devices are and are not.
    The most misleading bug: invalidating the context cache but not the IOTLB
    made `block` report success while the device kept working.
  * **Stage 5 FINISHED — the boundary permits precisely what was granted.**
    `iommu limit` confines a device to a window and grants ACCUMULATE into one
    domain (the shape a per-driver domain needs).  Four measurements, both x86
    arches: identity → clean; window excluding the buffers → refused at the exact
    buffer address; window covering ONE buffer → that access passes and a second
    is refused; both granted → clean, 0 faults.  The third proves a boundary that
    tracks each access rather than a switch.
  * **§M33 COMPLETE — 2026-08-29.**  The trigger fired: `kernel/drivers/misc/edu.c`
    (QEMU's educational device) is the first DMA driver written against drvrt,
    runs on ALL THREE arches from one source, and is placeable in ring 3.  With
    it: per-driver DMA domains built by `drv_dma_request`, ring-3 MMIO and DMA,
    and `drv_device_window` — because a placed driver cannot read PCI config
    space and must not.  **Measured:** the driver's device confined to its own
    4 KiB buffer, the round trip working, and the escape 64 KiB away REFUSED by
    the hardware and recorded by device and address, on both x86 arches.  The CPU
    and device addresses are finally different, which is what drvrt.h separated
    them for.  **`drv domain` reports `edu: at user, isolation full, DMA`** — the
    first `full` for a DMA driver in this tree.
  * **Found on the way, and a real bug:** the PS/2 keyboard ISR read one byte per
    interrupt.  With edge-triggered IRQ1 the rest waited in the controller until
    the next keypress, so fast typing lost keys — presenting as the test harness
    mistyping, which cost several rounds before the direction of the fault was
    believed.  It drains now, stopping at the mouse's AUX bytes.
  * **Still open, none of it gating §M33's claim:** the modern virtio transport
    (a measured limit, and a virtio-driver item); a REAL DMA driver ported to
    drvrt — `edu` proves the mechanisms, and what a synthetic client cannot
    answer is whether the interface is pleasant to write a complicated driver
    against; richer state replay.
  * **Modern virtio transport — a MEASURED hard limit, not an oversight.**
    `VIRTIO_F_ACCESS_PLATFORM` is feature bit 33 and does not exist in the 32-bit
    feature register of the legacy PCI transport our drivers speak.  QEMU refuses
    it outright: *"VIRTIO_F_IOMMU_PLATFORM was supported by neither legacy nor
    transitional device"*.  Until both drivers move to virtio 1.0, the disk and
    the network cannot be put behind the boundary at all.
  * **Tier 2, second half** — DMA drivers in ring 3, which needs stage 5.
  * `driver.profile` (desktop|server) is deliberately absent: with one
    reachable domain it would be a key with one legal value.

---

## §M33 (design, retained) — where a service runs (kernel / user / isolated)

> **See also §M68** — the investigation into making the domain CHOICE dynamic,
> i.e. derived from what the machine can actually enforce.  §M33 is the
> mechanism; §M68 is the policy that would drive it, and it must not grow a
> second mechanism of its own.

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

## §M35 — Threads & futex — ✅ shipped (i386, UP + SMP)

> ✅ **SHIPPED (2026-07-11, i386) — see DOCS.md §4.28.**  `proc_clone`
> (SYS_CLONE) creates a thread that SHARES the creator's address space
> (`task->mm_shared` stops the reap from freeing it) + dups the fd table,
> entering ring 3 at a given entry/stack; `futex` (SYS_FUTEX, `futex.c`):
> FUTEX_WAIT parks iff `*uaddr==val` (lost-wakeup-free over the Tier-A
> wait-queue) / FUTEX_WAKE, hashed by physical address; libc `thread_create`/
> `thread_join`/`futex` + a 3-state Drepper mutex in `threadtest`.  **Tested on
> UP *and* `-smp 2`: 4 threads × 5000 shared-counter increments = 20000/20000
> PASS** (truly parallel).  Bringing this up on SMP also fixed a pre-existing
> gap — ring-3 tasks didn't run on APs (single global TSS + no per-CPU `LTR`) —
> via a **per-CPU TSS** (array in `tss.c`, one GDT descriptor per CPU, each CPU
> LTRs its own in `gdt_init`/`ap_main`), which unblocked *all* ring-3 tasks on
> APs (`procspawn` now runs on `-smp 2` too).  **Plus thread-local storage** via
> `%gs`: per-CPU GDT TLS descriptors + `hal_set_tls_base` (scheduler switch-in
> hook) + `set_thread_area` (SYS_SET_TLS) + libc `set_tls`; `tlstest`'s 4 threads
> each read only their own id through `%gs` (0 mismatches on UP + `-smp 2`).
> **Still open:** the compiler `__thread` ABI runtime (PT_TLS template; lands
> with the §M36 libc); migration-safe TLS (threads are CPU-pinned today — needs
> a per-CPU GDT); PI/robust futexes; `gettid`; per-thread signal masks;
> x86_64/aarch64.

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

## §M35.5 — Package manager & isolation (the substrate for every port) — ✅ store shipped (i386)

> ✅ **STORE SLICE SHIPPED (2026-07-11, i386) — see DOCS.md §4.29.**  A
> content-addressed store on the VFS (`kernel/core/pkg.c`): `/store/<hash>-name-
> version/` immutable paths (hash folds in the recipe + each dep's recursive
> hash), version coexistence, pinned `.closure`, a symlink-free `/etc/pkg/
> profile`, and mark-sweep GC.  Shell `pkg build|install|remove|why|list|gc` +
> `pkgtest` (two `hello` versions coexist; install `hello-2` + `args`; `pkg gc`
> reclaims the unreferenced `hello-1.0`).  **Still open** (design below is the
> roadmap): hermetic source builds (fetch + a §M33 sandbox — needs the §M36
> toolchain); load-time RPATH isolation (co-designed with §M37) + run-time
> FS-view isolation; rollback generations; a binary substituter; package signing
> (§M39); `/proc/pkg`; a text recipe format.

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

## §M36 — POSIX syscall breadth + native libc (musl port) — ◐ in progress (i386)

> ◐ **IN PROGRESS (2026-07-11, i386).**  **Stage 1 SHIPPED** (DOCS §4.30): the
> syscall surface a real libc sits on — 30–35 (`stat`/`fstat`/`getdents`/`uname`/
> `clock_gettime`/`nanosleep`) + `errno` + a `%o` printf; `posixtest` exercises
> them from ring 3.  **Stage 2 foundation SHIPPED** (DOCS §4.31): the **modular
> Linux i386 syscall-ABI compat layer** — keep musl PRISTINE (vendored,
> `scripts/fetch-musl.sh`) and have d-os provide the Linux ABI via an isolated
> `linux_abi.c` + a `task->linux_abi` personality (`linuxtest` runs a Linux-ABI
> program end-to-end; doubles as §M41).  **Still open:** vendor+build musl
> (`make musl`), grow `linux_abi.c` to musl's startup set (chiefly
> `set_thread_area`/auxv — see `third_party/MUSL.md`), then run a static musl
> `hello` + coreutils, `pkg install`-ed into the §M35.5 store.  Also later:
> `getcwd`/`chdir` (per-task cwd), `brk`, epoll/eventfd/timerfd, `getrandom`
> (§M39), full `struct sockaddr`.

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

**Stage 3b SHIPPED (i386, real HTTPS)** — see DOCS.md §4.35.  An unmodified
musl binary does networking from ring 3: musl's `socketcall` (+ the direct
i386 socket syscalls 359–373) is translated in `linux_abi.c` onto the M24 BSD-
socket API (single `sockaddr_in` ⇄ host-order (ip,port) site).  `netmusl`
resolves + fetches over plain TCP; **`httpstest` does REAL HTTPS**: DNS → TCP
:443 → mbedTLS handshake with the BIO on a live socket → the Mozilla CA bundle
(`third_party/cacert.pem`, provisioned to `/etc/ssl/cert.pem`) as the trust
store → **`VERIFY_REQUIRED`, verify flags 0x0 (chain + hostname trusted)** →
HTTP/1.1 200 OK over TLS 1.3.  Boot-tested over QEMU SLIRP.  Still open:
musl `getaddrinfo` (needs the resolver to read `/etc/resolv.conf` — provisioned,
but `httpstest` currently does DNS by hand), a `wget` front-end, DHCP-populated
`resolv.conf`, x86_64/aarch64 (no mbedTLS/M24 there yet).

**Lesson learned — a latent large-order buddy corruption, exposed by an early
big allocation.**  Provisioning the 333 KiB CA bundle as one ramfs file forces
a single ≥256 KiB `kmalloc` (one order-6+ `page_alloc`).  Done from an *early*
`kernel_main` self-test that trips a deterministic fault ~1 s later: a fixed
`.data` function pointer (a procfs `nd_*` node) is smashed → the CPU calls it
and executes into `.data` (`cs:eip=8:0x0015202x`, garbage `cr2`).  Findings
from bisection: (1) it's the *allocation*, not the write — `kmalloc(256K)` +
immediate `kfree`, no touch, still faults; (2) the free-list metadata stays
*consistent* across the alloc (`pmm_validate` passes); (3) merely read-walking
every free page (the same `pmm_validate`) *masks* it, while an equal-time dummy
delay does **not** → not pure timing; (4) it is **boot-phase-dependent**: the
identical large ramfs write done later, in `pkg_init` (where the store already
writes the ~750 KiB musl `libc.so`), is fine.  **Workaround (shipped):** do all
large system provisioning from `pkg_init`, not the early self-test window.
**Update (2026-07-20) — the buddy allocator is EXONERATED.**  Built the minimal
reproducer the earlier note asked for: an early (right after `procfs_init`, so
the victim `nd_*` `.data` pointers already exist) sweep of forced big allocations
— 24 blocks at order 6/7/8 (256 KiB / 512 KiB / 1 MiB) marched *down* memory with
no free, plus the exact `kmalloc(256K)+kfree` "no touch" shape.  Findings on the
current tree: (a) every big block comes off the TOP of RAM (`0x0ff00000` first),
**never overlapping** the kernel image `[0x100000,0x941040)`; (b) a new invariant
guard in `link_store` — which fires if the intrusive free-list link is ever
written into `[kernel_start,kernel_end)` — **stays completely silent**; (c) no
crash.  This is structural, not luck: the carve pass (`carve_out_range` over the
kernel image + low mem + mbi + AP trampoline) marks those frames `PS_NONE`, the
seed loop only releases `PS_USED` frames, and coalesce refuses a buddy whose
`page_state` isn't a matching free order (guarded further by `zone_remove`
failing), so a kernel-image frame can never enter the pool.  Therefore the old
`.data` smash (`cs:eip=8:0x0015202x`, inside the image) was **not** a
"buddy hands out a kernel frame" bug and no longer reproduces via early large
allocs — it was a wild write from some other early-boot interaction that the
`pkg_init`-late-provisioning workaround already sidesteps.  **Root cause of the
original smash: unreproducible / moot**; the `link_store` guard now stays in tree
as a cheap permanent regression detector (alongside `pmm_validate()`/`memcheck`).

---

## §M40 — Client graphics stack (Wayland client + GL + Skia)

**Why:** a browser does not draw to the framebuffer directly — it talks
to a **display server** and renders through a GL/2D stack.  §M26 provides
the Wayland *server*; this milestone provides the *client* side plus the
rendering path the browser plugs into.

**Status: the Wayland-client half is SHIPPED — see DOCS §4.40.**  Upstream
libwayland-client is cross-built for musl (with libffi; `wayland-scanner` runs on
the host and nothing generated is committed), connects over `WAYLAND_SOCKET`,
drives a real `xdg_toplevel` with an shm buffer, gets its surface mapped to a
desktop window and receives real input on its `wl_seat`.  `weston-simple-shm`,
compiled unmodified out of the weston tree, animates in a d-os window — which
means step 1 below is done and step 4's "a client is a Wayland client either way"
premise is now demonstrated rather than assumed.  What remains of this milestone
is steps 2–3: EGL/GL and a rasteriser.

**Design.**
1. **Wayland client** — ✅ done (DOCS §4.40).  `libwayland-client` over the §M25
   unix-socket + fd-passing + mmap substrate (the same primitives §M26's server
   uses); `wayland-protocols` (xdg-shell) so a real client's surface/seat/
   keyboard/pointer wiring works.  `xkbcommon` still open (we forward raw
   keycodes; a toolkit that wants a keymap will need it).
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

   **Toolkit dependency profiles (a client is a Wayland client either way):**
   | Toolkit | Lang | Wayland client | Renderer | Needs Mesa? | Needs C++ (§M38)? |
   |---------|------|----------------|----------|-------------|-------------------|
   | SDL2 | C | libwayland-client (C) | `wl_shm` sw / GL | no (sw) | no |
   | GTK | C+GObject | libwayland-client | cairo/pixman | no | yes (deps) |
   | Qt | C++ | libwayland-client (QtWayland) | own / GL | opt | yes |
   | **iced** | **Rust** | **native Rust `wayland-client` crate** | **`tiny-skia` (pure-Rust CPU) or wgpu** | **no (tiny-skia)** | **no** |

   **iced is the interesting outlier:** pure Rust, its winit/`smithay-client-
   toolkit` stack speaks the Wayland wire protocol from Rust (NO upstream
   libwayland C port), and its `tiny-skia` backend is a pure-Rust CPU
   rasteriser (NO Mesa/GL).  So iced's path is **§M44 Rust std-on-musl + our
   §M26 server**, sidestepping libwayland-C, Mesa AND the C++ runtime — a
   genuinely lighter route to a real GUI app than the C/C++ toolkits.  The
   cost moves to: Rust std working on musl (syscall breadth) + winit's Wayland
   backend needing the protocol set we advertise (xdg-shell we have; it also
   wants seat/output/maybe xdg-decoration).

**Definition of done:**
- ✅ A `weston-terminal`-class Wayland client runs against the §M26 server:
  draws, takes keyboard + pointer input.  (`weston-simple-shm`, unmodified;
  input verified via the pointer/keyboard listeners.)
- 🔲 An EGL+GLES2 program clears + draws a triangle via a software rasteriser,
  presented through a Wayland buffer.  **Probed** (DOCS §4.40): the build image
  now has meson/ninja/mako/expat, `third_party/mesa-cross.txt` is a working musl
  cross file, and `meson setup -Dgallium-drivers=swrast -Dllvm=disabled` gets
  through most of configuration before stopping at a missing **libdrm**.  Next:
  cross-build libdrm, a cross `pkg-config`, then the runtime questions (Mesa
  `dlopen`s its DRI module; its EGL Wayland platform must take the `wl_shm`
  swrast path rather than opening `/dev/dri`).
- ✅ DOCS.md chapter (§4.40).

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

**Current state (2026-07-21) — the NetSurf BINARY compiles + links + RUNS
(x86_64).**  The whole browser is built by `scripts/build-netsurf.sh` (`make
ARCH=x86_64 netsurf`): a curated ~147-TU set (core content/desktop/utils/handlers
+ the framebuffer frontend + fbtk, JS via the `none` stub set, no curl/PDF/SVG/
JPEG/WebP) compiles clean and links into a **915 KB musl dynamic PIE**
(`user/netsurf.dynelf`, interp `/lib/ld-musl-x86_64.so.1`) whose DT_NEEDED is
exactly our store `.so`s.  Their buildsystem is bypassed (forced prelude header
for config macros + `_GNU_SOURCE`, synthesised `testament.h`, two `-I`-order
header-shadow fixes, `dos_image_data.c` chrome-bitmap stubs).  The binary is
embedded as a blob run by a **`netsurf [url]` shell command** under the linux-abi
personality, and a `/res` archive (fb resources + English `Messages` +
8 DejaVu TTFs) is unpacked into the VFS at boot.  **It RUNS:** `netsurf
about:blank` goes through complete init — musl ld.so loads the 14 store libs,
`/res` resources + fonts load, freetype initialises, the browser window is
created, the page is processed — and enters the fbtk event loop with **zero
unhandled syscalls** (confirmed interactively and via a boot autorun gated on
`x86_64.netsurf-test`; no fault, no early return).  Running it grew linux_abi by
`readlink`/`access`/`madvise`.

**THE NEXT STEP — the display bridge (makes it visible).**  The fb frontend
renders into a `libnsfb` RAM surface headless; nothing presents it.  Plan:
(1) add a **d-os `libnsfb` surface backend** (like `ram.c` but backed by a
`gui_window`) selected with `netsurf -f dos`; (2) new syscalls
`gui_window_create` / `gui_window_present(buffer,w,h,stride)` / poll-input —
`present` reuses the kernel's `gui_window_blit` (the exact primitive the §M26
Wayland bridge uses at `wayland.c:269` to blit a client buffer into a WM window),
input feeds the surface's `nsfb` event queue; (3) a Start-menu
`GUI_APP("NetSurf")` launcher spawns the browser targeting a new `gui_window`.
DoD: `netsurf https://example.com` renders text+layout into a desktop window,
links clickable (network fetch = the custom `dos` fetcher over M24+mbedTLS).

**Earlier this session — Tier 1 component libs + browser-runway libs COMPLETE
(x86_64).**  The NetSurf core library set is ported + running as store
packages, each with a ring-3 dyn-musl smoke test at boot (gated behind
`x86_64.boot-selftest`): libwapcaplet (string intern), libparserutils,
**libhubbub** (HTML5 parse), **libcss** (CSS), **libdom** (DOM), **libnsgif**
(GIF) and **libnsbmp** (BMP/ICO).  This session added the **runway libs** that
sit between the parsing/DOM core and the browser *binary*: **libnsutils**
(base64/time/unistd — hard dep; `nsutest` base64 round-trip PASS), **libnslog**
(logging + a flex/bison filter language — used by NetSurf's `utils/log.c`),
**libnspsl** (public-suffix list, pre-generated `psl.inc`), and **libnsfb** (the
framebuffer *surface* the fb frontend renders into — RAM surface only;
`nsfbtest` plots a grey rect + reads the pixel back, PASS).  All four install
into the store + soname into `/lib` for ld.so.  The `netsurf` app source is
fetched (`third_party/netsurf/`, gitignored).  Support libs from §M38/§M39
(zlib, libpng, freetype, harfbuzz, mbedTLS) are also in the store.

**Next concrete steps for the browser binary (Tier 1 bring-up, its own
multi-session push):**
1. **Frontend = libnsfb → framebuffer, DECIDED.**  The RAM surface is built and
   proven (`nsfbtest`); its buffer is exactly what we blit into a `gui_window`
   (like the Wayland bridge).  No §M40 needed.  The fb frontend also pulls in
   NetSurf's own `fbtk` widget toolkit + `font_freetype` (we have freetype).
2. **Fetcher = a custom NetSurf fetcher over M24 + §M39 mbedTLS, DECIDED (do NOT
   port libcurl).**  NetSurf's fetch layer is a scheme→`fetcher_operation_table`
   (`initialise/setup/start/poll/finalise`); the `curl.c` fetcher is one impl,
   gated by `NETSURF_USE_CURL`.  We already fetch https in ring 3 with
   `user/wget.c` (getaddrinfo + connect + mbedTLS CA-verify) — so write a `dos`
   fetcher that reuses that path, registered via `fetch_add_fetcher`.  Start
   with the built-in `data:`/`file:`/`about:`/`resource:` fetchers (no network)
   to bring layout+render up headless first, then add the network fetcher.
3. **Build the netsurf binary (~161 core+fb TUs).**  Bypass their buildsystem
   like the libs; disable JS (no Duktape/nsgenbind) and PDF/SVG.  Codegen needed:
   `split-messages.pl` (perl) for the i18n Messages only.  Provision the runtime
   resources (`resources/`: default.css, Messages, favicon, ca-bundle) + a TTF
   font to the VFS (`NETSURF_FB_RESPATH`/`NETSURF_FB_FONTPATH`).  Resolve the
   remaining core-lib API glue against our store `.so`s.
   *Recon done 2026-07-21 (no fundamental blocker — it's a flag-reconstruction
   job):* `utils/messages.c` + `content/content.c` already compile clean against
   the store-lib headers with `-std=gnu99 -Dnsframebuffer -Dsmall` + `-I{.,include,
   content,content/handlers,utils,frontends,frontends/framebuffer}` + each store
   lib's `include/` + `-I../zlib -I../freetype/include`.  Two concrete next
   blockers seen: (a) `desktop/browser.c` wants `content/handlers/` on the include
   path (the css/html handlers live there, not the old `render/`); (b)
   `frontends/framebuffer/gui.c` needs the config `-D` macro set the buildsystem
   normally supplies — `NETSURF_HOMEPAGE`, the version defines, and the
   `NETSURF_FB_RESPATH`/`_FONTPATH`/`_FONT_*` paths (see
   `frontends/framebuffer/Makefile.defaults`).  So the task is: assemble the TU
   list (core + fb, minus js/curl/pdf/svg), the full `-I` set, and the `-D`
   config batch — then write the `dos` fetcher and provision resources+font.
4. **DoD:** `netsurf https://example.com` renders text+layout into a
   `gui_window`; links clickable.  Stage it: render `file:`/`about:blank` to a
   RAM surface and dump headless first, then wire the surface to the desktop and
   add the network fetcher.

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

## §M66 — Driver agility: lifecycle, hot-plug, quarantine — ✅ shipped

**Shipped 2026-08-25 — see DOCS.md §4.80.**  Asked for from use: *"is driver
loading plug and play?  can we load one the moment it is needed, swap one, stop
a broken one?"*  The answer at the time was no on every count, and one part was
worse than that — `driver_ops.shutdown` had been declared since §M8 and
documented as "called on power-off / reboot", and **nothing had ever called
it**.

Six steps, each measured: an orderly power path that really stops drivers (in
reverse init order, because init order is dependency order); device LIFETIMES,
so a class registry can refuse to release a device somebody is still inside a
call on; `drv stop|start|swap`; **PCI hot-plug**, which needed BAR assignment
x86 had never had (firmware programs BARs at boot, so anything added later
arrives with none); and fault QUARANTINE, so a misbehaving driver costs one
attempt rather than a restart loop.

The registry became a slot table rather than an index into the linker section
— which is what makes §M67 possible at all.

**Deliberately NOT claimed:** this contains the CONSEQUENCES of a driver that
fails, not a driver that corrupts memory.  In one address space the damage is
done before anything notices; that is §M33, and calling this isolation would be
the "isolation theatre" §M33 refuses by name.

---

## §M67 — Loadable driver modules — ✅ shipped

**Shipped 2026-08-27, all three arches — see DOCS.md §4.81.**  A relocatable
ELF object on disk becomes a `struct driver` the registry cannot tell apart from
a built-in one: an `EXPORT_SYMBOL()` table, a version check that is a
compiler-computed struct fingerprint plus a hand-bumped number for the semantics
it cannot see, an ELF relocator for all three arches, and unload.  `hda` ships
as a module on x86 and `loopback` on every arch, both built from the SAME source
as their built-in form.

**Two deviations from the plan above, both deliberate:**

  * The symbol table is a **registry, not a generated scrape**.  `nm` over the
    linked kernel needs a multi-pass link and makes the export surface
    accidental; a linker section makes "what may a module call" a list somebody
    wrote.
  * The version check is **two** checks, because the plan's single "versioned
    ABI" cannot cover both cases: the fingerprint catches layout and cannot be
    forgotten, the number catches semantics and can.  It was needed on day one —
    `driver_ops.shutdown` changed signature without changing any struct's size.

**The ordering question this plan raised, answered:** §M67 shipped BEFORE §M33
and that is defensible only because its first customer is our own code,
differently packaged.  Nothing about the loader is a security boundary — module
code is ring 0 in one address space with no W^X — and the moment the goal is a
driver from a THIRD PARTY, §M33 is the prerequisite.  That has not changed; it
has only been made explicit in the source (modload.c's header says it) rather
than left as a note in a plan.

**Still open:** aarch64 B/BL relocations are refused rather than veneered when
out of ±128 MiB (untestable on the configurations this tree runs, and an
untested fallback is not a fallback — the trigger and the fix are written down
in modload.h); no module dependencies (a module cannot import from another
module, only from the kernel); no signing, which is meaningless before §M33
anyway.

---

## §M68 — A dynamic privilege model: an INVESTIGATION with a verdict required

**Status: study.  Not scheduled for implementation, and it must not be until
this section produces a measured answer.**

**Asked for directly, and it is a recurring topic** — which is the reason to
write it down properly rather than answer it from memory a fourth time.  The
request: a user-switchable *dynamic ring model*, where the system uses only the
privilege levels that are actually available and adapts automatically.  Ring 0/3
only, or ring 0/1/2/3.  A setting along the lines of **"Use dynamic RING
model"**.  And explicitly: **the effort does not matter, provided the system's
stability and modularity stay correct** — but whether it has any justification
at all is part of what this milestone must decide.

The underlying instinct is right and is worth stating on its own, because it
outlives whatever this section concludes about rings: **a system should adapt to
the protection the machine offers rather than compile in one model.**  The
question is whether RING COUNT is the axis on which to express that.

---

### 1. What "dynamic" can mean — three readings, and only one has content

**(a) Discover how many rings the CPU has.**  Near-zero content.  x86 has always
had four; aarch64 always has EL0 and EL1; RISC-V has M/S/U.  The *count* is not
a discoverable variable on any target we have or plausibly will have.  A probe
that always returns the same answer is a constant with a function call in front
of it.

**(b) Discover which privilege MECHANISMS the machine offers, and place code
accordingly.**  High content — this genuinely varies between machines, between
CPU generations, and between the three arches.  This is where the user's
requirement ("only work with what is available, push the system towards
automatic adaptation") actually lives.  §5 below is the list.

**(c) Move a component between privilege levels at runtime without a rebuild.**
This is **§M33 (execution domains)**, already designed: a service declares which
domains it *can* run in, config chooses among them, the broker resolves domain →
transport at bind.  §M68 must not build a second mechanism next to it.

So the honest shape of this milestone is: **(b) supplies the facts, §M33 supplies
the mechanism, and the "dynamic" part is the POLICY that connects them.**

---

### 2. The fact that decides the ring question

**Paging has exactly one privilege bit.**  The x86 page-table U/S bit and
aarch64's AP bits distinguish *supervisor* from *user* and nothing else.  Rings
0, 1 and 2 are all supervisor to the MMU.

**So a ring-1 driver can read and write every kernel page.**  It is not
memory-isolated from the kernel in any way the hardware enforces through paging.
What rings 1 and 2 *do* restrict is narrower:

  * privileged instructions (`LGDT`, `MOV CR*`, `HLT`, …) — real, and small;
  * I/O port access, via IOPL and the TSS I/O bitmap — real, and useful for a
    legacy device driver;
  * **segment limits** — and this is the whole argument.

**Segment limits are the only mechanism by which rings 1/2 have ever bought real
memory isolation**, and this is exactly what OS/2 used ring 2 for.  A ring-1
code/data segment can be limited to a subrange of the linear address space, and
the CPU enforces it on every access without paging's involvement.

**And it exists on exactly one of our three targets.**

| Target  | Rings / ELs available | Segment limits enforced? | Real isolation from rings 1/2 |
|---------|-----------------------|--------------------------|-------------------------------|
| i386    | 0,1,2,3               | **Yes**                  | **Yes** — via segmentation    |
| x86_64  | 0,1,2,3               | **No** (ignored in long mode for CS/DS/ES/SS) | **No** |
| aarch64 | EL0,EL1(,EL2,EL3)     | no such concept          | **No** — see below            |

**aarch64 has no intermediate level for this purpose.**  EL2 is the hypervisor
level and EL3 the secure monitor; neither is "a slightly less privileged kernel".
Using EL2 to contain a driver means *writing a hypervisor and running the kernel
as its guest* — a real technique (§5.3), but not "one more ring", and nothing
about it is served by a ring-count abstraction.

**PROVISIONAL VERDICT ON THE RING AXIS, to be confirmed or overturned by §6's
measurement:**

> Rings 1 and 2 buy real memory isolation on **exactly one** of our three
> targets — the 32-bit one — through a mechanism (segmentation) the other two do
> not have.  A dynamic ring model would therefore deliver its **strongest
> isolation on the oldest and least important architecture and nothing at all on
> the two that matter.**  That is backwards, and it is the specific reason the
> ring axis is a weak instrument for the goal, rather than a general objection to
> the goal.

This is also, restated with its evidence, the reasoning already recorded as
*"ring model LOCKED"* — but that entry asserted the conclusion without the
table, which is why the question kept coming back.

---

### 3. The cost that has to be paid against any benefit

Stated so that "the effort does not matter" is applied to a real number rather
than to an unknown.  The effort here is not the implementation, it is the
PERMANENT surface a second privilege level adds:

  * a second calling convention and a second stack discipline at every boundary
    crossing (the ring-1 ⇄ ring-0 gate is not free and is not the syscall path);
  * a second origin for every trap, fault and interrupt — every handler in the
    tree gains a case, and §M54's lesson is what happens when one of them is
    missed;
  * **`SYSCALL`/`SYSRET` on x86_64 only work between ring 0 and ring 3** —
    a ring-1 component cannot use the fast path at all, so it pays `int`-gate
    cost on the arch where the fast path was the point (§M52 is the file that
    would have to grow the case);
  * every `_k`/`_u` pointer-provenance rule (§M46) becomes three-valued.

That cost is unconditional and forever.  It is why this milestone demands a
measured benefit BEFORE the code, not after.

---

### 4. The anti-goal, borrowed from §M33 by name

**No isolation theatre.**  A `security.isolation = auto` that reports a component
as "isolated" while the mechanism cannot enforce it is strictly worse than
reporting `off` — it converts an accurate absence into a false presence, and the
user goes looking for the wrong problem (§M23's argument for three taskbar sound
icons rather than two, one layer down).

Concretely: **anything with DMA is not isolated by ANY ring or address-space
mechanism unless there is an IOMMU.**  A device that can be told to write
anywhere writes anywhere, whatever ring its driver sits in.  Any policy this
milestone produces must treat "has DMA" + "no IOMMU" as a hard downgrade to
`DOMAIN_KERNEL`, and say so.

---

### 5. Where the dynamism actually is — the capability ladder

This is reading (b), and it is the part with genuine content.  Each rung is
DISCOVERABLE at runtime, VARIES between real machines, and enforces something the
hardware actually checks.  A "dynamic" policy is a walk down this ladder.

**5.1 Cheap hardening, always taken when present** — `SMEP`/`SMAP` (x86_64),
`PXN`/`PAN` (aarch64).  Not a domain, not a placement choice; just on.  Worth
enumerating because a report that lists them is the first honest answer to "how
protected is this machine".

**5.2 Memory protection keys — the strongest candidate, and the one that
actually addresses §M67's gap.**  `PKS` (Protection Keys for Supervisor, x86_64)
and ARMv8.9's `S1POE` let SUPERVISOR pages carry a key, with access flipped by a
single register write — no address-space switch, no TLB flush.  That is
in-kernel isolation *without leaving ring 0*, which is precisely the shape "more
rings" was reaching for, at a fraction of the boundary cost.

**It is the direct answer to what §M67 shipped without:** a loaded module runs in
ring 0 with no isolation, and §M67's own header says so.  A PKS-tagged module
heap would make "a module cannot scribble on the kernel" enforceable rather than
advisory — *and it does not need a single new ring.*

**5.3 Virtualization** — VMX/SVM on x86, EL2 on aarch64.  A driver in its own
guest, with an IOMMU behind it.  Heavy, real, and the only mechanism on this list
that contains a DMA-capable driver completely.  Xen's driver domains are the
existence proof.

**5.4 Address spaces + capabilities** — §M25 and §M33's `DOMAIN_USER` /
`DOMAIN_ISOLATED`, which the tree already has the substrate for.  This is the
rung that is reachable today.

**5.5 Nothing available → `DOMAIN_KERNEL`**, which is where everything is now,
reported honestly rather than dressed up.

---

### 6. Definition of done — what this milestone must produce

It is a study; the deliverable is a decision with evidence, plus the one piece of
machinery that is useful regardless of which way the decision goes.

1. **A capability report, on all three arches** — `caps` / `/proc/security`,
   listing what the machine offers: SMEP, SMAP, PKU, **PKS**, VMX/SVM, IOMMU
   (VT-d / AMD-Vi / SMMU), and on ARM EL2 availability, PAN, POE.  *This is
   worth building whatever the verdict is*: no adaptive policy of any kind can
   exist without it, and "how protected is this machine" currently has no
   answer at all.  It is also the smallest honest thing to ship first.

2. **A measured answer on the ring axis, on i386** — a prototype putting one
   driver at ring 1 behind a limited segment, with numbers for: the boundary
   crossing cost versus a direct call, and what the limit actually prevents
   (demonstrated by an out-of-range access being FAULTED, not by assertion).
   *A prototype that only shows it works is not the measurement; the measurement
   is what it costs and what it stops.*

3. **A verdict, written down with the numbers behind it**, and if the verdict is
   no, the reason recorded here **so the topic stops recurring** — which is the
   stated purpose of this milestone.

4. **If yes:** the switch, and its NAME matters.  Calling a setting "rings" when
   the mechanism doing the work is a protection key or an IOMMU is a label that
   lies, and this tree has a rule about controls that misreport their subsystem.
   Proposed shape:
   `security.isolation = off | auto | strict`, where `auto` walks §5's ladder and
   `strict` refuses to run a component whose declared domain cannot be enforced;
   plus `security.isolation_report`, because the policy must be able to say WHY
   it placed something where it did.  A ring-specific sub-key
   (`security.x86_rings`) would then be an i386-only detail underneath, not the
   headline — because per §2 that is what it is.

---

### 7. Relationship to the milestones on either side

**§M33 is the prerequisite, not the sibling.**  §M33 already defines the
vocabulary (a declared set of domains, config choosing among them, the broker
resolving domain → transport).  §M68's product is the POLICY that picks a domain
from hardware capability — one function, given §M33's mechanism, and a fork of
the whole design without it.

**§M67 is what makes it urgent rather than academic.**  Before loadable modules,
"everything in ring 0" described code that shipped in the image and was reviewed
with it.  It now describes code that arrives as a file.  The scope written into
`modload.c` — *modules built from this tree* — is what holds until something on
§5's ladder is real, and §M68 is how that scope gets lifted.

**Ordering:** §M33 → §M68 step 1 (the capability report, useful immediately) →
§M68 steps 2–3 (the verdict) → implementation only if the verdict says so.

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

## §M43 — Native developer toolchain (self-hosting)

**Why:** the goal is to **develop d-os on d-os, in d-os** — stop needing a
Linux host + a Docker cross-toolchain to build software for the system.  The
**first packages installed into the §M35.5 store are the developer tools
themselves** (compiler, assembler/linker, `make`), so from then on new
software (and eventually d-os userland itself) can be built natively.

**Design.**
1. **Compiler** — port **clang/LLVM** (more portable than GCC; LLVM is the
   common backend for Rust/Swift/.NET-AOT too, so one port pays off widely) OR
   GCC.  Both are large C++ programs → **hard-depends on §M37 (dynamic
   linking) + §M38 (C++ runtime + support libs)**; they ship as `.so`s and
   assume a broad POSIX surface (fork/exec/pipes/files/mmap — mostly present).
2. **binutils** — assembler + linker (`as`, `ld`) — or LLVM's `lld`/
   integrated assembler, avoiding a separate binutils port.
3. **make / a build driver** — `make` (small C program) so existing Makefiles
   run; later `ninja`/`cmake` (C++, needs §M38).
4. **Headers + a native libc dev package** — the musl headers + `libc.so`/
   `libc.a` packaged in the store as a normal dependency closure.

**Definition of done:** `cc hello.c -o hello && ./hello` works **on d-os**,
the toolchain living in `/store`; a small multi-file program builds via a
Makefile.  Stretch: rebuild a piece of d-os userland natively.

**Key levers / notes.**
- **Cross-compile on the host stays valid and cheap** — self-hosting is the
  *bonus*, not a prerequisite for running compiled programs.  Effort is
  dominated by the **compiler being a huge C++ program**, i.e. by §M38, not by
  anything toolchain-specific.
- Distinguish **running a compiled binary** (works today for C via musl; §M38
  adds C++) from **running the compiler itself on d-os** (this milestone).

**Depends on:** §M37, §M38 (the compiler + its libs are dynamically-linked
C++), §M35.5 (installs into the store).  Primary arch: x86_64.

---

## §M44 — Language ecosystems (Rust / C++ / .NET / Java)

**Why:** broaden the platform beyond C.  Analysis (2026-07-18): the effort is
dominated by **(a) the language's runtime port and (b) syscall/ABI breadth —
NOT by the compiler** (cross-compiling on the host is free).  A second lever:
because the **Linux-ABI personality** (§M36/§M41) runs unmodified musl Linux
binaries, **any language that emits a static/dynamic musl Linux ELF (C, C++,
Rust, Go, Zig, .NET NativeAOT) can run with "just" syscall breadth — no
per-language runtime port.**  Only JIT/VM languages need the VM itself ported.

**Effort ranking (running cross-built binaries on d-os):**
| Lang | Path | Depends | Effort |
|------|------|---------|--------|
| C | musl ELF | — | trivial — **done** (static + §M37 dynamic) |
| **Rust** | `i686/x86_64-unknown-linux-musl` static | syscall breadth (std) | **low–medium** — it's just a musl ELF; a good early win after §M36 breadth |
| C++ | musl + C++ runtime | §M38 | medium |
| .NET **NativeAOT** | AOT → native musl ELF | §M38 + AOT runtime + GC | medium–high (sidesteps the JIT) |
| Java (JVM) | full OpenJDK port | §M38 + threads + JIT (W^X mmap) + GC + class lib | **high (months)** |
| .NET **CoreCLR** (JIT) | full CoreCLR port | as JVM | high |

**Guidance.** Order Rust → C++ (falls out of §M38) → .NET NativeAOT → then the
JIT/VM heavyweights.  A **"Rust hello world"** is a cheap, motivating target
soon after §M37 + §M36 breadth.  For .NET, prefer **NativeAOT** over porting
CoreCLR's JIT.  All of this is a strong argument for investing in **§M41 (the
Linux ABI shim)** — the universal "run any Linux binary" accelerator.

**Rust's GUI payoff = `iced` (§M40 table).**  Once Rust std runs on musl, `iced`
(Elm-style, pure Rust) reaches a real windowed GUI app with a *lighter* stack
than the C/C++ toolkits: its `tiny-skia` backend is a pure-Rust CPU rasteriser
(no Mesa) and its winit/`smithay-client-toolkit` layer speaks Wayland from Rust
(no upstream libwayland-C port, no C++ runtime).  So a compelling milestone is
"an `iced` window on the §M26 server", gated mainly on Rust-on-musl + our
compositor advertising the protocols winit needs.

**Depends on:** §M36 (syscall breadth), §M37, §M38 (C++-runtime consumers),
§M41 (accelerator).  Primary arch: x86_64.

---

## §M45 — Package manager frontend + GUI installer

**Why:** the §M35.5 **store is the storage model** (content-addressed,
Nix/Guix-shaped — atomic, rollbackable, no dependency hell); this milestone is
the **user-facing experience on top of it**: an apt-like install/update/search
flow plus a graphical installer wizard, extended to **drivers and modules**,
ideally swappable **without interrupting running user processes**.

**Design.**
1. **apt-like CLI** — `pkg install/update/search/remove/rollback` fetching from
   a **remote repo** and realizing a pinned closure into the store + a profile
   generation.  Remote fetch must be **secure → depends on §M39 (TLS) + signed
   packages**; the local realize/rollback mechanics already exist (§M35.5).
2. **GUI installer wizard** — an "InstallShield-style" friendly flow on the M22
   toolkit.  **Design tension to preserve:** InstallShield = imperative
   per-app installer *scripts* (Windows model); the store is **declarative**
   (install = realize a store path + flip a profile symlink, no arbitrary
   scripts).  The wizard is a friendly front over the **declarative**
   `pkg install` — NOT per-app scripts.  Keeps the "no clutter / reproducible /
   swappable" guarantees.
3. **Driver / module hot-swap** — NOTE: this is really **§M33 (execution
   domains)**, not the package manager.  The clean path is **user-mode
   drivers**: a driver as a userland process, replaced by an M29-supervisor
   restart; the M29 **service bus** (`contract@version` + transport
   indirection) is designed for transparent reconnect → swap a driver while
   clients keep running.  In-kernel LKM-style hot-load (relocatable kernel
   objects + kernel-side symbol resolution) is the harder, riskier alternative
   and is not the default.

**First content:** the **§M43 developer toolchain** — dogfooding the installer
by using it to bring up the self-hosting dev environment.

**Definition of done:** `pkg install <tool>` fetches + installs from a repo, a
GUI wizard drives the same flow, and a user-mode driver is replaced live
without killing its clients (the last item may land with §M33).

**Depends on:** §M35.5 (store), §M39 (secure remote fetch), §M22 (GUI toolkit),
§M33 (driver/module hot-swap).

---

## §M46 — Resilient control plane: secure-attention hotkeys + force-kill

> ✅ **SHIPPED (2026-08-01) — see DOCS.md §4.37.**  All four requirements below
> hold, plus the audit-driven hardening that turned "the control plane survives"
> into "nothing a user program does can take the box down": ring-3 fault ⇒ kill
> the process (3 arches), force-kill at the preemption boundary, NMI hard-lockup
> recovery, SAK hotkeys in the keyboard IRQ, chrome-force-kill, the two-layer
> user-pointer boundary (gate + `.ex_table` fault fixup), owner-bound dosgui
> handles, sanitised sigreturn, subtree-restricted `sys_kill`, x86_64 COW.
> **Lesson learned:** the `sys_*` handlers are DUAL-USE (ring-3 dispatchers AND
> in-kernel callers).  Putting the user-pointer check inside them rejected every
> kernel caller — `linux_abi`'s `fstat(fd, &kernel_kstat)` returned -1, so musl's
> ld.so failed to load EVERY shared object and NetSurf stopped starting on both
> x86 arches.  A pointer check belongs where the pointer's ORIGIN is known: the
> dispatcher marks the task (`in_user_syscall`), and kernel-destination calls go
> to explicit `*_k` cores.  Second lesson: a validity CHECK is not a guarantee —
> only the exception table makes a copy survive a range going bad mid-flight.
> Third: firmware tables (ACPI) live at the top of RAM, so any "the identity map
> covers everything" assumption breaks the moment the box has more memory than
> the map — it cost a silent boot-time freeze at `-m 512M`.

**Why (user priority — "fontos dolgok nagyon"):** processes are now separated
(per-process address spaces, §M25) and a watchdog exists (§M31), so the desktop
must gain the *always-available escape hatches* every real OS has: a way to
reach the Task Manager and to kill a runaway/frozen program that NEVER depends
on the misbehaving program cooperating.  The window chrome must stay a reliable
kill switch even when the app behind it is wedged.

**The four requirements (all must hold under load AND when an app is frozen):**

1. **A always-live Task Manager hotkey — `Ctrl+Alt+Del`.**
   A *secure-attention key* (SAK): trapped in the low-level keyboard input path
   (the IRQ / input router), BEFORE per-window focus routing, so no app can
   swallow it and a wedged compositor path can't block it.  It raises (or
   spawns) the Task Manager, which runs at an **elevated scheduling priority**
   so it stays responsive even while other tasks peg the CPU.  "Always works" is
   the contract — verify it while a busy-loop app is running.

2. **A "close last / frozen app" hotkey — `Ctrl+Alt+X`.**
   Closes the **most-recently-opened *user-launched* window** (track a launch
   order / "user-opened" stack — exclude system tasks), OR, if the target is
   detected frozen, **force-kills** it.  Graceful first (want_close + short
   grace), force-kill on timeout or when already flagged frozen by §M31.

3. **Window chrome stays live even when the owning app is wedged.**
   Close / minimize / restore(previous-size) hit-testing + drawing already run
   on the **compositor**, not the app (M22.7), so they are inherently
   independent — the gap is only the **close semantics**: today close sets
   `want_close` and *waits* for the app to quit, which never happens if it is
   frozen.  Fix: close does graceful-then-**force-kill** (grace timer, or
   immediate force-kill if §M31 already flagged the window's task frozen).
   Minimize/restore must keep working regardless (pure compositor state).

4. **Task Manager force End-task.**
   The Task Manager's "End task" must offer a **forced** kill (not only the
   cooperative kthread contract), for a wedged pure-ring-3 process.

**The key enabler — real force-kill of a wedged ring-3 process.**
The M22.3/M27 `task_kill` is *cooperative* (a kthread only dies at its next
`task_should_stop`/yield; the M25 note "force-kill of a wedged pure-ring3 task
(needs M25/§M33 isolation)" was the open item).  It is now buildable: a ring-3
app in a userland infinite loop is **preemptible** (the timer IRQ always
regains control) and holds **no kernel locks** while spinning, so the kernel can
forcibly reclaim it —
  * on next preemption of a force-killed task: mark `TASK_DEAD`, remove from all
    runqueues, never reschedule;
  * tear down its resources off a safe context (the reaper / compositor, not the
    victim): address space (`vmm_space` free), fd table, futexes, **its GUI
    windows** (dosgui client-managed release path), then reap;
  * kill the whole **subtree** (`task_kill_tree`, M27) so children/helpers go
    too.
  A task wedged *inside a syscall holding a kernel lock* is the hard residual
  case (can't be yanked mid-critical-section) — bound it with §M31 detection +
  a "kill on return to userland" pending flag; document what is and isn't
  force-killable (mirrors real kernels' "uninterruptible sleep").

**Design notes / where things plug in.**
- SAK trap: `kernel/drivers/keyboard/*` (+ USB HID) → a new input-router hook
  checked before `keyq`/window routing; keep it arch-independent (the combo is
  decoded from keycodes, not scancodes).  Make the two combos **config-keyed**
  (`hotkey.taskmgr`, `hotkey.killapp`) per convention #5 (swappable from day 1).
- Priority: needs a minimal **scheduling-priority tier** (the round-robin
  scheduler currently has none) OR a "boosted" flag the SAK sets on the Task
  Manager task — smallest viable change first.
- "Last user-opened" tracking: a launch-order stack maintained where windows are
  created (`gui_app_window_create` / the desktop launch path), tagged
  user-vs-system.
- Frozen detection: reuse §M31 L1 per-task heartbeat / an input-ack timeout on
  the window's app-host queue; the window gains a `frozen` flag the chrome reads.

**Dependencies:** §M22.3 (Task Manager + chrome + `task_kill`), §M31 (watchdog /
frozen detection), §M27 (kill-tree + reaper), §M25/§M34 (per-process address
spaces make force-kill safe).  All shipped → this milestone is unblocked.

**Definition of done:**
- `Ctrl+Alt+Del` raises the Task Manager within a frame even while a ring-3 app
  busy-loops on the CPU; the Task Manager stays interactive.
- `Ctrl+Alt+X` closes the last user-opened app; against a deliberately-frozen
  app it force-kills it and disposes its window.
- A frozen app's title-bar **X** force-kills it (window gone, task reaped, tree
  cleaned); minimize/restore still work on it beforehand.
- Task Manager "End task (force)" kills a wedged pure-ring-3 process.
- Boot-tested in QEMU with a purpose-built "hang" test app (spin in userland /
  spin in a syscall) proving each path, on i386 (then x86_64/aarch64).

---

## §M56 — A wait that is really a wait (poll timeouts + epoll) ✅

**Shipped 2026-08-11 — see DOCS.md §4.57.**  `poll(2)` honours a finite
timeout; `epoll_create`/`ctl`/`wait` exist, level-triggered, with the set kept
in the kernel; poll and epoll share one readiness definition and one blocking
loop.  Verified on all three arches, including through an unmodified musl
binary.

**Lessons learned.**

- *A timeout that returns early is not a conservative version of one that
  waits.*  poll's positive timeout was documented as "treated as a snapshot so
  it never blocks past the caller's intent", which reads like caution and is
  actually a different function: every correct event loop written against it
  became a busy loop.

- *Extracting a shared definition is a bug-finding technique.*  Merging poll's
  and epoll's readiness into one function surfaced, in the same hour, that
  AF_INET sockets had been reported permanently ready and that stdin had never
  been reported ready at all.  Neither was visible while the definition lived
  inside a single caller.

- *A poll must not lie about which reads will not block.*  Cooked stdin becomes
  readable at end of LINE, not at the first keystroke, because that is when a
  read will return.

- *Refuse what you cannot serve, even when a superset would "work".*  Serving
  EPOLLET as level-triggered satisfies the type system and spins the program.
  -EINVAL costs the author one line; the silent downgrade costs them a day.

- *Say what the implementation is not.*  epoll's whole reputation is O(ready);
  ours scans.  The interface is the win, and writing that in the header is
  cheaper than someone later measuring it.

- *A guest struct's size is a property of the guest, and not always of its word
  size.*  `struct epoll_event` is 12 bytes on i386 AND amd64 (Linux packs it on
  x86_64 so the layouts agree) but 16 on arm64.  Deriving the size from the word
  width passes on two of three arches — the worst possible failure mode.  It
  also leaves `data` unaligned on x86, so it is marshalled bytewise: code that
  works only on forgiving hardware is a port failure waiting to happen.

**§M56.1 — the open items, finished (same day).**  Hangup reporting
(POLLHUP/POLLRDHUP/POLLERR/POLLNVAL, with RDHUP kept separate from HUP so a
reader drains the tail before shutting down); generic `O_NONBLOCK` on the open
file description; epoll sets are themselves pollable, with a bounded recursion
depth; real `sigprocmask` + `rt_sigpending` + `epoll_pwait`'s mask.

- *A stub that answers "success" is worse than one that answers "unimplemented".*
  `h_sigprocmask` returned 0 for two milestones.  Nothing failed, so nothing was
  investigated — while every program that blocked a signal got no blocking.

- *When a fallback is superseded, delete it in the same change.*  The real
  sigprocmask handler was registered in the arm64 map only; the engine declines
  unknown numbers, so on both x86 guests the old switch's stub kept answering.
  The feature worked on one arch and was unreachable on two, silently.

- *Two self-consistent conventions still need a translation.*  A Linux
  `sigset_t` numbers signals from bit 0; this kernel numbers them from bit 1.
  Neither is wrong; copying the word across is.  It failed with one wrong
  number in one test and no other symptom.

- *Report a hangup separately from a hangup-with-nothing-left.*  POLLRDHUP and
  POLLHUP look redundant until a writer closes with data still buffered.

- *Measure before optimising, and write the number down.*  epoll's scan costs
  ~620 ns per registered fd here, so the 64-item cap bounds it at ~40 µs and a
  real loop pays ~6 µs.  Per-fd wakeups would buy none of that back and would
  add a lifetime relationship between epoll items and open file descriptions —
  §M54's defect class.  Declining is the decision; the measurement is why.

**§M56.2 — no bugs left behind.**  `EPOLLERR` has a producer (a TCP RST is not
a FIN — an error, not an orderly EOF); signal masks are handled at their real
8-byte width; and `epoll_wait`'s scan was finished by building the readiness
cache, measuring it, and removing it again.

- *A cache whose invalidation must be remembered at every mutation site is a bug
  generator.*  The memo was correct about lifetimes — the fd table is consulted
  every time, so a remembered pointer is only compared — and still wrong,
  because a pipe's readability changes when its OWNER reads and its writability
  when its PEER reads.  Two sites were missing on the first attempt, and the
  failure mode is an event that never arrives.

- *Write the test that can falsify the optimisation before the optimisation.*
  Twenty ready/idle transitions caught it on the first run.  Without it the
  cache would have shipped and the missing event would have surfaced weeks
  later, in something else.

- *Measure before AND after.*  It saved 15.5 µs against 16.4 µs — inside the
  noise — because the per-item cost was the descriptor lookup and the loop, not
  the readiness evaluation it was skipping.  The optimisation was aimed at the
  wrong thing.

- *Do not claim a speedup the benchmark does not show.*  Removing the redundant
  second lookup is obviously right and cannot be wrong; at this scale it is also
  not measurable, and the comment says so.

**Still open:** genuine per-fd wakeups (declined on the measurement, with the
trigger written down); `EPOLLEXCLUSIVE` and edge-triggered mode.

---

## §M57 — A task is not dead while it is still running ✅

**Shipped 2026-08-12 — see DOCS.md §4.58.**  §M54's three open items closed.
`cpu_home` becomes an ownership token (claimed by CAS, released after the
unlink, both under the owning queue's lock); migration becomes one transition
under both rq locks in ascending cpu_index order; `rq_rotate_to_tail_locked`,
`task_set_affinity` and `task_set_nice` stop acting on a queue named by an
unlocked read.  The residual itself turned out not to be a corrupted ring at
all: `task_exit_code` published DEAD and then did preemptible work, and since
`pick_next_local_locked` picks only RUNNABLE tasks, a preemption in that window
stranded the task forever — linked in a runqueue, never reaching its own sweep,
and freed by the reaper with a live frame on its stack.  New `rqcheck` and
`schedstorm` state the runqueue invariant in code and check it.  Measured: 2880
kills with 0 `STILL QUEUED` (was ~1 per 200–300), ~420k affinity/nice churn ops
with 0 structural violations, three arches.

**Lessons learned.**

- *A test that cannot fail is not evidence.*  The first `schedstorm` drove
  affinity from one task and reported `ok` even against the pre-fix code — the
  only thing it could race with was the balancer's 100 ms tick, so a 240 ms run
  offered about two chances to hit a window measured in instructions.  With four
  concurrent churners the same test takes the pre-fix kernel down with an NMI
  hard-lockup inside a minute.  **Run the new test against the OLD code before
  believing it about the new code.**

- *A diagnostic must be captured where the evidence still exists.*  The reap
  report printed the task's link state AFTER the sweep had removed it — which is
  identical for every possible cause and therefore says nothing.  Moving the
  capture under the lock and before the removal turned "STILL QUEUED, once in a
  few hundred kills" into one line naming the fault exactly.  It cost a round of
  wrong theories first.

- *Half of a two-sided invariant is worse than neither half.*  §M54 tried
  clearing `cpu_home` in the steal without adding the matching claim, and it
  hung tasks: a queued task carried -1 forever, so every block path silently
  failed to detach.  The pair (claim on insert / release on remove) only works
  as a pair.

- *A value documented as a fact must be enforced as one, or it is a hint.*
  `cpu_home` had said "which CPU's rq this task lives on" since M18.6.1 and was
  written by whoever felt like it.  Four sites then mutated a ring while holding
  another CPU's lock — two of them reachable from ordinary shell commands.

- *A state that makes a task unschedulable must be published atomically with
  the switch away.*  "Mark yourself DEAD, then tidy up" reads as obviously safe
  and is the opposite: everything after the store runs on borrowed time that the
  scheduler has already refused to grant.

- *A logging path that shreds under concurrency is a correctness bug, not a
  cosmetic one.*  `printf.c` still said "single-threaded today" — true when
  written, false since §M18 (the §M52 shape again).  Every automated check here
  is a grep over the serial log, so a passing `killstorm` was read as a frozen
  shell purely because two CPUs interleaved a line.  **A harness that silently
  loses output is worse than no harness, because it is trusted.**

- *Serialising output must not use the obvious lock.*  Interrupts-off across a
  message would hold the CPU through a multi-megabyte framebuffer scroll and
  trip the softlockup watchdog — the logging path would make the machine look
  wedged.  Preemption-off plus a re-entry test taken BEFORE the acquire is the
  version that cannot deadlock the one path you need when everything else is
  already broken.

**Still open:** `AARCH64_MAX_CPUS` ships at 2, so ARM verification is real SMP
at two cores; `load_balance_pull`'s migration counter still counts an attempt
whose insert can be refused (diagnostic only).

---

## §M55 — Network async: one poller, blocking waiters ✅

**Shipped 2026-08-11 — see DOCS.md §4.56.**  The network stack no longer
polls the NIC from whichever task happens to be waiting.  One poller task
(`netd`) is the only caller of `dev->poll`; every other task blocks on the
stack's wait queue until its condition holds or a real millisecond deadline
passes, and netd itself blocks whenever nobody is waiting.  Measured with the
new `netstorm [n]`: 8 and 16 concurrent waiters both finish in ~3.4 s at 2–3%
of 4 CPUs.

**Lessons learned.**

- *A comment describing a concurrency model is a dependency on a premise.*
  net.c's header said "everything runs in one task context → no locking" and
  that was true when written.  Nothing in the build noticed when blocking
  primitives, SMP and ring-3 sockets each made it less true.  (§M52's lesson,
  in a different file.)

- *A spin count is not a timeout.*  `20000000u` iterations means a different
  duration on every machine, every arch and every host CPU under emulation.
  Once §M53 gave the kernel a nanosecond clock there was no excuse left, and
  the musl-resolver hang of §M39 was this defect all along.

- *Holding a lock across a wait for the thing that will release you is a
  deadlock with extra steps.*  `net_wait_cond`'s "poller not up yet" fallback
  spun with the stack lock held and interrupts off — so it could not be
  preempted, and what it was waiting for was the scheduler starting `netd`.
  Eight tasks entering it at once starved the very task whose arrival would end
  it.

- *An implausible number is evidence, not noise.*  Two runs doing identical
  work reported 143 115 and 4 463 pumps.  One of them had to be wrong, and
  chasing the discrepancy is what found the bug above.  I nearly let it go
  because the number merely looked large rather than impossible.

- *Check a build's exit status, not its output.*  A `grep error:` filter over
  build output let a compile failure through, and the first explanation of that
  discrepancy was measured on a stale ISO — §M51's lesson arriving by a
  different route.

- *Ordering was load-bearing.*  Wiring the NIC interrupt first would have put
  two pollers on one virtqueue (the ISR and every spinning waiter).  The lock
  and the single poller had to land before the interrupt could be safe.

**Part 2 — the NIC interrupt (same day).**  Safe to wire only after part 1:
beforehand it would have been the ISR plus every spinning waiter on one
virtqueue.  The ISR acks the device and wakes the poller, and does not drain the
ring (draining runs `net_rx`, which can generate a TCP ACK, which spins on the
TX virtqueue).  netd blocks on the interrupt with a 10 ms backstop, so a missed
interrupt costs latency rather than liveness.  `nettest` 4982 → 26/39 pumps;
`netstorm 8` 4047 → 286 pumps at 0% of 4 CPUs.

- *An interrupt you have not received is a promise, not a capability.*  The
  stack flips to interrupt mode on the first one it actually takes, so a driver
  that wires a line which never fires degrades to polling instead of blocking
  forever.

- *A counter beats a flag when the question is "since when".*  Sampling the
  interrupt sequence before the pump and re-comparing it under the queue lock is
  what makes an interrupt that lands during the pump impossible to lose.

- *Separate the alarming number from the diagnostic one.*  Backstops fire ~300
  times during `netstorm` and that is the design (nothing is coming, so the
  timer is the only waker).  Only a backstop whose next pump finds frames is a
  missed interrupt, and that is counted on its own.

- *A self-contradictory measurement is evidence about the measuring apparatus.*
  A run reported "serialised, peak waiters 0" and "8 probes done in 2912 ms" at
  once.  Both cannot hold: I had changed a struct in a shared header and rebuilt
  without `make clean`, and this project has no header dependencies.  §M51's
  lesson, stated in CLAUDE.md, and it still caught me.

**Still open:** the stack remains single-instance above the transport (one ping,
one DNS query, one TCP connection) — this milestone makes concurrency safe, not
multi-connection.  Then `epoll` and non-blocking file I/O.

---

## §M50 — The guest-ABI translation engine (STARTED 2026-08-07)

**Shipped so far (DOCS §4.48):** the pipeline exists and two live
architectures run their musl userland through it.  What remains is
migrating the rest of the vocabulary and adding the aarch64 shim.

**Why it exists.**  `hal/x86/linux_abi.c` and `hal/x86_64/linux_abi.c` are
2275 lines and ~160 `case` labels between them — two copies of one idea.
aarch64 (PLAN_AARCH64 A2) would have been a third.  The syscall NUMBERS
differ per platform; the MEANINGS do not.

**The pipeline.**

```
guest trap ──► arch shim ──► number map ──► canonical op ──► handler
  (regs)      (frame→args)  (per guest ABI)  (arch-neutral)   (shared)
```

- a new ARCHITECTURE is a shim (~6 lines: which registers hold what);
- a new GUEST ABI is a table;
- a new SYSCALL is one handler, and every arch gets it at once.

**Migration is incremental by construction.**  The engine may DECLINE a
number it does not know, and the hand-written switch stays as the
fallback — so the 2275 lines move one operation at a time, with the old
path beside the new one for comparison, instead of in one unverifiable
jump.

---

### Can this reach Windows?  (design note, 2026-08-07)

Asked directly, and worth writing down because the honest answer changes
the shape of the vocabulary rather than just extending it.

**The syscall boundary is the wrong cut for Windows.**  Linux's syscall
numbers are a stable, documented contract — which is exactly why a number
map works.  Windows' NT syscall numbers are *not*: they are an internal
detail that changes between builds, and no Microsoft documentation
promises them.  This is why neither Wine nor WSL1 translates there.  Wine
cuts at the **library** boundary (a PE loader plus reimplemented
`ntdll`/`kernel32`), WSL1 cut at the NT syscall layer and Microsoft
eventually abandoned it for a real kernel in WSL2.

So the pipeline generalises, but the cut point is per-guest:

| guest | cut | why |
|---|---|---|
| Linux | syscall number → op | the ABI *is* the contract |
| Windows | PE loader + DLLs → op | the DLL exports are the contract |

The canonical operation vocabulary is what both land on — and that is the
part worth building now, because it is the same asset either way.

**Where the real chasms are.**  They are not in the syscall translation;
they are semantic, and a table cannot hide them:

- **Object model.**  A Linux fd is a small integer into a per-process
  table.  An NT HANDLE is a reference to a typed, named, security-checked
  kernel object.  Mapping one onto the other loses type and ACL.
- **Paths.**  Drive letters, backslash separators, case-insensitive-but-
  case-preserving semantics, the extended-length namespace, and the fact
  that a Windows path can name things that are not files.
- **Process creation.**  `fork` has no Windows equivalent — `CreateProcess`
  builds a process from scratch.  Emulating `fork` on a Windows host is a
  known-hard problem (Cygwin's is famously fragile); the reverse
  (`CreateProcess` on a fork/exec kernel) is much easier, and is the
  direction d-os would need.
- **Errors and control flow.**  errno vs `GetLastError` vs HRESULT vs
  structured exception handling; signals vs APCs.
- **Memory.**  `mmap` vs the reserve/commit split of `VirtualAlloc`.
- **The registry and security descriptors**, which have no analogue at all.

**What that implies for the design here.**  Do not let the vocabulary
quietly become "Linux with different numbers".  Two rules follow:

1. **Name operations after what they DO**, not after any one system's
   spelling — already the rule in `abi.h`, and the reason `ABI_SEEK` is
   not `ABI_LSEEK`.
2. **A semantic mismatch must be a visible adapter, not a silent lie.**
   Where a guest's operation cannot be satisfied exactly, that belongs in
   a named per-guest adapter with its own state (a HANDLE table, a path
   translator), and it should be *possible to refuse* rather than
   approximate.  §M29's service bus already has this shape — strict bind
   by default, `BUS_ADAPTER` only when `bus.allow-adaptation` is set —
   and the same discipline applies here.

**Realistic staging, if it is ever pursued:**

1. Finish the Linux side: migrate both x86 layers into the engine, add
   the aarch64 shim (PLAN_AARCH64 A2).  This is the part with immediate
   value, and it is what proves the vocabulary.
2. A PE/COFF loader — independently useful and self-contained, and it
   answers "can we even load one" before any ABI question arises.
3. A minimal `ntdll`/`kernel32` subset in USERLAND, implemented on d-os
   syscalls, with a HANDLE table and a path translator as real objects.
   Target console programs only; a GUI program means reimplementing
   `user32`/`gdi32`, which is a different project entirely.
4. Only then ask whether any of it should move into the kernel.

**Expectation setting:** stage 3 is where Wine has spent three decades.
As a teaching exercise on "hello world" console binaries it is a few
weekends; as compatibility with real software it is not a milestone, it
is a career.  Worth doing for what it teaches about the boundary — worth
being explicit that it will not run Photoshop.

---

## Desktop UX cluster (§M58 – §M62) — the things a person actually reaches for

Five milestones requested from USE, not from the architecture: select text,
copy it, change the wallpaper, change the resolution, and decide whether the
machine boots with a log or a splash.  They are grouped because they share one
property — **each looks like a small feature and each is blocked by a missing
piece of plumbing rather than by missing polish**, which is exactly the shape
§M48 hit when "NetSurf is not clickable" turned out to be *the compositor has
no mouse-button event at all*.  Every design below therefore starts by naming
the plumbing, not the pixels.

Order is dependency, not importance: §M58 produces a selection, §M59 is where
it goes, §M60 and §M61 are the display's own state, §M62 is what the user sees
before any of it exists.  §M63 is the container all of their settings land in,
§M64 puts the user's own choices on the desktop.

### Execution order (scheduled 2026-08-21 — this is the running plan)

Sequenced for **visible payoff per block**, not for architectural tidiness.
Every earlier milestone in this project was infrastructure whose success looked
identical to its absence; this cluster is the opposite, and the schedule should
exploit that — **each block below ends in something a screendump can show.**
Plumbing is pulled in only when the block above it needs it, which is also why
persistence lands second rather than first: the wallpaper works from `setconf`
on the first boot it exists, it simply does not stick yet.

| Block | Ships | Ends with (the screendump) |
|-------|-------|----------------------------|
| **1** | ✅ §M60 wallpaper (DOCS §4.62) + ✅ §M63 **stage 0** persistent config (DOCS §4.63) | A photograph as the desktop background — that survives a reboot |
| **2** | ✅ The icon primitive (DRAWN, not embedded) + the abstract item view + §M64 shortcuts (DOCS §4.64) | Icons on the wallpaper; double-click opens NetSurf |
| **3** | ✅ §M63 proper — `SETTINGS_PANEL()` + `CONFIG_KEY()` generic panel + System/Personalisation/Region panels (DOCS §4.65) | One Start-menu entry opening an icon grid that changes the desktop |
| **4** | ◐ §M58 selection + §M59 clipboard — kernel half DONE (DOCS §4.69); editor drag, Ctrl+C and the ring-3/Wayland tail open | Dragging across terminal output, pasting it into the editor |
| **5** | ◐ §M61 resolution + Display panel + confirm-or-revert (DOCS §4.70; x86) | The desktop re-laying itself at a new mode, NetSurf reflowing |
| **6** | ✅ §M62 boot splash (DOCS §4.71) | The machine booting with a logo — and dropping to the log on any key |
| **7** | ◐ **The tail** (2026-08-23, DOCS §4.79) — §M64 drag-to-move + keyboard + Send to desktop, and the ramfs-persistence bug they exposed; `gui.mode` confirmed applied at `gui_start`.  Open: `run:` targets, exFAT `rename` | An icon dragged to a new slot — that is still there after a reboot |

**After block 7 the cluster is done and there is no block 8.**  The next named
milestone is **§M23 stage 2 (audio)**, which is the last real ARCH ASYMMETRY in
the tree: AC97 output exists on i386 only, at `beep`/`tone` level, with no WAV
player, no `/dev/dsp`, no mixer and no driver at all on x86_64 or aarch64.
Everything else outstanding is either a large structural milestone (§M32
multi-user, §M33 execution domains) or a packaging question (boot time is ~80 %
GRUB reading a 61 MB image, §4.68).

Rules for the run: **shell command before panel, always** (a setting with no
headless path cannot be regression-tested here); `make clean ARCH=<arch>` after
every shared-header edit (§M51's lesson, which this cluster will touch
repeatedly — `widget.h`, `gui.h`, `gfx.h`, `config.h` are all shared); and each
block is verified by screendump on i386 first, then x86_64 and aarch64 before
it is called done — §M61 especially, where the three arches genuinely differ.

---

## §M65 — Widget toolkit with a seam ✅

**Shipped 2026-08-23 — DOCS §4.78.**  Asked for from use: components behind one
API, swappable, responsive.  The finding that shaped it: the missing piece was
LAYOUT and IDENTITY, not the control list — M22's five controls all took
absolute pixels, so every panel computed its own geometry and none survived a
resolution change.

Design decisions worth keeping: a class is named in DATA (`WIDGET_CLASS()`), a
window's interface is an array of `struct ui_spec` (ints + strings), events go
to ONE sink as `(id, type, value)` — all three so the same toolkit can be driven
from ring 3, which it now is (`dosgui_ui_build`, `user/uidemo.c`).  New
capabilities go on the CLASS so no existing `widget_ops` had to change.
Responsive = three size classes measured in character CELLS plus one rule
(a row becomes a column when narrow).

**Lessons learned:** a hover repaint that sets `need_frame` without claiming an
AREA paints only the cursor's own rectangle (§4.61) — the menu left a trail;
a box per row gives each row its own label width and nothing aligns (`UI_GRID`
shares one column); table columns sized by declared weights smear when the text
is longer than its share — size from CONTENT, bounded scan; and a client-managed
window has no app-host, so BOTH its input (fixed in §M40) and its drawing
(fixed here) need the compositor to step in.

## §M58 — Text selection (a selection that exists before anything can copy it) ✅

**Completed 2026-08-22 with SCROLLBACK — DOCS §4.75.**  The open item was
"scrollback-anchored selection", and the two turned out to be one feature: a
terminal you cannot scroll back is one whose output you cannot select once one
more line has arrived.  History is a ring of rows; the selection is addressed in
ABSOLUTE LINE NUMBERS, because a grid row is a screen position that every new
line renumbers.  `termcheck` proves it by selecting an off-screen line by number.


**Kernel half shipped 2026-08-21 — see DOCS.md §4.69.**  `widget_ops.pointer`
(press/drag/release) + a real pointer GRAB; terminal windows select over the
cell grid, linear in reading order, repainted and copied on the compositor
rather than in the mouse IRQ.  **Still open:** the editor's mouse drag (its
keyboard selection model exists, it needs the new op wired), `Ctrl+C` in a
terminal (needs the *selection ⇒ copy, none ⇒ SIGINT* rule in the keyboard
path), and scrollback-anchored selection.

**Lessons learned.**

- *A missing event is not a missing feature — it is a missing sentence in a
  struct.*  `widget_ops.mouse` said click-or-double, so a drag could not be
  expressed anywhere in the toolkit.
- *New optional ops go at the END of an ops struct.*  Every `widget_ops` in the
  tree is a positional initialiser; a field inserted in the middle re-binds all
  of them, and the compiler only warns where the types happen to differ.
- *Content clicks were gated on `kind == WIN_APP`* — so a press inside a
  terminal reached nothing at all, which is exactly why the text people most
  want to copy was the one thing that could not be selected.
- *Record in the IRQ, work on the compositor.*  A grid re-render is thousands
  of blits and the clipboard allocates.

**Original design (kept for the reasoning).**  Nothing on this desktop can be
selected with a mouse.  The editor
widget has a real selection model (`anchor` + cursor, `ed_sel_min/max`,
rendered inverted) but it is driven by **shift + arrow keys only** —
`editor_mouse()` moves the caret and drops the selection.  Terminal windows,
the file manager's text, NetSurf's page and every label have no selection of
any kind.  So "copy what is on screen" is currently impossible everywhere
except by retyping it.

**The plumbing that is missing.**  `struct widget_ops.mouse` is
`(w, lx, ly, kind)` with `kind: 0 = click, 1 = double` — **there is no press,
no motion-while-held, and no release.**  A drag-selection is by definition
"press here, move there, release", so today it has no transport, whatever the
widget does.  This is the §M48 shape again and it is the whole milestone: the
event, then the model, then the rendering.

**Design.**
1. **Pointer events grow a button phase.**  Extend the widget mouse callback
   (or add `press`/`drag`/`release` entries — a new entry keeps every existing
   widget compiling unchanged, which matters because the ops struct is
   positional and there are a dozen implementers).  The compositor already
   knows button state since §M48; what is missing is *routing motion to the
   widget that took the press* — i.e. a **pointer grab**: from press to
   release, motion goes to the pressing widget even when the pointer leaves it,
   otherwise a selection stops at the window edge.
2. **A selection is a range over a MODEL, never over pixels.**  Two models
   exist and they are not the same: a byte range in a text buffer (editor,
   labels) and a **cell rectangle over a terminal grid** (row/col, with the
   line-wrap question: a wrapped logical line should copy as one line, a
   deliberately short one should keep its newline — the standard terminal trap).
   Define both; do not force the terminal through the byte-range model.
3. **Rendering is the widget's job**, damage is the compositor's: a selection
   that changes must repaint only the affected rows (§4.61's damage discipline —
   selecting a paragraph must not become a full-screen recompose per mouse
   motion, and the drag arrives at motion rate).
4. **Double-click = word, triple-click = line** are part of the feature, not
   polish; they are also the only selection a user can make in a terminal
   without a steady hand.
5. **Where it applies, in order:** the terminal window (highest value — every
   command's output lives there), the editor (upgrade its existing model to
   mouse), the file manager's path bar, then read-only labels.  NetSurf's own
   selection is the browser's, not ours — it needs only the pointer events from
   step 1 reaching it through the dosgui bridge.

**Done when:** dragging across a terminal window's output highlights it, the
highlight survives scrolling, double-click takes a word, and `Ctrl+C` (or the
middle-click primary paste, §M59) yields exactly the bytes that were inverted —
verified by screendump plus a byte-for-byte comparison of the copied text, not
by looking at it.

**Watch out for:** a selection anchored to a screen row is wrong the moment the
terminal scrolls — anchor it to the scrollback's own line numbering.  And the
mouse IRQ context rule from `desktop.h` still holds: press/motion arrive with
the WM lock held, so the widget may record a range there but must not reflow or
allocate.

---

## §M59 — The clipboard, system-wide (one clipboard, every process) ◐

**2026-08-22 — DOCS §4.76.**  The ring-3 half is done, and the bug behind it was
not the clipboard: fds 0/1/2 were not descriptor-table entries, so `dup2(fd, 1)`
was refused and NO program here could redirect anything (silently, with a
successful exit status).  Fixed; `redirtest` proves stdout redirection into a
file and into `/dev/clipboard` on all three arches.  Typed offers shipped
(`clip type`, ioctl on the device so both personalities reach it).  **Still
open: Wayland `wl_data_device`** — deliberately not shipped, because nothing in
the tree would exercise it; it needs a client that copies and pastes first.


**Kernel half shipped 2026-08-21 — see DOCS.md §4.69.**  Two slots: the
explicit clipboard and the PRIMARY selection, because what you selected and what
you deliberately copied are two intentions and one slot means every drag
destroys the other.  Middle-click pastes primary into a terminal; `clip`
(both shells) shows, pastes, copies and promotes.  **Still open — and it is the
larger half:** typed offers, the ring-3 ABI surface (§M50 ops + `/dev/clipboard`)
and Wayland's `wl_data_device` with its ownership + fd hand-off.

**Original design (kept for the reasoning).**  `kernel/gui/clipboard.c` exists
(M22.5) and is exactly what its header
says — *"kernel-global text clipboard … in-kernel GUI widgets only.  A userland
clipboard protocol arrives with §M25/§M26"*.  Both of those shipped.  So today
copying works inside the editor and inside a text field, and **nothing in ring 3
can read or write the clipboard at all** — not a musl program, not NetSurf, not
a Wayland client — which means the clipboard cannot carry anything between the
two halves of the system that a user actually moves data between.

**Design.**
1. **Keep the kernel object, widen its shape.**  The store becomes a small
   set of **typed offers** (`mime` + bytes) instead of one `char[]`, because
   `text/plain;charset=utf-8` and `image/png` are the same clipboard with
   different contents and retrofitting a type later means changing every
   caller.  Bound the size and say what happens at the bound (refuse, do not
   truncate — a silently truncated paste is worse than a failed one).
2. **A ring-3 surface.**  As canonical §M50 operations (`ABI_CLIPBOARD_GET` /
   `_SET` / `_TYPES`), so all three arches get it from one handler — and as
   `/dev/clipboard` for the shell (`cat file > /dev/clipboard`), which costs
   nothing once the object is typed and makes the feature scriptable.
3. **Wayland: `wl_data_device_manager` + `wl_data_source`/`wl_data_offer`.**
   This is the one an unmodified upstream client speaks, and it is
   **ownership-based, not copy-based**: the source app keeps the data and hands
   over a pipe fd when someone pastes.  That is the right model anyway (a copied
   50 MB image is not memcpy'd into the kernel), but it means the clipboard must
   hold *either* bytes *or* a live owner, and the owner can die between the copy
   and the paste — decide that explicitly (owner dies ⇒ offer is revoked, paste
   fails cleanly; or the kernel snapshots small offers eagerly).
4. **Two clipboards, because X11 was right by accident:** the explicit one
   (Ctrl+C) and the **primary selection** (selecting text fills it, middle-click
   pastes it).  §M58 produces the second for free; skipping it makes terminal
   copy-paste feel broken to anyone who has used one.
5. **Ctrl+C is overloaded on a terminal** — in a shell it is SIGINT.  The rule:
   with a selection active, `Ctrl+C` copies and clears it; with none, it
   interrupts.  Write it down in the source, because whichever way it is
   decided, the other behaviour will be reported as a bug.

**Done when:** text selected in a terminal pastes into the editor, into a
`pkgrun sh` command line, and into an unmodified Wayland client; and something
copied in NetSurf pastes into the editor.  Verified with a byte comparison
through `/dev/clipboard`, not by eye.

**Watch out for:** the clipboard is a **cross-process channel**, which makes it
a §M32/§M46 boundary object the day multi-user exists — the ring-3 entry points
take user pointers, so they are `sys_*_k` cores plus a gated wrapper (§M46's
rule) from the first line, not after the first EFAULT.

---

## §M60 — Wallpaper: a picture, and a way to change it ✅

**Shipped 2026-08-21 — see DOCS.md §4.62** (all three arches).  `gui.wallpaper`
= gradient | `solid:RRGGBB` | a BMP path, `gui.wallpaper_fit` =
fill/stretch/center/tile, changeable at runtime with the `wallpaper` command —
which lives in `wallpaper.c`, not in a shell, so the ARM serial REPL runs the
same implementation.  Verified by screendump on i386 + x86_64 AND by
`wallpaper check`, an off-screen render printing corner pixels and a checksum,
which is the only verification available on aarch64: the test harness passes no
display device at all there.  All three arches produce byte-identical
checksums.  **Left to §M63:** the Personalisation panel (§M63 owns the window)
and persistence (§M63 stage 0 — settings still die at reboot).

**Lessons learned.**

- *A fallback must say why, and a status line must never report the fallback as
  if it were the choice.*  The first version answered "gradient" whenever
  nothing had been rendered yet — which is the normal state before `gui` starts,
  and exactly the moment a user has just set a picture.
- *Verification has to survive the machine having no screen.*  The ARM harness
  cannot screendump, so the decoder is checked by rendering into memory and
  printing pixels.  The by-product is better than a screenshot: the numbers are
  comparable across arches.
- *A harness that quietly stops driving the guest is worse than one that
  crashes.*  `dos-shell-test.py`'s default boot marker had gone stale against
  `serial_shell.c`, so every aarch64 run typed NOTHING and printed a log that
  reads like a healthy boot.
- *Stream, do not load.*  Row-at-a-time decoding straight into the destination
  removes the two large allocations an image loader normally needs, and it is
  the same decision that keeps `center`/`tile` cheap.

**Original design (kept for the reasoning).**  The desktop background was
`gfx_vgradient(&wallsurf, …)` — a gradient computed in `gui_start`, with the
milestone label drawn in the corner.  There was no way to put an image there,
and no way to change it without recompiling.

**Design.**
1. **The wallpaper becomes a surface with a SOURCE**, chosen by config:
   `gui.wallpaper` = `gradient` (today's, and the fallback) | a VFS path |
   `solid:RRGGBB`.  Plus `gui.wallpaper_fit` = `stretch` | `center` | `tile` |
   `fill` — a fit mode is not decoration, it is the difference between a photo
   and a smear, and the scaling code is the same code §M61 needs.
2. **Decoding.**  The kernel should learn ONE trivially decodable format
   (uncompressed BMP or QOI — a few hundred lines, no allocator surprises), and
   everything else should arrive **already decoded**: NetSurf's `nsgif`/`nsbmp`
   and any PNG library are ring-3 store packages (§M42), so a userland
   `setwall` tool decoding to RGBA and handing the pixels to the compositor
   through the dosgui bridge is both smaller and safer than a codec zoo in ring
   0.  *A kernel-resident image decoder is an attack surface with a mouse
   attached to it.*
3. **Persistence** is the config store (§M5) plus the exFAT mount for the image
   itself — with the failure path defined: **an unreadable or malformed
   wallpaper falls back to the gradient and says so in klog.**  A desktop that
   will not start because a JPEG moved is a worse desktop than one with a
   gradient.
4. **UI**: a `wallpaper <path|gradient>` shell command FIRST (testable
   headless), then a **`SETTINGS_PANEL()` under §M63** — "Personalisation" —
   rather than a bespoke app of its own.  This milestone owns the panel's
   contents; §M63 owns the window it sits in.
5. **Memory**: at 1920×1200×4 the wallpaper is 9.2 MiB, and it is a contiguous
   surface (the `BUDDY_MAX_ORDER` constraint M22.6 already hit).  A second one
   held during a change doubles that — so swap through a single allocation, or
   accept the peak deliberately and write down which.

**Done when:** `setconf gui.wallpaper /mnt/pic.bmp` + reboot shows the picture,
`wallpaper gradient` restores it live, a corrupt file falls back with a klog
line, and dragging a window over it still composites at §4.61's measured cost
(a wallpaper blit is the compositor's floor — measure it, because §4.61 showed
the wallpaper is 2 % of a drag and this could easily make it more).

---

## §M61 — Display mode: changing the resolution while the machine runs ◐

**Shipped 2026-08-21 on x86 — see DOCS.md §4.70.**  `fb_mode_*` behind the
`fb_present.h` seam (Bochs VBE), the scene resize on the compositor, the
`mode` command on both shells, the Display panel, and the confirm-or-revert
dialog with a ktimer countdown.  **aarch64 declines and says why** — one
reported mode is the interface's way of saying the display cannot change.

**Lessons learned.**

- *A queued change means the requester sees stale state.*  The dialog was
  centred on the screen that no longer existed; it is built by the compositor
  once the new mode is live.
- *A window is bound to the task that creates it, and only an APP-HOST loop
  runs `on_layout`/`on_tick`.*  Built anywhere else it is an empty box.  New
  `gui_queue_open(fn)` — which is `gui_queue_launch` minus the launcher entry.
- *A guard whose condition is backwards fails only in the case it was written
  for.*  The geometry snapshot was taken in every case EXCEPT a provisional
  change, so the revert had nothing to restore and did nothing, silently.
- *Read the mode back.*  The device clamps what it cannot do rather than
  failing, so believing the write leaves the kernel drawing at a size the
  display is not showing.

**Original design (kept for the reasoning).**  The resolution is a **constant
in the multiboot header** —
`dd 1920 / dd 1200 / dd 32` in `kernel/hal/x86/boot.s`, requested from GRUB at
boot — and `FB_WIDTH 1280` in `virtio_gpu.c` on ARM.  Changing it today means
editing assembly and rebuilding.  Everything above it (backbuffer, wallpaper,
panel surface, window geometry, `wl_output`) is sized once from that number and
never expects it to move.

**Design.**
1. **A mode-setting HAL entry point**, next to `fb_present.h`'s existing cut
   (`fb_present_map` / `fb_present_flush` — the seam M21 already carved for
   exactly this kind of arch difference): `fb_mode_list()` +
   `fb_mode_set(w, h, bpp)`.
   - **x86**: Bochs-VBE DISPI (`XRES`/`YRES`/`BPP` + `ENABLE`) — the same
     register file `fb_present.c` already drives for the page flip, so the
     double-buffer's `VIRT_HEIGHT` must be re-established after every mode set,
     and the LFB's pitch re-read rather than recomputed.
   - **aarch64**: virtio-gpu `resource_create_2d` + `set_scanout` with a new
     size, and a **new contiguous framebuffer allocation** — which is where the
     buddy-order ceiling bites before anything else does.
   - Anything that cannot mode-set reports a single mode and refuses the rest;
     `fb_mode_list` existing is what lets a Settings UI be honest.
2. **Resizing the scene is the actual work.**  A mode change must: reallocate
   the backbuffer and wallpaper, re-run the shell's `init(screen_w, screen_h)`
   and `bottom_reserve`, clamp every window into the new bounds (a window at
   x=1700 on a 1024-wide screen is unreachable — clamp, and remember the old
   geometry so switching back restores it), re-send **`wl_output` geometry/mode/
   done** to every Wayland client, and deliver the **dosgui RESIZE event**
   §4.60 built to every client-managed window.  *That §4.60 event is why this
   milestone is now affordable: the hard half — "who tells the app its canvas
   moved" — already exists and is tested.*
3. **Serialise it against the compositor.**  A mode set while `compose()` is
   mid-blit writes into a freed backbuffer.  It runs ON the compositor task,
   between frames, with the WM lock held for the geometry pass — not from a
   shell command's own task.
4. **UI**: `mode` / `mode <w>x<h>` shell command first (headless-testable),
   the §M63 "Display" `SETTINGS_PANEL()` second, and — **not optional** — a
   **confirm-or-revert dialog**, because an unsupported mode on real hardware
   is a black screen and nobody can click "revert" on a black screen.
   Specified exactly, since this is the part that must not be improvised:
   - the new mode is applied FIRST, then a dialog window opens **in the new
     mode** carrying a live **countdown**, an **OK** button and a **Cancel**
     button;
   - **OK** keeps the mode and writes it to config; **Cancel** reverts at once;
     **the countdown reaching zero reverts** — the safe outcome is the one that
     requires no input, which is the whole point of the dialog;
   - the timeout is **15 seconds by default and configurable**
     (`gui.mode_confirm_s`; 0 = never auto-revert, for someone who knows their
     monitor and is tired of the dialog);
   - the countdown runs on a **`ktimer` (§M53), never on a frame counter**: at a
     mode the display cannot show there may be no frames at all, and a revert
     that only fires while the compositor is drawing is a revert that never
     happens in the one case it exists for;
   - reverting restores the previous mode AND the window geometry saved in
     step 2, so a cancelled experiment leaves the desktop exactly as it was;
   - the dialog takes the keyboard too (Enter = OK, Esc = Cancel): at a broken
     mode the pointer is as invisible as everything else;
   - and the shell command gets the same contract (`mode 1280x800` prompts,
     `mode 1280x800 --force` skips it) so the headless test can drive both the
     confirm and the expire path — *a revert nothing can trigger on purpose is
     a revert nobody has tested.*
5. **The boot resolution stays a request**, but should come from config where
   the loader allows it (GRUB `gfxpayload`) instead of an assembler literal, so
   a chosen mode survives a reboot.

**Done when:** `mode 1280x800` on a 1920×1200 boot re-lays the desktop with
windows clamped, NetSurf and a Wayland client both re-layout their contents
(the §4.60 proof: text reflows, the image is not scaled), switching back
restores the previous window geometry, and a screendump at each step is
correct on i386, x86_64 and aarch64.

**Watch out for:** every `1280`/`1920` literal outside the mode setter is a bug
waiting for this milestone — `wayland.c` already carries `if (sw <= 0) sw =
1280;`.  Grep them out as step zero.

---

## §M62 — Boot screen, switchable (and never at the cost of the log) ✅

**Shipped 2026-08-21 — see DOCS.md §4.71.**  `boot.splash` = off/on/quiet, a
DRAWN splash (no file, no decoder, no allocation), the log suppressed but kept
in klog, any key dropping to it, and **every fault tearing it down** through one
hook in `crash_dump_begin()`.  Demonstrated by `splash faultkernel`.

**Lessons learned.**

- *Suppressing one path is not suppressing output.*  The console sink and the
  per-task VC hook are two paths to the screen, and after `vc_init` the live
  one is the VC.
- *There is more than one screen sink*, and the first match was the inactive
  VGA fallback while the framebuffer one kept printing.
- *A deliberate fault that does not fault is not a test.*  `*(int*)0x4 = …`
  succeeds — low memory is identity-mapped — so the screendump showed the
  splash still up for the innocent reason that nothing had crashed.
- *Handing the screen back is not clearing it.*  The first working report
  printed on top of the gradient; a panic report over a logo is still a panic
  report over a logo.

**Original design (kept for the reasoning).**  Boot today is the kernel log
scrolling up a framebuffer terminal.
That is the right default for a kernel under development and the wrong one for
showing the machine to anyone — and the interesting part is that **both
audiences are right**, so the answer is a switch, not a choice.

**Design.**
1. **`boot.splash` = `off` (default) | `on` | `quiet`.**  `off` = today's log.
   `on` = splash with the log suppressed to a klog ring (still readable with
   `dmesg` afterwards — *suppressed, never discarded*).  `quiet` = splash plus a
   one-line progress/status area.
2. **The splash is a compositor-less painter**: a logo + a progress indicator
   drawn straight into the linear framebuffer through `fb_present`, running
   before the GUI exists.  It must not depend on the heap being warm, on a
   filesystem being mounted, or on any driver that could be the thing that
   hangs — so the image is **embedded in the kernel image** (the objcopy blob
   pattern the userland programs already use), not loaded from disk.
3. **Any fault, panic, watchdog trip or NMI lockup TEARS THE SPLASH DOWN and
   restores the text console** before printing (§M46/§M47 paths).  This is the
   whole safety argument: a splash that stays up over a panic converts a
   diagnosable crash into "it froze at the logo", which is precisely the failure
   mode every distro has shipped at least once.  A `CRASH_SINK()`-adjacent hook
   or an explicit `splash_abort()` at the top of the fault path — decided in
   this milestone, not later.
4. **An escape hatch that works while it is up:** any keypress (or Esc) drops
   to the log immediately, and the kernel keeps feeding the framebuffer terminal
   underneath so the switch is instant rather than a replay.
5. **Progress is honest or absent.**  A bar that is a timer pretending to be
   progress is a lie the log does not tell; drive it from real boot phases
   (drivers probed / filesystems mounted / GUI up) or ship a spinner.
6. **The handover to the desktop** should not flash: the splash's last frame
   and the first composed frame are both full-screen blits, so present the
   desktop's first frame before tearing the splash down (M22.6's page flip makes
   this free — the splash is on one buffer, the desktop's first frame on the
   other).

**Done when:** `setconf boot.splash on` + reboot shows the splash and `dmesg`
still has every line the log would have printed; Esc during boot drops to the
log; a deliberately faulting boot (§M46's test hooks) shows the panic text, not
the logo; and all three arches behave the same — on aarch64 the "framebuffer"
is a virtio-gpu scanout that must be flushed per frame (`fb_present_flush`),
which is exactly the difference this design routes through the existing seam.

---

## §M63 — Control Panel (a place for settings, and a seam so it never grows) ✅

**Shipped 2026-08-21 — see DOCS.md §4.65** (stage 0 in §4.63).
`SETTINGS_PANEL()` + `CONFIG_KEY()` registries, a generic panel that renders
every declared key so most settings need NO UI code, panels as their own
§M22.7 windows, one Start-menu entry, and `conf` (list/show/set **with
validation**) on both shells.  Twelve keys declared, eleven of which had been
discoverable only by reading source.  Verified by screendump on i386 and
headlessly on all three arches.

**Lessons learned.**

- *A cap that silently drops the overflow reads as a broken registration.*
  `SM_MAX_APPS` was 10 with 10 apps; the eleventh simply never appeared.
- *Declare the setting next to the code that reads it.*  That is the entire
  payoff of a registry — otherwise the panel accumulates knowledge of every
  subsystem, which is the thing being avoided.
- *A descriptor buys validation, not just rendering.*  `conf set` can refuse a
  bad enum; `setconf` deliberately still cannot, because it must reach keys
  nobody has declared.
- *Cycle, don't type.*  With no dropdown widget, cycling through the legal
  values is the only affordance that tells the user what they are.

**Original design (kept for the reasoning).**  §M60, §M61 and §M62 each end
with "…and a UI for it".  Written three
times that is three bespoke apps, three Start-menu entries and three places to
keep consistent — and the Start menu cannot even take them: `shell_vista.c`
caps the launcher at **`SM_MAX_APPS 10`** and there are already **8 registered
`GUI_APP`s**.  So the container is not a nicety that comes after the settings;
**it is the thing that decides whether adding a setting is a line or an app.**

**The four rules that make this a milestone rather than a window.**

1. **A registry, not a hardcoded list.**  `SETTINGS_PANEL()` — a linker-section
   registry exactly like `GUI_APP()`, `DESKTOP_SHELL()`, `SERVICE()`,
   `CRASH_SINK()` — with `{ name, category, icon, open() }`.  A new setting
   ships its own panel next to the code it configures; **`controlpanel.c` never
   changes again.**  This is convention #2 (self-registration) and #5 (final API
   shape from day one) applied to settings, and it is the entire architectural
   content of the milestone.
2. **A panel is a VIEW over the config store — never the storage.**  Every
   setting is a `config` key that `setconf`/`getconf` can already reach, and the
   panel only reads and writes it.  The reason is not tidiness: **every
   automated check in this project is a grep over a serial log**
   (`scripts/dos-shell-test.py`), so a setting that exists only inside a GUI
   cannot be regression-tested at all.  Shell command first, panel second — for
   every single setting.  A corollary worth building on purpose: since a panel
   is a view, most panels need **no code at all** — a `CONFIG_KEY()` descriptor
   (`key`, type `bool|enum|int|string|path`, allowed values, default, one-line
   help) lets ONE generic panel render every plain setting, and a hand-written
   panel is then reserved for the few that need a preview, a file picker or a
   revert timer.  It also gives `getconf`/`setconf` validation and a `conf -l`
   listing for free — today a mistyped value is discovered by the subsystem
   that reads it, if at all.
3. **The icon grid is ONE view over an abstract item collection — not the
   layout.**  The Control Panel does not own a grid; it owns a **model** (a
   list of items: icon, label, sub-label, payload, enabled flag) and hands it to
   a **swappable view** — `grid` today, `list` / `details` / `tiles` later —
   chosen per site by config (`controlpanel.view`, `desktop.view`).  This is
   convention #5 applied where it is cheapest to apply: the model/view split
   costs nothing at the first implementation and is a rewrite afterwards.  It
   is also not speculative, because **there are already three consumers** — the
   Control Panel's panels, §M64's desktop shortcuts, and the file manager,
   whose `listview` is exactly this widget written once for one caller (and
   which should migrate onto it, giving the file manager icon view for free).
   The view is a widget with the ordinary `widget_ops`; selection, keyboard
   navigation, hit-testing and scrolling live in the model+view pair, so a new
   layout is a `draw` and a `hit` function, not a new app.
4. **Panels open as their own windows, not as pages inside one.**  §M22.7 put
   every `WIN_APP` on its own task precisely so a slow or wedged app cannot take
   the GUI down; hosting eight panels in one window undoes that for the one app
   whose job is to change display modes and keyboard layouts — the two settings
   most able to wedge.  The Control Panel is an **icon grid that launches**;
   each panel is an ordinary app window with its own task.

**Stage 0 — settings must survive a reboot.  ✅ SHIPPED 2026-08-21, DOCS §4.63**
— `config_attach_persistent("/mnt")` after the exFAT mount on both entry paths,
`saveconf` naming the file and saying outright when it will not survive, and
`CONFIG_WATCH()` + `config_apply` so a key read at boot hears about a later
change (the first two watchers: keyboard layout and wallpaper).  Verified
across a real reboot on i386.  The original statement of the problem:  `config_save()` writes `/etc/d-os.conf`, and **`/` is ramfs** — the
persistent volume is exFAT at `/mnt`.  Worse, the ordering forbids it anyway:
`config_init()` runs at `kernel.c:144` and the exFAT mount is ~125 lines later,
at `kernel.c:269`.  So today `saveconf` persists into RAM and every setting dies
at the next boot.  **A Control Panel on top of that is theatre**, so this comes
first: a two-phase config (defaults + ramfs overlay early, then a re-overlay
from the persistent volume once it is mounted, with `config_save` writing
there), plus the question every such design must answer out loud — *what happens
when the disk is absent?*  (Answer: run from defaults, mark the store
read-only, and let the panel say "not persisted" rather than silently accepting
changes.)

**The panels, in ascending order of honesty about their cost.**

- **Display** (§M61) — resolution, and later refresh/scale.  Cheap once §M61
  exists; it is a list from `fb_mode_list()` plus the confirm-or-revert dialog.
- **Personalisation** (§M60) — wallpaper, fit mode, accent colour, and the
  desktop-shell chooser (`gui.shell`, which is already a config key with two
  implementations — the seam is built, nothing exposes it).
- **System** — the cheapest panel in the list and one of the most useful,
  because **the keys already exist and nothing exposes them**: `boot.splash`
  (§M62), `kernel.fault_policy` (halt/reboot/kill), `crash.report`,
  `gui.drag_stats`, `bus.allow-adaptation`, `package.auto_fkill_ms`,
  `shell.provider`, `gui.close_forces_kill` / `close_grace_ms`.  Today these
  are discoverable only by reading source.  It also carries the read-only
  system facts that have no home — arch, milestone (`version.h`), CPU count,
  memory, uptime, boot device — which the About window shows a fraction of.
- **Packages (§M35.5 / §M45)** — the store is real and has **no UI at all**:
  installed closures, profiles, generations + rollback, GC, and the recipe's
  declared `.abi` backend.  §M45 already plans an apt-like frontend + a GUI
  installer; **it should register as a `SETTINGS_PANEL()` instead of becoming a
  separate app**, because "what is installed" and "what is configured" are the
  same question asked twice.  Note the one real design constraint: install/GC
  are long operations, so the panel drives them **asynchronously** (§M49's
  workqueue or its own task) and shows progress — a settings window that blocks
  its own task for a package install is a wedged window, and §M46 will treat it
  as one.
- **Region — the big one, and it is not a settings panel, it is three
  features.**  (a) **Keyboard layout** is the only part that already works:
  `keymap.h` says the layout is chosen from `keyboard.layout` and *can be
  switched at runtime* — but §M40 generates the Wayland **xkb keymap** from
  `keymap_active()` and sends it once, so switching live must **re-generate and
  re-send the keymap** to every client or they keep typing the old layout.
  (b) **Setting the clock has no driver support**: `rtc_read()` exists on both
  x86 (`cmos_rtc.c`) and ARM (`pl031_rtc.c`) and **there is no `rtc_write()` on
  either** — that is a new HAL entry point on two arches, and on ARM the PL031
  is read-only in QEMU's default wiring, which needs checking before promising
  it.  (c) **Timezone and date/time format do not exist as concepts anywhere**
  — no locale, no offset, one hardcoded rendering in the taskbar clock and
  another in the file manager.  So "Region" means introducing a time/locale
  layer first, and it should be split out rather than smuggled in behind an
  icon.
- **Later, for free:** Network (§M24 has `netstat`/DHCP state and no UI), Users
  (§M32), Sound (§M23 mixer), Devices (`lsnic`/`lsaudio`/`blk`/`lsusb` are all
  shell commands with a natural list view).

**Start menu.**  The launcher keeps application entries and gains **one**
"Control Panel" row; settings never appear as separate `GUI_APP`s.  That is
what the `SM_MAX_APPS 10` cap forces anyway, and it is the right shape: the
Start menu lists *what you run*, the Control Panel lists *what you change*.
(If the menu should shrink further, the alternative is a two-level menu —
`Programs ▸` + `Control Panel` — which is a separate, purely cosmetic change to
`shell_vista.c` and should not be bundled here.)

**Done when:** the Start menu has one settings entry; opening it shows an icon
grid built from the registry with nothing named in `controlpanel.c`; changing
the wallpaper and the resolution from it works and **survives a reboot**; every
one of those settings is also reachable from the shell and therefore covered by
a headless test; and a panel that hangs does not take the Control Panel or the
desktop with it (`wedgewin`'s guarantee, applied here).

**Watch out for:** the desktop shell's `click`/`motion` run **in the mouse IRQ
with the WM lock held** (`desktop.h`'s own contract), and §M22.7 already learned
this once — *app launches were moved to the desktop task* for exactly this
reason.  An icon that launches a panel must queue the launch, never perform it.

---

## §M64 — Desktop shortcuts (icons on the wallpaper, and nothing more) ✅

**Shipped 2026-08-21 — see DOCS.md §4.64.**  The icon primitive (drawn, not
stored), the model/view split the user asked for (`ITEM_VIEW()` + `desktop.view`
= grid | list), and shortcuts as `/desktop/*.lnk` files behind one resolver.
The compositor gained a background LAYER (`draw_under`) rather than a chrome
hook, so icons sit under every window.  Verified by screendump on i386
(select, double-click opens NetSurf) and headlessly by `shortcut check` on
every arch.

**Lessons learned.**

- *Draw the icons.*  Sixteen glyphs from gfx primitives beat a bitmap set here:
  one definition per size, no per-arch blob plumbing, and nothing that can fail
  at runtime.  The trade — geometry, not artwork — is written in the header
  along with the seam that replaces it.
- *A view that draws correctly and hit-tests wrongly is invisible in a
  screenshot.*  `shortcut check` prints the hit answer next to the checksum for
  exactly that reason.
- *Copy an existing caller's convention instead of assuming one.*  The reload
  loop tested `vfs_readdir(…) == 0`; this VFS returns >0 per entry, so `add`
  succeeded and `list` showed an empty desktop — two halves that each looked
  right.
- *Icons belong UNDER the windows.*  Putting them in the existing chrome hook
  would have been one line and would have drawn a shortcut on top of every
  application.

**Original design (kept for the reasoning).**  The desktop is wallpaper +
taskbar and nothing else — `gui_start`
deliberately boots to a bare desktop, "the user launches from Start".  Which
means the surface a person looks at all day holds **zero** of their own
choices.  Scope is deliberately narrow and worth stating in the milestone
title: **a shortcut is a pointer to something that already exists** — a name, a
target and a position.  No embedded app, no live tiles, no widgets.

**Design.**

1. **A shortcut is a FILE, not a config key.**  A tiny text record in
   `/desktop/<name>.lnk` (`target=`, `args=`, `icon=`, `x=`, `y=`), because
   files are what the system already has tools for: the file manager can create
   one ("Send to desktop"), `ls` lists them, `rm` deletes one, and the exFAT
   mount can hold them.  Config keys would need a whole parallel CRUD, and
   deleting one would mean editing a key namespace by hand.
2. **Four target kinds, one resolver:** a registered `GUI_APP` by name (the
   common case — no path, survives a rebuild), an ELF in the store
   (`pkgrun`-shaped), a shell command line, and a plain file (opened through
   the existing `GUI_APP_ASSOC` extension association the file manager already
   uses).  One `shortcut_launch()` covering all four, so "double-click a
   shortcut" and "double-click in the file manager" cannot drift apart.
3. **The desktop is a second consumer of §M63's item view, not its own
   layout.**  Shortcuts are a model (icon + label + payload); grid is the
   default view, list/details are config (`desktop.view`).  The desktop's
   constraint is that it draws onto the wallpaper from the desktop shell rather
   than into a window — so the view's `draw` must take a target surface and an
   origin, never assume a window, which is a five-minute decision now and a
   refactor later.
4. **Rendering: the desktop shell draws them, the compositor damages them.**
   `desktop_shell.draw` runs on the compositor task after the windows — icons
   belong there.  But **an icon must repaint as a small rect**, not by
   redrawing the wallpaper (§4.61 measured what a full-screen repaint costs);
   selecting or moving an icon damages two rects, the old and the new.
5. **Icons: there is no icon anywhere in this system.**  The only graphics
   primitive with pictures in it is the 8×8 glyph font.  Two honest options:
   a set of small embedded bitmaps (the objcopy blob pattern already used for
   userland programs) or draw-from-primitives placeholders.  **`GUI_APP` has no
   icon field**, so the registry grows one either way — that is the real cost
   item of this milestone, not the shortcut logic.
6. **Interaction:** single click selects, double click launches, **drag moves**
   — and drag needs §M58's press/motion/release plumbing, so §M64 either
   follows §M58 or ships without moving (place-on-create only).  Say which;
   do not discover it halfway.  Keyboard: Del removes, F2 renames, arrows move
   the selection — the desktop is only reachable by keyboard if these exist.
7. **Position is persisted in the file** — which makes §M63's stage 0 (a
   writable, surviving filesystem) a shared prerequisite: on a diskless boot,
   `/desktop` is ramfs and the icons a user arranges are gone at reboot.  Same
   answer as §M63: say so rather than pretend.
8. **Auto-population is a trap.**  Do NOT seed the desktop from the `GUI_APP`
   registry — a desktop that re-creates icons the user deleted is the single
   most-complained-about behaviour of every system that has done it.  Ship
   empty, plus a `shortcut add <name> <target>` shell command (headless-testable
   first, as always) and "Send to desktop" in the file manager and the Start
   menu.

**Done when:** `shortcut add NetSurf app:NetSurf` puts an icon on the wallpaper,
double-clicking it opens NetSurf as a normal windowed app, the icon survives a
reboot on a machine with a disk, deleting the file removes it, and a full drag
of a big window over the icon field still composites at §4.61's measured cost.

---

## Change log

- **2026-08-21** — **§M62 SHIPPED (DOCS §4.71) — the §M58–§M64 desktop-UX
  cluster is done.**  Six blocks in one run: wallpaper sources, persistent
  settings, icons + a swappable item view + desktop shortcuts, the Control
  Panel, text selection + two clipboards, runtime resolution switching, and the
  boot screen.  The through-line worth keeping is that **every one of them was
  blocked by missing plumbing rather than missing polish** — a drag had no
  transport, a setting had nowhere to persist, a layout had nowhere to be
  chosen, a window built on the wrong task never ticked — and that the bugs
  which mattered were found by tests that could FAIL: an off-screen render with
  a checksum, a deliberate hard lockup, a deliberate ring-0 fault.

- **2026-08-21** — **§M61 SHIPPED on x86 (DOCS §4.70)** — block 5.  The
  resolution stopped being an assembler literal.  The part worth remembering is
  not the register write but the protocol around it: apply, ask IN the new mode
  with a ktimer countdown, and make the outcome that needs no input the safe
  one — plus the same contract on the shell, so both the confirm and the expire
  path can be driven headlessly.

- **2026-08-21** — **§M58 + §M59 kernel half (DOCS §4.69)** — block 4.  The
  toolkit could not express a drag at all (`widget_ops.mouse` was click or
  double click), so the milestone starts with a pointer phase stream and a
  grab.  Terminal selection works on the cell grid; two clipboard slots
  (selection vs deliberate copy) with middle-click paste.  The ring-3 and
  Wayland surfaces remain — they are the larger half of §M59 and were always
  scheduled as its tail.

- **2026-08-21** — **§M63 SHIPPED (DOCS §4.65)** — block 3.  The container that
  stops every later setting from becoming an app: two registries, a generic
  panel driven by `CONFIG_KEY()` descriptors (so most settings need no UI
  code), panels as their own windows, one Start-menu entry, and `conf` with
  validation on both shells.  §M61's Display panel and §M45's package frontend
  now have somewhere to register instead of somewhere to be written.

- **2026-08-21** — **§M64 SHIPPED (DOCS §4.64)** — block 2.  Icons exist now
  (drawn from primitives, not stored), the layout is a swappable `ITEM_VIEW()`
  chosen by config rather than something each consumer hardcodes, and a
  shortcut is a file with four target kinds behind one resolver.  Design point
  worth keeping: the compositor gained a background LAYER (`draw_under`)
  instead of a chrome hook, because icons belong under the windows — the
  one-line alternative would have drawn a shortcut on top of every app.

- **2026-08-21** — **§M63 stage 0 SHIPPED (DOCS §4.63)** — block 1 finished.
  Settings now survive a reboot, and the lesson worth keeping is that the file
  copy was the easy half: a key read at BOOT has already been acted on by the
  time the persistent store is overlaid, so `CONFIG_WATCH()` +
  `config_apply` (set-and-notify, distinct from set) is what turns
  "the value is in the cache" into "the machine behaves that way".  Without it
  a saved keyboard layout applies one boot late, with the file and the machine
  disagreeing and nothing explaining why.  Also: the harness now passes
  `-boot d` with `--disk` (a formatted image otherwise makes SeaBIOS boot the
  disk and the guest produces NO serial output at all) **and attaches the disk
  on aarch64 at all** — `--disk` was an x86-only flag that failed silently
  everywhere else.  Two features turned out to exist on one arch only: that
  one, and `setconf`/`getconf`/`saveconf`, which lived in `shell.c` and left
  ARM able to create a persistent store but not write to it (now shared in
  `config.c`, §M24's rule).  Also: a persistent store makes test runs
  **stateful** — the image has to be re-made between runs.

- **2026-08-21** — **§M60 SHIPPED (DOCS §4.62, all three arches)** — block 1 of
  the desktop-UX schedule.  The background is a configurable SOURCE with fit
  modes and a fallback that explains itself; the command lives in `wallpaper.c`
  so both shells run one implementation.  The part worth carrying forward is the
  verification: **`wallpaper check` renders off-screen and prints pixels**,
  because the aarch64 harness has no display device and a screendump there is
  impossible, not merely awkward — and the three arches then return
  byte-identical checksums, which a screenshot could never establish.  Two
  process findings: a status line that reported the fallback while the config
  held a picture, and an **ARM harness that had silently stopped typing** for
  want of a boot marker that still matched.

- **2026-08-21** — **§M63 (Control Panel) and §M64 (desktop shortcuts) added**,
  and they change the shape of the four before them: §M60/§M61/§M62 stop each
  ending in "…and a UI for it" and instead ship a `SETTINGS_PANEL()` into one
  registry.  Two findings while designing it, both structural.  (1) **Settings
  do not survive a reboot and nothing says so**: `config_save()` writes
  `/etc/d-os.conf` on **ramfs** (the persistent volume is exFAT at `/mnt`), and
  the ordering forbids it regardless — `config_init()` is at `kernel.c:144`,
  the mount at `kernel.c:269`.  A Control Panel on top of that is theatre, so
  persistent config is stage 0 rather than a later nicety.  (2) **"Regional
  settings" is not a panel, it is three features**: the keyboard layout is the
  only part that already switches at runtime (and even that must re-generate
  and re-send §M40's xkb keymap or clients keep the old layout), `rtc_write()`
  exists on NEITHER arch, and timezone/locale/date-format are not concepts
  anywhere in the tree.  Also recorded: the Start menu physically cannot hold
  one entry per setting — `shell_vista.c` caps it at `SM_MAX_APPS 10` with 8
  apps already registered, which is the concrete argument for the container.

- **2026-08-21** — **Backlog: the desktop UX cluster §M58–§M62 added** (design
  only, nothing shipped).  Requested from use: text selection, a clipboard that
  crosses the ring boundary, a changeable wallpaper, a changeable resolution,
  and a switchable boot screen.  What they have in common is worth recording up
  front: **each is blocked by missing plumbing rather than by missing polish.**
  Selection has no transport at all (`widget_ops.mouse` carries click and
  double-click — a drag cannot be expressed); the clipboard is in-kernel-widgets
  only and its own header says the userland protocol arrives with §M25/§M26,
  both of which shipped; the wallpaper is a `gfx_vgradient` call; the resolution
  is an assembler literal in the multiboot header.  That is the §M48 shape
  ("NetSurf is not clickable" = the compositor has no button event), so each
  design names the plumbing before the pixels.  Two of them lean on work already
  done: §M61 inherits §4.60's dosgui RESIZE event, which is the hard half of a
  mode change, and §M62 inherits §M47's fault paths — which it must hook, since
  a splash left up over a panic converts a diagnosable crash into "it froze at
  the logo".

- **2026-08-15** — **§M24 COMPLETE: a network that can hold more than one
  conversation (DOCS §4.59, all three arches).**  The stack's second half:
  a bounded TCP connection table with four-tuple demultiplexing, a server role
  (listen/accept, RST for an unclaimed segment), per-connection receive ring +
  send buffer, RTO retransmission with zero-window probing, DHCP, /proc/net,
  the socket ABI as canonical §M50 operations with ONE sockaddr marshaller for
  three arches, and a virtio-mmio NIC for aarch64 — which is what finally made
  "the portable stack is in the ARM build" mean the ARM build runs it.  A
  LOOPBACK device with deliberate frame loss (`lo drop`) is what made any of it
  testable: the server half cannot be reached through SLIRP at all without a
  hostfwd rule the automated runs do not have.  Measured on i386, x86_64 and
  aarch64: 8 concurrent connections; 32 KiB through a 10 % loss link intact and
  in order; bind/listen/accept/getpeername/shutdown through an unmodified musl
  binary.  Three bugs found, each presenting as "the network is slow" — a
  window update that did not restart the sender, a probe byte that dug a
  sequence hole, and a FIN declared acknowledged because its position was
  inferred from a value a retransmission moves.  The lesson that carried them
  all: **instrumentation found them and inspection did not** — splitting the
  test's wall clock into recv/asleep/send, and dumping the connection table at
  the moment of the stall rather than after it.

- **2026-08-06** — **§M49 SHIPPED: load distribution across CPUs, measured
  (DOCS §4.46, i386 + x86_64, aarch64 builds).**  §M18.6.1's load balancer ran
  **only when a runqueue went empty** — a work-stealing rule, not a
  load-distribution one — so with every queue non-empty an arbitrarily bad split
  never corrected itself.  The design above (periodic pass, every N ticks) was
  written and never built, and the source comment described it anyway, naming a
  `LOAD_BALANCE_INTERVAL_MS` that existed nowhere.  **Nothing could have caught
  it: `run_qemu.sh` passed no `-smp`**, so the balancer never ran on the path a
  person uses — every SMP test supplied its own flag (the §M48 missing-NIC shape
  again).  New **`sched [ms]`** samples the spread twice and reports the delta;
  five hogs pinned to CPU0 got 15-20% of a core each while singletons got 66%,
  **while every CPU read 100% busy** — aggregate utilisation is blind to this
  entire class of bug.  Fixes: a periodic threshold pass (a migration swings the
  difference by TWICE the moved task, hence the anti-ping-pong margin); load
  measured as **demand** (share of time spent RUNNABLE) instead of queue length,
  since four hogs and four sleepers are the same depth; **`task_msleep`,
  `vc_getchar` and init's reaper made to really block** — three `hlt`+`yield`
  loops, each under a comment asserting it was cheap, together keeping a core at
  100% on an idle machine and making `cron`/`watchdog` measure as CPU hogs (*a
  metric is only as honest as the state it observes*); and **priority**
  (`nice -20..19` → a weight that is both quantum budget and load share).
  Plus a **deferred-work pool** (`kernel/core/workqueue.c`), whose first
  consumer is the **xHCI event-ring drain** — it ran inside the timer IRQ and
  now only submits from there, which also closed a latent bug (`evt_drain` is
  not reentrant, and the ISR could swallow the command completion the
  enumerating task was waiting for).  Results: queue spread 2..6 → 2..3;
  x86_64 seven of eight hogs at the ideal 49-50%; idle box one core at 100% →
  four at 0-2%; `wqtest` 16 items over 4 CPUs in 26 ms against ~80 ms serial.
  **Two lessons beyond the scheduler.**  (1) A ✅ row and a confident comment
  are not evidence a design was implemented — and **ship the way to measure a
  subsystem together with the subsystem**; `sched` took an hour and turned a
  guess into a number.  (2) Making the sleeps real shifted the boot timing and
  exposed a **latent SMP race**: four sites bound a task's console AFTER
  spawning it under `preempt_disable()`, which §M18.6.2 made PER-CPU while
  `task_enqueue` deliberately places the task on another core.  Every one of
  them carried a comment explaining why it was safe.  *A guard whose scope
  changed under it is worse than no guard, because the comment keeps vouching
  for it.*  Open: `keyboard_getchar` + the GUI compositor/app-host loops are
  still polls, and NIC RX needs `net.c` locked first (re-scoped under §M24).
- **2026-08-04** — **§M48 SHIPPED: the memory ceiling is discovered, not
  compiled in; NetSurf becomes usable; Mesa reaches i386.**  See DOCS §4.42–4.44.
  The lesson worth carrying forward is not any single bug but the pattern behind
  three of them: a constant chosen for the machines of the day, with a comment
  explaining why it was safe, that stopped being safe the moment the machine
  changed — the identity-map cap, the COW refcount window, and ACPI's "far below
  the user base".  Each was correct when written and each failed silently, not
  loudly.  A fourth of the same kind: `run_qemu.sh` had no NIC, and every network
  test passed its own `-netdev`, so the automated path and the path a person uses
  were never the same path.  Open after this: i386 kmap (and PAE for >4 GiB), a
  non-blocking fetcher `poll`, and `PLAN_AARCH64.md`'s A1–A7.

- **2026-07-21** — **§M42 NetSurf: runway libs + the browser BINARY compiles +
  links (x86_64).**  Ported the last framework deps as store packages —
  libnsutils (base64/time/unistd), libnslog (logging + a flex/bison filter),
  libnspsl (public-suffix list) and **libnsfb** (the framebuffer surface, RAM
  backend) — each with a dyn-musl boot self-test (`nsutest`, `nsfbtest` PASS).
  Then built the **NetSurf binary itself**: `scripts/build-netsurf.sh` /
  `make ARCH=x86_64 netsurf` compiles a curated ~146-TU set (core + fb frontend +
  fbtk, JS = the `none` stubs, no curl/PDF/SVG/JPEG/WebP) and links a **915 KB
  musl dynamic PIE** against the store `.so`s.  Their buildsystem is bypassed
  (forced prelude header for config macros + `_GNU_SOURCE`, synthesised
  `testament.h`, `dos_image_data.c` chrome-bitmap stubs); the two header-shadow
  traps (`<time.h>` vs `utils/time.h`; libhubbub vs libdom `hubbub/errors.h`)
  were solved via `-I` ordering.  Also earlier this session: musl `getaddrinfo`
  runs natively + a userland `wget` (§M39 stage 3c), x86_64 interactive shell
  confirmed, the buddy-bug exonerated + a permanent PMM invariant guard, and the
  libdom clean-rebuild include fix.  Left toward the DoD: provision the binary +
  `/res` (resources + DejaVu TTFs) into the ISO, a `netsurf` shell cmd +
  Start-menu `GUI_APP` launcher, first run (grow linux_abi's syscall surface),
  render → `gui_window`.
- **2026-07-19** — **§M43 first slice: on-device C compiler (TinyCC) shipped
  (i386, DOCS §4.36).**  `tcc /hello.c -o /hello` + `exec /hello` compile and run
  C ON d-os; the M22.5 Editor gains a "Run" button (compile+run the buffer).
  TinyCC cross-built PIE with the musl toolchain (`--config-musl/pie`),
  provisioned via a rootfs archive.  Chose TinyCC over gcc/clang (too big to run
  on d-os yet); full gcc self-hosting remains the §M43 goal.  `DOS_MILESTONE=M43`.
- **2026-07-19** — **§M38 C++ runtime + §M39 stages 1–3 shipped (i386).**  M38
  (DOCS §4.34): a from-source musl C++ toolchain (musl-cross-make g++ 11.2.0) +
  libstdc++; `cpptest` throws+catches an exception across a `.so` boundary
  (DWARF unwinding) with the STL, dynamically linked.  M39 (DOCS §4.35): a
  ChaCha20 CSPRNG (arch-generic) + `/dev/urandom`/`getrandom` (stage 1); Mbed
  TLS v3.6.2 crypto (stage 2, `crypttest`); a verified TLS 1.3 handshake +
  encrypted app data (stage 3, `ssltest`).  Also wired the Linux-ABI time
  syscalls (mbedTLS x509 needs a clock) and grew the user stack to a 1 MiB
  multi-page region.  `DOS_MILESTONE=M39`.  Toolchains built under amd64
  emulation on an Apple-Silicon Mac (~10 h one-time for gcc).  Branch `m38-m39`.
- **2026-07-18** — Roadmap expanded (design only, no code): added **§M43**
  (native self-hosting toolchain — dev tools become the first store packages so
  we build d-os on d-os), **§M44** (language ecosystems — Rust/C++/.NET/Java
  effort analysis; the Linux-ABI personality lets any musl-ELF-emitting language
  run with just syscall breadth, JIT/VM langs need the VM ported), and **§M45**
  (apt-like package-manager frontend + GUI installer over the §M35.5 store +
  driver/module hot-swap, which is really §M33 user-mode drivers).  Separately,
  **§M37 dynamic linking SHIPPED (i386, DOCS §4.33)**: shared musl (libc.so is
  the dynamic linker), ET_DYN/PIE loader + PT_INTERP + full auxv, and the real
  syscall surface ld.so needs (full mmap2, real mprotect, fstat64).  Verified:
  `musldyntest` (PIE hello), `solibtest` (a separate libgreet.so via DT_NEEDED
  incl. a `.so` `__thread`), `dlopentest` (dlopen/dlsym/dlclose); static musl
  regression-free.
- **2026-07-11** — **§M36 stage 1 shipped (POSIX syscall breadth, i386).**
  Syscalls 30–35 (`stat`/`fstat`/`getdents`/`uname`/`clock_gettime`/`nanosleep`)
  + `errno` + a `%o` printf; the in-tree libc grew the matching structs +
  wrappers.  `posixtest` exercises them from ring 3.  Stage 2 — the actual musl
  cross-compile as the native libc + coreutils into the §M35.5 store — is
  external-toolchain infra, deferred.  See DOCS.md §4.30.
- **2026-07-11** — **§M35.5 store slice shipped (package manager, i386).**  A
  content-addressed store on the VFS (`kernel/core/pkg.c`): immutable
  `/store/<hash>-name-version/` paths (hash folds in recipe + each dep's
  recursive hash), version coexistence, pinned `.closure`, symlink-free
  `/etc/pkg/profile`, mark-sweep GC.  Shell `pkg …` + `pkgtest`.  Boot-tested:
  two `hello` versions coexist, install `hello-2` + `args`, gc reclaims
  `hello-1.0`.  The porting gate before musl (§M36).  Deferred: hermetic source
  builds (§M36 toolchain), RPATH isolation (§M37), sandbox (§M33), signing
  (§M39).  Next: §M36 libc (musl port).  See DOCS.md §4.29.
- **2026-07-11** — **§M35 TLS + per-CPU TSS (i386, UP + SMP).**  Thread-local
  storage via `%gs`: per-CPU GDT TLS descriptors whose base the scheduler
  reloads on switch-in (`hal_set_tls_base`), `set_thread_area` (SYS_SET_TLS) +
  libc `set_tls`; `tlstest`'s 4 threads each read only their own id (0
  mismatches, UP + `-smp 2`).  Also the per-CPU TSS fix (see below) that
  unblocked all ring-3 tasks on APs.  §M35 is now UP + SMP + TLS complete on
  i386.  Next: §M35.5 pkg store → §M36 libc.  (TLS threads are CPU-pinned;
  migration-safe TLS + the compiler `__thread` runtime are follow-ups.)  See
  DOCS.md §4.28.
- **2026-07-11** — **§M35 shipped (threads + futex, i386, UP).**  `proc_clone`
  (SYS_CLONE) = a task sharing the creator's address space (`mm_shared`) + fds,
  at a ring-3 entry/stack; `futex` (SYS_FUTEX) FUTEX_WAIT/WAKE over hashed
  Tier-A wait-queues (lost-wakeup-free); libc `thread_create`/`thread_join` +
  a 3-state Drepper mutex.  Tested on UP AND `-smp 2`: 4 threads × 5000 increments = 20000/20000.
  Bringing it up on SMP also fixed a pre-existing gap — ring-3 tasks didn't run
  on APs (single global TSS + no per-CPU LTR) — via a **per-CPU TSS** (array in
  tss.c + one GDT descriptor per CPU + each CPU LTRs its own); this unblocked all
  ring-3-on-AP (procspawn now runs on `-smp 2` too).  Next: TLS → §M35.5 pkg.
  See DOCS.md §4.28.
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
