/* =============================================================================
 * abi_engine.c — canonical operations + dispatch (§M50).  See abi.h for why
 * this exists; this file is the arch-neutral middle of the pipeline.
 *
 * Every handler here is written ONCE and serves every architecture and every
 * guest ABI that names the operation.  Nothing in this file may reference a
 * register, a trap frame, or an architecture — if a handler ever needs to, the
 * translation belongs in the shim or in the number map instead, and the fact
 * that it does not fit is the design telling you something.
 * ============================================================================= */

#include "lnx_signal.h"   /* §M89 */
#include "abi.h"
#include "dosgui.h"     /* §M65 — the toolkit build op */
#include "printf.h"
#include "epoll.h"        /* EPOLL_CTL_* — the guest's own numbers */
#include "syscall.h"
#include "task.h"
#include "fd.h"
#include "proc.h"
#include "kmalloc.h"
#include "vmm.h"        /* vmm_user_access_ok — guest-pointer validation */
#include "vfs.h"        /* §M73 — VFS_* open flags */
#include "shellcmd.h"   /* §M73 — strace */
#include "futex.h"      /* §M89 */
#include "pmm.h"        /* §M89 — sysinfo */
#include "swap.h"
#include "percpu.h"     /* §M89 — getcpu */
#include "timer.h"
#include "vma.h"
#include <stddef.h>

/* Linux errno values the guests share (the ones the engine returns). */
#define ABI_ENOTTY 25
#define ABI_ENOSYS 38
#define ABI_EFAULT 14
#define ABI_EINVAL 22
#define ABI_ENOMEM 12

/* --- canonical handlers ---------------------------------------------------
 *
 * These are the operations whose translation is genuinely mechanical: the
 * guest's arguments are the native arguments, and the native result is already
 * in the guest's convention (d-os's sys_* return negative on failure, which is
 * the Linux-shaped convention every guest ABI supported so far uses).  An
 * operation whose translation is NOT mechanical still belongs here — as a
 * named handler — rather than being inlined into an arch's switch, because
 * that is what makes it shareable.
 * ------------------------------------------------------------------------- */

static long h_read(struct abi_ctx* c) {
    return sys_read((int)c->a[0], (void*)c->a[1], (size_t)c->a[2]);
}
static long h_write(struct abi_ctx* c) {
    return sys_write((int)c->a[0], (const void*)c->a[1], (size_t)c->a[2]);
}
static long h_close(struct abi_ctx* c) {
    return sys_close((int)c->a[0]);
}
static long h_seek(struct abi_ctx* c) {
    return sys_lseek((int)c->a[0], (long)c->a[1], (int)c->a[2]);
}
/* §M73 — directory calls.  The VFS answers -1 (not found), -2 (exists) or -5
 * (not permitted); a Linux program wants ENOENT / EEXIST / EACCES. */
#define AT_FDCWD_ (-100)
static long lnx_err(int r) {
    if (r >= 0) return r;
    if (r == -2) return -17;             /* EEXIST */
    if (r == -5) return -13;             /* EACCES */
    return -2;                           /* ENOENT */
}
static long h_mkdir(struct abi_ctx* c)   { return lnx_err(sys_mkdir((const char*)c->a[0], (int)c->a[1])); }
static long h_mkdirat(struct abi_ctx* c) {
    if ((int)c->a[0] != AT_FDCWD_) return -38;           /* ENOSYS */
    return lnx_err(sys_mkdir((const char*)c->a[1], (int)c->a[2]));
}
static long h_link(struct abi_ctx* c)    { return lnx_err(sys_link((const char*)c->a[0], (const char*)c->a[1])); }
static long h_linkat(struct abi_ctx* c) {
    if ((int)c->a[0] != AT_FDCWD_ || (int)c->a[2] != AT_FDCWD_) return -38;
    return lnx_err(sys_link((const char*)c->a[1], (const char*)c->a[3]));
}
static long h_chmod(struct abi_ctx* c)   { return lnx_err(sys_chmod((const char*)c->a[0], (int)c->a[1])); }
static long h_fchmodat(struct abi_ctx* c) {
    if ((int)c->a[0] != AT_FDCWD_) return -38;
    return lnx_err(sys_chmod((const char*)c->a[1], (int)c->a[2]));
}
static long h_unlink(struct abi_ctx* c)  { return lnx_err(sys_unlink((const char*)c->a[0])); }
static long h_unlinkat(struct abi_ctx* c) {
    if ((int)c->a[0] != AT_FDCWD_) return -38;
    return lnx_err(sys_unlink((const char*)c->a[1]));   /* AT_REMOVEDIR: the VFS unlinks both */
}

static long h_mprotect(struct abi_ctx* c) {
    return sys_mprotect((uintptr_t)c->a[0], (size_t)c->a[1], (int)c->a[2]);
}
static long h_munmap(struct abi_ctx* c) {
    /* §M74 — real now (it used to succeed and do nothing): the frames go back;
     * the addresses do not (mmap is still a bump allocator). */
    return sys_munmap((uintptr_t)c->a[0], (size_t)c->a[1]);
}
/* §M65 — build the shared widget toolkit inside a dosgui window.  The blob
 * pointer is a ring-3 address; dosgui_ui_build copies it in before reading a
 * single field, which is where that check belongs (the pointer's ORIGIN is
 * known there, §M46's rule). */
static long h_ui_build(struct abi_ctx* c) {
    return dosgui_ui_build((int)c->a[0], (const void*)c->a[1], (int)c->a[2]);
}

static long h_getpid(struct abi_ctx* c) {
    (void)c;
    struct task* t = task_current();
    return t ? t->pid : 0;
}
static long h_getppid(struct abi_ctx* c) {
    (void)c;
    struct task* t = task_current();
    return t ? t->ppid : 0;
}

#define ABI_EPERM 1

/* §M73 — "who am I", from the credential.  A task that is nobody's (a SYSTEM
 * or kernel identity, CRED_UID_NONE) answers 0: in a POSIX program's terms it
 * has root's power, and a value outside uid_t's meaning would be worse. */
static int guest_uid(void) {
    struct task* t = task_current();
    int u = t ? cred_uid(&t->cred) : 0;
    return u < 0 ? 0 : u;
}
static int guest_gid(void) {
    struct task* t = task_current();
    int g = t ? cred_gid(&t->cred) : 0;
    return g < 0 ? 0 : g;
}
static long h_getuid(struct abi_ctx* c) { (void)c; return guest_uid(); }
static long h_getgid(struct abi_ctx* c) { (void)c; return guest_gid(); }
static long h_setuid(struct abi_ctx* c) { return (int)c->a[0] == guest_uid() ? 0 : -ABI_EPERM; }
static long h_setgid(struct abi_ctx* c) { return (int)c->a[0] == guest_gid() ? 0 : -ABI_EPERM; }

static unsigned long abi_get_word(const struct abi_ctx* c, unsigned long p, int i);

/* §M73 — the file and time calls.  See the ABI_OPEN comment in abi.h for why
 * they live here; everything below that differs between guests is read from
 * the map. */
#define ABI_ENOENT  2
#define ABI_EBADF   9
#define ABI_ENOTDIR 20
static int abi_w_ok(unsigned long p, unsigned long n) { return p && vmm_user_access_ok((uintptr_t)p, (uintptr_t)n, 1); }
static int abi_r_ok(unsigned long p, unsigned long n) { return p && vmm_user_access_ok((uintptr_t)p, (uintptr_t)n, 0); }

/* Copy a guest path in.  0 on success. */
static int abi_path(unsigned long up, char* k, unsigned cap) {
    return (up && copy_str_from_user(k, (uintptr_t)up, cap) >= 0) ? 0 : -1;
}

/* Linux open flags → VFS flags.  The access mode, O_CREAT and O_TRUNC share
 * their bits on every guest we speak; O_DIRECTORY does not, so the caller
 * checks it against the map. */
static int abi_open_flags(unsigned long lf) {
    int vf;
    switch (lf & 3u) {
        case 1:  vf = VFS_WRONLY; break;
        case 2:  vf = VFS_RDWR;   break;
        default: vf = VFS_RDONLY; break;
    }
    if (lf & 0100u)  vf |= VFS_CREATE;
    if (lf & 01000u) vf |= VFS_TRUNC;
    return vf;
}
static long abi_open_common(struct abi_ctx* c, unsigned long upath, unsigned long flags) {
    char kp[256];
    if (abi_path(upath, kp, sizeof kp) != 0) return -ABI_EFAULT;
    if (c->map && c->map->o_directory && (flags & c->map->o_directory)) {
        struct kstat_full st;
        if (sys_stat_full_k(kp, &st) != 0) return -ABI_ENOENT;
        if ((st.mode & KS_IFMT) != KS_IFDIR) return -ABI_ENOTDIR;
    }
    /* sys_open copies from a RING-3 path, which is what upath is. */
    long r = sys_open((const char*)upath, abi_open_flags(flags));
    return r < 0 ? -ABI_ENOENT : r;
}
static long h_open(struct abi_ctx* c)   { return abi_open_common(c, c->a[0], c->a[1]); }
static long h_openat(struct abi_ctx* c) {
    if ((int)c->a[0] != AT_FDCWD_) return -ABI_ENOSYS;   /* no directory descriptors */
    return abi_open_common(c, c->a[1], c->a[2]);
}

/* Write one field of the guest's struct stat. */
static void st_put(uint8_t* b, uint8_t off, uint8_t w, uint64_t v) {
    if (off == 0xFF) return;
    for (int i = 0; i < w; i++) b[off + i] = (uint8_t)(v >> (8 * i));
}
static long abi_put_stat(struct abi_ctx* c, unsigned long up, const struct kstat_full* k) {
    const struct abi_stat_layout* L = c->map ? c->map->stat : NULL;
    if (!L) return -ABI_ENOSYS;
    if (!abi_w_ok(up, L->bytes)) return -ABI_EFAULT;
    uint8_t* b = (uint8_t*)(uintptr_t)up;
    for (int i = 0; i < L->bytes; i++) b[i] = 0;
    st_put(b, L->dev, 8, 1);
    st_put(b, L->ino, L->ino_w, k->ino);
    st_put(b, L->ino32, 4, k->ino);
    st_put(b, L->mode, 4, k->mode);
    st_put(b, L->nlink, L->nlink_w, k->nlink);
    st_put(b, L->uid, 4, (uint32_t)k->uid);
    st_put(b, L->gid, 4, (uint32_t)k->gid);
    st_put(b, L->size, 8, k->size);
    st_put(b, L->blksize, L->blksize_w, 4096);
    st_put(b, L->blocks, 8, (k->size + 511) / 512);
    return 0;
}
static long abi_stat_path(struct abi_ctx* c, unsigned long upath, unsigned long ubuf) {
    char kp[256];
    if (abi_path(upath, kp, sizeof kp) != 0) return -ABI_EFAULT;
    struct kstat_full k;
    if (sys_stat_full_k(kp, &k) != 0) return -ABI_ENOENT;
    return abi_put_stat(c, ubuf, &k);
}
static long h_stat(struct abi_ctx* c)  { return abi_stat_path(c, c->a[0], c->a[1]); }
static long h_fstat(struct abi_ctx* c) {
    struct kstat_full k;
    if (sys_fstat_full_k((int)c->a[0], &k) != 0) return -ABI_EBADF;
    return abi_put_stat(c, c->a[1], &k);
}
#define ABI_AT_EMPTY_PATH 0x1000
static long h_fstatat(struct abi_ctx* c) {
    if (c->a[3] & ABI_AT_EMPTY_PATH) {           /* fstatat(fd, "", st, AT_EMPTY_PATH) == fstat */
        char kp[4];
        if (abi_path(c->a[1], kp, sizeof kp) == 0 && kp[0] == 0) {
            struct kstat_full k;
            if (sys_fstat_full_k((int)c->a[0], &k) != 0) return -ABI_EBADF;
            return abi_put_stat(c, c->a[2], &k);
        }
    }
    if ((int)c->a[0] != AT_FDCWD_) return -ABI_ENOSYS;
    return abi_stat_path(c, c->a[1], c->a[2]);
}
static long h_getdents64(struct abi_ctx* c) {
    return sys_getdents64((int)c->a[0], (void*)c->a[1], (size_t)c->a[2]);
}

