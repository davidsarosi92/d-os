/* =============================================================================
 * cmd_test.c — the ring-3 / userland test battery (§M70).
 *
 * Split out of shell.c, where these forty-odd commands were more than a third
 * of the file.  Almost all have the same body — set the Linux personality,
 * exec an embedded blob, restore the personality, print the exit code — and
 * seeing them together is the point: the differences between them are the
 * interesting part, and they were invisible while the identical parts were
 * scattered through four thousand lines.
 *
 * EVERY BLOB SYMBOL IS WEAK.  A tree built without `make musl`, without Mesa
 * or without the NetSurf stack still links, and the command reports "not
 * embedded" instead of failing at the linker — which is what lets one source
 * tree build on three architectures with different subsets of userland
 * available.  That is also why each command CHECKS its pointer before using
 * it: a weak symbol that is absent is NULL, and dereferencing it would turn a
 * missing optional component into a page fault at the prompt.
 * =========================================================================== */

#include "shellcmd.h"
#include "cmd_util.h"
#include "console.h"
#include "printf.h"
#include "task.h"
#include "proc.h"
#include "elf.h"
#include "vfs.h"
#include "kmalloc.h"
#include "fd.h"
#include "config.h"
#include "gui.h"
#include "usermode.h"
#include "drvuser.h"
#include "syscall.h"
#include "hal_api.h"
#include "wayland.h"
#include "block.h"
#include "block_cache.h"
#include "pmm.h"
#include "lock.h"
#include "timer.h"
#include "driver.h"
#include "vmm.h"
#include "random.h"
#include <stdint.h>
#include <stddef.h>

/* Blob symbols for the embedded ring-3 programs.  ALL WEAK: a tree built
 * without the optional userland still links, and the command says "not
 * embedded" instead of the linker failing.  Hence the NULL check in each. */
extern const unsigned char _binary_user_args_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_args_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_forktest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_forktest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_forkexec_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_forkexec_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_pipetest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_pipetest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_sigtest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_sigtest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_dnstest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_dnstest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_httptest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_httptest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_threadtest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_threadtest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_tlstest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_tlstest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_posixtest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_posixtest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_redirtest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_redirtest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_drvtest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_drvtest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_linuxhello_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_linuxhello_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_netmuslserv_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_netmuslserv_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_epollmusl_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_epollmusl_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_muslhello_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_muslhello_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_wedgewin_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_wedgewin_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_pthreadtest_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_pthreadtest_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_muslhellodyn_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_muslhellodyn_dynelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_cpptest_cxxelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_cpptest_cxxelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_crypttest_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_crypttest_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_ssltest_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_ssltest_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_httpstest_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_httpstest_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_netmusl_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_netmusl_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_thrdyn_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_thrdyn_dynelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_solibtest_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_solibtest_dynelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_dlopentest_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_dlopentest_dynelf_end[]   __attribute__((weak));

/* --- file-scope state these commands own (moved with them out of shell.c) --- */
/* Part 1 — task_wait.  The child burns a little CPU (so the parent reaches
 * task_wait and truly BLOCKS before the child exits — exercising the sleep
 * path, not a fast-path pickup), stamps a marker, then exits with code 42. */
static volatile int g_waitkid_marker;

/* Part 2 — blocking socket read across two tasks.  The producer runs on its
 * own task; the shell task is the consumer and does a BLOCKING usock_recv on
 * the empty endpoint, so it parks on the socket's read wait-queue until the
 * producer's send wakes it.  Raw usock_* (not fds) because fd numbers are
 * per-task — the shared object is the endpoint pointer. */
static struct usock*    g_bt_prod_ep;



/* `blktest [dev]` — a sector round trip on ANY registered block device.
 *
 * It was hard-wired to "vda", which was fine while virtio-blk was the only
 * block driver in the tree and became a gap the moment a second one existed:
 * the AHCI driver's whole claim is that it reads and writes a real disk, and
 * the one test that could establish it could not be pointed at it. */
static void cmd_blktest(const char* args) {
    while (args && *args == ' ') args++;
    const char* name = (args && *args) ? args : "vda";
    struct block_device* dev = blk_find(name);
    if (!dev) {
        kprintf("blktest: no block device '%s' (try `lsblk`)\n", name);
        return;
    }
    kprintf("blktest: %s, %u-byte sectors, %u sectors\n",
            name, dev->sector_size, (unsigned)dev->sector_count);

    /* Use PMM-allocated frames as DMA buffers.  A kmalloc'd 512-byte
     * buffer could land at an offset that straddles a virtual page,
     * splitting its physical backing across two non-adjacent frames —
     * fatal for a single-descriptor DMA.  A whole frame is over-
     * allocated for 512 bytes but trivially correct. */
    pmm_phys_t wf = pmm_alloc_frame();
    pmm_phys_t rf = pmm_alloc_frame();
    if (!wf || !rf) {
        console_write("blktest: PMM OOM\n");
        if (wf) pmm_free_frame(wf);
        if (rf) pmm_free_frame(rf);
        return;
    }
    uint8_t* wbuf = (uint8_t*)(uintptr_t)wf;
    uint8_t* rbuf = (uint8_t*)(uintptr_t)rf;

    /* Fill write buffer with a recognizable pattern: 0xA5 0x5A 0xA5 ... */
    for (int i = 0; i < 512; i++) wbuf[i] = (i & 1) ? 0x5A : 0xA5;
    for (int i = 0; i < 512; i++) rbuf[i] = 0x00;

    kprintf("blktest: writing 512 bytes of pattern to sector 1...\n");
    if (blk_write(dev, 1, 1, wbuf) != 0) {
        console_write("blktest: write failed\n");
        goto out;
    }
    kprintf("blktest: reading back...\n");
    if (blk_read(dev, 1, 1, rbuf) != 0) {
        console_write("blktest: read failed\n");
        goto out;
    }

    int ok = 1;
    int first_bad = -1;
    for (int i = 0; i < 512; i++) {
        if (rbuf[i] != wbuf[i]) { ok = 0; first_bad = i; break; }
    }
    if (ok) {
        console_write("blktest: PASS (512 bytes round-tripped)\n");
    } else {
        kprintf("blktest: FAIL — first mismatch at offset %d (wrote %x, got %x)\n",
                first_bad, wbuf[first_bad], rbuf[first_bad]);
    }

out:
    pmm_free_frame(wf);
    pmm_free_frame(rf);
}

