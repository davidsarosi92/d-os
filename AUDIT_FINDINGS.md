# d-os kernel audit — correctness & operating-principle violations

Auditor: Claude (Opus). Date: 2026-07-25. Scope: `kernel/` (i386 focus, with
x86_64 / aarch64 parity notes). No code was modified.

Focus, in the owner's priority order:
1. Freeze-freeness / hang risks
2. Ring-0 / ring-3 privilege boundary
3. Modularity / convention violations
4. Other clear correctness bugs

Legend: **High** = can freeze the box or breach the kernel from ring 3 today;
**Med** = real bug, narrower trigger; **Low** = latent / defense-in-depth.

---

## STATUS (2026-08-01) — what has been fixed since the audit

Everything below is marked with its resolution.  Implementation + rationale:
DOCS.md §4.37; the milestone is §M46.

| # | Finding | Status |
|---|---------|--------|
| 1.1 | Bad user pointer in any syscall = kernel #PF = halt | ✅ **FIXED** — two layers: a per-syscall gate (`task->in_user_syscall` + `vmm_user_access_ok`) and a real exception table (`.ex_table` + `uaccess_copy_in/out/str_in`, fixup in every arch fault handler).  `SYS_PRINT` and the whole `linux_abi` surface routed through it.  Proof: `faulttest`.  Residual: bulk read/write payloads handed straight to the VFS/socket layers still rely on the pre-check alone (no bounce buffer) |
| 1.2 | aarch64 has no ring-3-fault-kill | ✅ **FIXED** — EL0 faults kill the process; EL1 faults apply `kernel.fault_policy` (halt/reboot/kill) |
| 1.3 | `SYS_SIGRETURN` restores EFLAGS from an unvalidated user stack | ✅ **FIXED** — frame validated, EFLAGS masked to `0xCD5` (IF/IOPL/NT/TF kept from the kernel frame) |
| 1.4 | `signal_deliver` writes to an unchecked user ESP | ✅ **FIXED** — frame range validated; SIGSEGV instead of a kernel fault |
| 1.5 | `stdin_read_line` ignores a pending kill | ✅ **FIXED** — `task_should_stop()` per iteration |
| 1.6 | `task_exit_code` fallback | ✅ verified-good, and now idles with IRQs ON + reports to COM1 instead of `cli;hlt` |
| 1.7 | Device poll loops bounded | ✅ verified-good (unchanged) |
| 2.1 | dosgui handles not bound to the calling process | ✅ **FIXED** — owner pid recorded, every call ownership-checked |
| 2.2 | `dosgui_present` trusts the client's px/w/h/stride | ✅ **FIXED** — geometry clamped + whole source range validated |
| 2.3 | `sys_kill` can signal any task | ✅ **FIXED** — a ring-3 caller may only signal itself or a descendant; kernel threads / pid 0 / init refused |
| 2.4 | `pipe`/`socketpair` write a raw user array | ✅ **FIXED** — gated (`user_w`) |
| 2.5 | `MAP_FIXED` honours an arbitrary addr | ✅ **FIXED** — must lie in the user range, no wrap |
| 3.1 | Resilience features diverge across arches | ✅ **FIXED** — x86_64: force-kill safe point, NMI handler, **real COW**; aarch64: EL0-fault-kill, fault policy, force-kill safe point.  Remaining by design: no HW watchdog on `virt` (no ib700) |
| 3.2 | `usyscall.c` assumes the identity map | ◐ unchanged (documented assumption; the ACPI variant of this bug — tables above the identity window — is fixed, see below) |
| 3.3 | Driver self-registration | ✅ verified-good |
| 4.1 | GUI `state_lock` IRQ discipline | ✅ verified-good |
| 4.2 | `wq_wake_locked` re-entrancy | ◐ analysed as serialised by `wq->lock`; comment added, no code change |
| 4.3 | `task_wait` vs the universal reaper | ✅ verified-good (fork/spawn children flag `reap_owned`) |
| 4.4 | `getdents` writes to an unvalidated buf | ✅ **FIXED** — gated |
| 4.5 | Huge `len` integer handling | ✅ **FIXED** — single mapping capped at 256 MiB |
| 4.6 | `next_pid` wrap | ✅ **FIXED** — never wraps to a negative pid |