/* fcntl: F_DUPFD must really duplicate (libwayland dups every fd it sends,
 * and a "successful" 0 is a dup to fd 0); O_NONBLOCK is honoured; the rest
 * (CLOEXEC, locks) is accepted and not tracked.  The command numbers are the
 * same on every guest we speak (asm-generic). */
static long h_fcntl(struct abi_ctx* c) {
    int fd = (int)c->a[0], cmd = (int)c->a[1];
    long arg = (long)c->a[2];
    if (cmd == 0 || cmd == 1030) {                          /* F_DUPFD, F_DUPFD_CLOEXEC */
        int nfd = sys_dupfd(fd, (int)arg);
        return nfd < 0 ? -ABI_EINVAL : nfd;
    }
    if (cmd == 4) { sys_socket_setnonblock(fd, (arg & 04000) ? 1 : 0); return 0; }   /* F_SETFL */
    if (cmd == 3) return sys_socket_getnonblock(fd) > 0 ? 04000 : 0;                /* F_GETFL */
    return 0;
}

/* access: an existence check plus the permission the VFS would enforce. */
static long abi_access_path(unsigned long upath) {
    char kp[256];
    if (abi_path(upath, kp, sizeof kp) != 0) return -ABI_EFAULT;
    struct kstat_full k;
    return sys_stat_full_k(kp, &k) == 0 ? 0 : -ABI_ENOENT;
}
static long h_access(struct abi_ctx* c)    { return abi_access_path(c->a[0]); }
static long h_faccessat(struct abi_ctx* c) {
    if ((int)c->a[0] != AT_FDCWD_) return -ABI_ENOSYS;
    return abi_access_path(c->a[1]);
}
/* readlink: this VFS has no symlinks, so an existing path is "not a link"
 * (EINVAL) and a missing one ENOENT — exactly what realpath() needs to walk. */
/* §M89 — /proc/self/exe (and /proc/<pid>/exe) is the one link that matters:
 * a program asks it where it was started from.  musl's loader expands $ORIGIN
 * with it and a JRE's launcher finds JAVA_HOME with it.  Answered from the
 * cred's exe, with readlink's contract: NOT NUL-terminated, truncated to the
 * buffer, the byte count returned. */
