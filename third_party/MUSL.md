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
  hand-written Linux-ABI program (`write`=4/`exit`=1) end to end.
- 🔲 **Vendor + build musl** — `scripts/fetch-musl.sh` fetches it; a `make musl`
  target (below) builds it.
- 🔲 **Grow the Linux ABI to musl's startup + runtime set** (the real work).
- 🔲 **Link + run a musl "hello world"**, then a minimal coreutils, all
  installed into the §M35.5 store.

## Build (inside the build container)

```sh
./scripts/fetch-musl.sh                     # once: pin + clone musl
# then, in the container (extend the Makefile with a `musl` target):
cd third_party/musl
CC='gcc -m32' ./configure --target=i386 --disable-shared \
    --prefix="$PWD/../musl-i386"
make -j && make install
# → third_party/musl-i386/lib/{libc.a, crt1.o, crti.o, crtn.o} + include/
```

A musl program then links (static, freestanding-from-the-host):

```sh
gcc -m32 -c hello.c -Ithird_party/musl-i386/include -o hello.o
ld -m elf_i386 -static -Ttext 0x40000000 -e _start -o hello.elf \
   third_party/musl-i386/lib/crt1.o hello.o \
   --start-group third_party/musl-i386/lib/libc.a --end-group \
   third_party/musl-i386/lib/crtn.o
```

Run it as an embedded blob under the Linux personality (like `linuxtest`), or —
the goal — `pkg install` it into `/store` and exec from there.

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