**Found during the fix work, not in the original audit:**

| Finding | Status |
|---------|--------|
| ACPI tables sit at the TOP of low RAM, above the i386 256 MiB identity map → the first table read is an unmapped kernel access → **silent boot freeze with `-m 512M`** | ✅ **FIXED** — `acpi_reach()` identity-maps each table on demand |
| The `sys_*` layer is dual-use (ring-3 dispatchers + in-kernel callers); gating inside it broke `linux_abi`'s kernel-buffer calls → ld.so could not `fstat` any `.so` → **NetSurf stopped starting on both x86 arches**, plus `fdtest`/`socktest`/`polltest` | ✅ **FIXED** — gate keyed on `task->in_user_syscall`; kernel-destination calls use `sys_*_k` cores |

---

## CATEGORY 1 — FREEZE-FREENESS / HANG RISKS

### 1.1 [High] A bad user pointer in ANY syscall = kernel #PF = whole-box halt
`kernel/hal/x86/syscall.c` (all cases), `kernel/hal/x86/linux_abi.c` (all cases),
`kernel/core/usyscall.c`, dereference path ends at `kernel/hal/x86/idt.c:436`
(`ring0_fault_policy`, default `halt` → `for(;;) cli;hlt`).

Every syscall takes user-supplied pointers straight from the trapframe and
dereferences them in ring 0 with **no `copy_from_user`/`copy_to_user` and no
range check** (`usyscall.c` header even says "A hardened copy_from/to_user is a
later refinement"). Examples: `SYS_WRITE` walks `f->ecx`; `SYS_PIPE`/`SYS_SOCKETPAIR`
write `*(int*)f->ebx`; `sys_stat/uname/clock_gettime` write `*out`; `linux_abi`
`writev`/`readv` walk a user `iovec` array; `proc_execve` walks the user `argv[]`
array and each string.

A user program that passes a kernel address, an unmapped address, or `NULL+offset`
causes a **ring-0** page fault (CS ring bits = 0 because we're inside the kernel).
The ring-3-fault-kill path at `idt.c:421` only fires when `(f->cs & 3) == 3`, so it
does NOT catch a kernel-mode fault triggered by servicing a user pointer. That
falls through to `ring0_fault_policy` → default `halt` → the CPU spins in `cli;hlt`
forever, killing the timer, scheduler, cursor and watchdog on that CPU. **This is
exactly the "a frozen program freezes the whole system" the owner wants to prevent,
and it is trivially reachable from any package** (e.g. `write(1, (char*)0xC0000000, 8)`).

Fix direction: introduce real `copy_from_user`/`copy_to_user`/`strncpy_from_user`
that (a) reject any address outside the user range (`>= vmm_user_base()` and below
the kernel split), and (b) survive a fault. Two viable mechanisms: validate the
address range up front against the task's `mm` mappings, or install a fault
fixup so a kernel #PF on a "user access" instruction returns -EFAULT instead of
halting. Route every syscall's user buffer through it. This single change closes
the largest class of package-induced hangs.

### 1.2 [High] aarch64 has NO ring-3-fault-kill — any EL0 fault halts the box
`kernel/hal/aarch64/exceptions.c:95,99` (`dump_and_halt("synchronous", tf)` for the
EL0/EL1 synchronous path that is not an SVC).

On i386/x86_64 a ring-3 exception terminates just the process (`idt.c:421` /
`x86_64/idt.c:288`). On aarch64 the synchronous-exception handler only special-cases
`EC_SVC64` (syscall); every other EL0 fault (data abort, undef, etc.) goes to
`dump_and_halt` → `for(;;) wfe`. A userspace crash on ARM therefore halts the whole
machine — a direct violation of the freeze-freeness rule and a cross-arch divergence
from the x86 policy.

Fix direction: in the aarch64 synchronous handler, decode the fault EL from
SPSR/ESR; if it came from EL0, kill the current task (`task_exit_code(128+SIGSEGV)`)
and reschedule, mirroring the x86 `isr_handler` ring-3 branch. Only EL1 faults
should reach `dump_and_halt`.

### 1.3 [High] `SYS_SIGRETURN` restores EFLAGS/EIP from an unvalidated user stack
`kernel/hal/x86/signal.c:77` (`signal_sigreturn`).

`signal_sigreturn` reads the saved register block from `f->user_esp + 4` with no
check that the pointer is a mapped user address and no sanitization of the restored
EFLAGS. A ring-3 program can invoke `int 0x80` with `eax=SYS_SIGRETURN` directly and
a crafted `user_esp`, causing the kernel to (a) read from an arbitrary user address
(bad → ring-0 #PF → halt, see 1.1), and (b) load an attacker-controlled EFLAGS into
the trapframe. Restoring EFLAGS wholesale lets the process set/clear flags it should
not control (e.g. IOPL bits, direction/trap flags) on the iret back to ring 3.

Fix direction: mask the restored EFLAGS to a safe set (keep the fixed reserved bit,
force IF on, clear IOPL/NT/etc.), and validate `user_esp` via copy_from_user (1.1).

### 1.4 [Med] `signal_deliver` writes the signal frame to an unchecked user ESP
`kernel/hal/x86/signal.c:60-71`.

Delivery decrements `f->user_esp` and writes the saved-context frame there directly.
If the user stack pointer is bad/exhausted (a program that smashed its own `%esp`,
or a deliberately tiny/misaligned stack), the kernel-mode stores fault → ring-0 #PF
→ halt (1.1). A crashing package should not be able to halt the box via its own bad
stack during signal delivery.

Fix direction: validate/copy_to_user the frame; on failure force-terminate the task
(SIGSEGV default) instead of faulting in the kernel.

### 1.5 [Med] `stdin_read_line` can loop forever ignoring a pending kill
`kernel/core/usyscall.c:90-107`, reached from `sys_read(fd=0)`.

The cooked-stdin loop `for(;;) { c = vc_getchar(v); ... }` blocks until a newline
arrives and has no `task_should_stop()`/kill check. A ring-3 process blocked in
`read(0,...)` cannot be force-killed while parked here (the force-kill safe point
only fires on a return-to-user timer preemption, and this path is in-kernel). If the
focused VC never receives input, the task is unkillable until Enter is typed. Not a
whole-box freeze, but it defeats "kill a frozen app": a `pkgrun sh` waiting on stdin
resists End-task.

Fix direction: check `task_should_stop()` each iteration (as `task_msleep` does) and
return, or make `vc_getchar` interruptible by the kill flag.

### 1.6 [Low] `task_exit_code` "unreachable" fallback idles but never truly exits
`kernel/core/task.c:1122` (`for(;;){ hal_cpu_idle(); schedule(); }`).

Correctly avoids a hard freeze (idles with IRQs on, re-runs the scheduler), so this
is a benign safety net, not a bug. Noted as verified-good.

### 1.7 [Low] `dosgui`/net RX poll loops are bounded — verified OK
`sys_recvfrom` (`usyscall.c:742`) spins ≤ 40,000,000 with `hal_cpu_pause`;
virtio-blk/net TX loops cap at 50,000,000 spins; xhci reset/command loops are
`timer_ticks_ms()` deadline-bounded. No unbounded device polls found. The buddy
PMM allocation loops are order-bounded (`pmm.c:383,393,418`). Verified-good.

---

## CATEGORY 2 — RING-0 / RING-3 PRIVILEGE BOUNDARY

(1.1, 1.3, 1.4 above are also boundary breaches; not repeated here.)

### 2.1 [High] `dosgui` handles are not bound to the calling process
`kernel/gui/dosgui.c:94` (`dosgui_present`), `:103` (`dosgui_poll`), `:123`
(`dosgui_destroy`). Reached from `linux_abi.c:803-816`.

`dosgui_present(handle, px, …)` validates only `0 <= handle < DOSGUI_MAX` and
`g_dg[handle].used`. It does NOT check that the handle was created by the calling
task. Any ring-3 process can pass another process's handle and blit arbitrary pixels
into a window it does not own, poll/steal its input events, or `dosgui_destroy` it
out from under the owner (→ the owner's later calls act on a recycled slot). This is
a cross-process integrity breach and, via destroy/recreate races, a route to
compositor state corruption.

Fix direction: store the owner pid in `struct dosgui_win` at create time and reject
any dosgui call whose handle owner != `task_current()->pid`.

### 2.2 [High] `dosgui_present` passes an unvalidated user framebuffer to the compositor
`kernel/gui/dosgui.c:99` → `gui_window_blit(win, 0,0, px, w, h, stride)`.

`px`, `w`, `h`, `stride` come straight from ring 3 (`linux_abi.c:808`). Only
`px != NULL && w>0 && h>0` is checked; `stride` and the `w*h*stride` extent are not
bounded against the source mapping. `gui_window_blit` will read `h*stride` words
from `px`; a large/negative-cast `stride` or `h` makes the compositor read far past
the client's buffer (kernel-mode read of arbitrary/unmapped memory → info leak into
a window, or ring-0 #PF → halt, per 1.1). The client also controls the read region
independent of the window's real surface size.

Fix direction: clamp `w`/`h` to the window surface dimensions, reject non-positive
or oversized `stride`, and validate that `[px, px + h*stride)` lies within the
caller's user address space before the blit.

### 2.3 [Med] `sys_kill` lets a ring-3 process signal ANY task, including kernel threads
`kernel/core/usyscall.c:348-354`.

`sys_kill(pid,sig)` does `task_find(pid); t->sig_pending |= (1u<<sig)`. There is no
check that the target is the caller's own process (or child), no credential check
(§M32 not yet), and no exclusion of kernel threads/init/idle. A user can set
`sig_pending` on `init`, the compositor, a driver kthread, etc. Kernel threads never
run `signal_deliver` (it early-returns unless returning to ring 3), so the bit is
usually inert — but any kthread that later drops to ring 3, or any future signal
handling on kthreads, would act on an attacker-posted signal. At minimum it is an
unenforced boundary.

Fix direction: restrict `sys_kill` targets to the caller's process tree / same uid
(once §M32 lands), and refuse `is_idle`/pid 0/kernel-only tasks.

### 2.4 [Med] `sys_dup2`/`sys_close`/`fd_lookup` bounds are OK, but `newfd` write path trusts pipe/socketpair user array
`kernel/core/usyscall.c:302-343`.

`fd_lookup`/`fd_install` correctly bound fds to `[3, TASK_MAX_FDS)` (verified-good).
However `sys_pipe`/`sys_socketpair` write `fds[0]/fds[1]` through the raw user
pointer `f->ebx` with no validation (same class as 1.1). A bad `int* fds` faults the
kernel. Fix with copy_to_user (1.1).

### 2.5 [Low] `sys_mmap_full` MAP_FIXED honors an arbitrary user `addr`
`kernel/core/usyscall.c:229`.

`MAP_FIXED` maps at `addr & ~0xFFF` with only "non-zero" as the guard. A user could
request a fixed mapping over its own critical pages; it cannot map over kernel space
because `vmm_space_map` targets the task's own space, but there is no check that
`addr` is within the legal user range. Low risk today (self-harm only), but worth a
`va >= vmm_user_base() && va < user_top` guard for hygiene.

---

## CATEGORY 3 — MODULARITY / CONVENTION VIOLATIONS

### 3.1 [Med] Ring-3-fault-kill / resilience features diverge sharply across the three arches
Cross-arch matrix of the freeze-freeness mechanisms:

| Mechanism | i386 | x86_64 | aarch64 |
|---|---|---|---|
| Ring-3 fault → kill process (not halt) | yes (`idt.c:421`) | yes (`x86_64/idt.c:288`) | **NO** — halts (`exceptions.c:95`) — see 1.2 |
| `ring0_fault_policy` (halt/reboot/kill) | yes | yes | no (`dump_and_halt` only) |
| COW #PF handler (`vmm_cow_fault`) | yes (`idt.c:400`) | **no** | no |
| NMI hard-lockup handler (L3 watchdog) | yes (`idt.c:359`) | **no** | no |
| Force-kill safe point at IRQ exit (`task_force_kill_point`) | yes (`idt.c:450,467`) | **no** (no call in `x86_64/idt.c`) | n/a path |
| SAK hotkeys (Ctrl+Alt+Del/X) | GUI (i386/x86_64 shared) | shared | ARM GUI parity claimed |

The owner's resilience model (§M46: force-kill a wedged ring-3 task at the timer
preemption boundary) is **only wired on i386**. x86_64's `isr_handler` IRQ paths
(`x86_64/idt.c:305-328`) call `schedule_check()` but never `task_force_kill_point`,
so a wedged busy-looping ring-3 program on x86_64 cannot be force-killed the way it
can on i386. This is a convention/parity gap that undermines the flagship
freeze-freeness feature on the 64-bit target.

Fix direction: add `task_force_kill_point((f->cs&3)==3)` to the x86_64 IRQ/timer
exit paths; add the EL0-fault-kill to aarch64 (1.2); document the COW/NMI gaps as
i386-only-by-design or port them.

### 3.2 [Low] `usyscall.c` (portable core) reaches into arch-ish assumptions — acceptable but noted
`kernel/core/usyscall.c` assumes the identity map covers user frames (`shm_create`
zeroes via `(uint8_t*)(uintptr_t)f`, `sys_mmap` likewise). This is a documented
"< 1 GiB / identity map" assumption, not `__asm__`/port I/O, so it does not violate
convention 3 literally, but it is an arch-layout assumption living in `kernel/core`.
Low priority; flagged for the x86_64 HIGHMEM / >1 GiB path.

### 3.3 [OK] Driver self-registration — verified good
Spot-checked `ps2_mouse.c`, `virtio_blk`, `ac97`, `virtio_net`: all use
`MODULE()`/`DRIVER()` registration, none are hand-wired into `kernel_main`.
Verified-good.

---

## CATEGORY 4 — OTHER CORRECTNESS BUGS / RACES

### 4.1 [OK / Low] GUI `state_lock` IRQ-vs-task discipline — verified correct
`kernel/gui/gui.c:1749` (`gui_mouse`, mouse IRQ12) and `:1965` (`gui_raw_key`,
keyboard IRQ) take `spin_lock(&state_lock)` plain; all task-context sites
(`gui.c:472,913,1265,1357,1398,…`) take it via `spin_lock_irqsave`.

I initially flagged this as a reentrancy deadlock, but on verification it is the
**textbook-correct** pairing: the IRQ handlers run with IRQs already off (plain
`spin_lock` is right there), and every task-context acquirer uses `irqsave` which
disables IRQs for the duration — so the mouse/keyboard IRQ can never fire on the same
CPU while a task holds `state_lock`. No same-CPU self-deadlock, and on SMP the IRQ
CPU simply spins until the holder releases (bounded). **Verified-good.**

The only residual note (Low): `state_lock` critical sections in task context that
call into long compose/damage work run with IRQs disabled on that CPU, which can add
IRQ latency (cursor/timer jitter) but not a hang. If jitter is observed, moving mouse
handling onto the compositor task (a deferred queue, as the keyboard char path already
does) would shorten IRQ-off windows. Not a correctness bug.

### 4.2 [Med] `waitq_wake_all` clears `wq->head` but walks `t->wq_next` after nulling — re-entrancy on the freed chain
`kernel/core/task.c:1198-1222` (`wq_wake_locked`).

In the `all` branch it sets `wq->head = NULL`, then loops reading `nxt = t->wq_next`
before nulling `t->wq_next`. `task_enqueue(t)` is called inside the loop while still
walking the old chain via the saved `nxt`. This is correct as written (nxt is
captured before enqueue, and an enqueued task's `wq_next` was already nulled), so it
is race-free on its own. HOWEVER `task_enqueue` may `smp_send_reschedule` and the
woken task can start running on another CPU and re-block on the SAME wq before the
loop finishes — it would set `wq->head = itself` with `wq_next` pointing into the
chain we're mid-walk. Under contention this can corrupt the list. Low-to-med
likelihood but worth hardening.

Fix direction: snapshot the whole wake list into a local array under the lock, null
all `wq_next`, THEN enqueue after the walk completes (or keep `wq->lock` held across
the enqueues, which it currently is via the caller — verify the caller holds it for
the full loop; `waitq_wake_all` is documented as "caller holds wq->lock", and
`task_enqueue` takes rq_lock not wq->lock, so the window is real only if a woken task
re-blocks on the same wq, which requires it to acquire wq->lock — so it actually
serializes. Downgrade to Low after that reasoning, but flag for a comment.)

### 4.3 [Low, mitigated] `task_wait` vs universal reaper — fork children are safe, other spawn paths rely on the contract
`kernel/core/task.c:975-1005` + `reaper_pass` (`:901`); `proc_fork` at
`kernel/hal/x86/fork.c:128` DOES call `task_set_reap_owned(child, 1)`, and
`proc.c:460` does likewise for spawned user tasks.

Verified: `proc_fork` and the user-spawn path both flag the child `reap_owned`, so
init's universal reaper leaves the DEAD child in place for the parent's `waitpid` —
the race I first suspected does not occur for fork/spawn children. The residual (Low)
risk is only for any FUTURE `task_wait` on a child created through a path that forgets
`reap_owned`; a defensive "init skips reaping a task whose parent is alive and not
init" would make the guarantee structural rather than by-convention.

### 4.4 [Med] `getdents`/`getdents64` trust `cap` but write records before the final bounds recheck edge
`kernel/core/usyscall.c:395-446`.

Bounds are checked (`if (used + reclen > cap) break;`) before each write — correct.
But `buf` itself is an unvalidated user pointer (class 1.1). With copy_to_user staged
writes this is fine; today a bad `buf` faults ring-0. Grouped with 1.1.

### 4.5 [Low] `shm_create`/`sys_mmap` integer handling on huge `len`
`kernel/core/fd.c:76` / `kernel/core/usyscall.c:166`.

`n = (len + 4095)/4096` can wrap for `len` near `SIZE_MAX` (user-controlled via
`SYS_MMAP`/`SYS_MEMFD`). `shm_create` guards with `n > SHM_MAX_FRAMES`, but
`sys_mmap`'s anon path computes `n` then loops `pmm_alloc_frame` `n` times with only
per-frame OOM unwinding — a wrapped small `n` under-allocates (benign), a large `n`
just OOMs gracefully. Low risk; add an explicit `len` upper bound for clarity.

### 4.6 [Low] `next_pid` monotonic with no wrap/reuse guard
`kernel/core/task.c:93` (`next_pid++`).

pids never wrap in practice, but there's no guard against `next_pid` overflow or
collision with a still-live pid after ~2^31 spawns. Purely theoretical for a hobby
OS uptime; noted for completeness.

---

## Coverage summary (what was checked)

- Read in full: `task.c`, `syscall.c` (i386), `linux_abi.c` (i386), `usyscall.c`,
  `fd.c`, `dosgui.c`, `signal.c` (i386), `idt.c` fault paths (i386 + x86_64),
  `proc.c` exec/argv, `ps2_mouse.c`, GUI lock map (`gui.c`).
- Grepped: driver lock discipline (no `spin_lock` in `*_irq` handlers in
  `kernel/drivers/` — the only IRQ-context lock user is `gui_mouse`), device poll
  loops (all bounded), buddy PMM loops (order-bounded), aarch64 exception policy.
- Verified-good: driver self-registration (MODULE/DRIVER), fd bounds in `fd_lookup`,
  bounded device timeouts, `task_exit_code` idle fallback.
- Not deeply audited (out of time / lower priority): `usock.c`, `vfs.c`/`exfat.c`
  internal locking, `futex.c`, `bus.c`/`service.c`, x86_64 `linux_abi.c` (assumed to
  mirror i386's user-pointer issues), full `gui.c` compose path.

## Top recommendations (by leverage)
1. **copy_from/to_user with fault-tolerance** (fixes 1.1, 1.3, 1.4, 2.2, 2.4, 4.4) —
   the single highest-value change; closes the biggest package-induced-freeze class.
2. **aarch64 EL0-fault-kill** (1.2) and **x86_64 force-kill safe point** (3.1) —
   bring the two other arches up to i386's freeze-freeness.
3. **dosgui handle ownership + blit clamping** (2.1, 2.2) — cross-process integrity.
4. **Sanitize SYS_SIGRETURN EFLAGS** (1.3) and **restrict sys_kill targets** (2.3).