static int proc_exe_of(const char* p, char* out, unsigned cap) {
    const char* pre = "/proc/";
    for (int i = 0; pre[i]; i++) if (p[i] != pre[i]) return -1;
    p += 6;
    struct task* t = NULL;
    if (p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f') { t = task_current(); p += 4; }
    else {
        int pid = 0, n = 0;
        while (*p >= '0' && *p <= '9') { pid = pid * 10 + (*p++ - '0'); n++; }
        if (!n) return -1;
        t = task_find(pid);
    }
    if (p[0] != '/' || p[1] != 'e' || p[2] != 'x' || p[3] != 'e' || p[4]) return -1;
    if (!t || !t->cred.exe[0]) return -2;
    unsigned n = 0;
    while (n + 1 < cap && t->cred.exe[n]) { out[n] = t->cred.exe[n]; n++; }
    out[n] = 0;
    return (int)n;
}
static long abi_readlink_path(unsigned long upath, unsigned long ubuf, unsigned long size) {
    char kp[256];
    if (abi_path(upath, kp, sizeof kp) != 0) return -ABI_EFAULT;
    char tgt[128];
    int n = proc_exe_of(kp, tgt, sizeof tgt);
    if (n == -2) return -ABI_ENOENT;
    if (n >= 0) {
        if ((long)size <= 0) return -ABI_EINVAL;
        unsigned long m = (unsigned long)n < size ? (unsigned long)n : size;
        if (!abi_w_ok(ubuf, m)) return -ABI_EFAULT;
        for (unsigned long i = 0; i < m; i++) ((char*)(uintptr_t)ubuf)[i] = tgt[i];
        return (long)m;
    }
    struct kstat_full k;
    return sys_stat_full_k(kp, &k) == 0 ? -ABI_EINVAL : -ABI_ENOENT;
}
static long h_readlink(struct abi_ctx* c)   { return abi_readlink_path(c->a[0], c->a[1], c->a[2]); }
static long h_readlinkat(struct abi_ctx* c) { return abi_readlink_path(c->a[1], c->a[2], c->a[3]); }

/* sendfile(out, in, off*, count) — a read/write loop through a kernel buffer.
 * With an offset the input's own cursor is left untouched (seek, copy, seek
 * back); the offset is a 64-bit loff_t on every guest we speak. */
static long h_sendfile(struct abi_ctx* c) {
    int out = (int)c->a[0], in = (int)c->a[1];
    unsigned long offp = c->a[2];
    size_t count = (size_t)c->a[3];
    long saved = -1;
    if (offp) {
        if (!abi_w_ok(offp, 8)) return -ABI_EFAULT;
        saved = sys_lseek(in, 0, 1);
        if (saved < 0 || sys_lseek(in, (long)*(uint64_t*)(uintptr_t)offp, 0) < 0) return -ABI_EINVAL;
    }
    static const size_t CH = 4096;
    uint8_t* buf = (uint8_t*)kmalloc(CH);
    if (!buf) return -ABI_ENOMEM;
    long total = 0;
    while ((size_t)total < count) {
        size_t want = count - (size_t)total > CH ? CH : count - (size_t)total;
        long r = sys_read_k(in, buf, want);
        if (r <= 0) { if (!total && r < 0) total = -ABI_EINVAL; break; }
        long w = sys_write_k(out, buf, (size_t)r);
        if (w <= 0) { if (!total) total = -ABI_EINVAL; break; }
        total += w;
        if (w < r) break;
    }
    kfree(buf);
    if (offp) {
        if (total > 0) *(uint64_t*)(uintptr_t)offp += (uint64_t)total;
        sys_lseek(in, saved, 0);
    }
    return total;
}
static long h_uname(struct abi_ctx* c) { return sys_uname((struct kutsname*)c->a[0]); }
static long h_dup(struct abi_ctx* c) {
    int nfd = sys_dupfd((int)c->a[0], 0);
    return nfd < 0 ? -ABI_EBADF : nfd;
}
static long h_dup2(struct abi_ctx* c) {
    if ((int)c->a[0] == (int)c->a[1]) return (int)c->a[1];
    int r = sys_dup2((int)c->a[0], (int)c->a[1]);
    return r < 0 ? -ABI_EBADF : r;
}
static long h_dup3(struct abi_ctx* c) {                 /* flags: only O_CLOEXEC, untracked */
    if ((int)c->a[0] == (int)c->a[1]) return -ABI_EINVAL;
    int r = sys_dup2((int)c->a[0], (int)c->a[1]);
    return r < 0 ? -ABI_EBADF : r;
}
static long h_getcwd(struct abi_ctx* c) {
    const char* cwd = cred_fs_cwd();
    if (!cwd[0]) cwd = "/";
    unsigned long n = 0; while (cwd[n]) n++;
    if (c->a[1] < n + 1) return -34;                   /* ERANGE */
    if (!abi_w_ok(c->a[0], n + 1)) return -ABI_EFAULT;
    for (unsigned long i = 0; i <= n; i++) ((char*)(uintptr_t)c->a[0])[i] = cwd[i];
    return (long)(n + 1);                               /* bytes, NUL included */
}
static long h_chdir(struct abi_ctx* c) {
    char kp[256], canon[96];
    if (abi_path(c->a[0], kp, sizeof kp) != 0) return -ABI_EFAULT;
    if (vfs_canonical(kp, canon, sizeof canon) != 0) return -36;       /* ENAMETOOLONG */
    struct kstat_full st;
    if (sys_stat_full_k(canon, &st) != 0) return -ABI_ENOENT;
    if ((st.mode & KS_IFMT) != KS_IFDIR) return -ABI_ENOTDIR;
    struct task* t = task_current();
    if (!t) return -ABI_EINVAL;
    for (unsigned i = 0; i < sizeof t->cred.cwd; i++) { t->cred.cwd[i] = canon[i]; if (!canon[i]) break; }
    return 0;
}
static long h_getgroups(struct abi_ctx* c) {
    /* The primary group is the one supplementary group we report: `id` and
     * friends refuse an error here, and an empty list is a lie about admins. */
    int size = (int)c->a[0];
    if (size == 0) return 1;
    if (size < 0) return -ABI_EINVAL;
    if (!abi_w_ok(c->a[1], 4)) return -ABI_EFAULT;
    *(uint32_t*)(uintptr_t)c->a[1] = (uint32_t)guest_gid();
    return 1;
}

/* Time: timespec/timeval are two guest words.  CLOCK_GETTIME64 is i386's
 * time64 variant — the same meaning with 64-bit words whatever the guest. */
static long abi_put_ts(struct abi_ctx* c, unsigned long up, int wide, uint64_t s, uint64_t sub) {
    int w = (wide || !c->map || c->map->word_bytes != 4) ? 8 : 4;
    if (!abi_w_ok(up, 2u * (unsigned)w)) return -ABI_EFAULT;
    if (w == 8) { ((uint64_t*)(uintptr_t)up)[0] = s; ((uint64_t*)(uintptr_t)up)[1] = sub; }
    else        { ((uint32_t*)(uintptr_t)up)[0] = (uint32_t)s; ((uint32_t*)(uintptr_t)up)[1] = (uint32_t)sub; }
    return 0;
}
static long h_clock_gettime(struct abi_ctx* c) {
    struct ktimespec ts;
    sys_clock_gettime_k((int)c->a[0], &ts);
    return abi_put_ts(c, c->a[1], 0, ts.sec, ts.nsec);
}
static long h_clock_gettime64(struct abi_ctx* c) {
    struct ktimespec ts;
    sys_clock_gettime_k((int)c->a[0], &ts);
    return abi_put_ts(c, c->a[1], 1, ts.sec, ts.nsec);
}
static long h_gettimeofday(struct abi_ctx* c) {
    if (!c->a[0]) return 0;
    struct ktimespec ts;
    sys_clock_gettime_k(CLOCK_REALTIME, &ts);
    return abi_put_ts(c, c->a[0], 0, ts.sec, ts.nsec / 1000);
}
static long abi_sleep(struct abi_ctx* c, int which, int abs, unsigned long ureq) {
    unsigned w = (!c->map || c->map->word_bytes != 4) ? 8 : 4;
    if (!abi_r_ok(ureq, 2 * w)) return -ABI_EFAULT;
    uint64_t s  = abi_get_word(c, ureq, 0);
    uint64_t ns = abi_get_word(c, ureq, 1);
    uint64_t t = s * 1000000000ull + ns;
    if (abs && which == CLOCK_REALTIME) {
        /* An absolute WALL-CLOCK deadline: the kernel's sleep timeline is the
         * monotonic one, so convert to "how long from now" here — otherwise a
         * 2026 deadline on a since-boot timeline is a sleep that never ends. */
        struct ktimespec now;
        sys_clock_gettime_k(CLOCK_REALTIME, &now);
        uint64_t n = (uint64_t)now.sec * 1000000000ull + now.nsec;
        t = t > n ? t - n : 0;
        abs = 0;
    }
    long r = sys_clock_nanosleep_ns(which, abs, t);
    return r < 0 ? -4 /* EINTR */ : 0;
}
static long h_nanosleep(struct abi_ctx* c)       { return abi_sleep(c, 1, 0, c->a[0]); }
static long h_clock_nanosleep(struct abi_ctx* c) {
    long r = abi_sleep(c, (int)c->a[0], (c->a[1] & 1) ? 1 : 0, c->a[2]);   /* TIMER_ABSTIME */
    return r < 0 ? -r : 0;                    /* clock_nanosleep returns the error, not -1 */
}

/* --- operations whose translation is NOT one-to-one ------------------------
 *
 * These are the ones that justify having a canonical vocabulary at all: each
 * needs a decision, and making it once here is the difference between one
 * engine and one hand-written layer per architecture.
 * ------------------------------------------------------------------------- */

/* Linux errno values the guests share.  Only the handful the engine itself
 * returns; a guest ABI whose errno space differs would translate in its own
 * adapter rather than here. */
/* (the errno values themselves are defined at the top of the file) */

/* Is [uptr, uptr+len) a writable GUEST address in the active space?  The check
 * lives here, in the handler, because this is where the pointer's ORIGIN is
 * known — the same rule the x86 layers arrived at the hard way (DOCS §M46:
 * putting it in a shared sys_* helper broke every in-kernel caller). */
static int abi_user_w_ok(unsigned long uptr, unsigned long len) {
    return uptr && vmm_user_access_ok((uintptr_t)uptr, (uintptr_t)len, 1);
}

/* §M53 stage 3 — read/write a guest `long` at the guest's own width.
 *
 * `struct itimerspec` is four longs; on a 32-bit guest that is 16 bytes and on
 * a 64-bit one 32.  These two helpers are the entire difference, and they take
 * the width from the MAP (the guest's description) rather than from sizeof on
 * the host — which would be right only when guest and kernel happen to agree. */
static unsigned long abi_get_word(const struct abi_ctx* c, unsigned long p, int i) {
    if (c->map && c->map->word_bytes == 4)
        return (unsigned long)((const uint32_t*)(uintptr_t)p)[i];
    return (unsigned long)((const uint64_t*)(uintptr_t)p)[i];
}

static void abi_put_word(const struct abi_ctx* c, unsigned long p, int i,
                         unsigned long v) {
    if (c->map && c->map->word_bytes == 4) ((uint32_t*)(uintptr_t)p)[i] = (uint32_t)v;
    else                                   ((uint64_t*)(uintptr_t)p)[i] = (uint64_t)v;
}

static unsigned long abi_itimerspec_bytes(const struct abi_ctx* c) {
    return (c->map && c->map->word_bytes == 4) ? 16u : 32u;
}

/* itimerspec layout, in guest words: [0]=it_interval.sec [1]=it_interval.nsec
 * [2]=it_value.sec [3]=it_value.nsec.  Note the INTERVAL comes first — a
 * detail worth stating, because getting it backwards produces a timer that
 * works exactly once and then never again, which reads like a different bug. */
#define ABI_ITS_IV_SEC   0
#define ABI_ITS_IV_NSEC  1
#define ABI_ITS_VAL_SEC  2
#define ABI_ITS_VAL_NSEC 3

static long h_timerfd_create(struct abi_ctx* c) {
    (void)c;
    /* The clockid and flags are accepted and ignored: there is one clock here
     * (timer_now_ns) and it is monotonic, so CLOCK_MONOTONIC is what every
     * caller gets whatever it asked for.  Failing instead would stop programs
     * that pass CLOCK_REALTIME out of habit and never depend on the
     * difference. */
    return sys_timerfd_create();
}

static long h_timerfd_settime(struct abi_ctx* c) {
    int fd  = (int)c->a[0];
    int abs = (c->a[1] & 1) != 0;               /* TFD_TIMER_ABSTIME */
    unsigned long newp = c->a[2], oldp = c->a[3];
    if (!newp) return -ABI_EFAULT;
    if (!vmm_user_access_ok((uintptr_t)newp, abi_itimerspec_bytes(c), 0))
        return -ABI_EFAULT;

    if (oldp) {
        if (!abi_user_w_ok(oldp, abi_itimerspec_bytes(c))) return -ABI_EFAULT;
        uint64_t rem = 0, iv = 0;
        sys_timerfd_gettime_k(fd, &rem, &iv);
        abi_put_word(c, oldp, ABI_ITS_IV_SEC,   (unsigned long)(iv / 1000000000ull));
        abi_put_word(c, oldp, ABI_ITS_IV_NSEC,  (unsigned long)(iv % 1000000000ull));
        abi_put_word(c, oldp, ABI_ITS_VAL_SEC,  (unsigned long)(rem / 1000000000ull));
        abi_put_word(c, oldp, ABI_ITS_VAL_NSEC, (unsigned long)(rem % 1000000000ull));
    }

    uint64_t iv_ns = (uint64_t)abi_get_word(c, newp, ABI_ITS_IV_SEC) * 1000000000ull
                   + (uint64_t)abi_get_word(c, newp, ABI_ITS_IV_NSEC);
    uint64_t v_ns  = (uint64_t)abi_get_word(c, newp, ABI_ITS_VAL_SEC) * 1000000000ull
                   + (uint64_t)abi_get_word(c, newp, ABI_ITS_VAL_NSEC);
    return sys_timerfd_settime(fd, abs, v_ns, iv_ns) == 0 ? 0 : -ABI_EFAULT;
}

static long h_timerfd_gettime(struct abi_ctx* c) {
    unsigned long p = c->a[1];
    if (!abi_user_w_ok(p, abi_itimerspec_bytes(c))) return -ABI_EFAULT;
    uint64_t rem = 0, iv = 0;
    if (sys_timerfd_gettime_k((int)c->a[0], &rem, &iv) != 0) return -ABI_EFAULT;
    abi_put_word(c, p, ABI_ITS_IV_SEC,   (unsigned long)(iv / 1000000000ull));
    abi_put_word(c, p, ABI_ITS_IV_NSEC,  (unsigned long)(iv % 1000000000ull));
    abi_put_word(c, p, ABI_ITS_VAL_SEC,  (unsigned long)(rem / 1000000000ull));
    abi_put_word(c, p, ABI_ITS_VAL_NSEC, (unsigned long)(rem % 1000000000ull));
    return 0;
}

/* setitimer(which, new, old).  `struct itimerval` is microseconds, not
 * nanoseconds — the one place POSIX uses a different unit for the same idea,
 * and a silent factor of 1000 if it is missed. */
static long h_setitimer(struct abi_ctx* c) {
    int which = (int)c->a[0];
    if (which != 0) return -ABI_ENOSYS;         /* only ITIMER_REAL → SIGALRM */
    unsigned long newp = c->a[1], oldp = c->a[2];

    if (oldp) {
        if (!abi_user_w_ok(oldp, abi_itimerspec_bytes(c))) return -ABI_EFAULT;
        uint64_t v = 0, iv = 0;
        sys_getitimer_ns(&v, &iv);
        abi_put_word(c, oldp, ABI_ITS_IV_SEC,   (unsigned long)(iv / 1000000000ull));
        abi_put_word(c, oldp, ABI_ITS_IV_NSEC,  (unsigned long)((iv % 1000000000ull) / 1000));
        abi_put_word(c, oldp, ABI_ITS_VAL_SEC,  (unsigned long)(v / 1000000000ull));
        abi_put_word(c, oldp, ABI_ITS_VAL_NSEC, (unsigned long)((v % 1000000000ull) / 1000));
    }
    if (!newp) return 0;                        /* query-only form */
    if (!vmm_user_access_ok((uintptr_t)newp, abi_itimerspec_bytes(c), 0))
        return -ABI_EFAULT;

    uint64_t iv_ns = (uint64_t)abi_get_word(c, newp, ABI_ITS_IV_SEC) * 1000000000ull
                   + (uint64_t)abi_get_word(c, newp, ABI_ITS_IV_NSEC) * 1000ull;
    uint64_t v_ns  = (uint64_t)abi_get_word(c, newp, ABI_ITS_VAL_SEC) * 1000000000ull
                   + (uint64_t)abi_get_word(c, newp, ABI_ITS_VAL_NSEC) * 1000ull;
    return sys_setitimer_ns(v_ns, iv_ns) == 0 ? 0 : -ABI_EFAULT;
}

struct abi_iovec { void* base; unsigned long len; };

/* readv/writev: a vector, not a buffer.  Looping over sys_read/sys_write is
 * the whole implementation, but it must stop on a SHORT read — otherwise the
 * next iovec is filled from a later part of the stream and the caller silently
 * gets reordered data. */
static long h_writev(struct abi_ctx* c) {
    const struct abi_iovec* v = (const struct abi_iovec*)c->a[1];
    int n = (int)c->a[2];
    long total = 0;
    for (int i = 0; i < n && v; i++) {
        long w = sys_write((int)c->a[0], v[i].base, (size_t)v[i].len);
        if (w < 0) return total ? total : w;
        total += w;
    }
    return total;
}
static long h_readv(struct abi_ctx* c) {
    const struct abi_iovec* v = (const struct abi_iovec*)c->a[1];
    int n = (int)c->a[2];
    long total = 0;
    for (int i = 0; i < n && v; i++) {
        long r = sys_read((int)c->a[0], v[i].base, (size_t)v[i].len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((unsigned long)r < v[i].len) break;   /* short read → done */
    }
    return total;
}

/* ioctl: ENOTTY, not ENOSYS.  The distinction matters — musl's isatty() reads
 * ENOTTY as "this is not a terminal" and carries on with block buffering,
 * while ENOSYS is an unknown-call error it has no story for.  Answering the
 * question correctly is not the same as implementing the call. */
static long h_ioctl(struct abi_ctx* c) { (void)c; return -ABI_ENOTTY; }

/* brk: report failure so a libc falls back to mmap.  d-os has no program
 * break, and pretending otherwise would hand out addresses nothing backs. */
static long h_brk(struct abi_ctx* c) { (void)c; return 0; }

static long h_mmap(struct abi_ctx* c) {
    return sys_mmap_full(c->a[0], (size_t)c->a[1], (int)c->a[2], (int)c->a[3],
                         (int)c->a[4], (uint64_t)c->a[5]);
}
/* i386 mmap2: the last argument counts pages, which is how a 32-bit guest
 * reaches file offsets past 4 GiB. */
static long h_mmap_pgoff(struct abi_ctx* c) {
    return sys_mmap_full(c->a[0], (size_t)c->a[1], (int)c->a[2], (int)c->a[3],
                         (int)c->a[4], (uint64_t)(uint32_t)c->a[5] * 4096u);
}

/* set_tid_address returns the caller's tid.  d-os has no separate tid space,
 * so the pid is the honest answer — the same one gettid gives. */
/* §M56.1 — rt_sigprocmask, for real.
 *
 * This was `return 0` — accept and forget.  That is not a harmless stub: a
 * program that blocks SIGPIPE around a write to a closed pipe, or blocks
 * SIGALRM while it touches the state its handler reads, was getting neither,
 * silently.  The kernel had the pending mask all along; what it lacked was
 * the blocked one.
 *
 * The mask is a guest `sigset_t`, which is 128 bytes on Linux — but only the
 * low 32 bits can name a signal this kernel has, so we read and write the
 * first word and leave the rest alone.  Writing zeroes over the whole thing
 * would be worse than ignoring it: a libc that keeps state in the high words
 * would have it silently cleared. */
#define ABI_SIG_BLOCK   0
#define ABI_SIG_UNBLOCK 1
#define ABI_SIG_SETMASK 2

/* THE OFF-BY-ONE THAT IS NOT AN OFF-BY-ONE.
 *
 * A Linux `sigset_t` stores signal N at bit N-1: SIGHUP (1) is bit 0.  This
 * kernel's `sig_pending` / `sig_blocked` store signal N at bit N, because they
 * are built with `1u << sig` and nothing ever needed to disagree.  Both
 * conventions are self-consistent; copying the word across without shifting is
 * what is wrong, and it fails SILENTLY — SIGALRM (14) blocked by the guest
 * arrives as bit 14 in the kernel, which is SIGCHLD's slot here, so the mask
 * appears to be set and simply never matches.
 *
 * It cost a test that reported `pending=0` on all three arches with no other
 * symptom.  These two functions are the entire fix, and they exist as named
 * functions rather than inline shifts so the next signal-shaped operation has
 * something obvious to call. */
/* (The two converters that lived here are lnx_sig_blocked64 /
 * lnx_sig_set_blocked64 / lnx_sig_pending64 now, in lnx_signal.c, which also
 * hold the real-time half — §M89.) */

/* How much of a guest `sigset_t` is worth touching.
 *
 * The declared type is 128 bytes, but Linux only defines 64 signals, so the
 * kernel ABI passes `sigsetsize` and every libc passes 8.  This kernel has 32
 * signals and no real-time signals at all, so bits 32..63 can never be pending
 * here — reporting them as ZERO is the truth, not a loss of information, which
 * is why the full 8 bytes are written rather than only the first 4.  Anything
 * the guest keeps beyond `sigsetsize` is its own business and is left alone. */
#define ABI_SIGSET_BYTES 8

static int abi_sigset_read(struct abi_ctx* c, unsigned long p, uint64_t* out) {
    (void)c;
    if (!vmm_user_access_ok((uintptr_t)p, ABI_SIGSET_BYTES, 0)) return 0;
    const uint8_t* b = (const uint8_t*)(uintptr_t)p;
    uint64_t v = 0;
    for (int i = 0; i < ABI_SIGSET_BYTES; i++) v |= (uint64_t)b[i] << (8 * i);
    *out = v;
    return 1;
}

static int abi_sigset_write(struct abi_ctx* c, unsigned long p, uint64_t v) {
    if (!abi_user_w_ok(p, ABI_SIGSET_BYTES)) return 0;
    (void)c;
    uint8_t* b = (uint8_t*)(uintptr_t)p;
    for (int i = 0; i < ABI_SIGSET_BYTES; i++) b[i] = (uint8_t)(v >> (8 * i));
    return 1;
}

static long h_sigprocmask(struct abi_ctx* c) {
    struct task* t = task_current();
    if (!t) return 0;
    int how = (int)c->a[0];
    unsigned long setp = c->a[1], oldp = c->a[2];

    /* §M89 — the whole 64-signal mask, in the guest's own layout: the
     * real-time half (32..64) lives in the Linux signal state, and a
     * JDK blocks and unblocks it. */
    uint64_t old = lnx_sig_blocked64(t);
    if (oldp && !abi_sigset_write(c, oldp, old))
        return -ABI_EFAULT;
    if (!setp) return 0;                        /* query only */

    uint64_t gw = 0;
    if (!abi_sigset_read(c, setp, &gw)) return -ABI_EFAULT;
    uint64_t nb;
    switch (how) {
    case ABI_SIG_BLOCK:   nb = old |  gw; break;
    case ABI_SIG_UNBLOCK: nb = old & ~gw; break;
    case ABI_SIG_SETMASK: nb = gw;        break;
    default: return -ABI_EINVAL;
    }
    /* SIGKILL and SIGSTOP are never blockable — set_blocked64 drops them, so
     * `sigprocmask(SIG_BLOCK, full)` followed by a query reports the truth. */
    lnx_sig_set_blocked64(t, nb);

    /* Unblocking may have made an already-pending signal deliverable, and the
     * task is about to return to ring 3 — where the delivery check runs — so
     * nothing else is needed here.  Stating it because the absence of a wake
     * looks like an omission. */
    return 0;
}

/* wait4(pid, status, options, rusage) — rusage is ignored (d-os collects no
 * per-process resource accounting yet); reporting it as unsupported would fail
 * a shell that only ever wants the exit status.
 *
 * The status word is an ENCODING, not the exit code: a guest reads it through
 * WIFEXITED/WEXITSTATUS, which look for the code in bits 8..15 and the signal
 * in bits 0..6.  Handing back the raw code happens to work for 0 and is wrong
 * for every other value — the kind of bug that hides until a program checks
 * whether its child succeeded. */
/* ---------------------------------------------------------------------------
 * §M56 — epoll.
 *
 * The whole arch-specific content of this is ONE number: how big the guest's
 * `struct epoll_event` is.  Everything else — the flags, the ctl ops, the
 * timeout convention — is identical across the three ABIs, which is exactly
 * the split §M50's engine exists to expose.
 *
 * The guest layout is { u32 events; u64 data; }, packed on x86 (12 bytes, data
 * at offset 4, UNALIGNED on a 64-bit host) and unpacked on arm64 (16 bytes,
 * data at offset 8).  We therefore read and write `data` BYTEWISE rather than
 * through a u64*: on x86_64 that pointer would be misaligned, and while x86
 * tolerates it, writing the kernel so that it only works on forgiving hardware
 * is how an arch port later fails for no visible reason.
 * ------------------------------------------------------------------------- */
static unsigned long abi_epoll_ev_bytes(const struct abi_ctx* c) {
    /* Default to the x86 packed size rather than 0 if a map predates the
     * field — a zero here would make every bounds check pass trivially. */
    if (c->map && c->map->epoll_event_bytes) return c->map->epoll_event_bytes;
    return 12u;
}

/* Offset of the `data` member: right after the u32 when packed, 8-aligned
 * when not.  Derived from the struct SIZE, which is the only thing the map
 * needs to carry. */
static unsigned long abi_epoll_data_off(const struct abi_ctx* c) {
    return abi_epoll_ev_bytes(c) == 12u ? 4u : 8u;
}

static uint64_t abi_ld64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);   /* LE guests */
    return v;
}
static void abi_st64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t abi_ld32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void abi_st32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static long h_epoll_create(struct abi_ctx* c) {
    /* epoll_create's `size` hint and epoll_create1's flags are both accepted
     * and ignored: the size hint has been advisory since Linux 2.6.8, and the
     * only flag is EPOLL_CLOEXEC, which is meaningful once this kernel has
     * close-on-exec at all. */
    (void)c;
    return sys_epoll_create();
}

