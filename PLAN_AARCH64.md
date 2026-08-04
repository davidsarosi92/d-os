# PLAN_AARCH64.md — bringing the ARM64 port back to parity

> Separate from PLAN.md on purpose.  The x86 roadmap is organised by
> *milestone*; this one is organised by *gap*, because aarch64 is not behind on
> a feature it never started — it is behind on features that shipped for x86
> after M21 declared parity and were never carried across.

**Measured 2026-08-04**, by diffing the HAL trees and the per-arch build lists
rather than by reading the changelog.

---

## 1. Where the port actually stands

M21 (2026-07-10) reached **full x86 parity** — boot, SMP via PSCI, GICv2,
virtio-MMIO block, exFAT, DTB, framebuffer, EL0 userspace, the full `shell.c`,
the M22 GUI with keyboard and mouse, and USB (xHCI over PCIe ECAM).  That was
true when it was written.  Everything below landed on x86 afterwards.

The port has NOT rotted: it still builds clean (verified this session, after
the §M48 memory rework touched its VMM and syscall paths) and its GUI, block
and USB stacks are real.  The gap is **userland**, and it is one gap with many
symptoms.

### The single blocking gap

`kernel/hal/aarch64/` has no `linux_abi.c`, no `fork.c`, no `signal.c`.

Concretely, comparing symbol coverage:

| capability | x86_64 | aarch64 |
|---|---|---|
| `linux_syscall_dispatch` (Linux ABI personality) | yes | **absent** |
| `proc_fork` / `vmm_space_clone` / `vmm_cow_fault` | yes | **absent** |
| `proc_clone_thread` (pthreads) | yes | **absent** |
| signal delivery + sigreturn | yes | **absent** |

And the build's entire user-program list for aarch64 is:

```
ARCH_EXTRA_OBJS := user/hello_blob.o user/spin_blob.o user/wedge_blob.o
```

Three in-tree-libc programs.  x86 embeds ~60, including musl, the coreutils,
`sh`, ld.so, Mbed TLS, TinyCC, libwayland, NetSurf and Mesa.

That list is not a coincidence: **without the Linux-ABI personality no musl
binary can run at all**, and every port since §M36 is a musl binary.  So a
single missing translation layer accounts for the absence of coreutils, the
shell, dynamic linking, TLS, threads, the browser and the GL stack
simultaneously.  Nothing downstream can be attempted before it exists.

### What IS shared and already works

The arch-independent core is genuinely arch-independent, which is why the gap
is narrow rather than pervasive.  aarch64 already compiles and runs the stock
`crash.c` and `watchdog.c` (M46/M47), `net.c`, `audio.c`, `futex.c`, `pkg.c`,
the VFS, the block cache, exFAT, procfs, the scheduler, the GUI and the
compositor.  `exceptions.c` already calls `task_force_kill_point()`, so §M46's
force-kill safe point is wired.

---

## 2. Roadmap

Ordered by dependency, not by appeal.  Each stage is independently
verifiable — do not start one before its predecessor boots.

### A1 — COW fork + signals  *(prerequisite for everything)*
`vmm_space_clone` + `vmm_cow_fault` for the 4-level AArch64 translation
tables, and signal delivery/sigreturn on the EL0 exception frame.
Mirror `hal/x86_64/fork.c` and `signal.c`; the portable halves in `proc.c`
already exist and are arch-neutral.
**Note the §M48 lesson:** size the COW refcount table from `pmm_nr_frames`
(as both x86 ports now do).  A fixed window is a double free waiting for the
day the machine has more RAM than the constant assumed.
*Done when:* `forktest`, `sigtest`, `pipetest` pass over the serial shell.

### A2 — the Linux ABI personality
`hal/aarch64/linux_abi.c`: AArch64 Linux syscall numbers (`svc #0`, x8 =
number, x0–x5 = args) onto the same `sys_*` cores the x86 layers call.
This is the load-bearing stage; budget accordingly.
**Arm the pointer gate from the start** — `task->in_user_syscall` was left
unset on BOTH x86 layers for two milestones (§M47.2) precisely because
nothing failed visibly without it.
*Done when:* `musltest` runs an unmodified static musl binary.

### A3 — musl + the store
Cross-build musl for `aarch64-linux-musl`, then the coreutils and `sh` as
§M35.5 store packages.  The recipes are already arch-parameterised; the
work is toolchain provisioning, not new code.
*Done when:* `pkgrun sh -c "echo a; ls /store"` forks and execs.

### A4 — threads, TLS, dynamic linking
`proc_clone_thread` + `CLONE_CHILD_CLEARTID`, TLS via `TPIDR_EL0` (much
simpler than i386's per-CPU GDT descriptor), then ld.so.
*Done when:* `pthreadtest` and `solibtest` pass.

### A5 — the ported stack
Mbed TLS → zlib/png/freetype → libwayland → NetSurf.  No new kernel work is
expected here; if any is needed, that is a finding worth recording.

### A6 — Mesa / EGL
`ARCH=aarch64 ./scripts/build-mesa.sh` (the script takes an arch; only the
cross file is missing).  Watch for the aarch64 analogue of the SSE trap:
ported libraries use whatever the toolchain emits, and the kernel must have
enabled it — on i386 that was CR4.OSFXSR, here it is FP/SIMD trapping via
`CPACR_EL1`.  Note `USER_CFLAGS` for aarch64 currently carries
`-mgeneral-regs-only`, which is fine for the in-tree libc and impossible for
a ported library.

### A7 — hardware breadth
virtio-net on virtio-MMIO (`net.c` already builds; only the transport glue is
missing), then audio.  Lowest priority: it blocks nothing else.

---

## 3. Two things worth deciding before starting

**The §M48 user-region collision — measured, not assumed.**  aarch64's
`vmm_user_base()` is `0x1_0000_0000` (4 GiB) and `hal_extend_identity_map`
covers exactly `[0, 4 GiB)`.  The two are **adjacent, not overlapping**, so the
port does not have x86_64's bug (where the identity map's 1 GiB page landed on
top of a user base at 1 GiB and every `exec` returned `ELF_ENOMEM`).

But adjacency is a ceiling, not a design: aarch64 can never manage more than
4 GiB of RAM, because the next byte of physical memory would have to be mapped
where user space begins.  The fix is the one x86_64 already took — a kernel
direct map in the upper half.  `phys_to_virt`/`virt_to_phys` are already
portable and compile to nothing while `KERNEL_DIRECT_MAP_BASE` is 0, so the
change is: define the base for aarch64, map physical memory there in `mmu.c`,
and stop growing the low identity map.  Not urgent — but it is the same work,
and doing it before A5 costs less than doing it after.

**Where does the aarch64 build diverge structurally?**  It runs its own
`main_entry.c` rather than the x86-coupled `kernel_main`, and builds from
`Dockerfile.aarch64`.  That divergence is what let the port drift quietly:
a feature added to `kernel_main` reaches two arches, not three.  Converging
the entry paths is not required by any stage above, but every stage will be
cheaper if it happens first.
