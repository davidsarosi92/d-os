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

▶️ **§M53 STAGES 1–2 — TIME, IN NANOSECONDS (2026-08-09, DOCS §4.53, all 3
arches).**  `timer_now_ns()` — ONE monotonic nanosecond clock from whatever the
machine has, callers never learning which: `CNTPCT_EL0` on aarch64 (62.5 MHz,
16 ns; the architecture DEFINES the rate, nothing to calibrate) and the TSC on
x86 (~1.2 GHz, 1 ns) but **only once established to be constant-rate**
(`CPUID.80000007:EDX[8]`, or `CPUID.1:ECX[31]` — a hypervisor virtualises the
TSC so it cannot track a guest core's frequency scaling).  Neither → keep the
tick: *a coarse clock that is right beats a fine one that is wrong.*  Plus
**deadline timers** (`ktimer_arm`/`ktimer_cancel`, one sorted list, callbacks in
IRQ context with the lock dropped), `task_sleep_until_ns`, and
`sys_clock_nanosleep_ns` (the ABSOLUTE form matters: a relative sleep restarted
after a signal DRIFTS, so every non-drifting periodic loop is written against
the absolute one).  **THE CLOCK FOUND A REAL BUG IN ITS FIRST MINUTES:** `ktime`
reported a 100 ms sleep on aarch64 as **57 ms** — `timer_ticks_ms` divided a
tick counter that EVERY CPU increments by a PER-CPU rate, so the millisecond
clock ran N× too fast on an N-CPU box and every timeout/watchdog deadline/sleep
on that arch was wrong by the CPU count, silently, **for want of a second
opinion**.  Now read from `CNTPCT` directly (100.0/105/106 ms at -smp 1/2/4);
x86 was never affected (the PIT delivers to the BSP only).  **AND THE ACCURACY
FLOOR IS MEASURED, NOT ASSUMED:** expiry first went into `schedule_check`, which
LOOKS like the tick and runs at the QUANTUM rate — every timer up to 10 ms late
regardless of its deadline (a 500 µs sleep measured 9.7 ms).  Moved to the tick
ISR: worst lateness **9037 µs → 840–953 µs on i386**, i.e. the tick period
itself; aarch64's floor is 10 ms because it ticks at 100 Hz, and its `ktimer`
says so.  New **`ktime`** + **`ktimer`** commands on both shells report source,
resolution and the error on a spread of sleeps.  **Open:** one-shot hardware
deadlines (TSC-deadline / `CNTP_CVAL`) to remove the tick floor; raising the
aarch64 tick needs a quantum divider like x86's `SCHED_QUANTUM_TICKS` first;
`timerfd` + `timer_create`/`setitimer` (stage 3, and what `epoll`-shaped event
loops need); a clock read costs a 64-bit division (~2–4 µs under emulation) —
Linux precomputes a multiply-and-shift.

✅ **§M52 — THE NOTE THAT OUTLIVED ITS PREMISE (2026-08-09, DOCS §4.52; x86_64
clean at -smp 1/2/4).**  x86_64's SYSCALL entry stub kept the kernel stack and
the stashed user `rsp` in **two GLOBALS**, so two CPUs inside `syscall` at once
overwrote each other's stash **and ran the kernel on the SAME stack** — each
then returned to ring 3 with the other's stack pointer (a child executing its
own argv strings; the kernel returning to address 3).  **`syscall_entry.s` had
said so in its own header since §M20.6.1** — *"UP-correct only … ring-3 tasks
only run on the BSP today … **Noted, not built**"* — and every word was TRUE
when written.  §M35 then gave x86_64 a per-CPU TSS and ring-3 tasks began
running on APs; nothing went back to the note.  **A COMMENT CANNOT FAIL A
TEST.**  Fixed with **`swapgs`**, the instruction that exists for exactly this:
it swaps `IA32_KERNEL_GS_BASE` into `GS.base` atomically so the stub reaches a
per-CPU slot (`[gs:0]` kernel rsp, `[gs:8]` user-rsp stash) **without needing a
spare register** — and `syscall` leaves none (`rcx`/`r11` clobbered by the
instruction, everything else holds an argument).  GS is free because x86_64 musl
keeps TLS in FS.  Swaps back BEFORE the shared `isr_common` tail, so nothing
else in the kernel knows.  **WHY IT HID — THREE INDEPENDENT IMMUNITIES:** (1)
i386 enters via `int 0x80`, and an interrupt gate switches stacks through the
TSS, per-CPU since §M35; (2) native d-os programs (`forktest`/`forkexec`/
`pipetest`) use `int 0x80` too and passed at -smp 2 throughout — only **musl**
binaries issue `syscall` (musl hard-codes it, and not patching musl is the whole
point of the personality); (3) ONE musl process rarely collides with itself, so
`musltest` passed — it took TWO (a shell and the coreutil it forks).  **GENERAL
LESSON: a deferred note is a DEPENDENCY ON A PREMISE, and nothing in the build
checks that the premise still holds.**  Both milestones that invalidated it were
green.