static void cmd_bctest(void) {
    struct block_device* dev = blk_find("vda");
    if (!dev) {
        console_write("bctest: /dev/vda not registered (no virtio-blk?)\n");
        return;
    }

    /* First get — expect a miss (or a hit if a previous run cached it). */
    struct bcache_buf* b = bcache_get(dev, 2);
    if (!b) { console_write("bctest: bcache_get sector 2 failed\n"); return; }

    /* Mutate: write 0xC3 0x3C ... pattern and mark dirty. */
    for (uint32_t i = 0; i < dev->sector_size; i++) {
        b->data[i] = (i & 1) ? 0x3C : 0xC3;
    }
    bcache_mark_dirty(b);
    bcache_release(b);

    /* Re-get — should be an instant cache hit on the same slot. */
    struct bcache_buf* b2 = bcache_get(dev, 2);
    if (b2 != b) {
        console_write("bctest: WARN cache returned a different slot on re-get\n");
    }
    int ok = 1;
    for (uint32_t i = 0; i < dev->sector_size; i++) {
        uint8_t want = (i & 1) ? 0x3C : 0xC3;
        if (b2->data[i] != want) { ok = 0; break; }
    }
    bcache_release(b2);
    console_write(ok ? "bctest: in-cache content matches written pattern\n"
                     : "bctest: FAIL — cached content diverges from write\n");

    /* Flush dirty entries to disk so a subsequent reboot sees the pattern. */
    if (bcache_sync(dev) == 0) console_write("bctest: sync OK\n");
    else                       console_write("bctest: sync FAILED\n");

    bcache_print_stats();
}

/* M25 stage 2a — ELF loader self-test.  Synthesises a minimal static ELF of
 * this arch's native class (one PT_LOAD segment carrying a known payload at
 * the user-region base), loads it into a fresh vmm_space via elf_load(), then
 * switches to that space to confirm the segment bytes + entry landed where the
 * program headers said — and that the mapping is PRIVATE (invisible to the
 * kernel space).  This exercises the loader end-to-end without needing a
 * userland toolchain in the tree yet; actually *running* the loaded image in
 * ring 3 is stage 2b. */
static void cmd_elftest(void) {
    const uintptr_t base = vmm_user_base();
    const char payload[] = "ELF-LOAD-OK";           /* the segment's contents */

    static uint8_t image[2 * 4096];                 /* scratch ELF image */
    size_t ilen = elf_build_selftest(image, sizeof image, base,
                                     payload, sizeof payload);
    if (!ilen) { console_write("elftest: build failed\n"); return; }

    struct vmm_space* s = vmm_space_create();
    if (!s) { console_write("elftest: vmm_space_create failed\n"); return; }

    uintptr_t entry = 0;
    int rc = elf_load(s, image, ilen, &entry);
    if (rc != ELF_OK) {
        kprintf("elftest: elf_load failed (%d)\n", rc);
        vmm_space_destroy(s); return;
    }

    /* Switch into the space, read the loaded payload back at its vaddr. */
    char got[sizeof payload];
    spinlock_t lk = SPINLOCK_INIT;
    uint32_t flags = spin_lock_irqsave(&lk);
    vmm_space_switch(s);
    for (size_t i = 0; i < sizeof payload; i++)
        got[i] = ((volatile char*)base)[i];
    vmm_space_switch(NULL);
    spin_unlock_irqrestore(&lk, flags);

    int match = 1;
    for (size_t i = 0; i < sizeof payload; i++)
        if (got[i] != payload[i]) { match = 0; break; }

    uintptr_t kview = vmm_translate(base);          /* kernel-space view */

    kprintf("elftest: loaded, entry=%p (want %p) -> %s; segment='%s' -> %s; "
            "kernel translate(base)=%p -> %s\n",
            (void*)entry, (void*)base, entry == base ? "PASS" : "FAIL",
            got, match ? "PASS" : "FAIL",
            (void*)kview, kview == 0 ? "PASS" : "FAIL");

    vmm_space_destroy(s);
}

/* M25 stage 2b — build the arch's hello program, wrap it in a static ELF, and
 * actually RUN it in ring 3 / EL0 in its own address space via proc_exec_elf.
 * The program SYS_PRINTs a greeting then SYS_EXITs (returning here).  This is
 * the ELF-loader path's payoff: a loaded-from-image user program executing,
 * isolated in a private space — not hand-poked machine code in the shared
 * kernel map (that's the older `ringtest`). */
static void cmd_userrun(void) {
    const uintptr_t base = vmm_user_base();

    static uint8_t payload[512];
    size_t plen = arch_user_hello(payload, sizeof payload, base);
    if (!plen) { console_write("userrun: hello build failed\n"); return; }

    static uint8_t image[3 * 4096];
    size_t ilen = elf_build_selftest(image, sizeof image, base, payload, plen);
    if (!ilen) { console_write("userrun: elf build failed\n"); return; }

    console_write("userrun: exec'ing user ELF...\n");
    int rc = proc_exec_elf(image, ilen);
    kprintf("userrun: returned from user program (rc=%d)\n", rc);
}

/* M25 stage 3 — per-process fd table + open/read/write/close/lseek.  Drives
 * the SAME sys_* handlers the ring-3 syscall dispatchers call: create a ramfs
 * file, then open/read/lseek/close it through the fd layer and echo it via
 * sys_write(1, …).  (userrun already proves the ring-3 → syscall trap; this
 * validates the fd-table semantics directly.) */
