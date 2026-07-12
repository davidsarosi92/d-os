# d-os native libc — PARKED design debate (do NOT start yet)

> **Status: PARKED.** This is a captured design discussion, not an active
> milestone. Revisit *after* §M36 (the "two brothers": Linux-ABI + native
> musl-fork) and ideally after §M37 (dynamic linking), when we can judge it
> against a working ecosystem rather than in the abstract. Nothing here is
> scheduled. It exists so the argument is not lost.

## Update (2026-07-12): the two-brothers SEAM is proven; the full native musl is the parked project

The `abi_to_personality` seam now routes `pkgrun` to **two real ABI backends**
by the package's declared `.abi`, visibly:

```
pkgrun: hello  [abi=native → d-os native backend]   # in-tree d-os libc, native syscall.c
pkgrun: echo   [abi=linux  → linux-abi backend]     # musl, via linux_abi translator
```

So the *minimal* "second brother" exists today: the small **in-tree d-os libc**
(`user/libc.c`) IS a native-ABI libc, and it runs through the native syscall
path with `linux_abi` bypassed. What is PARKED (this doc) is a **full, complete
libc on the native ABI** — and the concrete shape it would take, a musl
`arch/dos` fork, has a precisely-identified blocker set:

- **The native d-os ABI is a different SHAPE, not just different numbers.**
  `SYS_SET_TLS`(29) takes a bare base (not a Linux `user_desc`); `SYS_MMAP`(7)
  is `(len, fd)` (not the 6-arg Linux `mmap`); `SYS_STAT`(30) fills `kstat`
  (not Linux `struct stat`).  So a native musl is NOT "pristine musl with a
  renumbered `bits/syscall.h`" — it needs musl `src/` patches (chiefly
  `src/thread/i386/__set_thread_area.s` → `SYS_SET_TLS`, plus stat/mmap
  wrappers), i.e. a genuine fork, not a clean `arch/` addition.
- That is exactly why it is the "other project": low functional value (it does
  what the Linux-ABI brother already does, with different numbers) and real
  fork-maintenance cost.  Its worth is architectural (a 2nd real backend) —
  already delivered in minimal form by the in-tree libc above.

When the full native libc is pursued, this is the starting point: either
(a) fork musl with `arch/dos` + the `src/` shape patches, or (b) grow a
non-musl native libc (newlib/picolibc/own) — see below.

## The question that produced this doc

"Should d-os have its *own* libc (a 'saját musl') instead of leaning on musl?"

The honest conclusion below is: **not as an ecosystem libc — that is the
classic hobby-OS graveyard — but yes, eventually, as a small, d-os-idiomatic
native libc that exposes what makes d-os d-os.** Two very different things.

## A libc has two roles — and "ours" is right for exactly one

| | **Role A — libc for *our own* native software** | **Role B — libc for the *ecosystem*** |
|---|---|---|
| Clients | d-os shell, native GUI apps, our own tools | ported packages, binary blobs |
| Bar to clear | "enough for our things" | "decades of musl bug-fixes, correct FP" |
| API freedom | we can add d-os-native surface | fixed C/POSIX spec, zero room to innovate |
| Verdict | **ours, small — already exists (`user/` libc)** | **musl (pristine / forked) — never reinvent** |

The trap to avoid: letting Role A's "this is ours" pride expand into Role B's
"so let's make ours complete enough for everything." Role B's bar is
unreachable (correct `strtod`/`printf %f`/`libm`, pthreads + cancellation +
signal interaction, malloc that survives real load, a dynamic linker, an
endless edge-case tail), the API is frozen so you cannot even differentiate,
and every hour spent there starves what is actually d-os's soul — the kernel
architecture (registry-everything, service bus, execution domains,
content-addressed store, the ring-0/3 argument). **d-os's identity was never
supposed to live in the libc/ABI; it lives in how the kernel is organized.**
A libc speaking POSIX is a novelist writing in English: the language is
shared, the work is yours.

## Why the ecosystem path does NOT need our own libc

Settled in §M36 (see `third_party/MUSL.md`, DOCS.md §4.31): Role B is covered
by **two peer "brothers"**, both bottoming out in the *same* d-os kernel
primitives — they differ only in which front door (syscall ABI) the libc
speaks:

- **Linux-ABI peer** — pristine (vendored, unmodified) musl → Linux syscall
  numbers → `kernel/hal/x86/linux_abi.c` translates → d-os primitives. For
  binary blobs + packages too painful to port cleanly. A quarantined guest
  subsystem (NT-POSIX-subsystem / FreeBSD-Linuxulator shape).
- **Native musl-fork peer** — a light `arch/dos/` fork of musl → d-os's own
  syscall numbers → native `syscall.c` → same primitives. The store's default
  libc. Ours at the *numbering / ABI-surface* level (honest caveat: it still
  inherits musl's Linux-shaped *semantics* — that is the price of using musl at
  all; only a genuinely-own libc escapes it, see below).

So neither role forces us to write a full libc.

## The version of "our own libc" that IS worth it (some day)

The only version worth funding is a **small, d-os-idiomatic native libc** that
does NOT try to be a complete POSIX libc, and therefore stays small and truly
ours. Its value comes not from re-implementing stdio (a losing race against
musl) but from making **first-class C APIs out of what musl structurally can
never expose**:

- capability handles,
- the service bus (`bus_connect` / contract calls as native wrappers),
- the content-addressed store (paths, profiles),
- execution domains.

Rule of thumb: **do not compete with musl on the commodity surface (stdio /
libm / locale) — there you lose; differentiate on the d-os surface — there you
win, because musl cannot follow.** Keep the commodity bulk minimal (or delegate
it), and let this libc's worth be the d-os-native API. This is the `user/`
in-tree libc grown deliberately in the d-os direction, not a musl clone.

## The one legitimate exception: pedagogy as the *goal*

d-os is a teaching project. "I want to write a libc *because writing a libc is
the lesson*" is a legitimate goal. But then fund it from the "I want to have
written a libc" budget, not the "run real software" budget — and know it is a
long road that mostly duplicates musl. It is a destination, not a path to the
ecosystem. Keep the two accounts separate.

## When to un-park

Revisit this doc when at least one of these holds:

1. §M36 + §M37 are done and the ecosystem (musl-linked) actually runs, so the
   native libc can be judged as *addition*, not *substitute*.
2. We have concrete d-os-native APIs (bus/store/domains/capabilities) that
   native apps repeatedly want from C, i.e. Role A has real, recurring demand.
3. Someone explicitly wants the pedagogy track and is willing to budget it as
   its own goal.

Until then: `user/` libc for Role A (grow modestly, d-os-idiomatic), the two
brothers for Role B. **Do not start a full own-libc.**

## See also

- `third_party/MUSL.md` — the two-brothers plan + Linux-ABI checklist.
- DOCS.md §4.31 — Linux i386 syscall-ABI compat layer (the Linux-ABI peer).
- DOCS.md §4.30 — POSIX syscall breadth + the in-tree `user/` libc (Role A base).
- CLAUDE.md convention #6 — "Linux-inspired, not Linux-bound."