✅ **§M51 — THE BROADCAST x86 DOES NOT HAVE (2026-08-08, DOCS §4.51, i386
verified at -smp 1/2/4).**  x86 does NOT broadcast TLB invalidation — `invlpg`
and a CR3 reload are strictly local — and **nothing in the tree ever sent an
invalidation IPI**.  COW's whole safety argument is "the next write faults", so
a parent runnable on a second core in the window between `fork` and the child's
`execve` kept a WRITABLE entry for pages `fork` had just protected: its next
write did NOT fault, it landed in the frame the child was sharing, and the two
processes corrupted each other **with no fault and no log**.  That is why
`pkgrun sh -c "echo A; echo B"` failed ~2 runs in 3 at `-smp 2` and passed every
time at `-smp 1`.  New **`kernel/hal/x86/tlb.c`** (shared by both x86 arches):
IPI vector 0x42 + a per-CPU **ticket pair** (`percpu.tlb_req`/`tlb_ack`) — no
lock and no request slot, because the remote action is always a **FULL flush**,
which makes overlapping requests harmless; the ack is published only AFTER the
flush; and **the wait loop services its own slot**, without which two
simultaneous shootdowns from interrupt-disabled contexts wait on each other
forever.  aarch64 needs none of it (`tlbi ...is` IS the hardware broadcast) —
its `hal_tlb_shootdown` is an empty function with the reason written down.
**Only WEAKENING edits pay:** remap-over-present, unmap, mprotect and the COW
resolution broadcast; a FRESH map does not, and that distinction is what keeps
`map_in_pd` (once per page of every ELF load) affordable.  `vmm_space_clone`
suppresses the per-page broadcast entirely and issues ONE whole-space shootdown
at the end — the first version broadcast per page and turned a fork into
thousands of IPI round trips that never finished.  **LESSON — THE HARNESS LIED
FOR AN HOUR:** after adding fields to `struct percpu` the shootdown *appeared*
to hang; the target CPU was demonstrably alive (tick/switch counters advancing)
yet never took the vector, and its `apic_id` read back as 3 on a box whose boot
log said 1.  **This project has no header dependencies** — CLAUDE.md says to
`make clean ARCH=<arch>` after editing a shared header, and I had not, so half
the tree used the OLD `struct percpu` layout.  Every measurement in that window
was fiction; the first run after `make clean` passed.  *A documented build
convention is a CORRECTNESS convention, and an impossible measurement — a field
holding a value it cannot hold — is evidence about the BUILD, not the code.*
The x86_64 SMP failure left open here turned out to be a
DIFFERENT bug entirely and is fixed in §M52 above — and the pointer that led
there was exactly "i386 and x86_64 share the VMM shape but NOT the syscall entry
path".

✅ **AARCH64 A3 — A SHELL THAT FORKS AND EXECS, AND THE REGISTER NOBODY SAVED
(2026-08-08, DOCS §4.50).**  `pkgrun sh -c "echo A; echo B; echo C"` prints all
three on ARM — a musl shell forking musl coreutils out of the store, third arch,
`-smp 1` and `-smp 4`.  The predicted work (cross toolchain, arch-parametric
recipes) was an afternoon; the stage was held up by **per-task state kept where
nothing saves it, twice**: `TPIDR_EL0` (EL0 writes it with one `msr`, so
`has_tls` is never set and a forked musl child dies at `TP-0xc8`), then
**`SP_EL0`** — banked, absent from the trapframe, written by the kernel exactly
once at the `eret` into EL0, and **never saved by `context_switch`**.  A task
that blocked at EL0 resumed with whichever task ran there next, so `sh -c "echo
one"` printed `one` and THEN the shell died at a wild PC (a shell's first
schedule-out at EL0 is its `waitpid`).  **x86 cannot have this bug — the user SP
is a field in the frame the CPU pushes — which is why no design review caught
it.**  RULE FOR A4 AND ANY NEW ARCH: *enumerate the registers the kernel writes
once and the user owns thereafter; every one belongs in the switch* (here:
TPIDR_EL0, SP_EL0, FP/SIMD — all three found one crash at a time).  Found by
NARROWING, not guessing: a never-free experiment, a `-smp 1` run, a **canary in
the trapframe — which proved the frame INTACT and killed every theory about the
return path at once**, `ESR` in the fault print, then a dump of the user stack
showing kernel frame data at user addresses.  `ESR`+`SP_EL0`+task name are now
permanent in that print.  Three pre-existing defects fixed on the way:
`cow_release` decremented the LAST reference without freeing (every fork-shared
page leaked); the COW paths used CPU-local `tlbi vmalle1` where a sibling core
keeps a stale writable entry (→ `vmalle1is`); `ABI_WAIT` returned the raw exit
code instead of `(code & 0xFF) << 8`, unvalidated.  **Build lesson:** the
per-arch artifact cache filed `user/*.muslelf` by `build/.last_arch` — a HINT,
wrong whenever `make` runs directly — and had parked an AArch64 `sh.muslelf` in
the x86_64 slot, whence it was linked into the x86_64 kernel and surfaced as
`rc=-7` (ELF_EBADARCH); **27 cached artifacts were mis-filed**.  The cache now
keys on the file's own `e_machine`: *ask the file, not the stamp.*  **OPEN, NARROWED, NOT ROOT-CAUSED —
TOP OF THE LIST (full state of the hunt in DOCS §4.50): a multi-command `sh -c`
fails on BOTH x86 arches and ONLY UNDER SMP.**  `-smp 1` passes every time;
`-smp 2` fails ~2 runs in 3; adding a `kprintf` to the syscall path makes it pass
— a RACE, not a logic error.  The parent's `esp`/`ebp` at the fault are IDENTICAL
every run (inside `run_command`, its local `argv[]` intact) while the faulting
`eip` is DIFFERENT every run and always garbage — so a corrupted **code pointer**,
not a corrupted stack pointer (which is what distinguishes it from the ARM bug).
Ruled out by measurement: the artifact cache (i386 never affected), `task_reap`
freeing a still-queued task (probe never fired), stale cross-CPU TLB on migration
(forced an unconditional CR3 reload — **still fails**), `signal_deliver` on a
ring-0 frame (correctly guarded).  **Still standing and where to look first:
THERE IS NO TLB SHOOTDOWN IPI IN THE TREE** — every `invlpg` on both x86 arches
is CPU-local and `lapic_send_ipi` serves only the preempt IPI.  A CR3 reload
closes the MIGRATION window but not the one where two tasks share an address
space on two cores concurrently — exactly what a `fork` in flight is before the
child's `execve` lands.  Also seen twice: an SMP kernel fault in `load_steal_one`
(§M49 balancer) taking the runqueue lock down; probably the same corruption, not
yet shown.  Also open: `ls` needs `openat`+`getdents64`, which need a place in
the §M50 pipeline for per-guest FLAG translation (a second table, a design step).