static void cmd_fdtest(void) {
    const char* path    = "/fdtest.txt";
    const char* content = "M25 fd table works";
    size_t clen = 0; while (content[clen]) clen++;

    struct file* wf = vfs_open(path, VFS_WRONLY | VFS_CREATE);
    if (!wf) { console_write("fdtest: create failed\n"); return; }
    vfs_write(wf, content, clen);
    vfs_close(wf);

    int fd = sys_open(path, VFS_RDONLY);
    if (fd < 0) { console_write("fdtest: sys_open failed\n"); return; }

    char buf[64];
    long n = sys_read(fd, buf, sizeof buf - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    int read_ok = ((size_t)n == clen);
    for (long i = 0; i < n; i++) if (buf[i] != content[i]) read_ok = 0;

    long pos = sys_lseek(fd, 0, SEEK_SET);          /* rewind */
    char b2[4];
    long n2 = sys_read(fd, b2, 3);
    int seek_ok = (pos == 0 && n2 == 3 && b2[0] == content[0]);

    console_write("fdtest: sys_write(1) echo: ");
    sys_write(1, content, clen);
    console_putchar('\n');

    int close_ok = (sys_close(fd) == 0);
    int reuse    = sys_open(path, VFS_RDONLY);      /* freed slot reused? */
    int reuse_ok = (reuse == fd);
    if (reuse >= 0) sys_close(reuse);

    kprintf("fdtest: open=%d read=%s(%ld) lseek=%s close=%s reuse=%s\n",
            fd, read_ok ? "PASS" : "FAIL", n, seek_ok ? "PASS" : "FAIL",
            close_ok ? "PASS" : "FAIL", reuse_ok ? "PASS" : "FAIL");
}

/* M25 stage 4 — anonymous mmap + memfd shared memory.  Borrows a private
 * address space (like proc_exec_elf) so sys_mmap has a user space to map
 * into, then: (1) mmaps an anonymous region and read/writes it; (2) creates a
 * memfd and mmaps it TWICE — a write through one mapping is visible through
 * the other, proving one backing frame set behind two VAs (the shm-sharing
 * mechanism; cross-process sharing is stage 5).  VMM_SHARED keeps the space
 * teardown from double-freeing the shm frames. */
static void cmd_shmtest(void) {
    struct task* me = task_current();
    struct vmm_space* s = vmm_space_create();
    if (!s) { console_write("shmtest: no space\n"); return; }
    struct vmm_space* prev = me->mm;
    me->mm = s;
    vmm_space_switch(s);

    long a = sys_mmap(8192, -1);                     /* anonymous, 2 pages */
    int anon_ok = 0;
    if (a > 0) {
        volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)a;
        p[0] = 0xABCD1234u; p[1500] = 0x5678u;       /* touch both pages */
        anon_ok = (p[0] == 0xABCD1234u && p[1500] == 0x5678u);
    }

    int  fd = sys_memfd(4096);
    long m1 = (fd >= 0) ? sys_mmap(4096, fd) : -1;
    long m2 = (fd >= 0) ? sys_mmap(4096, fd) : -1;   /* second mapping, same object */
    int shm_ok = 0;
    if (m1 > 0 && m2 > 0 && m1 != m2) {
        *(volatile uint32_t*)(uintptr_t)m1 = 0xFEEDFACEu;
        shm_ok = (*(volatile uint32_t*)(uintptr_t)m2 == 0xFEEDFACEu);
    }
    if (fd >= 0) sys_close(fd);                       /* frees shm frames once */

    vmm_space_switch(prev);
    me->mm = prev;
    vmm_space_destroy(s);                             /* frees anon; skips shm */

    kprintf("shmtest: anon-mmap=%s shm-shared=%s (a=%p m1=%p m2=%p)\n",
            anon_ok ? "PASS" : "FAIL", shm_ok ? "PASS" : "FAIL",
            (void*)(uintptr_t)a, (void*)(uintptr_t)m1, (void*)(uintptr_t)m2);
}

extern const unsigned char _binary_user_hello_elf_start[]    __attribute__((weak));
extern const unsigned char _binary_user_hello_elf_end[]      __attribute__((weak));
extern const unsigned char _binary_user_hello_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_hello_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_hello_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_hello_aarch64_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_spin_elf_start[]    __attribute__((weak));
extern const unsigned char _binary_user_spin_elf_end[]      __attribute__((weak));
extern const unsigned char _binary_user_spin_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_spin_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_spin_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_spin_aarch64_elf_end[]   __attribute__((weak));
/* M25 stage 5 — unix socketpair + fd passing (SCM_RIGHTS).  Creates a
 * connected pair, sends bytes one way and receives them the other, then the
 * payoff: creates a memfd, writes a sentinel through a mapping, PASSES the fd
 * across the socket, and on the receiving side maps the received fd and reads
 * the sentinel back — one shm object reached via a descriptor that travelled
 * over a socket (the Wayland wl_shm / keymap handover).  Borrows a private
 * space so mmap has somewhere to map. */