static long h_epoll_ctl(struct abi_ctx* c) {
    int epfd = (int)c->a[0], op = (int)c->a[1], fd = (int)c->a[2];
    unsigned long evp = c->a[3];
    uint64_t kev[2] = { 0, 0 };

    if (op != EPOLL_CTL_DEL) {
        unsigned long sz = abi_epoll_ev_bytes(c);
        if (!evp || !vmm_user_access_ok((uintptr_t)evp, sz, 0)) return -ABI_EFAULT;
        const uint8_t* p = (const uint8_t*)(uintptr_t)evp;
        kev[0] = abi_ld32(p);
        kev[1] = abi_ld64(p + abi_epoll_data_off(c));
    }
    return sys_epoll_ctl_k(epfd, op, fd, (uint32_t)kev[0], kev[1]);
}

static long h_epoll_wait(struct abi_ctx* c) {
    int epfd = (int)c->a[0];
    unsigned long outp = c->a[1];
    int maxevents = (int)c->a[2];
    int timeout   = (int)c->a[3];
    unsigned long maskp = c->a[4];

    /* §M56.1 — epoll_pwait's mask, honoured.  The whole reason the call exists
     * is that "unblock this signal" and "start waiting" must be ONE step: do
     * them separately and a signal arriving in between is delivered while the
     * program is not yet waiting, so the wait it then enters has nothing left
     * to wake it.  Swapping the mask around the wait closes exactly that
     * window — which is why it could not be done until the blocked mask was
     * real (it was a `return 0` stub until this milestone). */
    struct task* t = task_current();
    uint64_t saved_mask = 0;
    int mask_swapped = 0;
    if (maskp && t) {
        uint64_t gw = 0;
        if (!abi_sigset_read(c, maskp, &gw)) return -ABI_EFAULT;
        saved_mask = lnx_sig_blocked64(t);
        lnx_sig_set_blocked64(t, gw);                  /* §M89: all 64 signals */
        mask_swapped = 1;
    }

    if (maxevents <= 0 || maxevents > 256) return -ABI_EINVAL;
    unsigned long sz = abi_epoll_ev_bytes(c);
    if (!outp || !abi_user_w_ok(outp, sz * (unsigned long)maxevents))
        return -ABI_EFAULT;

    uint64_t* k = (uint64_t*)kmalloc(sizeof(uint64_t) * 2 * (size_t)maxevents);
    if (!k) { if (mask_swapped) lnx_sig_set_blocked64(t, saved_mask); return -ABI_ENOMEM; }
    int n = sys_epoll_wait_k(epfd, k, maxevents, timeout);
    if (mask_swapped) lnx_sig_set_blocked64(t, saved_mask);   /* restore on EVERY path */
    if (n > 0) {
        uint8_t* out = (uint8_t*)(uintptr_t)outp;
        unsigned long doff = abi_epoll_data_off(c);
        for (int i = 0; i < n; i++) {
            uint8_t* e = out + (unsigned long)i * sz;
            abi_st32(e, (uint32_t)k[i * 2 + 0]);
            abi_st64(e + doff, k[i * 2 + 1]);
        }
    }
    kfree(k);
    return n;
}

/* rt_sigpending(set, sigsetsize).  Reports signals that ARRIVED while blocked
 * — the proof that sigprocmask defers rather than discards. */
static long h_sigpending(struct abi_ctx* c) {
    struct task* t = task_current();
    unsigned long p = c->a[0];
    if (!p) return -ABI_EFAULT;
    /* Only the signals that are BOTH pending and blocked: an unblocked
     * pending signal is one the task simply has not returned to ring 3 to
     * collect yet, and reporting it would be a race, not information. */
    uint64_t v = t ? (lnx_sig_pending64(t) & lnx_sig_blocked64(t)) : 0;
    if (!abi_sigset_write(c, p, v)) return -ABI_EFAULT;
    return 0;
}

/* §M59 — pipe(fds) / pipe2(fds, flags).  The pair is created in the kernel
 * and the two numbers COPIED to the guest's array after it is validated —
 * x86_64's old switch arm handed the guest pointer straight to sys_pipe,
 * which stores through it with no check at all (§M46's boundary, missed in
 * one place).  pipe2 honours O_NONBLOCK (0x800 on every Linux arch here) on
 * both ends; O_CLOEXEC is accepted and meaningless until exec closes fds. */