✅ **AARCH64 A2 — UNMODIFIED musl RUNS ON ARM, IN ~80 LINES (2026-08-07, DOCS
§4.49).**  `hal/aarch64/linux_abi.c` is **~80 lines** vs 1211 (x86) and 1064
(x86_64) — not less capability but §M50's engine landing FIRST, so all that
remains is the genuinely arch-specific part: **x8 = number, x0..x5 = args,
result in x0**.  `musltest` passes with **ZERO unhandled syscalls** — the
vocabulary grown for x86 was already enough for an ARM musl startup, which is
the strongest evidence the engine's split is right.  Pointer gate armed from
line one (§M47.2's lesson).  **THE TRAP PLAN_AARCH64 PREDICTED FOR A6 ARRIVED AT
A2:** musl's `memset` opens with `dup v0.16b, w1` (NEON), so libc startup
trapped — presenting as an EL0 fault with **`FAR_EL1 = 0`, which reads exactly
like a null dereference and is nothing of the kind**.  *When a fault address
looks impossible, DISASSEMBLE THE FAULTING INSTRUCTION before theorising about
the address* — it took one step.  `fpu.c` had described this failure AND both
halves of the fix in advance; both now implemented: `CPACR_EL1.FPEN` per CPU
(**BSP and AP** — enable one core only and FP works there and traps on the
other) + Q0..Q31/FPCR/FPSR saved on context switch (`stp` of 64-bit regs tops
out at 504, so the control words at 512 need plain `str`; a zeroed image IS
valid here, unlike x86 FXSAVE).  A1's forktest/sigtest/pipetest still pass with
FP live on the switch path.  **This unblocks A5/A6 early** — the FP unit is what
every ported library needs.  Also: A2's proof needed the toolchain half of A3
(plan ordering was wrong), cheap because `fetch-musl-cross-prebuilt.sh` was
already arch-parametric.

▶️ **§M50 STARTED — ONE GUEST-ABI TRANSLATION ENGINE (2026-08-07, DOCS §4.48,
PLAN §M50).**  `hal/x86/linux_abi.c` + `hal/x86_64/linux_abi.c` = 2275 lines,
~160 `case`s, **two copies of one idea**; aarch64 (A2) would have been a third.
Linux numbers `read` 3/0/63 on i386/amd64/arm64 — same meaning, different DATA.
Pipeline: **arch shim (frame→args) → per-guest number map → canonical op →
shared handler** (`includes/abi.h`, `core/abi_engine.c`, `core/abi_linux.c`).
A new ARCH = ~6 lines; a new GUEST ABI = a table; a new SYSCALL = one handler
every arch gets at once.  **The engine may DECLINE** and the old switch stays as
fallback — that is what makes the 2275 lines migrate ONE OPERATION AT A TIME
with both paths side by side.  Vocabulary is named after MEANINGS not Linux
spellings (`ABI_SEEK`, not `ABI_LSEEK`) — otherwise the interlingua quietly
becomes "Linux with different numbers".  Both x86 arches now serve read/write/
close/seek/mprotect/munmap/getpid/getppid through it, musl userland unchanged
(`musltest`+`solibtest`+`crypttest` pass on both); `abi` prints the three number
spaces side by side.  **Windows analysis in PLAN §M50:** the pipeline
generalises, the CUT POINT does not — NT syscall numbers are not a contract
(they change between builds), which is why Wine cuts at the DLL boundary and why
WSL1 was replaced by a real kernel; the hard part is SEMANTICS (HANDLEs vs fds,
CreateProcess vs fork, SEH vs signals, reserve/commit vs mmap), not numbering.
Next: migrate more ops, then the aarch64 shim (PLAN_AARCH64 A2) — which also
needs `make musl` made arch-parametric (today hardwired `--target=i386`).

✅ **AARCH64 A1 — A POSIX PROCESS MODEL ON ARM (2026-08-07, DOCS §4.47,
PLAN_AARCH64 stage A1).**  M21's "full x86 parity" was true when written; §M34's
fork + signals landed on x86 afterwards and were never carried across.
PLAN_AARCH64 scoped this as "mirror `hal/x86_64/fork.c`" — **that file was about
a quarter of the job**: the port also had NO aarch64 `struct user_regs` (it fell
through to the **i386** one — `eax`/`ebx` names at 64-bit width), no
`enter_user_mode_regs`, and no data-abort decode into a COW resolution.
Shipped: `vmm_space_clone` (both sides read-only + `PTE_SW_COW`, bit 56) +
`vmm_cow_fault`, refcount table sized from `pmm_nr_frames` (§M48 lesson),
`fork.c` + `signal.c`, and SYS_FORK/WAITPID/EXECVE/PIPE/DUP2/KILL/SIGACTION/
SIGRETURN.  **Three ARM traps worth carrying into A2+:** (1) **SP_EL0 is NOT in
the trapframe** — an EL0 exception switches to SP_EL1 and leaves it banked, so
fork and signal delivery read/write it with `mrs`/`msr`; (2) **the signal return
address is a REGISTER** (x30), not a stack slot, so nothing is pushed and
`SYS_SIGRETURN` finds the saved context exactly at the user SP; (3) **COW must
be resolved BEFORE the uaccess fixup** — a kernel write into a forked child's
buffer must copy the page, not unwind as `-EFAULT`.  Verified over the ARM
SERIAL shell (`serial_shell.c` is its own small REPL — the full `shell.c` needs
a VC behind virtio-input, undrivable headless, which is why PLAN_AARCH64 scopes
the proof there): `forktest` prints `secret still=111` (real COW isolation),
`pipetest` + `sigtest` pass.  Also fixed a pre-existing truncation: teardown cast
physical addresses to `uint32_t` before freeing them.  Next on this arch: **A2,
the Linux-ABI personality** — without it no musl binary runs at all, which is
why ARM embeds 3 in-tree-libc programs where x86 embeds ~60.

✅ **§M49 — LOAD DISTRIBUTION, MEASURED (2026-08-06, DOCS §4.46, i386 +
x86_64, aarch64 builds).**  §M18.6.1's balancer ran **only when a runqueue went
empty** — work stealing, not load distribution — so with every queue non-empty
an arbitrarily bad split never corrected itself; the file's own comment
described a periodic pass and named a `LOAD_BALANCE_INTERVAL_MS` **that existed
nowhere in the tree**.  Nothing caught it because **`run_qemu.sh` passed no
`-smp`**: the everyday run was uniprocessor, so the balancer never executed on
the path a person uses (the §M48 missing-NIC shape again — the measured path and
the used path were different paths).  New **`sched [ms]`** samples twice and
reports the delta (per-CPU rq/load/busy%/switches/migrations, per-task
demand-vs-actual); it showed five hogs on CPU0 at 15-20% of a core while
singletons got 66% — **and every CPU read 100% busy, so aggregate utilisation is
blind to this whole class of bug.**  Four fixes: (1) a **periodic threshold
pass** (a migration swings the difference by TWICE the moved task, hence a
minimum delta — at 1 it ping-pongs forever); (2) load = **demand** (share of
time spent RUNNABLE), because queue length scores four hogs and four sleepers
alike, and *which* task moves matters — the balancer asks for one of roughly
half the imbalance; (3) **`task_msleep` really blocks** — it was a spin-yield
loop that kept every service queued AND `hlt`ed the CPU with work behind it, so
`cron`/`watchdog` measured as CPU hogs (**a metric is only as honest as the
state it observes**); `task_kill` now wakes a timed sleeper so §M46 teardown
latency is unchanged; (4) **priority** — `nice <pid> <-20..19>` → a weight that
is both quantum budget and load share (measured 65/16/13/4% for -10/0/0/+10),
degenerating to the old behaviour at the default.  Result: queue spread 2..6 →
2..3, x86_64 seven of eight hogs at the ideal 49-50%.  **Lesson:** `struct task`
is constructed in FOUR places and only one is `spawn_common` — the new weight
field stayed zero in the other three and the first boot took a #DE in the
scheduler; one `task_sched_defaults()` plus a guard at the division.
**Then the two remaining polls:** `vc_getchar` and init's reaper now BLOCK (per-VC waitq woken from the keyboard IRQ; the ring
write stays lock-free but the WAKE takes the lock — that is what closes the
lost-wakeup window), so an idle 4-CPU box went from **one core pegged at 100% to
all four at 0-2%**.  That needed `task_kill` to wake waitq-parked tasks (new
`task->wq` back-pointer — `wq_next` alone says "in some queue", not WHICH), and
it exposed a **latent SMP race**: the boot shell's VC was bound AFTER spawn
under `preempt_disable`, which has been PER-CPU since §M18.6.2 while
`task_enqueue` puts the task on another core and IPIs it — the shell reached its
entry point first and exited with "no VC bound".  Four sites had it; fixed with
`task_spawn_console`, binding it inside the spawn exactly as `start_arg` already
was.  And a **deferred-work pool** (`kernel/core/workqueue.c`,
`work_submit`/`work_flush`/`workqueue_stats`, one `kworker` per CPU, blocking so
an idle pool costs nothing; pending list and waitq share ONE lock by waitq's own
contract; `work_flush` counts callbacks that RETURNED, not items dequeued; NO
NMI submission — §M47's crash capture keeps its lock-free ring, an NMI-safe path
needs `irq_work`'s shape).  `wqtest [n]`: 16 items over 4 CPUs in **26 ms vs
~80 ms serial**.  **Lesson:** that test first reported FAIL on duplicate runs —
the TEST was wrong, not the queue; re-queueing an item that is already RUNNING
is the intended semantic (how a driver says "more arrived while you drained").
**First production consumer: the xHCI event-ring drain** — `xhci_poll()` runs
from the TIMER IRQ on both arches and used to drain the whole ring there (MMIO
walk + HID decode + a `vc_kbd_push` that now wakes a task and may IPI a core);
the ISR now only `work_submit`s.  One change in `xhci.c` covers both arches
because both timers call the same function.  It also closed a LATENT bug:
`evt_drain` is not reentrant and its other caller (`cmd_submit_wait`, task
context, enumeration) could be interrupted mid-drain — and `evt_drain(NULL)`
from the ISR SWALLOWS the command completion the enumerating task waits for,
turning success into a 200 ms timeout.  New `spin_trylock` (deferred drain skips
if someone is already draining; the enumeration path holds the lock across its
whole wait).  **Verified with `-device qemu-xhci -device usb-kbd`:
`usb-hid: first key delivered over USB` — a one-shot marker added because every
PC target ALSO has PS/2, so "typing still works" is not evidence the USB path
works.**  Still open: `keyboard_getchar` + the GUI compositor/app-host loops are
still `hlt`+`yield` polls (the GUI ones need waitqs on the compositor's event
queues — a compositor change), and NIC RX is the next workqueue consumer but
needs `net.c` LOCKED first: the stack is single-task by construction ("everything
runs in one task context → no locking") and every blocking helper spins calling
`dev->poll`.

✅ **§M48 — THE MEMORY CEILING IS DISCOVERED, NOT COMPILED IN + A USABLE
BROWSER (2026-08-04, DOCS §4.42–§4.44).**  `pmm_init` sizes its metadata from the
firmware map instead of a per-arch `#define`, so ONE image boots on 128 MiB and
on 128 GiB (x86_64 verified at 1G/2G/3G/4G/8G/128G, userland running, zero
faults).  `pmm_phys_t` widens physical addresses to arch width; seeding emits
maximal ALIGNED runs instead of releasing 33.8 M frames one at a time; new
`ZONE_DMA32` — past 4 GiB, "any frame" and "a frame a 32-bit device can reach"
stop being the same thing, and a device handed a truncated address does not fail
loudly.  **Raising the ceiling exposed that x86_64 userland was broken on ANY
machine with >1 GiB RAM**: the identity map's 1 GiB page landed exactly on
`vmm_user_base()` and every `exec` returned `ELF_ENOMEM` — invisible because
every x86_64 test used `-m 1024M`.  User programs cannot move (small code model
⇒ symbols below 2 GiB), so the KERNEL's physical window moved: a direct map in
the canonical upper half (`KERNEL_DIRECT_MAP_BASE`, `phys_to_virt`), which
compiles to nothing where the base is 0.  **Four more latent bugs, all silent:**
slab's `page_of` masked with a 32-bit `~(4096u-1)`; the COW refcount table
covered 1 GiB and a frame outside it became a DOUBLE FREE (fork shares it, both
spaces free it — same shape on i386, safe only by coincidence); ACPI
identity-mapped its tables "far below the user base", true on i386 and false on
x86_64 where they sit at the top of low RAM, so `fork()` read ACPI memory; and
i386 ring 3 could not execute SSE (`CR4.OSFXSR` deliberately clear — fine while
every binary was ours, fatal for a ported library).  **i386's identity map now
runs to 1 GiB** (it stopped at 256 MiB while user space starts at 1 GiB — three
quarters of the window unused): 234 → 473 MiB on a 512 MiB box.  Past that the
limit is REAL: 32-bit paging = 32-bit physical addresses, and **64 GiB on i386
is exactly the PAE maximum — a different page-table format, not a bigger
constant.**  **NetSurf is now usable**: the compositor had NO mouse-button event
at all (`{KEY, MOTION}`), so a click arrived as motion and nothing in any client
was ever clickable; typing forwarded raw scancodes, which libnsfb reads as ASCII;
and there was no http fetcher compiled in.  `user/netsurf/fetch_dos.c` attaches
through `fetcher_init`'s own `WITH_CURL` hook (vendored tree untouched) over
ring-3 sockets + Mbed TLS with CA + hostname verification.  Transport lessons:
**`send`/`recv` do not work on a connected TCP socket** (the Linux-ABI layer
wires `connect`/`read`/`write`), **never wait for EOF** (the FIN is not surfaced
as `read()==0` — take the length from `Content-Length`), and **the read must stay
BLOCKING** (RX is polled from the calling task, so the blocking read is what
drives the NIC).  **`run_qemu.sh` had no NIC** — every network test passed its
own `-netdev`, so the automated path and the path a person uses were not the same
path.  **Mesa/EGL runs on i386** (`egltri win`, softpipe, GLES 3.1); the blob
block was x86_64-only by where ten Makefile lines lived, not by anything in the
code.  New: `scripts/build-mesa.sh` (arch-parametric), `PLAN_AARCH64.md`.
Open: i386 kmap/PAE, a non-blocking fetcher `poll`.

✅ **§M47 — CRASH RECORDS & REPORTING (2026-08-02, DOCS §4.38, all 3 arches).**
M46 stopped the box from dying; M47 makes sure that when something *does* go
wrong the system SAYS SO.  Two phases on purpose: **capture** (`crash_report`)
runs in fault/NMI context so it only copies a fixed record into a static ring —
no locks, no alloc, no I/O; **delivery** (`crash_drain`, watchdog task) runs
ordinary so a sink may allocate, block or draw.  New destinations register with
`CRASH_SINK()` — **a reporting mechanism can be armed at any time WITHOUT
touching a fault path again.**  Sinks: `klog` (always) + `gui-report` (the Crash
Reports window, `gui/apps/crashapp.c`, gated by `crash.report`, opens itself when
a record is delivered).  Surfaces: `crash`, **`/proc/crash`**, klog, the GUI
window — one record, several views; the window is never the storage.  The one
event nothing in the guest can log (triple fault / reset / power loss) is
reported on the NEXT boot from a CMOS NVRAM marker + a **40-byte checksummed
breadcrumb** (kind/cpu/pid/pc/addr/code/uptime/comm).  Also: taskbar clock shows
the ISO date + keyboard layout, wallpaper label carries the arch
(`d-os M47  x32`/`x64`/`arm64`).

✅ **§M47.2 — THE RING-3 POINTER GATE WAS NEVER ARMED FOR THE LINUX ABI
(2026-08-03, DOCS §4.41).**  `linux_syscall_dispatch` never set
`task->in_user_syscall` on EITHER arch, so §M46's first boundary layer (the
per-syscall pointer gate) was off for **every** musl program — coreutils, sh,
TLS, NetSurf, Wayland.  Nothing failed visibly, which is why it survived two
milestones.  Now armed on both; the `_k`/`_u` discipline covers the places that
legitimately pass kernel buffers (new `sys_recv_u`).

✅ **§M40 STAGE 9 — REAL musl PTHREADS (2026-08-03, DOCS §4.40).**  `clone` was
the last hard `-ENOSYS` in the Linux ABI and it blocked every toolkit AND Mesa.
`proc_clone_thread()` = `proc_fork` but the child SHARES the address space,
resumes on the caller's stack at the SAME instruction with rax/eax = 0 (musl's
`__clone` pre-lays fn+arg there), and installs the caller's thread pointer.
**`pthread_join` needs the kernel**: `CLONE_CHILD_CLEARTID` — `task_exit_code`
zeroes the tid word and futex-wakes before marking DEAD, else a program prints
everything and hangs in join.  **i386 traps:** arg order is (flags, stack, ptid,
TLS, ctid) — TLS BEFORE ctid, opposite of amd64; TLS is a per-CPU GDT descriptor
so the thread must be CPU-pinned and entered with the TLS selector in `%gs`
(else musl faults at `%gs:0x10`); and musl passes a `struct user_desc*`, not a
raw base.  Shell: `pthreadtest`.