static void cmd_socktest(void) {
    struct task* me = task_current();
    struct vmm_space* s = vmm_space_create();
    if (!s) { console_write("socktest: no space\n"); return; }
    struct vmm_space* prev = me->mm;
    me->mm = s;
    vmm_space_switch(s);

    int fds[2] = { -1, -1 };
    int sp = sys_socketpair(fds);

    /* byte stream one way */
    long sent = sys_send(fds[0], "ping", 4, -1);
    char rb[8]; int pf = -2;
    long got = sys_recv(fds[1], rb, sizeof rb, &pf);
    int data_ok = (sp == 0 && sent == 4 && got == 4 &&
                   rb[0] == 'p' && rb[3] == 'g' && pf == -1);

    /* fd passing → shared memory across the socket */
    int  fdshm = sys_memfd(4096);
    long m1 = (fdshm >= 0) ? sys_mmap(4096, fdshm) : -1;
    if (m1 > 0) *(volatile uint32_t*)(uintptr_t)m1 = 0xCAFEBABEu;

    long sent2 = sys_send(fds[0], "fd", 2, fdshm);      /* pass the memfd */
    char rb2[8]; int passed = -2;
    long got2 = sys_recv(fds[1], rb2, sizeof rb2, &passed);   /* receive new fd */

    int m2_ok = 0;
    if (passed >= 3) {
        long m2 = sys_mmap(4096, passed);
        if (m2 > 0) m2_ok = (*(volatile uint32_t*)(uintptr_t)m2 == 0xCAFEBABEu);
        sys_close(passed);
    }
    int pass_ok = (sent2 == 2 && got2 == 2 && passed >= 3 && m2_ok);

    if (fdshm >= 0) sys_close(fdshm);
    if (fds[0] >= 0) sys_close(fds[0]);
    if (fds[1] >= 0) sys_close(fds[1]);

    vmm_space_switch(prev);
    me->mm = prev;
    vmm_space_destroy(s);

    kprintf("socktest: pair+data=%s fd-passing(shared mem)=%s (passed fd=%d)\n",
            data_ok ? "PASS" : "FAIL", pass_ok ? "PASS" : "FAIL", passed);
}

static const unsigned char* hello_blob(size_t* len) {
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_hello_elf_start)    { s = _binary_user_hello_elf_start;    e = _binary_user_hello_elf_end; }
    else if (_binary_user_hello_x86_64_elf_start)  { s = _binary_user_hello_x86_64_elf_start;  e = _binary_user_hello_x86_64_elf_end; }
    else if (_binary_user_hello_aarch64_elf_start) { s = _binary_user_hello_aarch64_elf_start; e = _binary_user_hello_aarch64_elf_end; }
    if (!s || !e) return 0;
    *len = (size_t)(e - s);
    return s;
}

/* Pick this arch's embedded spin blob (only one arch's is linked into the
 * image; the other weak symbols resolve to NULL). */
static const unsigned char* spin_blob(size_t* len) {
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_spin_elf_start)    { s = _binary_user_spin_elf_start;    e = _binary_user_spin_elf_end; }
    else if (_binary_user_spin_x86_64_elf_start)  { s = _binary_user_spin_x86_64_elf_start;  e = _binary_user_spin_x86_64_elf_end; }
    else if (_binary_user_spin_aarch64_elf_start) { s = _binary_user_spin_aarch64_elf_start; e = _binary_user_spin_aarch64_elf_end; }
    if (!s || !e) return 0;
    *len = (size_t)(e - s);
    return s;
}

/* M25 stage 6 — poll readiness.  On a socketpair: poll reports NOT-readable
 * before a send, readable after, and NOT-readable again once drained.  (This
 * is the non-blocking readiness snapshot; true sleep-until-ready arrives with
 * the concurrent-process scheduler.) */
static void cmd_polltest(void) {
    int fds[2] = { -1, -1 };
    if (sys_socketpair(fds) != 0) { console_write("polltest: pair failed\n"); return; }

    struct pollfd pf = { .fd = fds[1], .events = POLLIN, .revents = 0 };
    int r_before = sys_poll(&pf, 1, 0);
    int before_ok = (r_before == 0 && !(pf.revents & POLLIN));

    sys_send(fds[0], "x", 1, -1);
    pf.revents = 0;
    int r_after = sys_poll(&pf, 1, 0);
    int after_ok = (r_after == 1 && (pf.revents & POLLIN));

    char c; sys_recv(fds[1], &c, 1, NULL);          /* drain */
    pf.revents = 0;
    int r_drain = sys_poll(&pf, 1, 0);
    int drain_ok = (r_drain == 0);

    sys_close(fds[0]); sys_close(fds[1]);

    kprintf("polltest: before-send=%s after-send=%s after-drain=%s\n",
            before_ok ? "PASS" : "FAIL", after_ok ? "PASS" : "FAIL",
            drain_ok ? "PASS" : "FAIL");
}

static void waitkid_entry(void) {
    for (volatile int i = 0; i < 3000000; i++) { }
    g_waitkid_marker = 0x1234;
    task_exit_code(42);
}

static void blockprod_entry(void) {
    for (volatile int i = 0; i < 4000000; i++) { }   /* let the consumer block first */
    usock_send(g_bt_prod_ep, "PONG", 4, NULL);
    task_exit();
}

static void cmd_waittest(void) {
    /* --- Part 1: task_wait blocks until the child exits, returns its code. */
    g_waitkid_marker = 0;
    struct task* c = task_spawn("waitkid", waitkid_entry);
    if (!c) { console_write("waittest: spawn failed\n"); return; }
    int kpid = c->pid;
    task_set_reap_owned(c, 1);         /* claim the reap so init won't harvest first */
    int code = -1;
    int r = task_wait(kpid, &code);
    int wait_ok = (r == kpid && code == 42 && g_waitkid_marker == 0x1234);

    /* --- Part 2: blocking recv parks until a producer task sends. */
    struct usock *a = NULL, *b = NULL;
    int read_ok = 0, eof_ok = 0;
    if (usock_pair(&a, &b) == 0) {
        g_bt_prod_ep = a;              /* producer sends on a → fills b's ring */
        struct task* p = task_spawn("blockprod", blockprod_entry);
        int ppid = p ? p->pid : -1;
        if (p) task_set_reap_owned(p, 1);

        char rb[8];
        long got = usock_recv(b, rb, sizeof rb, 1, NULL);   /* BLOCKS until send */
        read_ok = (got == 4 && rb[0] == 'P' && rb[1] == 'O' &&
                   rb[2] == 'N' && rb[3] == 'G');

        if (ppid >= 0) task_wait(ppid, NULL);               /* reap the producer */

        /* Blocking recv on a now-empty endpoint whose peer we close returns
         * 0 (EOF) rather than hanging — the close wakes the reader. */
        usock_close(a);                                     /* peer of b closes */
        long eof = usock_recv(b, rb, sizeof rb, 1, NULL);
        eof_ok = (eof == 0);
        usock_close(b);
    }

    kprintf("waittest: task_wait(block+code)=%s blocking-recv=%s peer-close-EOF=%s\n",
            wait_ok ? "PASS" : "FAIL", read_ok ? "PASS" : "FAIL",
            eof_ok ? "PASS" : "FAIL");
}