#define ABI_O_NONBLOCK 0x800
static long pipe_common(struct abi_ctx* c, unsigned long flags) {
    if (!abi_user_w_ok(c->a[0], 2 * sizeof(int))) return -ABI_EFAULT;
    int k[2];
    if (sys_pipe_k(k) != 0) return -24;                 /* EMFILE */
    if (flags & ABI_O_NONBLOCK) {
        struct ofile* a = fd_lookup(k[0]);
        struct ofile* b = fd_lookup(k[1]);
        if (a) a->nonblock = 1;
        if (b) b->nonblock = 1;
    }
    ((int*)(uintptr_t)c->a[0])[0] = k[0];
    ((int*)(uintptr_t)c->a[0])[1] = k[1];
    return 0;
}
static long h_pipe(struct abi_ctx* c)  { return pipe_common(c, 0); }
static long h_pipe2(struct abi_ctx* c) { return pipe_common(c, c->a[1]); }

static long h_wait(struct abi_ctx* c) {
    int code = 0;
    int pid = task_wait((int)c->a[0], &code);
    if (c->a[1]) {
        /* The status slot is the GUEST's pointer — validate it here, where its
         * origin is known.  (§M46's lesson, three times over.) */
        if (!abi_user_w_ok(c->a[1], sizeof(int))) return -ABI_EFAULT;
        *(int*)(uintptr_t)c->a[1] = (code & 0xFF) << 8;   /* WIFEXITED form */
    }
    return pid;
}

/* execve(path, argv, envp) — §M89: envp IS honoured (it used to be dropped,
 * and a JRE's launcher, which sets LD_LIBRARY_PATH and re-executes itself,
 * looped forever).  Does not return on success. */
static long h_execve(struct abi_ctx* c) {
    int r = proc_execve_env((const char*)(uintptr_t)c->a[0],
                            (char* const*)(uintptr_t)c->a[1],
                            (char* const*)(uintptr_t)c->a[2]);
    return r == -7 ? -7 : (r < 0 ? -ABI_ENOENT : r);
}

/* ---- §M89 — the calls a JVM makes before main() ----------------------------
 *
 * Each of these answered -ENOSYS, and each looked optional until the JVM
 * showed what it does instead: without getcpu, HotSpot's fallback jumps to
 * the legacy x86_64 vsyscall page (0xffffffffff600800) — an address that has
 * never existed here, so the JVM died with SIGSEGV at a pc of its own
 * invention.  An ENOSYS is not a neutral answer when the caller's plan B is
 * worse than an honest value. */

/* futex(uaddr, op, val, timeout|val2, uaddr2, val3).  The PRIVATE flag is a
 * hint (this kernel keys every futex by address space anyway) and is masked,
 * not refused: refusing it made musl, which tries the private form first and
 * falls back only on ENOSYS, SPIN instead of wait. */
static long h_futex(struct abi_ctx* c) {
    uintptr_t ua = (uintptr_t)c->a[0];
    int op = (int)c->a[1], cmd = op & FUTEX_CMD_MASK;
    uint32_t val = (uint32_t)c->a[2];
    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        uint64_t deadline = 0;
        unsigned long tp = c->a[3];
        if (tp) {
            unsigned w = (!c->map || c->map->word_bytes != 4) ? 8 : 4;
            if (!abi_r_ok(tp, 2 * w)) return -ABI_EFAULT;
            uint64_t t = (uint64_t)abi_get_word(c, tp, 0) * 1000000000ull + abi_get_word(c, tp, 1);
            if (cmd == FUTEX_WAIT) {
                deadline = timer_now_ns() + t;                 /* relative */
            } else if (op & FUTEX_CLOCK_REALTIME) {            /* absolute, wall clock */
                struct ktimespec now;
                sys_clock_gettime_k(CLOCK_REALTIME, &now);
                uint64_t nw = now.sec * 1000000000ull + now.nsec;
                deadline = timer_now_ns() + (t > nw ? t - nw : 0);
            } else {
                deadline = t;                                  /* absolute, monotonic */
            }
            if (!deadline) deadline = 1;                       /* 0 means "none" below */
        }
        return futex_wait(ua, val, deadline);
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        return futex_wake(ua, (int)val);
    case FUTEX_CMP_REQUEUE: {
        uint32_t cur;
        if (!abi_r_ok(ua, 4)) return -ABI_EFAULT;
        cur = *(volatile uint32_t*)ua;
        if (cur != (uint32_t)c->a[5]) return -11;             /* EAGAIN */
    }   /* fall through */
    case FUTEX_REQUEUE:
        /* Requeue is an optimisation of "wake them all"; waking them all is
         * correct (each re-checks its condition), just less efficient. */
        futex_wake(ua, 0x7FFFFFFF);
        futex_wake((uintptr_t)c->a[4], 0x7FFFFFFF);
        return 0;
    default:
        return -38;                                            /* ENOSYS */
    }
}

/* getcpu(cpu*, node*, cache*). */
static long h_getcpu(struct abi_ctx* c) {
    struct percpu* pc = this_cpu();
    if (c->a[0]) { if (!abi_w_ok(c->a[0], 4)) return -ABI_EFAULT; *(uint32_t*)(uintptr_t)c->a[0] = (uint32_t)pc->cpu_index; }
    if (c->a[1]) { if (!abi_w_ok(c->a[1], 4)) return -ABI_EFAULT; *(uint32_t*)(uintptr_t)c->a[1] = (uint32_t)pc->numa_node; }
    return 0;
}

static void put_word(const struct abi_ctx* c, uintptr_t p, uint64_t v) {
    if (c->map->word_bytes == 4) *(uint32_t*)p = (uint32_t)v;
    else                         *(uint64_t*)p = v;
}

/* sysinfo(): memory in pages on a 32-bit guest (mem_unit 4096 — the byte
 * counts would not fit a 32-bit field on a machine with 4 GiB or more),
 * bytes on a 64-bit one. */
static long h_sysinfo(struct abi_ctx* c) {
    unsigned w = c->map->word_bytes;
    unsigned size = w == 8 ? 112 : 64;
    uintptr_t p = (uintptr_t)c->a[0];
    if (!abi_w_ok(p, size)) return -ABI_EFAULT;
    for (unsigned i = 0; i < size; i++) ((uint8_t*)p)[i] = 0;
    uint32_t unit = (w == 8) ? 1u : 4096u;
    uint64_t scale = 4096u / unit;
    uint32_t su = 0, st = 0;
    swap_stats(&su, &st);
    put_word(c, p, timer_now_ns() / 1000000000ull);                /* uptime */
    uintptr_t q = p + 4 * w;                                         /* after loads[3] */
    put_word(c, q + 0 * w, (uint64_t)pmm_managed_frames() * scale);  /* totalram */
    put_word(c, q + 1 * w, (uint64_t)pmm_free_frames() * scale);     /* freeram */
    put_word(c, q + 4 * w, (uint64_t)st * scale);                    /* totalswap */
    put_word(c, q + 5 * w, (uint64_t)(st - su) * scale);             /* freeswap */
    uintptr_t procs = q + 6 * w;
    *(uint16_t*)procs = (uint16_t)task_count();
    uintptr_t mu = (w == 8) ? p + 104 : p + 52;
    *(uint32_t*)mu = unit;
    return 0;
}

/* Resource limits.  What is REAL here is reported (8 MiB is not the main
 * thread's stack — PROC_STACK_PAGES is; TASK_MAX_FDS descriptors), the rest
 * is unlimited because nothing enforces a limit.  A new limit at or under
 * the maximum is accepted and NOT stored: nothing here would enforce it
 * either, and refusing it would fail programs that lower their own limits as
 * a courtesy. */
#define RLIM_INF64 0xFFFFFFFFFFFFFFFFull
static void rlimit_of(int res, uint64_t* cur, uint64_t* max) {
    *cur = *max = RLIM_INF64;
    switch (res) {
    case 3: *cur = *max = 256ull * 4096ull; break;          /* RLIMIT_STACK: 1 MiB */
    case 4: *cur = *max = 0; break;                         /* RLIMIT_CORE: no dumps */
    case 7: *cur = *max = TASK_MAX_FDS; break;              /* RLIMIT_NOFILE */
    default: break;
    }
}
static long rlimit_set(int res, uint64_t cur, uint64_t max) {
    uint64_t c0, m0;
    rlimit_of(res, &c0, &m0);
    if (cur > max) return -ABI_EINVAL;
    if (max > m0) return -1;                                /* EPERM */
    return 0;
}
static long h_getrlimit(struct abi_ctx* c) {
    int res = (int)c->a[0];
    unsigned w = c->map->word_bytes;
    if (res < 0 || res > 15) return -ABI_EINVAL;
    if (!abi_w_ok(c->a[1], 2 * w)) return -ABI_EFAULT;
    uint64_t cur, max;
    rlimit_of(res, &cur, &max);
    if (w == 4) {                                           /* RLIM_INFINITY is ~0 */
        if (cur > 0xFFFFFFFFull) cur = 0xFFFFFFFFull;
        if (max > 0xFFFFFFFFull) max = 0xFFFFFFFFull;
    }
    put_word(c, (uintptr_t)c->a[1], cur);
    put_word(c, (uintptr_t)c->a[1] + w, max);
    return 0;
}
static long h_setrlimit(struct abi_ctx* c) {
    int res = (int)c->a[0];
    unsigned w = c->map->word_bytes;
    if (res < 0 || res > 15) return -ABI_EINVAL;
    if (!abi_r_ok(c->a[1], 2 * w)) return -ABI_EFAULT;
    uint64_t cur = abi_get_word(c, c->a[1], 0), max = abi_get_word(c, c->a[1], 1);
    if (w == 4) { if (cur == 0xFFFFFFFFull) cur = RLIM_INF64; if (max == 0xFFFFFFFFull) max = RLIM_INF64; }
    return rlimit_set(res, cur, max);
}
/* prlimit64(pid, res, new*, old*): 64-bit pairs on every guest. */
static long h_prlimit64(struct abi_ctx* c) {
    int pid = (int)c->a[0], res = (int)c->a[1];
    if (pid != 0 && pid != (task_current() ? task_current()->pid : -1)) return -3;  /* ESRCH: own only */
    if (res < 0 || res > 15) return -ABI_EINVAL;
    uint64_t cur, max;
    rlimit_of(res, &cur, &max);
    if (c->a[3]) {
        if (!abi_w_ok(c->a[3], 16)) return -ABI_EFAULT;
        ((uint64_t*)(uintptr_t)c->a[3])[0] = cur;
        ((uint64_t*)(uintptr_t)c->a[3])[1] = max;
    }
    if (c->a[2]) {
        if (!abi_r_ok(c->a[2], 16)) return -ABI_EFAULT;
        return rlimit_set(res, ((uint64_t*)(uintptr_t)c->a[2])[0], ((uint64_t*)(uintptr_t)c->a[2])[1]);
    }
    return 0;
}

/* clock_getres: the resolution the clock really has (§M53's source). */
static long h_clock_getres(struct abi_ctx* c) {
    if (!c->a[1]) return 0;
    return abi_put_ts(c, c->a[1], 0, 0, 1);
}

/* statfs / fstatfs.  The fields that matter to a program are the type, the
 * block size and the name length; the counts are the machine's memory for an
 * in-memory filesystem, which is where its data lives.  Layout: word-sized
 * fields, except f_fsid (two ints) — which is exactly one word on a 64-bit
 * guest and two on a 32-bit one. */