✅ **§M40 STAGES 7–8 (2026-08-03, DOCS §4.40).**  **Focus**: `wl_pointer.enter`
/ `wl_keyboard.enter` + `modifiers` + `wl_pointer.frame` — a real client IGNORES
input that arrives without a preceding enter.  (libwayland caught a malformed
`modifiers` we had sized 24 bytes instead of 28 — "message too short" — the kind
of thing a hand-written test client never notices.)  **Keymap**: an xkb keymap
GENERATED from d-os's live layout (`keymap_active()`), passed as a memfd over
SCM_RIGHTS; keycodes = d-os scancode + 8, text self-contained (no `include`).
Verified with a REAL xkb compiler (`xkbcli compile-keymap --from-xkb`: 297 lines,
zero diagnostics; exit codes are inverted in 1.4.0, so the output is the signal —
establish it with a known-bad control).  Chain: scancode 4 → key 4 → xkb 12 →
keysym a/A.  Shell: `waykeymap`.

✅ **§M40 STAGE 6 — AN UNMODIFIED UPSTREAM WAYLAND APP RUNS (2026-08-03, DOCS
§4.40).**  `weston-simple-shm` (weston's own reference client, compiled exactly
as it sits in its tree — nothing patched) animates continuously in a real d-os
window on both x86 arches; 716 frames in one i386 run.  `config.h` sets
HAVE_MEMFD_CREATE because d-os has no writable XDG_RUNTIME_DIR for the mkostemp
fallback.  Accepted two more requests: `xdg_toplevel.set_app_id` and
`wl_shm_pool.destroy` (**buffers outlive the pool** — simple-shm destroys its
pool right after creating its buffers, so releasing the frames there would pull
the pixels out from under a live window).  **Build trap:** never put
`-I<weston>/shared` on the include path — weston has its own `shared/signal.h`
and it shadows the C library's.  Shell: `simpleshm [win]`.

