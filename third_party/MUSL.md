# Porting musl to d-os — the modular plan (§M36 stage 2)

**Principle: keep musl pristine.**  musl is a clean, *unmodified*, pinned
external dependency (fetched by `scripts/fetch-musl.sh` into `third_party/musl`,
gitignored — not committed, not forked).  We do **not** patch musl's syscall
layer to d-os numbers; instead d-os provides the **Linux i386 syscall ABI** musl
already targets (`kernel/hal/x86/linux_abi.c`), selected per-process by the
`task->linux_abi` personality.  This keeps the two worlds — d-os-native programs
and Linux/musl programs — cleanly separated and musl trivially updatable.

This doubles as **§M41** (Linux ABI shim): the same layer runs *any* unmodified
prebuilt Linux i386 binary, not just musl.

## Status

- ✅ **Linux-ABI compat layer + personality** — DONE (DOCS §4.31): an isolated
  Linux i386 syscall translator + `task->linux_abi`; `linuxtest` runs a
  hand-written Linux-ABI program end to end.
- ✅ **Vendor + build musl** — DONE: `scripts/fetch-musl.sh` pins/fetches musl
  v1.2.5; `make musl` builds it static-for-i386 into `third_party/musl-i386/`
  (both gitignored).  Toolchain note: configure with `--target=i386 AR=ar
  RANLIB=ranlib` — the target triple sets `CROSS_COMPILE=i386-`, so overriding
  AR/RANLIB keeps the native multilib tools (CC='gcc -m32').
- ✅ **Grow the Linux ABI to musl's startup set** — DONE: `set_thread_area`
  (TLS → §M35 `%gs` GDT-TLS), a real `auxv` (`AT_PAGESZ`/`AT_CLKTCK`/
  `AT_RANDOM`/`AT_SECURE`), plus `set_tid_address` + `ioctl`→ENOTTY (isatty).
- ✅ **Link + run a musl "hello world"** — DONE: `user/muslhello.c` (a normal
  `#include <stdio.h>` / `printf` program) links statically against musl's
  crt1/crti/libc.a/crtn (a stock Linux i386 ELF, relocated to 0x40000000 via
  `-Ttext-segment`), embedded as a blob, run by the `musltest` shell command
  under the Linux personality.  Prints via real musl `printf` (`%d`+`%s`),
  returns 0, **zero unhandled syscalls**.
- 🔲 **Minimal coreutils** (`sh`/`ls`/`cat`/`echo`/`env`), `pkg install`ed into
  the §M35.5 store — the next step.
- 🔲 **Native musl-fork peer** (`arch/dos/`, d-os syscall numbers) — the twin
  track (store's default libc); own-libc debate parked in `NATIVE_LIBC.md`.

## Build (inside the build container)

```sh
./scripts/fetch-musl.sh                     # once: pin + clone musl v1.2.5
# then, in the container (the `make musl` target does exactly this):
docker run --rm --platform=linux/amd64 -v "$PWD":/src d-os-build make musl
# → third_party/musl-i386/lib/{libc.a, crt1.o, crti.o, crtn.o} + include/
```

`make musl` runs, inside `third_party/musl`:

```sh
CC='gcc -m32' ./configure --target=i386 --disable-shared \
    --prefix=.../third_party/musl-i386 AR=ar RANLIB=ranlib
make -j && make install
```

`AR=ar RANLIB=ranlib` is essential: `--target=i386` sets `CROSS_COMPILE=i386-`,
so without the override musl tries the nonexistent `i386-ar`.

A musl program links (static, relocated to the d-os user base 0x40000000 — use
`-Ttext-segment`, NOT `-Ttext`, so the ELF headers move too and the whole image
is one contiguous span below the user stack at base+1 MiB; add libgcc for the
64-bit `__udivmoddi4` helper musl's `printf` pulls in):

```sh
gcc -m32 -static -fno-pie -c hello.c -Ithird_party/musl-i386/include -o hello.o
ld -m elf_i386 -static -Ttext-segment=0x40000000 -e _start -o hello.elf \
   third_party/musl-i386/lib/crt1.o third_party/musl-i386/lib/crti.o hello.o \
   --start-group third_party/musl-i386/lib/libc.a \
   `gcc -m32 -print-libgcc-file-name` --end-group \
   third_party/musl-i386/lib/crtn.o
```

Run it as an embedded blob under the Linux personality (the `musltest` shell
command does this for `user/muslhello.c`), or — the goal — `pkg install` it into
`/store` and exec from there.

## The Linux ABI musl needs at startup (the checklist to grow linux_abi.c)

musl's `_start → __libc_start_main` is demanding; these must work before a musl
program reaches `main`:

1. **`set_thread_area` (243)** — TLS is *mandatory*; musl aborts without it.
   Translate the Linux `struct user_desc` (base/limit/flags) to the existing
   per-CPU `%gs` GDT-TLS mechanism (`hal_set_tls_base` + `gdt_tls_selector`,
   already built for §M35 TLS).  **The single biggest blocker.**
2. **A proper SysV initial stack with an `auxv`** — at least `AT_PAGESZ`,
   `AT_RANDOM` (16 bytes; musl seeds the stack canary + TLS from it), `AT_NULL`;
   ideally `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_ENTRY` (for dynamic later).
   Extend `proc.c build_initial_stack` (already lays out argc/argv/envp) to add
   auxv when the personality is Linux.
3. **`brk` (45)** or a working **`mmap2` (192)** — musl's malloc needs one; our
   `mmap2`→`sys_mmap` path already works, so `brk` can keep failing.
4. **`rt_sigprocmask` (175)**, **`ioctl` (54)** (musl's `__stdio` `isatty`),
   **`readv` (145)/`writev` (146)** — small, add as musl demands them (the
   dispatcher already logs each unhandled number).
5. **`stat64`/`fstat64` (195/197)** + Linux `struct stat64` layout — for file
   I/O (distinct from d-os's `kstat`; keep the Linux layout isolated here).

Add each as the `linux-abi: unhandled syscall N` log points them out.  Once a
static musl `hello` prints, wire the coreutils and the store install.