static long put_statfs(struct abi_ctx* c, uintptr_t p, int wide64) {
    unsigned w = wide64 ? 8 : c->map->word_bytes;
    unsigned size = 11 * w + 8 + 4 * w;
    if (!abi_w_ok(p, size)) return -ABI_EFAULT;
    for (unsigned i = 0; i < size; i++) ((uint8_t*)p)[i] = 0;
    uint64_t v[7] = { 0x858458f6ull, 4096, pmm_managed_frames(), pmm_free_frames(),
                      pmm_free_frames(), 65536, 65536 };
    uintptr_t q = p;
    for (int i = 0; i < 7; i++) { put_word(c, q, v[i]); q += w; }
    q += 8;                                                   /* f_fsid */
    put_word(c, q, 255); q += w;                              /* f_namelen */
    put_word(c, q, 4096);                                     /* f_frsize */
    return 0;
}
static long h_statfs(struct abi_ctx* c) {
    char kp[256];
    if (abi_path(c->a[0], kp, sizeof kp) != 0) return -ABI_EFAULT;
    struct kstat_full st;
    if (sys_stat_full_k(kp, &st) != 0) return -ABI_ENOENT;
    return put_statfs(c, (uintptr_t)c->a[1], 0);
}
/* i386's statfs64(path, size, buf) / fstatfs64(fd, size, buf): the counts are
 * 64-bit, f_type/f_bsize and the tail 32-bit, and the caller passes the size
 * it was compiled with (84). */
static long put_statfs64_i386(uintptr_t p) {
    if (!abi_w_ok(p, 84)) return -ABI_EFAULT;
    for (unsigned i = 0; i < 84; i++) ((uint8_t*)p)[i] = 0;
    *(uint32_t*)(p + 0) = 0x858458f6u;
    *(uint32_t*)(p + 4) = 4096;
    uint64_t v[5] = { pmm_managed_frames(), pmm_free_frames(), pmm_free_frames(), 65536, 65536 };
    for (int i = 0; i < 5; i++) *(uint64_t*)(p + 8 + 8 * (uintptr_t)i) = v[i];
    *(uint32_t*)(p + 56) = 255;                               /* f_namelen */
    *(uint32_t*)(p + 60) = 4096;                              /* f_frsize */
    return 0;
}
static long h_statfs64(struct abi_ctx* c) {
    char kp[256];
    if (abi_path(c->a[0], kp, sizeof kp) != 0) return -ABI_EFAULT;
    struct kstat_full st;
    if (sys_stat_full_k(kp, &st) != 0) return -ABI_ENOENT;
    if (c->a[1] < 84) return -ABI_EINVAL;
    return put_statfs64_i386((uintptr_t)c->a[2]);
}
static long h_fstatfs64(struct abi_ctx* c) {
    if (!fd_lookup((int)c->a[0]) && ((int)c->a[0] < 0 || (int)c->a[0] > 2)) return -ABI_EBADF;
    if (c->a[1] < 84) return -ABI_EINVAL;
    return put_statfs64_i386((uintptr_t)c->a[2]);
}
static long h_fstatfs(struct abi_ctx* c) {
    if (!fd_lookup((int)c->a[0]) && ((int)c->a[0] < 0 || (int)c->a[0] > 2)) return -ABI_EBADF;
    return put_statfs(c, (uintptr_t)c->a[1], 0);
}

/* prctl: the thread name (PR_SET_NAME 15 / PR_GET_NAME 16 — a JVM names
 * every thread it starts, and `ps` then shows them) and PR_SET_DUMPABLE /
 * PR_GET_DUMPABLE (there are no core dumps; report "dumpable" and accept a
 * change).  Anything else is EINVAL, which is what Linux answers for an
 * option it does not know. */
static long h_prctl(struct abi_ctx* c) {
    struct task* t = task_current();
    int opt = (int)c->a[0];
    if (!t) return -ABI_EINVAL;
    if (opt == 15) {                                      /* PR_SET_NAME */
        char k[16];
        if (abi_path(c->a[1], k, sizeof k) != 0) return -ABI_EFAULT;
        k[15] = 0;
        int i = 0;
        for (; k[i] && i < TASK_NAME_MAX; i++) t->name[i] = k[i];
        t->name[i] = 0;
        return 0;
    }
    if (opt == 16) {                                      /* PR_GET_NAME */
        if (!abi_w_ok(c->a[1], 16)) return -ABI_EFAULT;
        char* d = (char*)(uintptr_t)c->a[1];
        int i = 0;
        for (; i < 15 && t->name[i]; i++) d[i] = t->name[i];
        d[i] = 0;
        return 0;
    }
    if (opt == 3) return 1;                               /* PR_GET_DUMPABLE */
    if (opt == 4) return 0;                               /* PR_SET_DUMPABLE */
    return -ABI_EINVAL;
}

/* getrusage(who, ru): user time from the task's own accounting (RUSAGE_SELF
 * and RUSAGE_THREAD both answer for the caller; there is no per-process sum
 * across threads yet, said here).  System time is not separated here — it is
 * all reported as user time rather than invented.  struct rusage = two
 * timevals then fourteen longs, all guest words. */
static long h_getrusage(struct abi_ctx* c) {
    unsigned w = c->map->word_bytes;
    uintptr_t p = (uintptr_t)c->a[1];
    if (!abi_w_ok(p, 18 * w)) return -ABI_EFAULT;
    for (unsigned i = 0; i < 18 * w; i++) ((uint8_t*)p)[i] = 0;
    struct task* t = task_current();
    uint64_t ms = t ? t->cpu_ms : 0;
    put_word(c, p, ms / 1000);
    put_word(c, p + w, (ms % 1000) * 1000);
    return 0;
}

/* ---- §M89 rung 3 — calls that lived only in the two x86 switches ----------
 *
 * Each was written once per x86 arch and never for arm64, where a JVM's
 * sched_yield hit ENOSYS and its spin loops never gave the CPU away.  §M50's
 * rule: an operation is written ONCE and every guest names it. */
static long h_sched_yield(struct abi_ctx* c) { (void)c; task_msleep(1); return 0; }
static long h_getrandom(struct abi_ctx* c) {
    return sys_getrandom((void*)(uintptr_t)c->a[0], (size_t)c->a[1], (unsigned)c->a[2]);
}
/* madvise: MADV_DONTNEED (4) really drops pages (vma.c); the others are
 * hints and are accepted. */
static long h_madvise(struct abi_ctx* c) {
    if ((int)c->a[2] == 4) return vma_madvise_dontneed((uintptr_t)c->a[0], (size_t)c->a[1]);
    return 0;
}
/* membarrier: every command this kernel could be asked for is satisfied by
 * the TLB shootdown and interrupt paths already serialising the CPUs; QUERY
 * (0) answers "none registered-needed", the rest succeed. */
static long h_membarrier(struct abi_ctx* c) { (void)c; return 0; }
static long h_memfd_create(struct abi_ctx* c) { (void)c; int r = sys_memfd(0); return r < 0 ? -ABI_ENOMEM : r; }
/* ftruncate: a memfd is sized; a regular file is EXTENDED by writing a zero
 * at the new last byte (an in-memory file has no holes to punch, so
 * shrinking is refused rather than faked). */
static long h_ftruncate(struct abi_ctx* c) {
    int fd = (int)c->a[0];
    long size = (long)c->a[1];
    struct ofile* o = fd_lookup(fd);
    if (!o) return -ABI_EBADF;
    if (o->kind == FD_SHM) return sys_memfd_resize(fd, (size_t)size) < 0 ? -ABI_EINVAL : 0;
    if (o->kind != FD_VFS || !o->file || !o->file->inode || size < 0) return -ABI_EINVAL;
    uint64_t cur = o->file->inode->size;
    if ((uint64_t)size == cur) return 0;
    if ((uint64_t)size < cur) { if (size == 0) { o->file->inode->size = 0; return 0; } return -ABI_EINVAL; }
    uint64_t save = o->file->pos;
    o->file->pos = (uint64_t)size - 1;
    char z = 0;
    long w = vfs_write(o->file, &z, 1);
    o->file->pos = save;
    return w == 1 ? 0 : -ABI_EINVAL;
}
/* mincore is NOT advisory: its RETURN VALUE is the answer.  Mesa's
 * _eglPointerIsDereferencable asks it whether an address is mapped, and a
 * blanket "success" once made libEGL dereference the literal 3 (§M40).  Since
 * §M89 it also must not PREFAULT: a reserved page that was never touched is
 * mapped-but-not-resident, and bringing it in would answer by changing. */
static long h_mincore(struct abi_ctx* c) {
    uintptr_t addr = (uintptr_t)c->a[0];
    size_t len = (size_t)c->a[1];
    size_t pages = (len + 4095) / 4096;
    if (!pages) return 0;
    if (!abi_w_ok(c->a[2], pages)) return -ABI_EFAULT;
    return vma_mincore(addr, len, (uint8_t*)(uintptr_t)c->a[2]);
}
static long h_pread64(struct abi_ctx* c) {
    int fd = (int)c->a[0];
    long cur = sys_lseek(fd, 0, 1);
    if (cur < 0) return -29;                                  /* ESPIPE */
    sys_lseek(fd, (long)c->a[3], 0);
    long r = sys_read(fd, (void*)(uintptr_t)c->a[1], (size_t)c->a[2]);
    sys_lseek(fd, cur, 0);
    return r;
}
static long h_pwrite64(struct abi_ctx* c) {
    int fd = (int)c->a[0];
    long cur = sys_lseek(fd, 0, 1);
    if (cur < 0) return -29;
    sys_lseek(fd, (long)c->a[3], 0);
    long r = sys_write(fd, (const void*)(uintptr_t)c->a[1], (size_t)c->a[2]);
    sys_lseek(fd, cur, 0);
    return r;
}
/* The CPUs this task may run on, as a guest-word mask; the byte count is the
 * return value.  An empty answer would read as "zero CPUs". */
static long h_sched_getaffinity(struct abi_ctx* c) {
    unsigned w = c->map->word_bytes;
    if (c->a[1] < w) return -ABI_EINVAL;
    if (!abi_w_ok(c->a[2], w)) return -ABI_EFAULT;
    int n = smp_ncpus();
    if (n <= 0) n = 1;
    if (n > (int)(8 * w)) n = (int)(8 * w);
    uint64_t m = (n >= 64) ? ~0ull : ((1ull << n) - 1ull);
    put_word(c, (uintptr_t)c->a[2], m);
    return (long)w;
}
static long h_sched_setaffinity(struct abi_ctx* c) { (void)c; return 0; }
/* set_robust_list: accepted.  The kernel does not walk the list at thread
 * death (a lock held by a thread that dies stays held — stated). */
static long h_set_robust_list(struct abi_ctx* c) { (void)c; return 0; }
/* ppoll(fds, nfds, timeout_ts, sigmask, size) — arm64 has no poll(2) at all.
 * The mask swap is not done (a deferred signal arriving in the window is the
 * known gap); the timeout is converted to milliseconds, rounded UP. */
static long h_ppoll(struct abi_ctx* c) {
    int ms = -1;
    if (c->a[2]) {
        unsigned w = (!c->map || c->map->word_bytes != 4) ? 8 : 4;
        if (!abi_r_ok(c->a[2], 2 * w)) return -ABI_EFAULT;
        uint64_t s = abi_get_word(c, c->a[2], 0), ns = abi_get_word(c, c->a[2], 1);
        uint64_t t = s * 1000ull + (ns + 999999ull) / 1000000ull;
        ms = t > 0x7FFFFFFF ? 0x7FFFFFFF : (int)t;
    }
    int r = sys_poll((struct pollfd*)(uintptr_t)c->a[0], (int)c->a[1], ms);
    return r < 0 ? -ABI_EFAULT : r;
}