✅ **§M40 STAGE 5 (2026-08-03, DOCS §4.40).**  The two globals a real toolkit
REFUSES to start without: **`wl_output`** (SDL/GTK/Qt all need the size+scale
before laying out; full geometry/mode/scale/done burst, real framebuffer size)
and **`wl_surface.frame`** (a render loop BLOCKS on the callback — ignoring it
stops the app drawing entirely; answered on the next commit, then the callback
object is deleted).  Verified on both arches; the client's frame loop is
self-sustaining (~130 commits/run).

✅ **§M40 STAGE 4 (2026-08-03, DOCS §4.40).**  REAL desktop input reaches the
upstream client's `wl_seat` (pointer motion + keys, both arches).  Three gaps,
all hidden by §M26's demo synthesising input instead of taking it from the
desktop: (1) nobody drained a hook-backed window's queue — a Wayland window's
"host" is the server task (blocked on its socket) and a dosgui window has
`host_task` cleared, so the COMPOSITOR now pumps any window with an
`input_hook`; **this also fixed NetSurf's input, broken the same way**; (2)
pointer motion was only delivered on click (fine for widgets, useless for a
client); (3) only nav/Ctrl-letter keycodes were forwarded.  Client-side lesson:
fill listener structs COMPLETELY — libwayland calls whatever arrives and a NULL
slot is a jump to zero.

