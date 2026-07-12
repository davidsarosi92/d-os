# d-os — orientation for Claude

> **Purpose of this file:** give the assistant enough state to start
> working without re-reading every doc.  Keep it tight — it loads
> automatically into every session.  Update only when status moves
> milestones or when a hard convention changes.

## What this is

Hobby / teaching i386 OS kernel.  Boots from GRUB → installs its own
GDT / IDT / paging → talks to a 1280×800 framebuffer with an embedded
8×8 bitmap font → IRQ-driven PS/2 keyboard + xHCI USB host with
boot-HID keyboards → ramfs + devfs + procfs mounted at
`/`, `/dev`, `/proc` → preemptive round-robin scheduler with ring-3
syscalls via `int 0x80` → virtio-blk block device exposed as
`/dev/vda` → exFAT mountable as `/mnt` → screen split into multiple
shell panes (Alt-N to focus, `pane split h|v` to split).

## Status (update when a milestone ships)

✅ **M1 – M20 + M18.5 + M20.5 + M18.6 + M19.5 + M21 (full ARM parity) +
M22 – M22.7 + M27 + M28 + M25 (incl. Tier B tail) + Tier A + M29 + M30 +
M31 + M24 (net, stages 1–3, i386) + M23 (audio, stage 1, i386) + M34 (POSIX
process model, i386)** shipped
(10/11 polish sub-items; the lone outstanding one is §M20.6.1
SYSCALL/SYSRET).  **M34** (2026-07-11, DOCS §4.27): POSIX process model (i386)
— SysV argv/env/auxv initial stack; **copy-on-write fork** (`vmm_space_clone` +
`vmm_cow_fault` on #PF + `enter_user_mode_regs`); `waitpid` (Tier-A); `execve`
loading `/bin/*` from the VFS; `pipe`+`dup2`; **signals** (sigaction/kill/raise,
return-to-user delivery + `__sig_trampoline`→SYS_SIGRETURN).  Syscalls 14–21;
shell runargs/forktest/forkexec/pipetest/sigtest.  Open: EINTR, sigprocmask,
user #PF→SIGSEGV, x86_64/aarch64.  Next: net socket syscall API → §M35 threads.
**M23** (2026-07-11, DOCS §4.26): audio (i386) — `audio_dev`
registry + AC97 codec driver (BDL bus-master DMA, 48 kHz 16-bit stereo out) +
square-wave tone generator; shell `lsaudio`/`beep`/`tone`; boot-tested via QEMU
`-audiodev wav` (440 Hz ±8000 square wave captured).  Open: `play <path>` WAV
player, `/dev/dsp`, mixer/multi-stream, input, Intel HDA, x86_64/aarch64.
**M24** (2026-07-11, DOCS §4.25): network stack (i386) —
virtio-net driver + `net_device` registry + arch-independent
Ethernet/ARP/IPv4/ICMP/UDP/TCP + DNS stub resolver; shell
`lsnic`/`ping`/`arp`/`nslookup`/`wget`/`nettest`; boot-tested through QEMU
SLIRP (ICMP 3/3, DNS example.com, TCP `HTTP/1.1 200 OK`).  RX polled from the
calling task (no IRQ/lock yet); TCP client-only, no retransmit/congestion.
**Stage 6 (2026-07-11): BSD socket API to userland** — `FD_NETSOCK` +
`socket`/`bind`/`connect`/`sendto`/`recvfrom` (syscalls 22–26), ring-3 UDP+TCP;
`dnstest`/`httptest` resolve + fetch a page from ring 3.  Open: sockaddr,
multiple TCP conns, IRQ RX, DHCP, IPv6.  **M35** (2026-07-11, DOCS §4.28):
threads + futex + TLS (i386) — `proc_clone` (shared address space, `mm_shared`)
+ `futex` (SYS_CLONE/SYS_FUTEX) + libc `thread_create`/`thread_join` + `%gs`
thread-local storage (SYS_SET_TLS, per-CPU GDT TLS descriptors); tested
20000/20000 (threadtest) + tlstest 0-mismatch on **UP and `-smp 2`**.  Also
fixed a pre-existing gap it exposed (ring-3 tasks didn't run on APs) with a
**per-CPU TSS** (array in tss.c + one GDT descriptor per CPU + each CPU LTRs its
own) — unblocks all ring-3-on-AP.  **Tier A** (2026-07-10, DOCS §4.20): blocking
primitives — `waitq` (block/wake, lost-wakeup-free, SMP cross-CPU wake;
`TASK_SLEEPING` now real), `task_wait(pid,&code)`, blocking socket
read + `poll(timeout<0)`, `task_msleep`.  **M29** (DOCS §4.21):
services — supervisor (`SERVICE()` + `task_wait` restart w/ backoff +
config gate + `service` cmd + `/proc/services`) + service bus
(endpoint/contract\@ver/transport, strict bind + opt-in `BUS_ADAPTER`
gated by `bus.allow-adaptation` + `/proc/bus`).  **M31** (DOCS §4.22):
watchdog — L1 per-task heartbeat (`watchdog_register/kick` → detect +
kill-tree + M29 restart) + L2 per-CPU softlockup (`percpu.ticks`);
`/proc/watchdog` + `wdtest`; L3 HW watchdog deferred.  **M30** (DOCS
§4.23): cron — itself an M29 service; `CRON_JOB()` registry + interval
schedules (`/etc/crontab` / config) + `/proc/cron`.  **M25 Tier B tail**
(DOCS §4.24): concurrent preemptible user processes (`proc_spawn`,
per-task TSS.esp0/rsp0 via `hal_set_kernel_stack`; SP_EL1 auto on ARM;
one-way `enter_user_mode`; SYS_EXIT→task_exit; `user_task` flag) +
**full-arch libc** (arch-cond `syscall3` + per-arch crt0 + Makefile
USER_* knobs; `hello`/`spin` build on all 3; `SYS_GETPID`); tests
`procspawn`/`libctest` green on i386/x86_64/aarch64.  M28 (2026-07-10):
system log — klog static ring
+ `kprintf` auto-tee + `klog(level,tag,…)` + `dmesg [-l level]` +
`/proc/kmsg` (DOCS §4.18).  M25 (2026-07-10): userland foundation
stages 1–7 (DOCS §4.19) — per-process address spaces (`vmm_space` +
`task.mm`, scheduler CR3/TTBR0 switch), ELF loader (`elf.c`) + run
(`proc_exec_elf`, ring3/EL0 excursion), fd table + `write/read/open/
close/lseek/mmap/memfd/socketpair/send/recv/poll` (generic `struct
ofile`), memfd shared memory (`VMM_SHARED` PTE bit), unix socketpair +
SCM_RIGHTS fd passing (`usock.c`), poll, in-tree libc (`user/`,
compiled-C runs in ring 3).  All on 3 arches (libc now all 3 via Tier B).
**Ring model LOCKED: only ring 0/3 (EL1/EL0) — rings 1/2 never
(paging is binary → no isolation; security axis = address spaces +
capabilities, not ring count).**  The former deferred tail (concurrent
preemptible user processes + x86_64/aarch64 libc) SHIPPED as Tier B
(DOCS §4.24) — `proc_spawn` runs many at once; the synchronous excursion
(`proc_exec_elf`) is kept for the self-tests.  Self-tests: `userrun/
fdtest/shmtest/socktest/polltest/libctest/waittest/procspawn`.  Still
open: force-kill of a wedged pure-ring3 task (needs M25/§M33 isolation),
argv/env, fork/COW.  M22 + M22.1 + M22.2 (2026-07-04): GUI — gfx
surfaces + compositor + WM core + widget toolkit + file manager,
PS/2 mouse (IRQ12), CMOS RTC, `vfs_unlink`, 1280×800 FB; desktop
shells + apps + command shells are REGISTRY-swappable
(`DESKTOP_SHELL()` / `GUI_APP()` / `SHELL_PROVIDER()` linker
sections; `gui.shell` + `shell.provider` config keys; vista + bare
desktops, d-os + rescue shells, apps under `kernel/gui/apps/`,
`launch` command); GUI dev guide in DOCS §4.14.  M22.3: task
manager app, cooperative task_kill/reap (kthread contract) +
cpu_ms, terminal-window close, minimize, Alt-Tab, dirty-rect
composition (`gui stats`).  M22.4 (2026-07-04): compositor
smoothness — cursor-damage race fix (compositor-side bookkeeping),
rect-bounded drag damage, tearing notes; instant Task Manager
(task_set_change_hook + DEAD reaping via vc_task_bound).  M22.5
(2026-07-04): desktop apps — nav keys end-to-end (PS/2 E0 → HID →
widget keycode events), multiline editor widget + kernel clipboard,
Editor app, Tiny-BASIC (`core/basic.c`, BASIC window via
gui_window_create_task, `run <path>` cmd), file manager 2.0 (path
bar, sorting, Ren/Copy/recursive-Del, GUI_APP_ASSOC extension
associations, vfs_rename/vfs_copy/vfs_unlink_recursive),
maximize/restore.  M22.6 (2026-07-04): tear-free presentation —
Bochs-VBE hardware page flip (DISPI VIRT_HEIGHT double buffer +
Y_OFFSET pan; buffer-age-2 dirty∪prev copy; graceful fallback to
single-buffer blit), plus the QEMU display-scaling fix
(zoom-to-fit=off); corrects M22.4's "not fixable" tearing note.
Same session: 1920×1200 desktop (needs `-device VGA,vgamem_mb=32`
for the double buffer + `BUDDY_MAX_ORDER` 10→12 for 9.2 MiB
contiguous surfaces + `-m 256M`), and terminal-window auto-close
when its hosted task dies (flagged at TASK_DEAD → reused close
teardown → also leaves the Task Manager list).  M27 (2026-07-04):
process model — `struct task` gains ppid/exit_code/reap_owned; an
always-on **init** task universally reaps DEAD non-owned tasks
(closes the zombie-leak gap) + re-parents orphans; `task_kill_tree`
takes a subtree down (GUI window close uses it); `task_spawn_detached`
(parent=init) for daemons; ps + /proc/tasks grow PPID, Task Manager
shows a process tree; pid 0 + init reap-guarded.  M22.7-A (2026-07-05):
per-task GUI apps — every WIN_APP window runs on its own `app:<name>`
task (`app_host_main` + `task_spawn_arg`); compositor = surface-
compositor + input router (per-window `aq` queue, host does widget
dispatch + render + tick); host↔compositor teardown dance; apps now
visible/killable in the Task Manager, a slow app no longer freezes the
GUI.  M22.7-B: the desktop shell/taskbar runs on its own `desktop` task
too (full-screen `panelsurf`; compositor composites taskbar strip +
launcher popup on top; input via `pevq`).  **Net: the compositor is now
a pure surface-compositor + input router; windows, apps AND the panel
are each their own task (the M26 Wayland shape, internal API).**
M22.7 refinements (2026-07-05): idle loops halt only when idle (was:
every iteration → cursor lag with menu/taskman open); vista_motion is
chrome-only repaint not full recompose; **app launches moved to the
desktop task → launched apps are children of `desktop`, not the
compositor**; `panelsurf` is a bottom strip not full-screen (~5 MiB
saved); bare shell reserves a hint strip.  Session vs detached GUI
shells: `task_spawn_under(name,entry,ppid)` parents a launched terminal
to the desktop ("New Shell" = session, dies with the desktop) or to
init ("Detached Shell" = outlives the session — nohup/tmux-detach in a
GUI).  GUI session root: `gui_start` spawns `desktop` first, parents
compositor + windows under it (`boot-shell → desktop → {compositor,
apps}`); no auto-started shells — the GUI boots as a bare desktop
(wallpaper + taskbar), user launches from Start.  Damage is now a LIST
of disjoint rects (was a single bounding box) — `compose()` paints +
presents each rect separately, so a Task Manager refresh + a far-away
cursor stay two small blits instead of one huge union (fixed the
cursor stutter: ~630 KB/frame vs ~2.4–5.3 MB).  Plus: a window click
damages only the two affected windows (was a full 9 MB frame), and the
Task Manager repaints only its listview (`gui_window_request_redraw_rect`).
All on both archs.
Highlights so far: VFS + ramfs + exFAT on virtio-blk, devfs +
procfs, preemptive scheduler, multi-pane shell, xHCI USB + HID,
keyboard layouts, HAL cut (`hal_api.h`), **SMP on i386 + x86_64**
with per-CPU runqueue + load balancer + per-CPU preempt_count + task
affinity (`taskset`) + cross-CPU preempt IPI + MSI/MSI-X allocator,
memory at scale (per-zone buddy PMM + slab + per-CPU magazines +
empty-slab caching + x86_64 HIGHMEM via 1 GiB-page identity-map
extension + ACPI SRAT-derived per-CPU NUMA nodes), APs scheduling,
**x86_64 (long mode) — full parity with i386 INCLUDING xHCI USB +
virtio-blk + exFAT**.  `m20_stubs.c` is empty.

▶️ **DECIDED NEXT (2026-07-12): REAL musl RUNS → coreutils → §M35.5 store next.**
§M36 IN PROGRESS.  **Stage 1** (syscall breadth: stat/fstat/getdents/uname/
clock_gettime/nanosleep + errno; DOCS §4.30).  **Stage 2 = "two brothers"**
(design settled; parked own-libc debate in `NATIVE_LIBC.md`): Role B (ecosystem
libc) via TWO peers onto the SAME kernel primitives — (a) **Linux-ABI peer**:
PRISTINE vendored musl → Linux numbers → isolated `kernel/hal/x86/linux_abi.c` +
`task->linux_abi` personality (doubles as §M41); (b) **native musl-fork peer**:
a light `arch/dos/` musl fork → d-os numbers → native `syscall.c` (store
default).  **Linux-ABI peer GOAL ACHIEVED (DOCS §4.31): an unmodified static
musl binary runs on d-os** — `make musl` builds static i386 musl
(`third_party/musl-i386/`), `user/muslhello.c` (stdio/printf) links against
musl crt1/libc.a into a stock Linux ELF (`-Ttext-segment=0x40000000` + libgcc),
run by the **`musltest`** cmd under the personality; prints via real musl
`printf`, rc=0, ZERO unhandled syscalls.  Startup welds: `set_thread_area`
(→§M35 `%gs` GDT-TLS), `auxv` (`AT_PAGESZ/CLKTCK/RANDOM/SECURE` in
`build_initial_stack`), `set_tid_address`, `ioctl`→ENOTTY.  **musl COREUTILS in
the store — DONE (§4.31):** `echo`+`cat` (generic `user/%.muslelf` pattern) are
`pkg install`ed into the §M35.5 store + run FROM `/store` by `pkgrun <name>
[args]` — real argv + musl file I/O.  **The ABI is DATA-DRIVEN (the swappable
seam the user demanded): a package declares `.abi` (`pkg_recipe.abi`), `pkg_run`
maps it → personality in ONE place (`abi_to_personality`) — no hardcoded
"musl"/"linux"** (see memory [[feedback-dos-swappable-layers]] + `NATIVE_LIBC.md`).
`linux_abi.c` grew open-flag xlat/openat/readv/mprotect/munmap + an `mmap2`
decode fix.  **Coreutils `echo`/`cat`/`ls`/`env` + a real (non-interactive)
`sh` DONE:** `pkgrun sh -c "echo a; echo b; ls /store"` forks 3 children, each
execve's a coreutil from `/bin` (fork/execve/waitpid/rt_sigprocmask in
linux_abi; `pkg install` exposes `/bin/<name>` + `PATH=/bin`).  Forced two
fixes: **TLS-after-fork** (proc_fork inherits has_tls/tls_base; child %gs=TLS
selector via g_entry_gs) + a **pre-existing COW double-fork bug** in
vmm_space_clone (already-COW page misclassified as RO code → fixed by routing
VMM_COW through the COW branch).  **Next: the native musl-fork peer (`arch/dos/`)
— the 2nd ABI backend that validates the `abi_to_personality` seam; also
interactive `sh` (blocking stdin).**  **Checklist in `third_party/MUSL.md`.**
§M35 (threads/futex/TLS/per-CPU TSS) COMPLETE (UP+SMP, §4.28); also: §M34 POSIX
(§4.27), §M24 sockets (§4.25), §M35.5 store (§4.29).  **§M26 Wayland deferred
until POSIX + libc exist.**

🔲 **Other options** (was "pick one"; superseded by the decision above):

- **M21** — aarch64 port.  Third arch, real torture test of HAL
  portability (no port I/O, GIC instead of APIC, EL1/EL0 instead
  of rings).  ✅ **Phase A–M shipped — FULL x86 parity** (2026-07-07..10,
  DOCS §4.17) — boot + SMP + virtio-blk + exFAT + DTB + framebuffer +
  EL0 userspace + **full shell.c + M22 GUI** (kbd/mouse) + **USB (xHCI+HID
  over PCIe ECAM)** on ARM64:
  A = raw-ELF boot on QEMU `-M virt` (no GRUB/multiboot), EL2→EL1 drop,
  PL011 UART, EL1 exception vectors, MMU identity map on;
  B = GICv2 (GICD 0x08000000 / GICC 0x08010000) + ARM generic timer
  (CNTP, INTID 30) + IRQ dispatch API;
  C = context switch (switch.S over x19–x30) + full hal_arch.c
  (DAIF/wfi) + PMM/kmalloc (stock pmm/slab/kmalloc; synthesised RAM map;
  BUDDY_MAX_FRAMES 4 GiB cap) + PL011 console sink + the stock
  preemptive scheduler (task/percpu/lock with UP stubs);
  D = interactive serial shell (`serial_shell.c` REPL on a scheduler
  task, PL011 RX poll+yield) + VFS + ramfs (stock vfs/ramfs/block/module)
  — ls/cat/mkdir/write/rm/ps/meminfo work over the UART;
  E = SMP via PSCI (`smp.c` + `smp_entry.S`) — secondary cores join the
  STOCK per-CPU runqueue + load balancer (percpu topology hook =
  MPIDR.Aff0; per-CPU mmu/gic/timer bring-up); verified two hogs running
  on two cores in parallel (`AARCH64_MAX_CPUS`+`-smp`, shipped at 2);
  F = virtio-MMIO block driver (`virtio_mmio_blk.c`, modern/version-2
  transport) → `/dev/vda` on the stock block layer; write→read self-test
  + shell `blk` command (needs `-global virtio-mmio.force-legacy=false`);
  G = exFAT at /mnt off /dev/vda — the STOCK block_cache.c + exfat.c link
  unchanged (arch-independent); shell ls/cat/write/rm hit persistent disk,
  writes survive a reboot;
  H = device-tree (FDT/DTB) parsing (`dtb.c`) — discovers RAM size + CPU
  count, sizes the PMM to the actual `-m` (DTB loaded at 0x48000000 via
  `-device loader`; falls back to defaults);
  I = virtio-gpu framebuffer (`virtio_gpu.c`) — QEMU `virt` has no
  VGA/Bochs-VBE, so the display is a virtio-gpu on a virtio-MMIO slot; a
  1280×800 2D scanout backed by a contiguous RAM framebuffer runs the *same*
  portable `fb_terminal.c` x86 uses (boot log + shell render graphically).
  The one x86-only bit of fb_terminal (Bochs-VBE port I/O + vmm map) was
  hoisted behind `fb_present.h` — `fb_present_map` + `fb_present_flush`
  (x86: no-op, linear FB is the scanout; ARM: virtio-gpu transfer+flush) —
  and the M22.6 page flip moved to `kernel/hal/x86/fb_present.c` (gui.c
  unchanged); i386 GUI re-verified regression-free;
  L = EL0 userspace substrate (`vmm.c` per-process TTBR0 spaces + EL0-page
  mappings; `usermode.S` `eret`-to-EL0 + SYS_EXIT teleport; `syscall.c` SVC
  dispatcher, x8=num/x0..x5=args, shared `syscall.h`; ESR.EC==0x15 decode in
  `exceptions.c`).  `usertest` runs a program at EL0 → SYS_PRINT/SYS_EXIT.
  This is the ARM analogue of x86 M6/M20.5 ring-3+`int 0x80` → **all 3
  arches are now M25-ready** (each can enter user mode + service a syscall);
  J/K = the *same* full `shell.c` on a VC + the **M22 GUI** (compositor +
  taskbar + PL031 clock + windows) driven by **virtio-input** kbd/mouse over
  the virtio-gpu framebuffer.  Portability shims: `arch_ringtest()`, PSCI
  `hal_shutdown/reboot`, `pl031_rtc.c`, `fb_present_flush()` in gui.c's present
  path, `virtio_input.c`.  **Scheduler lesson:** pid 0's idle loop must
  `hal_intr_enable()` each pass (like `cpu_idle_entry`) — a bare `for(;;)
  hal_cpu_halt()` wedges the CPU if DAIF masks IRQs (wfi wakes but won't take a
  masked IRQ) → its timer stops → it stops scheduling → CPU-homed tasks starve.
  aarch64 runs its OWN `main_entry.c` (NOT the x86-coupled kernel_main), builds
  via a separate `Dockerfile.aarch64`.  M = USB: a new PCIe-ECAM layer
  (`kernel/hal/aarch64/pci.c` — config via MMIO at 0x40_1000_0000 + BAR
  assignment, no firmware) lets the stock `xhci.c` + `usb_hid.c` link + run
  (MMIO, polled from the timer ISR); a USB HID keyboard drives the shell.
  **aarch64 now has full x86 parity** — M21 complete.
- **M23** — Audio — ✅ stage 1 shipped (i386, DOCS §4.26): AC97 PCM output +
  tone (`lsaudio`/`beep`/`tone`).  Open: WAV player, /dev/dsp, mixer, input,
  HDA, x86_64/aarch64.
- **M24** — Network — ✅ stages 1–3 shipped (i386, DOCS §4.25): virtio-net +
  ARP/IPv4/ICMP/UDP/TCP + DNS + ping/nslookup/wget.  Open: socket syscall API
  to userland, IRQ RX, TCP timers/server, DHCP, IPv6, x86_64/aarch64.
- **§M19.5.1 i386 kmap** — the deferred half of HIGHMEM: real
  kmap-style temp mappings so i386 can manage > 256 MiB of RAM.
- **§M19.5.3 per-NUMA-node PMM zones** — the deferred deeper half
  of SRAT integration; today the parser populates per-CPU node IDs
  but PMM still has a single zone set.
- **§M20.6.1** — SYSCALL/SYSRET instruction path (needs GDT slot
  reorg to satisfy SYSRET's selector arithmetic).

🔲 **PLAN extensions (placeholders, design only):**
- §M23 — Audio subsystem (AC97 → HDA → I2S).
- §M24 — Network stack — ✅ stages 1–3 shipped (i386, DOCS §4.25); socket
  syscall API + DHCP + IPv6 + x86_64/aarch64 still open.
- §M25 — ✅ SHIPPED stages 1–7 (DOCS §4.19) + Tier B tail (DOCS §4.24,
  concurrent preemptible user processes + full-arch libc): per-process
  VMM, ELF loader + exec, fd table, mmap + memfd shm, unix sockets + fd
  passing, poll, in-tree libc (all 3 arches), `proc_spawn`.  Wayland
  prerequisites in place.  Blocking primitives (Tier A, DOCS §4.20:
  waitq / task_wait / blocking read+poll / task_msleep) also shipped.
- §M26 — Wayland server (wire protocol over M22 compositor +
  M25 substrate; depends on both).  **Now the next natural target — its
  M25 + M22.7 prerequisites are all in place.**
- **Workload-management cluster** (order M27→M30 — ✅ ALL SHIPPED):
  - §M27 — ✅ SHIPPED (DOCS §4.15): init + parent/child hierarchy +
    universal reaper + kill-tree + task_spawn_detached + ps/procfs
    PPID + Task Manager tree.
  - §M28 — ✅ SHIPPED (DOCS §4.18): klog static ring (seq + ms + printk
    severity + tag + msg); `kprintf` auto-tees via `emit`→`klog_feed_char`;
    `klog(level,tag,fmt,…)` structured entry; `dmesg [-l level]` +
    `/proc/kmsg`.  (Pitfall: `va_list` is an array type on x86_64 — forward
    it by `va_copy`, never `&`-a-parameter; see the §M28 lesson.)
  - §M29 — ✅ SHIPPED (DOCS §4.21): `SERVICE()` registry + supervisor
    (autostart + restart policy w/ crash-loop backoff, `task_wait`-driven,
    config gate, `/proc/services`) — systemd-lite — PLUS the **service
    bus** (endpoint / contract\@version / transport; strict bind + opt-in
    `BUS_ADAPTER` gated by `bus.allow-adaptation`; `/proc/bus`).  Contracts
    marshalling-shaped so a `LocalCall` service can later move to
    IPC/SharedMemory.  The bus makes §M33 execution domains a config
    (not code) decision.  Non-local transports still reserved for real
    isolation.
  - §M30 — ✅ SHIPPED (DOCS §4.23): cron — itself an M29 service;
    `CRON_JOB()` registry + interval schedules (`/etc/crontab` / config,
    run-once-no-backfill) + `crontab -l` / `/proc/cron`.
  - §M31 — ✅ SHIPPED L1+L2 (DOCS §4.22): watchdog — per-task heartbeat
    (detect + kill-tree + M29 restart) + per-CPU softlockup; `/proc/
    watchdog` + `wdtest`.  L3 (HW watchdog device) deferred.  Blocking
    substrate = Tier A (DOCS §4.20).
- **§M32 — Multi-user** (design only): credentials (uid/gid) on tasks,
  `/etc/passwd`-style user DB, login/sessions, VFS file ownership +
  rwx perms, privilege gating, per-user process isolation.  Hard-depends
  on §M25 (real isolation needs per-process address spaces; today's
  ring-0 kthreads share one, so users would be advisory until then).
- **§M33 — Execution domains** (design only): a service's run location
  (`DOMAIN_KERNEL` / `USER` / `ISOLATED`) is a *declared capability*
  (`.domains` field), config *chooses* among the declared set; the §M29
  broker resolves domain → transport at bind.  Domain constrains
  transport (KERNEL→LocalCall, USER/ISOLATED→IPC/SharedMemory).  Only
  `KERNEL`+LocalCall is real today; `USER`/`ISOLATED` reserved until
  §M25 (no isolation theatre).  Flagship case = switchable **driver
  placement** (Tier 0 fault-tolerant in-kernel hosting → Tier 1
  user-mode non-DMA → Tier 2 DMA+IOMMU); the driver-runtime "narrow
  waist, two backends" IS the M29 transport abstraction.  Hybrid kernel
  (NT/XNU), not a micro-vs-monolith flip.
- **§M35.5 — Package manager & isolation** — ✅ **store slice shipped** (i386,
  DOCS §4.29): content-addressed `/store/<hash>-name-ver/` + profiles + GC
  (`pkg …`/`pkgtest`).  Design (rest still open) — a hard **gate before any
  porting** — a **content-addressed store** (Nix/Guix-
  shaped, NOT dpkg/apt; convention #6): immutable `/store/<hash>-name-ver/`
  paths, pinned dependency closures (versions coexist, no global `/lib`
  soup), hermetic §M33-sandboxed builds, symlink-profile + GC (no cruft,
  rollback), text recipes.  Two-level isolation: §M37 RPATH (load) +
  §M25/§M33/§M32 FS-view (run).  Gates §M36–§M42 (every port installs into
  the store, never the global FS).  Satisfies: isolate ports, no clutter,
  minimal version coupling.
- **Userland maturation §M34–§M42** (design only, PLAN.md): **the goal is
  the POSIX platform, NOT a browser** — each milestone is independently
  necessary and valuable (unblocks shells, build tools, servers, native
  apps, language runtimes); §M42 (browser) is only the *completeness
  proof / bonus*, not the driver.  §M34 POSIX process & signals
  (fork/execve-argv/waitpid/pipes/job-control/signals — the general POSIX
  abstraction layer) → §M35 threads & futex (clone/TLS/pthreads/futex on
  the SMP scheduler) → §M35.5 pkg store → §M36 POSIX syscall breadth +
  native libc (musl port) → §M37 dynamic linking (ld.so/`.so`/dlopen) →
  §M38 C++ runtime + support libs (libc++/unwind, zlib, freetype, ICU,
  harfbuzz, Skia…); side-branches §M39 crypto+entropy+TLS+DNS (`/dev/
  urandom`, mbedTLS/BoringSSL, getaddrinfo — needs §M24) and §M40 client
  graphics (libwayland-client + Mesa `llvmpipe` EGL/GL + Skia — needs
  §M26); §M41 optional Linux syscall ABI shim (binary-compat accelerator,
  useful on its own).  §M42 validation target only: NetSurf (realistic
  first) → WPE-WebKit → Firefox/Chromium (multi-year north star).
  Hard-depends on §M25; target x86_64/aarch64 (i386 out of scope for the
  heavy ports).
- **§M-registry** — Windows-style registry PARKED (accidental history;
  /etc + procfs already covers it).

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

# Per-arch convenience wrappers (thin shims over the ARCH= scripts above):
./scripts/build-i386.sh   ./scripts/run-i386.sh
./scripts/build-x86_64.sh ./scripts/run-x86_64.sh
./scripts/build-aarch64.sh ./scripts/run-aarch64.sh   # ARM64 (raw ELF on -M virt)
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

NB: a FORMATTED image (e.g. `build/exfat.img`, mkfs.exfat'd in the
Docker container) carries a boot signature — add `-boot d` or SeaBIOS
boots the empty disk instead of the CD and hangs with no serial
output at all.

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