static void cmd_libctest(void) {
    size_t len = 0;
    const unsigned char* start = hello_blob(&len);
    if (!start) {
        console_write("libctest: no user ELF embedded for this arch\n");
        return;
    }
    console_write("libctest: exec'ing compiled-C user ELF (in-tree libc)...\n");
    int rc = proc_exec_elf(start, len);
    kprintf("libctest: returned (rc=%d, %u bytes)\n", rc, (unsigned)len);
}

static void cmd_procspawn(void) {
    size_t len = 0;
    const unsigned char* start = spin_blob(&len);
    if (!start) {
        console_write("procspawn: no spin ELF embedded for this arch\n");
        return;
    }
    int a = proc_spawn("spin-a", start, len);
    int b = proc_spawn("spin-b", start, len);
    kprintf("procspawn: launched two user processes (pids %d, %d) — watch them interleave\n",
            a, b);
}

static void cmd_runargs(const char* line) {
    if (!_binary_user_args_elf_start) {
        console_write("runargs: args ELF not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_args_elf_end - _binary_user_args_elf_start);

    /* Split `line` into up to 15 whitespace-separated argv strings, in place
     * (a scratch copy).  argv[0] is the program name. */
    static char scratch[256];
    const char* argv[16];
    int argc = 0;
    argv[argc++] = "args";               /* argv[0] */

    int n = 0;
    while (line[n] && n < 255) { scratch[n] = line[n]; n++; }
    scratch[n] = '\0';
    int i = 0;
    while (scratch[i] && argc < 16) {
        while (scratch[i] == ' ') i++;
        if (!scratch[i]) break;
        argv[argc++] = &scratch[i];
        while (scratch[i] && scratch[i] != ' ') i++;
        if (scratch[i]) scratch[i++] = '\0';
    }

    kprintf("runargs: exec'ing args program with %d argv...\n", argc);
    int rc = proc_exec_elf_argv(_binary_user_args_elf_start, len,
                                argc, (const char* const*)argv);
    kprintf("runargs: returned rc=%d\n", rc);
}

static void cmd_forktest(void) {
    if (!_binary_user_forktest_elf_start) {
        console_write("forktest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_forktest_elf_end -
                          _binary_user_forktest_elf_start);
    console_write("forktest: exec'ing fork()+waitpid() program...\n");
    int rc = proc_exec_elf(_binary_user_forktest_elf_start, len);
    kprintf("forktest: returned rc=%d\n", rc);
}

static void cmd_forkexec(void) {
    if (!_binary_user_forkexec_elf_start) {
        console_write("forkexec: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_forkexec_elf_end -
                          _binary_user_forkexec_elf_start);
    console_write("forkexec: exec'ing fork()+execv()+waitpid() program...\n");
    int rc = proc_exec_elf(_binary_user_forkexec_elf_start, len);
    kprintf("forkexec: returned rc=%d\n", rc);
}

static void cmd_pipetest(void) {
    if (!_binary_user_pipetest_elf_start) {
        console_write("pipetest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_pipetest_elf_end -
                          _binary_user_pipetest_elf_start);
    console_write("pipetest: exec'ing pipe()+dup2()+fork() program...\n");
    int rc = proc_exec_elf(_binary_user_pipetest_elf_start, len);
    kprintf("pipetest: returned rc=%d\n", rc);
}

static void cmd_sigtest(void) {
    if (!_binary_user_sigtest_elf_start) {
        console_write("sigtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_sigtest_elf_end -
                          _binary_user_sigtest_elf_start);
    console_write("sigtest: exec'ing signal()+raise() program...\n");
    int rc = proc_exec_elf(_binary_user_sigtest_elf_start, len);
    kprintf("sigtest: returned rc=%d\n", rc);
}

static void cmd_dnstest(void) {
    if (!_binary_user_dnstest_elf_start) {
        console_write("dnstest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_dnstest_elf_end -
                          _binary_user_dnstest_elf_start);
    console_write("dnstest: exec'ing UDP-socket DNS resolver...\n");
    int rc = proc_exec_elf(_binary_user_dnstest_elf_start, len);
    kprintf("dnstest: returned rc=%d\n", rc);
}

static void cmd_httptest(void) {
    if (!_binary_user_httptest_elf_start) {
        console_write("httptest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_httptest_elf_end -
                          _binary_user_httptest_elf_start);
    console_write("httptest: exec'ing TCP-socket HTTP client...\n");
    int rc = proc_exec_elf(_binary_user_httptest_elf_start, len);
    kprintf("httptest: returned rc=%d\n", rc);
}

static void cmd_threadtest(void) {
    if (!_binary_user_threadtest_elf_start) {
        console_write("threadtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_threadtest_elf_end -
                          _binary_user_threadtest_elf_start);
    console_write("threadtest: exec'ing threads + futex-mutex program...\n");
    int rc = proc_exec_elf(_binary_user_threadtest_elf_start, len);
    kprintf("threadtest: returned rc=%d\n", rc);
}

static void cmd_tlstest(void) {
    if (!_binary_user_tlstest_elf_start) {
        console_write("tlstest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_tlstest_elf_end -
                          _binary_user_tlstest_elf_start);
    console_write("tlstest: exec'ing thread-local-storage program...\n");
    int rc = proc_exec_elf(_binary_user_tlstest_elf_start, len);
    kprintf("tlstest: returned rc=%d\n", rc);
}

static void cmd_posixtest(void) {
    if (!_binary_user_posixtest_elf_start) {
        console_write("posixtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_posixtest_elf_end -
                          _binary_user_posixtest_elf_start);
    console_write("posixtest: exec'ing POSIX-surface program...\n");
    int rc = proc_exec_elf(_binary_user_posixtest_elf_start, len);
    kprintf("posixtest: returned rc=%d\n", rc);
}

static void cmd_redirtest(void) {
    if (!_binary_user_redirtest_elf_start) {
        console_write("redirtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_redirtest_elf_end -
                          _binary_user_redirtest_elf_start);
    int rc = proc_exec_elf(_binary_user_redirtest_elf_start, len);
    kprintf("redirtest: returned rc=%d\n", rc);
}

static void cmd_drvtest(void) {
    if (!_binary_user_drvtest_elf_start) {
        console_write("drvtest: not embedded for this arch\n");
        return;
    }
    /* STOP THE IN-KERNEL MOUSE DRIVER FIRST, and this is a bug fix rather than
     * tidiness: the manifest's window is the 8042's, the built-in ps2_mouse
     * holds exactly that window, and drvrt.c's conflict check refused the test
     * every time — so `drvtest` has FAILED on every ordinary boot since it
     * shipped, with a message ("granted request refused") that reads like the
     * grant machinery is broken rather than like the test asked for something
     * already taken.
     *
     * A test that cannot pass in the default configuration is a test nobody
     * runs, and this one is what proves the port bitmap works at all. */
    struct driver* mdrv = driver_find("ps2_mouse");
    int was_running = (mdrv && (driver_state(mdrv) & DRV_S_INITED)) ? 1 : 0;
    if (was_running) driver_stop("ps2_mouse");

    /* A SEPARATE PROCESS, NOT AN EXCURSION ON THIS TASK.
     *
     * The test's last step FAULTS ON PURPOSE — that is its pass condition — and
     * as an excursion the faulting task was the SHELL.  So the machine lost its
     * shell every time the test succeeded, and nothing after `proc_exec_elf`
     * ever ran, including putting the mouse driver back.  A test whose success
     * costs you the terminal you ran it from is one people learn not to run.
     *
     * Reserved BEFORE the spawn and claimed by the child by name (see
     * drvuser.c): the child's first act is to ask for its ports, and attaching
     * after the spawn returned would be a race whose loser is a confusing
     * failure in the test rather than in the thing being tested. */
    if (drvuser_attach(0, "ps2_mouse") != 0) {
        console_write("drvtest: could not attach the manifest\n");
        if (was_running) driver_start("ps2_mouse");
        return;
    }
    size_t len = (size_t)(_binary_user_drvtest_elf_end -
                          _binary_user_drvtest_elf_start);
    int pid = proc_spawn_argv("ps2_mouse", _binary_user_drvtest_elf_start,
                              len, 0, NULL, 0);
    if (pid < 0) {
        console_write("drvtest: could not spawn\n");
        drvuser_detach(drvuser_pid("ps2_mouse"));
        if (was_running) driver_start("ps2_mouse");
        return;
    }
    /* Poll for DISAPPEARANCE — §M57: init is a universal reaper and may collect
     * it first, so a wait would never complete. */
    for (int k = 0; k < 200; k++) {
        struct task* t = task_find(pid);
        if (!t || t->state == TASK_DEAD) break;
        task_msleep(25);
    }
    drvuser_detach(pid);
    if (was_running) driver_start("ps2_mouse");
    kprintf("drvtest: done (the fault above is the pass)\n");
}

static void cmd_linuxtest(void) {
    if (!_binary_user_linuxhello_elf_start) {
        console_write("linuxtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_linuxhello_elf_end -
                          _binary_user_linuxhello_elf_start);
    console_write("linuxtest: exec'ing a Linux-ABI program (Linux personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;                 /* Linux syscall ABI for the excursion */
    int rc = proc_exec_elf(_binary_user_linuxhello_elf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("linuxtest: returned rc=%d\n", rc);
}

static void cmd_wedgewin(void) {
    if (!_binary_user_wedgewin_muslelf_start) {
        console_write("wedgewin: not embedded — run `make musl` then rebuild\n");
        return;
    }
    gui_start();
    task_msleep(300);
    size_t len = (size_t)(_binary_user_wedgewin_muslelf_end -
                          _binary_user_wedgewin_muslelf_start);
    const char* argv[] = { "wedgewin" };
    int pid = proc_spawn_argv_under("wedgewin", _binary_user_wedgewin_muslelf_start,
                                    len, 1, argv, 1, gui_desktop_pid());
    kprintf("wedgewin: spawned pid %d — its window's X must still close it\n", pid);
}

static void cmd_pthreadtest(void) {
    const unsigned char* sp = _binary_user_pthreadtest_muslelf_start;
    if (!sp) { console_write("pthreadtest: not embedded\n"); return; }
    console_write("pthreadtest: exec'ing a REAL musl pthread program...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(sp, (size_t)(_binary_user_pthreadtest_muslelf_end - sp));
    if (me) me->linux_abi = prev;
    kprintf("pthreadtest: returned rc=%d\n", rc);
}

/* §M56 — `epollmusltest`: epoll through UNMODIFIED musl.  `epolltest` proves
 * the mechanism; this proves the TRANSLATION, which is where the trap is —
 * `struct epoll_event` is 12 bytes on i386 AND amd64 but 16 on arm64, so its
 * size does not follow the word size and cannot be derived from it.  The
 * cookie carries bits in both halves of the u64 precisely so an offset error
 * of four bytes shows up instead of looking plausible. */
static void cmd_epollmusltest(void) {
    const unsigned char* a = _binary_user_epollmusl_muslelf_start;
    const unsigned char* b = _binary_user_epollmusl_muslelf_end;
    if (!a || !b) { console_write("epollmusl: not embedded for this arch\n"); return; }
    console_write("epollmusl: exec'ing a REAL musl binary (Linux personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(a, (unsigned long)(b - a));
    if (me) me->linux_abi = prev;
    kprintf("epollmusl: returned rc=%d\n", rc);
}

/* §M24 — `netmuslserv`: the SERVER socket API through an unmodified musl
 * binary.  `tcptest` proves the stack; this proves the ABI — sockaddr_in in
 * network byte order, socklen_t in and out, accept/getpeername agreeing, and
 * a shutdown that really sends a FIN.  One program, and it runs on all three
 * architectures because the handlers behind it are shared (§M50). */
static void cmd_netmuslserv(void) {
    const unsigned char* a = _binary_user_netmuslserv_muslelf_start;
    const unsigned char* b = _binary_user_netmuslserv_muslelf_end;
    if (!a || !b) { console_write("netmuslserv: not embedded for this arch\n"); return; }
    console_write("netmuslserv: exec'ing a REAL musl binary (Linux personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(a, (unsigned long)(b - a));
    if (me) me->linux_abi = prev;
    kprintf("netmuslserv: returned rc=%d\n", rc);
}

static void cmd_musltest(void) {
    if (!_binary_user_muslhello_muslelf_start) {
        console_write("musltest: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_muslhello_muslelf_end -
                          _binary_user_muslhello_muslelf_start);
    console_write("musltest: exec'ing a REAL musl binary (Linux personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_muslhello_muslelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("musltest: returned rc=%d\n", rc);
}

static void cmd_musldyntest(void) {
    if (!_binary_user_muslhellodyn_dynelf_start) {
        console_write("musldyntest: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_muslhellodyn_dynelf_end -
                          _binary_user_muslhellodyn_dynelf_start);
    console_write("musldyntest: exec'ing a DYNAMICALLY-linked musl binary...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_muslhellodyn_dynelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("musldyntest: returned rc=%d\n", rc);
}

/* §M39 stage 1 — `randtest`: exercise the kernel CSPRNG + /dev/urandom.  Prints
 * two 16-byte draws (must differ) and reads /dev/urandom through the VFS. */
static void cmd_randtest(void) {
    uint8_t a[16], b[16];
    random_bytes(a, sizeof a);
    random_bytes(b, sizeof b);
    int differ = 0;
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) { differ = 1; break; }
    console_write("randtest: draw1 =");
    for (int i = 0; i < 16; i++) kprintf(" %x", a[i]);
    console_write("\nrandtest: draw2 =");
    for (int i = 0; i < 16; i++) kprintf(" %x", b[i]);
    kprintf("\nrandtest: two draws %s\n", differ ? "DIFFER (ok)" : "MATCH (BAD)");

    struct file* f = vfs_open("/dev/urandom", VFS_RDONLY);
    if (!f) { console_write("randtest: /dev/urandom open FAILED\n"); return; }
    uint8_t c[8];
    ssize_t r = vfs_read(f, c, sizeof c);
    vfs_close(f);
    kprintf("randtest: /dev/urandom read %d bytes:", (int)r);
    for (int i = 0; i < (int)r && i < 8; i++) kprintf(" %x", c[i]);
    console_write("\n");
}

static void cmd_cpptest(void) {
    if (!_binary_user_cpptest_cxxelf_start) {
        console_write("cpptest: not embedded — run `make musl-cross-i686` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_cpptest_cxxelf_end -
                          _binary_user_cpptest_cxxelf_start);
    console_write("cpptest: exec'ing a C++ program (exceptions across a .so)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_cpptest_cxxelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("cpptest: returned rc=%d\n", rc);
}

static void cmd_crypttest(void) {
    if (!_binary_user_crypttest_muslelf_start) {
        console_write("crypttest: not embedded — run `make mbedtls` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_crypttest_muslelf_end -
                          _binary_user_crypttest_muslelf_start);
    console_write("crypttest: exec'ing the Mbed TLS crypto self-test...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_crypttest_muslelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("crypttest: returned rc=%d\n", rc);
}

static void cmd_ssltest(void) {
    if (!_binary_user_ssltest_muslelf_start) {
        console_write("ssltest: not embedded — run `make mbedtls` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_ssltest_muslelf_end -
                          _binary_user_ssltest_muslelf_start);
    console_write("ssltest: exec'ing an in-memory TLS handshake...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_ssltest_muslelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("ssltest: returned rc=%d\n", rc);
}

static void cmd_httpstest(void) {
    if (!_binary_user_httpstest_muslelf_start) {
        console_write("httpstest: not embedded — run `make mbedtls` + `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_httpstest_muslelf_end -
                          _binary_user_httpstest_muslelf_start);
    console_write("httpstest: exec'ing a musl HTTPS fetch w/ CA verify (needs QEMU net)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_httpstest_muslelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("httpstest: returned rc=%d\n", rc);
}

static void cmd_netmusl(void) {
    if (!_binary_user_netmusl_muslelf_start) {
        console_write("netmusl: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_netmusl_muslelf_end -
                          _binary_user_netmusl_muslelf_start);
    console_write("netmusl: exec'ing a musl ring-3 HTTP fetch (needs QEMU net)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_netmusl_muslelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("netmusl: returned rc=%d\n", rc);
}

/* `pthreadtest` covers threads in a STATIC binary.  This covers them in a
 * DYNAMIC one, which is where NetSurf died the moment it grew a worker. */
static void cmd_thrdyn(void) {
    if (!_binary_user_thrdyn_dynelf_start) {
        console_write("thrdyn: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_thrdyn_dynelf_end -
                          _binary_user_thrdyn_dynelf_start);
    console_write("thrdyn: threads inside a DYNAMIC musl binary...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_thrdyn_dynelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("thrdyn: returned rc=%d\n", rc);
}

static void cmd_solibtest(void) {
    if (!_binary_user_solibtest_dynelf_start) {
        console_write("solibtest: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_solibtest_dynelf_end -
                          _binary_user_solibtest_dynelf_start);
    console_write("solibtest: exec'ing a program that needs a separate .so...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_solibtest_dynelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("solibtest: returned rc=%d\n", rc);
}

static void cmd_dlopentest(void) {
    if (!_binary_user_dlopentest_dynelf_start) {
        console_write("dlopentest: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_dlopentest_dynelf_end -
                          _binary_user_dlopentest_dynelf_start);
    console_write("dlopentest: exec'ing a program that dlopen's a .so...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_dlopentest_dynelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("dlopentest: returned rc=%d\n", rc);
}

/* --- registrations --------------------------------------------------------- */

#define T0(fn) static void t_##fn(const char* a) { (void)a; cmd_##fn(); }
T0(elftest)   T0(userrun)   T0(fdtest)     T0(shmtest)    T0(socktest)
T0(polltest)  T0(waittest)  T0(libctest)   T0(procspawn)  T0(forktest)
T0(forkexec)  T0(pipetest)  T0(sigtest)    T0(dnstest)    T0(httptest)
T0(threadtest) T0(tlstest)  T0(posixtest)  T0(redirtest)  T0(drvtest)
T0(linuxtest) T0(wedgewin)  T0(pthreadtest) T0(epollmusltest) T0(netmuslserv)
T0(musltest)  T0(musldyntest) T0(randtest) T0(crypttest)  T0(ssltest)
T0(netmusl)   T0(httpstest) T0(cpptest)    T0(thrdyn)     T0(solibtest)
T0(dlopentest) T0(bctest)
#undef T0

/* §M32 stage 7 — the whole TEST family is SHELL_P_ADMIN.  These drive the
 * kernel's own self-test battery (ring-3 excursions, deliberate faults,
 * allocator torture); a limited user has no business in any of them.
 *
 * THE SWEEP THAT ADDED THAT FIELD DID NOT SEE THIS MACRO.  It matched the
 * literal `SHELL_CMD(x) = { ... };`, and a macro BODY has no semicolon, so 39
 * commands kept the zero — and the checker written to find what the sweep had
 * missed used the SAME regex, so it reported the sweep complete.  *An
 * instrument that shares the sweep's blind spot reports the sweep as
 * finished.*  What found it was -Wmissing-field-initializers, pointing (as
 * §M70 also recorded) at the struct DEFINITION rather than at any offending
 * initialiser: a warning about the symptom.
 *
 * The comment lives ABOVE the #define and not inside it — a continued macro
 * body cannot carry one, which is a second mistake this line has now had. */
#define TEST(verb, fn, use, txt) \
    SHELL_CMD(verb) = { #verb, use, txt, SHELL_G_TEST, fn, SHELL_P_ADMIN }

TEST(elftest,       t_elftest,       "", "load an ELF built at run time");
TEST(userrun,       t_userrun,       "", "a ring-3 excursion and back");
TEST(fdtest,        t_fdtest,        "", "the descriptor table");
TEST(shmtest,       t_shmtest,       "", "memfd shared memory");
TEST(socktest,      t_socktest,      "", "unix socketpair + SCM_RIGHTS fd passing");
TEST(polltest,      t_polltest,      "", "poll readiness");
TEST(waittest,      t_waittest,      "", "task_wait and the blocking primitives");
TEST(libctest,      t_libctest,      "", "the in-tree libc, in ring 3");
TEST(procspawn,     t_procspawn,     "", "concurrent preemptible user processes");
TEST(runargs,       cmd_runargs,     "[args]", "argv/env/auxv on the initial stack");
TEST(forktest,      t_forktest,      "", "fork with real copy-on-write isolation");
TEST(forkexec,      t_forkexec,      "", "fork then execve");
TEST(pipetest,      t_pipetest,      "", "pipe + dup2");
TEST(sigtest,       t_sigtest,       "", "sigaction, delivery and sigreturn");
TEST(dnstest,       t_dnstest,       "", "resolve a name from ring 3");
TEST(httptest,      t_httptest,      "", "fetch a page from ring 3");
TEST(threadtest,    t_threadtest,    "", "clone + futex");
TEST(tlstest,       t_tlstest,       "", "thread-local storage");
TEST(posixtest,     t_posixtest,     "", "the POSIX syscall breadth");
TEST(redirtest,     t_redirtest,     "", "dup2 onto stdout, and onto /dev/clipboard");
TEST(drvtest,       t_drvtest,       "", "the ring-3 driver runtime and its port bitmap");
TEST(linuxtest,     t_linuxtest,     "", "a hand-built Linux-ABI binary");
TEST(wedgewin,      t_wedgewin,      "", "a client that opens a window and then freezes");
TEST(pthreadtest,   t_pthreadtest,   "", "REAL musl pthreads");
TEST(epollmusltest, t_epollmusltest, "", "epoll through unmodified musl");
TEST(netmuslserv,   t_netmuslserv,   "", "bind/listen/accept through real musl");
TEST(musltest,      t_musltest,      "", "an unmodified static musl binary");
TEST(musldyntest,   t_musldyntest,   "", "a dynamically linked musl binary");
TEST(randtest,      t_randtest,      "", "/dev/urandom and the entropy pool");
TEST(crypttest,     t_crypttest,     "", "Mbed TLS crypto primitives");
TEST(ssltest,       t_ssltest,       "", "a TLS 1.3 handshake");
TEST(netmusl,       t_netmusl,       "", "sockets through unmodified musl");
TEST(httpstest,     t_httpstest,     "", "HTTPS with CA and hostname verification");
TEST(cpptest,       t_cpptest,       "", "the C++ runtime");
TEST(thrdyn,        t_thrdyn,        "", "threads in a dynamically linked program");
TEST(solibtest,     t_solibtest,     "", "a shared object resolved by ld.so");
TEST(dlopentest,    t_dlopentest,    "", "dlopen at run time");
TEST(blktest,       cmd_blktest,     "[dev]", "the block layer, write then read back");
TEST(bctest,        t_bctest,        "", "the block cache");

#undef TEST