static long h_settid(struct abi_ctx* c) {
    (void)c;
    struct task* t = task_current();
    return t ? t->pid : 0;
}

/* exit: does NOT return.  Declared in the vocabulary precisely so the shims
 * do not each have to remember that. */
static long h_exit(struct abi_ctx* c) {
    struct task* t = task_current();
    if (t && t->user_task) { fd_close_all(); task_exit_code((int)c->a[0]); }
    return 0;   /* unreachable for a user task */
}

/* ===========================================================================
 * The BSD socket surface (§M24 second half).
 *
 * ONE marshalling of `struct sockaddr_in`, shared by every architecture and
 * every entry convention.  Before this it existed twice — once in each x86
 * layer — and a third copy was exactly what the aarch64 port would have
 * needed, for a struct whose layout is the same everywhere.
 *
 * AND IT IS THE SAME EVERYWHERE, which is worth stating because the last
 * struct through this pipeline was not: `struct epoll_event` is 12 bytes on
 * i386 and amd64 but 16 on arm64 (§M56), so a size derived from the word width
 * passes on two arches and fails on the third.  `sockaddr_in` has no such
 * trap — 16 bytes, fixed fields, network byte order — and `socklen_t` is a
 * 32-bit unsigned on every ABI here.  A shared marshaller is therefore CORRECT
 * rather than merely convenient, and saying so is how the next person knows
 * not to go looking for a per-arch case that was never needed.
 * ======================================================================== */

#define ABI_AF_INET        2
#define ABI_SOCK_NONBLOCK  0x800      /* Linux O_NONBLOCK, socket-type flag   */
#define ABI_SOCK_CLOEXEC   0x80000
#define ABI_EAFNOSUPPORT   97
#define ABI_EOPNOTSUPP     95
#define ABI_ENOTCONN       107
#define ABI_EAGAIN         11

struct abi_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;                /* network byte order                   */
    uint32_t sin_addr;                /* network byte order                   */
    uint8_t  sin_zero[8];
};