✅ **§M40 STAGE 3 (2026-08-03, DOCS §4.40).**  `wayupstream win` → the upstream
client's `xdg_toplevel` IS a desktop window (title bar + taskbar button + its
pixels as contents), verified by screenshot on both arches.  The window is now
created at the FIRST COMMIT WITH CONTENT (our configure says 0×0 = "you pick",
so the size is unknown at `get_toplevel`) and sized via the new
`gui_window_outer_for_content()`; the title arrives before any content, so
`wl_conn.title` holds it until the window exists.

✅ **§M40 STAGE 2 (2026-08-03, DOCS §4.40, i386 + x86_64).**  Upstream libwayland
drives a REAL `xdg_toplevel` + shm buffer; the server reads the client's pixels
(`top-left=ff102040`).  Added: SCM_RIGHTS in both ABI control paths,
`memfd_create`/`ftruncate` (`sys_memfd_resize`/`shm_grow`), memfd mapping from
the Linux `mmap` path, `wl_surface.damage`, real `F_DUPFD`/`F_DUPFD_CLOEXEC`.
**Lesson:** libwayland DUPS every fd it sends; our `fcntl` "succeeded" with 0,
and 0 IS a valid descriptor — the pool silently carried fd 0.  An unimplemented
command that should yield a descriptor must fail loudly.  `sys_dupfd` must skip
0–2 (console-reserved, absent from the fd table).

✅ **§M40 STAGE 1 — UPSTREAM libwayland-client RUNS (2026-08-03, DOCS §4.40,
i386 + x86_64).**  The REAL library (not §M26's mini `user/libwl`) does
connect + get_registry + listener + roundtrip against the d-os server; all 4
globals arrive through libwayland's own libffi closure dispatch.  `make [ARCH=…]
wayland` cross-builds libffi + libwayland-client; `wayland-scanner` runs on the
HOST (in the image) and nothing generated is committed; the vendored tree stays
pristine (build runs in a /tmp copy because wayland `#include "../config.h"`).
Client connects via **`WAYLAND_SOCKET`** (upstream's already-connected-fd
mechanism — no named unix socket needed), which added `proc_set_exec_env()` /
`task.exec_extra_env`: ONE `KEY=VALUE` for the next exec, consumed by it.
**Bug found:** both Linux-ABI layers did recvmsg/sendmsg for AF_INET only, so
libwayland's first read on the UNIX socket returned a bare -1 → musl reported
EPERM; both now route by `sys_fd_kind()`.  Shell: `wayupstream`.  Open: SCM_RIGHTS
in the ABI control path (wl_shm pools), an upstream-driven xdg_toplevel, then a
real toolkit.

✅ **CLOSING A WINDOW IS NOT A CRASH (2026-08-03, DOCS §4.38.1).**  The X button
force-killed a client-managed window's client on the FIRST compositor pass, so
closing a healthy NetSurf was recorded as "unresponsive task reclaimed by force"
and popped the Crash Reports window.  The kill is the FALLBACK for a wedged
client, and the escalation is the USER's: **1st X click = ask, 2nd X click =
force immediately**.  `gui.close_grace_ms` (10000 ms) is only the unattended
backstop.
New `wedgewin` cmd + `user/wedgewin.c` (a client that opens a window then
freezes) is the automated test M46's "chrome works when the app is frozen"
guarantee never had.  **Harness lesson:** step the QEMU mouse in <=100 px hops —
the PS/2 delta is a signed byte, so one big `mouse_move` is clamped and the click
lands elsewhere (the first repro attempt clicked the page and wrongly concluded
there was no bug).

✅ **x86_64 USERLAND PARITY (2026-08-02, DOCS §4.39).**  x86_64 now runs the same
userland i386 does: musl coreutils + `sh`, ring-3 sockets, threads/TLS/signals,
Mbed TLS (crypto + TLSv1.3 + HTTPS w/ CA verify + `wget`), and on-device TinyCC.
The gap was **duplication, not missing kernel support**: objcopy blob symbols
carried the arch in their NAME (`_binary_user_X_i386_elf_start`) so every in-tree
program was i386-only by construction; the program lists were written twice; the
x86_64 dispatcher stopped at M25.  Fixed with one blob pattern rule +
`--redefine-sym`, shared lists above the arch branches, the missing dispatcher
cases and `hal/x86_64/signal.c`.  **Four 32-bit assumptions** surfaced: crt0
never read argc/argv; `thread_create` passed its arg the cdecl way (amd64 wants
RDI); `tls_load4` hard-coded `%gs` AND a literal offset 4 (x86_64 = FS.base, and
the field sits at 8); virtio-net + AC97 were absent from the x86_64 source list.
`make mbedtls` / `make tcc` are arch-aware (`third_party/{mbedtls,tinycc}-<arch>`).
**Pre-existing i386 bug fixed on the way:** `linux_socketcall` validated its
argument array as a USER pointer, but the direct socket syscalls (359+) hand it a
KERNEL array → every direct call -EFAULT and musl doesn't fall back on -EFAULT,
so `socket()` failed.  Split into `linux_socketcall_k` + a gated wrapper.
**musl `getaddrinfo` FIXED (2026-08-03, both arches, was pre-existing).**  Two
defects: (1) `SOCK_NONBLOCK` was discarded — musl drains its resolver socket with
`while (recvmsg(...) >= 0)` and needs the EAGAIN only a non-blocking socket
gives, so we sat in a 40M-iteration spin (minutes on emulated i386 → looked
arch-specific); sockets now carry `nonblock`, honoured by the recv paths and by
`SOCK_NONBLOCK` + `fcntl(F_SETFL)` in both Linux-ABI layers.  (2)
`hostorder_to_sockaddr` validated a KERNEL word (`msg_namelen` from an
already-checked msghdr) as a ring-3 pointer → returned without writing
`msg_name`, and **musl drops any DNS reply whose source doesn't match a queried
nameserver**, so every answer was silently discarded.  **Third instance of the
same lesson** (after `sys_*_k` and `linux_sendmsg`): the user-pointer check
belongs where the pointer's ORIGIN is known, never in a shared helper.

✅ **§M46 — RESILIENCE / freeze-freeness (2026-08-01, DOCS §4.37, i386 + x86_64,
aarch64 parity).**  The rule now enforced: *nothing a user program does can take
the machine down.*  Ring-3 fault ⇒ kill only that process (all 3 arches; ring-0
follows `kernel.fault_policy` halt|reboot|kill).  **Force-kill of a WEDGED ring-3
task** at the timer-preemption safe point (`fkill`, Task Manager "Force kill",
opt-in `package[.<name>].auto_fkill_ms` runaway auto-kill).  **NMI hard-lockup**
recovery via the ib700 HW watchdog (logs the stuck EIP lock-free to COM1, kills
a ring-3 lockup, reboots a kernel one) + spinlock-deadlock reporting +
`scripts/dos-dump.sh` for a frozen guest.  **Chrome works while an app is
frozen:** Ctrl+Alt+Del / Ctrl+Alt+X trapped in the keyboard IRQ, window X
force-kills an unresponsive client.  **Ring-3 pointer boundary in THREE layers:**
a per-syscall gate (`task->in_user_syscall` + `vmm_user_access_ok`), a real
exception table (`.ex_table` + `uaccess_*` — a fault DURING a copy returns
-EFAULT instead of panicking), and **bounce buffers** (2026-08-01) so the bulk
payloads never reach the VFS/socket/console layers as ring-3 pointers — those
dereference deep inside their own call chains where no fixup entry covers them
(`sys_*_k` cores + a gated staging wrapper); `faulttest` proves all three.
Also: dosgui handles
owner-bound + blits range-checked, sigreturn EFLAGS sanitised, `sys_kill`
restricted to the caller's subtree, **x86_64 real COW**, ACPI tables above the
identity map mapped on demand (i386 boots with `-m 512M` again).
**Two lessons worth keeping:** (1) the `sys_*` layer is DUAL-USE (ring-3
dispatchers *and* in-kernel callers) — putting the user-pointer check inside it
broke every kernel caller, so ld.so's `fstat` of each `.so` failed and NetSurf
stopped starting on both x86 arches; a check belongs where the pointer's ORIGIN
is known (`in_user_syscall` + `sys_*_k` cores).  (2) A validity CHECK is not a
guarantee — only the exception table survives a range going bad mid-copy.
Build: `scripts/build.sh` now keeps a **per-arch `user/` artifact cache**
(`build/.userartifacts/<arch>/`), so an ARCH flip no longer re-compiles the
NetSurf + freetype stack (~25 min → instant).

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
VMM_COW through the COW branch).  **Two-brothers SEAM PROVEN with a native
backend:** `pkg_run` logs the backend; `pkgrun hello` (in-tree d-os libc,
`abi=native`) → native syscall path, `pkgrun echo` (musl, `abi=linux`) →
linux_abi — same store, two real backends by data.  The minimal 2nd brother =
the in-tree native libc; the **full native musl (`arch/dos` fork) is PARKED**
(`NATIVE_LIBC.md`) — it needs musl `src/` shape patches (bare-base SYS_SET_TLS,
`(len,fd)` mmap, `kstat`), not a clean `arch/` add → a separate project.
**Checklist in `third_party/MUSL.md`.**  **§M26 Wayland STARTED — stage 1+2 (DOCS
§4.32, i386+x86_64, `kernel/gui/wayland.c`, shell `waytest`; hand-marshalled
client à la linuxhello): stage 1 = real wire protocol + wl_display/wl_registry/
wl_callback handshake; stage 2 = the SHM BUFFER PATH — bind + wl_shm(formats) +
create_surface + create_pool (client memfd passed OUT-OF-BAND via SCM_RIGHTS) +
create_buffer + attach + commit → the server reads the client's pixels back
(4×4 0x3366CCFF → top-left+checksum verified); stage 3 = xdg_shell top-level
(bind xdg_wm_base → get_xdg_surface → get_toplevel → configure pair → set_title
→ ack_configure).**  Also this session: **interactive `sh`** (cooked stdin via
`vc_focused`/`vc_getchar` — `pkgrun sh` → `d-os$` REPL) and **x86_64 build parity
restored** (trampoline stubs + net/audio/futex/pkg cores; Wayland runs on x86_64
too).  **§M26 CORE COMPLETE (i386+x86_64): wire handshake + shm buffers
(SCM_RIGHTS) + xdg_shell + framebuffer bridge (`waydemo` VISIBLE OK) + a
WM-managed `gui_window` target (`gui_window_blit`; `waywin` IN-WINDOW OK) +
`wl_seat` input (`wl_send_key`/`wl_send_motion`; `wayinput`) + a REAL ring-3
client (`user/wlclient.c` speaks the wire protocol over an inherited fd 3, server
runs on its own `wl_conn_serve` task; `wayclient` parses 4 globals from user
space) + **server-per-surface** (`wl_conn.wm_mode`: `xdg get_toplevel` spawns a
`gui_window`, commits fill it, input routed to the client's wl_seat via
`gui_window_set_input_hook`; `waycomp` = SURFACE-IN-WINDOW OK + key/motion) + a
**mini-libwayland client library** (`user/libwl` + `user/wlapp.c`; `wayapp`).
The UPSTREAM libwayland port landed as **§M40, now COMPLETE** (DOCS §4.40 +
§4.40.1, x86_64): upstream libwayland-client cross-built for musl runs
`weston-simple-shm` UNMODIFIED in a d-os window, and **Mesa EGL + GLES2 on
gallium softpipe** spins a shader-drawn triangle presented through `wl_shm`
(`egltri win`).  Two lessons: libwayland must be a SHARED object (libEGL had
absorbed it statically — two protocol object tables in one process crash the
first event dispatch), and **`mincore` must answer truthfully** — stubbed as
"succeed and ignore" alongside `madvise`, it told Mesa every address was mapped,
so Mesa dereferenced the literal 3 stored in a version-3 `wl_egl_window`.**  The
desktop label is now dynamic (`kernel/includes/version.h` `DOS_MILESTONE` — bump
it when a milestone ships).  Also open: more coreutils, tty line-editing/`isatty`.
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
  - Bump `DOS_MILESTONE` in `kernel/includes/version.h` to the new M number
    (the desktop wallpaper draws it — always show the latest shipped milestone).
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