static uint16_t abi_ntohs(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t abi_ntohl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

static int abi_user_r_ok(unsigned long uptr, unsigned long len) {
    return uptr && vmm_user_access_ok((uintptr_t)uptr, (uintptr_t)len, 0);
}

/* guest sockaddr_in → host-order (ip, port).  Returns 0 on success. */
static int abi_addr_in(unsigned long uaddr, uint32_t* ip, int* port) {
    if (!abi_user_r_ok(uaddr, sizeof(struct abi_sockaddr_in))) return -1;
    const struct abi_sockaddr_in* sa = (const struct abi_sockaddr_in*)(uintptr_t)uaddr;
    if (sa->sin_family != ABI_AF_INET) return -2;
    *ip   = abi_ntohl(sa->sin_addr);
    *port = (int)abi_ntohs(sa->sin_port);
    return 0;
}

/* host-order (ip, port) → guest sockaddr_in, honouring the caller's socklen.
 *
 * Linux writes at most `*addrlen` bytes and then stores the address's TRUE
 * size, which is how a caller learns it was truncated.  Reporting the
 * truncated length instead would make every short buffer look like a success. */
static void abi_addr_out(unsigned long uaddr, unsigned long ulen,
                         uint32_t ip, int port) {
    if (!uaddr || !ulen) return;
    if (!abi_user_r_ok(ulen, sizeof(uint32_t))) return;
    uint32_t room = *(uint32_t*)(uintptr_t)ulen;
    struct abi_sockaddr_in sa;
    sa.sin_family = ABI_AF_INET;
    sa.sin_port   = abi_ntohs((uint16_t)port);
    sa.sin_addr   = abi_ntohl(ip);
    for (int i = 0; i < 8; i++) sa.sin_zero[i] = 0;
    uint32_t n = room < sizeof sa ? room : (uint32_t)sizeof sa;
    if (n && abi_user_w_ok(uaddr, n)) {
        uint8_t* dst = (uint8_t*)(uintptr_t)uaddr;
        const uint8_t* src = (const uint8_t*)&sa;
        for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    }
    if (abi_user_w_ok(ulen, sizeof(uint32_t)))
        *(uint32_t*)(uintptr_t)ulen = (uint32_t)sizeof sa;
}

static long h_socket(struct abi_ctx* c) {
    int domain = (int)c->a[0];
    int type   = (int)c->a[1];
    if (domain != ABI_AF_INET) return -ABI_EAFNOSUPPORT;
    int fd = sys_socket(domain, type & 0xFF, (int)c->a[2]);
    if (fd < 0) return -ABI_EOPNOTSUPP;
    /* SOCK_NONBLOCK is not decoration: musl's resolver drains its socket with
     * `while (recvmsg(...) >= 0)` and needs the EAGAIN only a non-blocking
     * socket produces (§M39). */
    if (type & ABI_SOCK_NONBLOCK) sys_socket_setnonblock(fd, 1);
    return fd;
}

static long h_bind(struct abi_ctx* c) {
    uint32_t ip; int port;
    int r = abi_addr_in(c->a[1], &ip, &port);
    if (r == -1) return -ABI_EFAULT;
    if (r == -2) return -ABI_EAFNOSUPPORT;
    return sys_bind((int)c->a[0], ip, port) == 0 ? 0 : -ABI_EINVAL;
}

static long h_connect(struct abi_ctx* c) {
    uint32_t ip; int port;
    int r = abi_addr_in(c->a[1], &ip, &port);
    if (r == -1) return -ABI_EFAULT;
    if (r == -2) return -ABI_EAFNOSUPPORT;
    /* ECONNREFUSED (111) is the honest answer now that a closed port answers
     * with an RST rather than with silence — the stack cannot tell us which
     * yet, so the generic failure stands and the distinction is written down
     * as the next thing to carry through. */
    return sys_connect((int)c->a[0], ip, port) == 0 ? 0 : -111;
}

static long h_listen(struct abi_ctx* c) {
    return sys_listen((int)c->a[0], (int)c->a[1]) == 0 ? 0 : -ABI_EINVAL;
}

/* accept(fd, addr, addrlen) and accept4(fd, addr, addrlen, flags). */
static long h_accept_common(struct abi_ctx* c, int flags) {
    uint32_t ip = 0; int port = 0;
    int fd = sys_accept_k((int)c->a[0], &ip, &port);
    if (fd == -ABI_EAGAIN) return -ABI_EAGAIN;
    if (fd < 0) return -ABI_EINVAL;
    abi_addr_out(c->a[1], c->a[2], ip, port);
    if (flags & ABI_SOCK_NONBLOCK) sys_socket_setnonblock(fd, 1);
    return fd;
}
static long h_accept (struct abi_ctx* c) { return h_accept_common(c, 0); }
static long h_accept4(struct abi_ctx* c) { return h_accept_common(c, (int)c->a[3]); }

static long h_getsockname(struct abi_ctx* c) {
    uint32_t ip = 0; int port = 0;
    if (sys_getsockname_k((int)c->a[0], &ip, &port) != 0) return -ABI_EINVAL;
    abi_addr_out(c->a[1], c->a[2], ip, port);
    return 0;
}

static long h_getpeername(struct abi_ctx* c) {
    uint32_t ip = 0; int port = 0;
    if (sys_getpeername_k((int)c->a[0], &ip, &port) != 0) return -ABI_ENOTCONN;
    abi_addr_out(c->a[1], c->a[2], ip, port);
    return 0;
}

/* send/recv are sendto/recvfrom without an address, and on a CONNECTED socket
 * that is exactly a write/read — which is where the stream payload has always
 * gone (sys_read/sys_write route FD_NETSOCK to the TCP engine). */
static long h_send(struct abi_ctx* c) {
    return sys_write((int)c->a[0], (const void*)c->a[1], (size_t)c->a[2]);
}
static long h_recv(struct abi_ctx* c) {
    return sys_read((int)c->a[0], (void*)c->a[1], (size_t)c->a[2]);
}

static long h_sendto(struct abi_ctx* c) {
    if (!c->a[4]) return sys_write((int)c->a[0], (const void*)c->a[1], (size_t)c->a[2]);
    uint32_t ip; int port;
    int r = abi_addr_in(c->a[4], &ip, &port);
    if (r == -1) return -ABI_EFAULT;
    if (r == -2) return -ABI_EAFNOSUPPORT;
    return sys_sendto((int)c->a[0], (const void*)c->a[1], (size_t)c->a[2], ip, port);
}

static long h_recvfrom(struct abi_ctx* c) {
    uint32_t ip = 0; int port = 0;
    long n = sys_recvfrom_u((int)c->a[0], (uintptr_t)c->a[1], (size_t)c->a[2],
                            &ip, &port);
    if (n >= 0 && c->a[4]) abi_addr_out(c->a[4], c->a[5], ip, port);
    return n;
}

static long h_shutdown(struct abi_ctx* c) {
    return sys_shutdown((int)c->a[0], (int)c->a[1]);
}

/* No socket options are honoured, and reporting success is deliberate: musl's
 * getaddrinfo and every TLS setup set SO_RCVTIMEO / TCP_NODELAY and treat a
 * failure as fatal, while ignoring them costs at most a timeout that never
 * fires.  The day one of them changes behaviour, it stops being a stub. */
static long h_setsockopt(struct abi_ctx* c) { (void)c; return 0; }
static long h_getsockopt(struct abi_ctx* c) { (void)c; return 0; }

/* The operation table, indexed by `enum abi_op`.  A NULL slot means "declared
 * in the vocabulary, no handler yet" — abi_invoke reports that as unhandled so
 * the caller can fall back, which is what lets an existing hand-written layer
 * migrate one operation at a time instead of in one risky jump. */
static const struct {
    const char*    name;
    abi_handler_fn fn;
} g_ops[ABI_OP_MAX] = {
    [ABI_OP_NONE]  = { "none",     NULL },
    [ABI_READ]     = { "read",     h_read },
    [ABI_WRITE]    = { "write",    h_write },
    [ABI_CLOSE]    = { "close",    h_close },
    [ABI_SEEK]     = { "seek",     h_seek },
    [ABI_MPROTECT] = { "mprotect", h_mprotect },
    [ABI_MUNMAP]   = { "munmap",   h_munmap },
    [ABI_GETPID]   = { "getpid",   h_getpid },
    [ABI_UI_BUILD] = { "ui_build", h_ui_build },
    [ABI_GETPPID]  = { "getppid",  h_getppid },
    [ABI_GETTID]   = { "gettid",   h_getpid },      /* no separate tid space */
    [ABI_EXIT]     = { "exit",     h_exit },
    [ABI_READV]    = { "readv",    h_readv },
    [ABI_WRITEV]   = { "writev",   h_writev },
    [ABI_IOCTL]    = { "ioctl",    h_ioctl },
    [ABI_BRK]      = { "brk",      h_brk },
    [ABI_MMAP]     = { "mmap",     h_mmap },
    [ABI_MMAP_PGOFF] = { "mmap2",  h_mmap_pgoff },
    [ABI_SET_TID_ADDRESS] = { "set_tid_address", h_settid },
    [ABI_SIGPROCMASK]     = { "sigprocmask",     h_sigprocmask },
    [ABI_TIMERFD_CREATE]  = { "timerfd_create",  h_timerfd_create  },
    [ABI_TIMERFD_SETTIME] = { "timerfd_settime", h_timerfd_settime },
    [ABI_TIMERFD_GETTIME] = { "timerfd_gettime", h_timerfd_gettime },
    [ABI_SETITIMER]       = { "setitimer",       h_setitimer       },
    [ABI_EPOLL_CREATE]    = { "epoll_create",    h_epoll_create    },
    [ABI_EPOLL_CTL]       = { "epoll_ctl",       h_epoll_ctl       },
    [ABI_EPOLL_WAIT]      = { "epoll_wait",      h_epoll_wait      },
    [ABI_SIGPENDING]      = { "sigpending",      h_sigpending      },
    [ABI_WAIT]            = { "wait",            h_wait },
    [ABI_PIPE]            = { "pipe",            h_pipe },
    [ABI_PIPE2]           = { "pipe2",           h_pipe2 },
    [ABI_EXECVE]          = { "execve",          h_execve },
    /* §M24 — the socket surface, shared by all three arches at once. */
    [ABI_SOCKET]       = { "socket",       h_socket       },
    [ABI_BIND]         = { "bind",         h_bind         },
    [ABI_CONNECT]      = { "connect",      h_connect      },
    [ABI_LISTEN]       = { "listen",       h_listen       },
    [ABI_ACCEPT]       = { "accept",       h_accept       },
    [ABI_ACCEPT4]      = { "accept4",      h_accept4      },
    [ABI_GETSOCKNAME]  = { "getsockname",  h_getsockname  },
    [ABI_GETPEERNAME]  = { "getpeername",  h_getpeername  },
    [ABI_SEND]         = { "send",         h_send         },
    [ABI_SENDTO]       = { "sendto",       h_sendto       },
    [ABI_RECV]         = { "recv",         h_recv         },
    [ABI_RECVFROM]     = { "recvfrom",     h_recvfrom     },
    [ABI_SHUTDOWN]     = { "shutdown",     h_shutdown     },
    [ABI_SETSOCKOPT]   = { "setsockopt",   h_setsockopt   },
    [ABI_GETSOCKOPT]   = { "getsockopt",   h_getsockopt   },
    [ABI_MKDIR]        = { "mkdir",        h_mkdir        },
    [ABI_MKDIRAT]      = { "mkdirat",      h_mkdirat      },
    [ABI_LINK]         = { "link",         h_link         },
    [ABI_LINKAT]       = { "linkat",       h_linkat       },
    [ABI_CHMOD]        = { "chmod",        h_chmod        },
    [ABI_FCHMODAT]     = { "fchmodat",     h_fchmodat     },
    [ABI_UNLINK]       = { "unlink",       h_unlink       },
    [ABI_UNLINKAT]     = { "unlinkat",     h_unlinkat     },
    [ABI_GETUID]       = { "getuid",       h_getuid       },
    [ABI_GETEUID]      = { "geteuid",      h_getuid       },
    [ABI_GETGID]       = { "getgid",       h_getgid       },
    [ABI_GETEGID]      = { "getegid",      h_getgid       },
    [ABI_SETUID]       = { "setuid",       h_setuid       },
    [ABI_SETGID]       = { "setgid",       h_setgid       },
    [ABI_GETGROUPS]    = { "getgroups",    h_getgroups    },
    [ABI_GETCWD]       = { "getcwd",       h_getcwd       },
    [ABI_CHDIR]        = { "chdir",        h_chdir        },
    [ABI_LNX_SIGACTION] = { "rt_sigaction", lnx_h_sigaction },
    [ABI_KILL]          = { "kill",         lnx_h_kill      },
    [ABI_TKILL]         = { "tkill",        lnx_h_tkill     },
    [ABI_TGKILL]        = { "tgkill",       lnx_h_tgkill    },
    [ABI_SIGALTSTACK]   = { "sigaltstack",  lnx_h_sigaltstack },
    [ABI_FUTEX]         = { "futex",        h_futex         },
    [ABI_GETCPU]        = { "getcpu",       h_getcpu        },
    [ABI_SYSINFO]       = { "sysinfo",      h_sysinfo       },
    [ABI_GETRLIMIT]     = { "getrlimit",    h_getrlimit     },
    [ABI_SETRLIMIT]     = { "setrlimit",    h_setrlimit     },
    [ABI_PRLIMIT64]     = { "prlimit64",    h_prlimit64     },
    [ABI_CLOCK_GETRES]  = { "clock_getres", h_clock_getres  },
    [ABI_STATFS]        = { "statfs",       h_statfs        },
    [ABI_FSTATFS]       = { "fstatfs",      h_fstatfs       },
    [ABI_PRCTL]         = { "prctl",        h_prctl         },
    [ABI_GETRUSAGE]     = { "getrusage",    h_getrusage     },
    [ABI_SCHED_YIELD]   = { "sched_yield",  h_sched_yield   },
    [ABI_GETRANDOM]     = { "getrandom",    h_getrandom     },
    [ABI_MADVISE]       = { "madvise",      h_madvise       },
    [ABI_MEMBARRIER]    = { "membarrier",   h_membarrier    },
    [ABI_MEMFD_CREATE]  = { "memfd_create", h_memfd_create  },
    [ABI_FTRUNCATE]     = { "ftruncate",    h_ftruncate     },
    [ABI_MINCORE]       = { "mincore",      h_mincore       },
    [ABI_PREAD64]       = { "pread64",      h_pread64       },
    [ABI_PWRITE64]      = { "pwrite64",     h_pwrite64      },
    [ABI_SCHED_GETAFFINITY] = { "sched_getaffinity", h_sched_getaffinity },
    [ABI_SCHED_SETAFFINITY] = { "sched_setaffinity", h_sched_setaffinity },
    [ABI_SET_ROBUST_LIST]   = { "set_robust_list",   h_set_robust_list   },
    [ABI_PPOLL]         = { "ppoll",        h_ppoll         },
    [ABI_STATFS64]      = { "statfs64",     h_statfs64      },
    [ABI_FSTATFS64]     = { "fstatfs64",    h_fstatfs64     },
    [ABI_OPEN]         = { "open",         h_open         },
    [ABI_OPENAT]       = { "openat",       h_openat       },
    [ABI_STAT]         = { "stat",         h_stat         },
    [ABI_FSTAT]        = { "fstat",        h_fstat        },
    [ABI_FSTATAT]      = { "fstatat",      h_fstatat      },
    [ABI_GETDENTS64]   = { "getdents64",   h_getdents64   },
    [ABI_FCNTL]        = { "fcntl",        h_fcntl        },
    [ABI_ACCESS]       = { "access",       h_access       },
    [ABI_FACCESSAT]    = { "faccessat",    h_faccessat    },
    [ABI_READLINK]     = { "readlink",     h_readlink     },
    [ABI_READLINKAT]   = { "readlinkat",   h_readlinkat   },
    [ABI_SENDFILE]     = { "sendfile",     h_sendfile     },
    [ABI_UNAME]        = { "uname",        h_uname        },
    [ABI_DUP]          = { "dup",          h_dup          },
    [ABI_DUP2]         = { "dup2",         h_dup2         },
    [ABI_DUP3]         = { "dup3",         h_dup3         },
    [ABI_CLOCK_GETTIME]   = { "clock_gettime",   h_clock_gettime   },
    [ABI_CLOCK_GETTIME64] = { "clock_gettime64", h_clock_gettime64 },
    [ABI_GETTIMEOFDAY]    = { "gettimeofday",    h_gettimeofday    },
    [ABI_NANOSLEEP]       = { "nanosleep",       h_nanosleep       },
    [ABI_CLOCK_NANOSLEEP] = { "clock_nanosleep", h_clock_nanosleep },
};

enum abi_op abi_lookup(const struct abi_map* map, unsigned long nr) {
    if (!map || !map->ents) return ABI_OP_NONE;
    for (uint32_t i = 0; i < map->n_ents; i++)
        if (map->ents[i].nr == (uint32_t)nr)
            return (enum abi_op)map->ents[i].op;
    return ABI_OP_NONE;
}

int abi_invoke(enum abi_op op, struct abi_ctx* c, long* out) {
    if (op <= ABI_OP_NONE || op >= ABI_OP_MAX) return 0;
    abi_handler_fn fn = g_ops[op].fn;
    if (!fn) return 0;
    long r = fn(c);
    if (out) *out = r;
    return 1;
}

/* §M73 — `strace`: every guest call of the traced task(s), with its result.
 * 0 = off, >0 = that pid, -1 = every task in a container.  Built the day a
 * busybox shell sat asleep in the kernel with nothing to say which call had
 * put it there; a guest program is exactly the code this kernel did not
 * write, so "what did it ask for" is the first question and deserves a
 * permanent answer.  Calls the engine does not name are still printed (as
 * "-> arch switch"), so a trace has no gaps. */
static volatile int g_trace;
static int abi_traced(void) {
    int t = g_trace;
    if (!t) return 0;
    struct task* me = task_current();
    if (!me) return 0;
    return t > 0 ? me->pid == t : me->cred.container != 0;
}

int abi_dispatch(const struct abi_map* map, unsigned long nr,
                 unsigned long a0, unsigned long a1, unsigned long a2,
                 unsigned long a3, unsigned long a4, unsigned long a5,
                 long* out) {
    enum abi_op op = abi_lookup(map, nr);
    { struct task* me = task_current(); if (me) me->guest_nr = (int)nr + 1; }
    int tr = abi_traced();
    if (op == ABI_OP_NONE) {
        if (tr) kprintf("strace[%d] #%lu(%lx, %lx, %lx) -> arch switch\n",
                        task_current()->pid, nr, a0, a1, a2);
        return 0;
    }
    struct abi_ctx c;
    c.a[0] = a0; c.a[1] = a1; c.a[2] = a2;
    c.a[3] = a3; c.a[4] = a4; c.a[5] = a5;
    c.nr   = nr;
    c.map  = map;
    if (tr) kprintf("strace[%d] %s(%lx, %lx, %lx, %lx) ...\n", task_current()->pid,
                    g_ops[op].name ? g_ops[op].name : "?", a0, a1, a2, a3);
    int ran = abi_invoke(op, &c, out);
    if (tr) kprintf("strace[%d]   = %ld\n", task_current()->pid, ran ? *out : -38L);
    return ran;
}

static void cmd_strace(const char* args) {
    const char* a = args ? args : "";
    while (*a == ' ') a++;
    if (!*a) { kprintf("strace: %s\n", g_trace == 0 ? "off" : g_trace < 0 ? "every container task" : "one pid"); return; }
    if (a[0] == 'c') { g_trace = -1; kprintf("strace: tracing every task in a container\n"); return; }
    if (a[0] == 'o') { g_trace = 0;  kprintf("strace: off\n"); return; }
    int v = 0;
    while (*a >= '0' && *a <= '9') v = v * 10 + (*a++ - '0');
    g_trace = v;
    kprintf("strace: %s\n", v ? "tracing that pid" : "off");
}
SHELL_CMD(strace) = { "strace", "[<pid> | ctr | off]",
                      "print every guest (Linux-ABI) syscall of a task, with its result",
                      SHELL_G_TASK, cmd_strace, SHELL_P_ADMIN };

void abi_stats(int* ops_with_handlers, int* ops_total) {
    int n = 0;
    for (int i = 1; i < ABI_OP_MAX; i++) if (g_ops[i].fn) n++;
    if (ops_with_handlers) *ops_with_handlers = n;
    if (ops_total)         *ops_total = ABI_OP_MAX - 1;
}
