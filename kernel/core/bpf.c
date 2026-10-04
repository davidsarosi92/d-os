/* =============================================================================
 * bpf.c — eBPF, as much as cgroup DEVICE control needs (§M90).
 *
 * WHY.  On cgroup v2 a container's device policy is not a file: runc compiles
 * the allow-list into an eBPF program (BPF_PROG_TYPE_CGROUP_DEVICE) and
 * attaches it to the container's cgroup; the kernel runs it on every device
 * access and refuses what it rejects.  Without bpf(2) runc stops ("bpf:
 * function not implemented") — and pretending would be worse: a container
 * whose device list is accepted and not enforced may open any device node it
 * can reach.  So this is the real thing, sized to that one use.
 *
 * WHAT IS SUPPORTED, AND WHAT IS REFUSED (EINVAL, named on the console):
 *   programs   type CGROUP_DEVICE only; at most 4096 instructions; the
 *              classes ALU/ALU64, JMP/JMP32 (every comparison), LDX (from
 *              the context only), LD_IMM64 and EXIT.  No helper CALLs, no
 *              stores, no stack, no maps (BPF_MAP_CREATE says EINVAL, which
 *              is how libraries learn a feature is missing).
 *   commands   PROG_LOAD, PROG_ATTACH (BPF_F_ALLOW_MULTI, BPF_F_REPLACE),
 *              PROG_DETACH, PROG_QUERY, PROG_GET_FD_BY_ID,
 *              OBJ_GET_INFO_BY_FD.
 *
 * SAFETY WITHOUT A FULL VERIFIER.  Linux proves a program safe before running
 * it; this kernel makes the same guarantees by construction instead:
 *   - termination: every jump must go FORWARD (checked at load), so the
 *     program counter only grows — at most n steps, ever;
 *   - memory: the only load is from the context, and it is checked AT RUN
 *     TIME — the context pointer is a tagged value no arithmetic can forge
 *     into anything but itself, and the offset must lie inside the 12-byte
 *     context; anything else ends the program as a REFUSAL;
 *   - falling off the end, dividing by zero, an unknown opcode: refusal.
 * A program that is refused at run time therefore denies the access — the
 * safe direction for a security policy.
 *
 * ENFORCEMENT.  bpf_devcg_allowed(): every program attached to the task's
 * cgroup and to each ancestor must return non-zero (Linux's ALLOW_MULTI
 * semantics).  Called on every open of a device node by a Linux process, and
 * for mknod.  Device numbers come from devfs (devfs_devnum).
 * ============================================================================= */

#include "bpf.h"
#include "fd.h"
#include "kmalloc.h"
#include "lock.h"
#include "task.h"
#include "printf.h"
#include "vfs.h"
#include "uaccess.h"
#include "vmm.h"
#include <stdint.h>
#include <stddef.h>

#define BPF_MAXI        4096
#define BPF_MAX_ATTACH  64      /* Linux's own per-cgroup limit for device programs */
#define BPF_PROG_TYPE_CGROUP_DEVICE 15
#define BPF_CGROUP_DEVICE           6
#define BPF_F_ALLOW_MULTI 2u
#define BPF_F_REPLACE     4u

struct insn { uint8_t code; uint8_t regs; int16_t off; int32_t imm; };

struct bpf_prog {
    int          refs;
    uint32_t     id;
    uint32_t     n;
    struct insn* in;
    char         name[16];
    uint8_t      tag[8];
};

static spinlock_t g_bpf_lock = SPINLOCK_INIT;     /* ids, refs, attachments */
static uint32_t   g_next_id = 1;

/* Every loaded program that still has a reference, for GET_FD_BY_ID. */
#define BPF_MAX_PROGS 256
static struct bpf_prog* g_progs[BPF_MAX_PROGS];

/* Attachments: (cgroup, program) pairs. */
struct attach { void* cg; struct bpf_prog* p; };
static struct attach g_att[BPF_MAX_PROGS];
static int g_natt;

/* cgroupfs.c */
void* cgroupfs_of_inode(struct inode* in);
void* cgroupfs_parent(void* cg);
void* cgroupfs_task_node(const struct task* t);

void bpf_prog_ref_get(struct bpf_prog* p) {
    uint32_t f = spin_lock_irqsave(&g_bpf_lock);
    p->refs++;
    spin_unlock_irqrestore(&g_bpf_lock, f);
}
void bpf_prog_put(struct bpf_prog* p) {
    if (!p) return;
    uint32_t f = spin_lock_irqsave(&g_bpf_lock);
    int last = --p->refs == 0;
    if (last) for (int i = 0; i < BPF_MAX_PROGS; i++) if (g_progs[i] == p) g_progs[i] = NULL;
    spin_unlock_irqrestore(&g_bpf_lock, f);
    if (last) { kfree(p->in); kfree(p); }
}

/* ---- the verifier we need: opcodes we run, jumps that go forward ------------ */

static int known_alu(uint8_t op) { return op <= 0xc0; }        /* ADD … ARSH */
static int verify(const struct insn* in, uint32_t n, const char** why) {
    for (uint32_t pc = 0; pc < n; pc++) {
        uint8_t code = in[pc].code, cls = code & 7;
        uint8_t dst = in[pc].regs & 0xf, src = in[pc].regs >> 4;
        if (dst > 10 || src > 10) { *why = "register out of range"; return -1; }
        switch (cls) {
        case 4: case 7:                                  /* ALU, ALU64 */
            if (!known_alu(code & 0xf0)) { *why = "unsupported ALU operation"; return -1; }
            if (dst == 10) { *why = "write to the frame pointer"; return -1; }
            break;
        case 5: case 6: {                                /* JMP, JMP32 */
            uint8_t op = code & 0xf0;
            if (op == 0x90) {                            /* EXIT */
                if (cls != 5) { *why = "bad EXIT"; return -1; }
                break;
            }
            if (op == 0x80) { *why = "helper calls are not supported"; return -1; }
            if (op > 0xd0) { *why = "unsupported jump"; return -1; }
            int32_t t = (int32_t)pc + 1 + in[pc].off;
            if (in[pc].off < 0) { *why = "backward jump (programs must provably end)"; return -1; }
            if (t > (int32_t)n) { *why = "jump out of the program"; return -1; }
            if (t < (int32_t)n && t > 0 && in[t - 1].code == 0x18) { *why = "jump into a wide load"; return -1; }
            break;
        }
        case 1:                                          /* LDX */
            if ((code & 0xe0) != 0x60) { *why = "only memory loads"; return -1; }
            if (dst == 10) { *why = "write to the frame pointer"; return -1; }
            break;
        case 0:                                          /* LD: only LD_IMM64 */
            if (code != 0x18 || pc + 1 >= n || src != 0) { *why = "unsupported load"; return -1; }
            pc++;                                        /* its second slot */
            break;
        default:
            *why = "stores and the stack are not supported";
            return -1;
        }
    }
    return 0;
}

/* ---- the interpreter ------------------------------------------------------------ */

#define CTX_TAG 0xC0C7000000000000ull       /* the context pointer: only ever compared */

static int64_t sx32(uint64_t v) { return (int64_t)(int32_t)(uint32_t)v; }
static int cmp_true(uint8_t op, uint64_t a, uint64_t b, int w32) {
    if (w32) { a = (uint32_t)a; b = (uint32_t)b; }
    int64_t sa = w32 ? sx32(a) : (int64_t)a, sb = w32 ? sx32(b) : (int64_t)b;
    switch (op) {
    case 0x10: return a == b;  case 0x20: return a > b;   case 0x30: return a >= b;
    case 0x40: return (a & b) != 0;                       case 0x50: return a != b;
    case 0x60: return sa > sb; case 0x70: return sa >= sb;
    case 0xa0: return a < b;   case 0xb0: return a <= b;
    case 0xc0: return sa < sb; case 0xd0: return sa <= sb;
    default:   return 0;
    }
}

/* 1 = allow, 0 = refuse (including every run-time fault). */
static int run(const struct bpf_prog* p, const uint8_t* ctx, unsigned ctxlen) {
    uint64_t r[11] = { 0 };
    r[1] = CTX_TAG;
    for (uint32_t pc = 0; pc < p->n; pc++) {
        const struct insn* i = &p->in[pc];
        uint8_t code = i->code, cls = code & 7, op = code & 0xf0;
        uint8_t d = i->regs & 0xf, s = i->regs >> 4;
        switch (cls) {
        case 4: case 7: {
            int w32 = cls == 4;
            uint64_t a = r[d], b = (code & 0x08) ? r[s] : (uint64_t)(int64_t)i->imm;
            if (w32) { a = (uint32_t)a; b = (uint32_t)b; }
            uint64_t v;
            switch (op) {
            case 0x00: v = a + b; break;  case 0x10: v = a - b; break;
            case 0x20: v = a * b; break;
            case 0x30: if (!b) return 0; v = a / b; break;
            case 0x40: v = a | b; break;  case 0x50: v = a & b; break;
            case 0x60: v = a << (b & (w32 ? 31 : 63)); break;
            case 0x70: v = a >> (b & (w32 ? 31 : 63)); break;
            case 0x80: v = (uint64_t)(-(int64_t)a); break;
            case 0x90: if (!b) return 0; v = a % b; break;
            case 0xa0: v = a ^ b; break;  case 0xb0: v = b; break;
            case 0xc0: v = w32 ? (uint64_t)(uint32_t)(sx32(a) >> (b & 31))
                               : (uint64_t)((int64_t)a >> (b & 63)); break;
            default: return 0;
            }
            r[d] = w32 ? (uint32_t)v : v;
            break;
        }
        case 5: case 6: {
            if (op == 0x90) return r[0] != 0;            /* EXIT */
            int take = op == 0x00 ? 1
                     : cmp_true(op, r[d], (code & 0x08) ? r[s] : (uint64_t)(int64_t)i->imm, cls == 6);
            if (take) pc += (uint32_t)i->off;
            break;
        }
        case 1: {                                        /* LDX — context only */
            unsigned sz = ((code >> 3) & 3) == 0 ? 4 : ((code >> 3) & 3) == 1 ? 2
                        : ((code >> 3) & 3) == 2 ? 1 : 8;
            if (r[s] != CTX_TAG || i->off < 0 || (unsigned)i->off + sz > ctxlen) return 0;
            uint64_t v = 0;
            for (unsigned k = 0; k < sz; k++) v |= (uint64_t)ctx[i->off + k] << (8 * k);
            r[d] = v;
            break;
        }
        case 0:                                          /* LD_IMM64 */
            r[d] = (uint32_t)i->imm | ((uint64_t)(uint32_t)p->in[pc + 1].imm << 32);
            pc++;
            break;
        default:
            return 0;
        }
    }
    return 0;                                            /* fell off the end */
}

int bpf_devcg_allowed(const struct task* t, int chr, uint32_t major, uint32_t minor,
                      uint32_t access) {
    uint8_t ctx[12];
    uint32_t at = (access << 16) | (chr ? 2u : 1u);
    for (int k = 0; k < 4; k++) {
        ctx[k] = (uint8_t)(at >> (8 * k));
        ctx[4 + k] = (uint8_t)(major >> (8 * k));
        ctx[8 + k] = (uint8_t)(minor >> (8 * k));
    }
    struct bpf_prog* run_list[BPF_MAX_PROGS];
    int nr = 0;
    uint32_t f = spin_lock_irqsave(&g_bpf_lock);
    for (void* cg = cgroupfs_task_node(t); cg; cg = cgroupfs_parent(cg))
        for (int i = 0; i < g_natt; i++)
            if (g_att[i].cg == cg && nr < BPF_MAX_PROGS) { g_att[i].p->refs++; run_list[nr++] = g_att[i].p; }
    spin_unlock_irqrestore(&g_bpf_lock, f);
    int ok = 1;
    for (int i = 0; i < nr; i++) {
        if (ok && !run(run_list[i], ctx, sizeof ctx)) ok = 0;
        bpf_prog_put(run_list[i]);
    }
    return ok;
}

void bpf_cgroup_gone(void* cg) {
    struct bpf_prog* drop[BPF_MAX_PROGS];
    int nd = 0;
    uint32_t f = spin_lock_irqsave(&g_bpf_lock);
    for (int i = 0; i < g_natt; ) {
        if (g_att[i].cg == cg) { drop[nd++] = g_att[i].p; g_att[i] = g_att[--g_natt]; }
        else i++;
    }
    spin_unlock_irqrestore(&g_bpf_lock, f);
    for (int i = 0; i < nd; i++) bpf_prog_put(drop[i]);
}

/* ---- bpf(2) ---------------------------------------------------------------------- */

static uint32_t rd32(const void* a, unsigned off) {
    const uint8_t* p = (const uint8_t*)a + off;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const void* a, unsigned off) { return rd32(a, off) | (uint64_t)rd32(a, off + 4) << 32; }
static void wr32(void* a, unsigned off, uint32_t v) {
    uint8_t* p = (uint8_t*)a + off;
    for (int k = 0; k < 4; k++) p[k] = (uint8_t)(v >> (8 * k));
}

static struct bpf_prog* prog_of_fd(int fd) {
    struct ofile* o = fd_lookup(fd);
    return (o && o->kind == FD_BPF) ? o->bpf : NULL;
}
static void* cgroup_of_fd(int fd) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_VFS || !o->file || !o->file->inode) return NULL;
    return cgroupfs_of_inode(o->file->inode);
}
static long install_prog(struct bpf_prog* p) {
    struct ofile* o = ofile_from_bpf(p);              /* takes a reference */
    if (!o) return -12;
    int fd = fd_install_k(o);
    if (fd < 0) { ofile_unref(o); return -24; }
    fd_set_cloexec(fd, 1);                            /* bpf fds are always CLOEXEC */
    return fd;
}

static long prog_load(void* a, unsigned size) {
    /* Linux takes a SHORTER attr and reads the missing fields as zero (the
     * libraries send only up to their last non-zero field — cilium/ebpf sends
     * 40 bytes); h_bpf already zero-filled the rest.  The fields that must be
     * there end with the licence pointer. */
    if (size < 24) { kprintf("bpf: PROG_LOAD attr of %u bytes refused (too small)\n", size); return -22; }
    uint32_t type = rd32(a, 0), n = rd32(a, 4);
    uint64_t uins = rd64(a, 8);
    uint32_t flags = rd32(a, 44);
    if (flags) { kprintf("bpf: PROG_LOAD flags %x refused\n", flags); return -22; }
    if (type != BPF_PROG_TYPE_CGROUP_DEVICE) {
        kprintf("bpf: program type %u refused (only cgroup device programs)\n", type);
        return -22;
    }
    if (!n || n > BPF_MAXI) { kprintf("bpf: PROG_LOAD of %u instructions refused\n", n); return -22; }
    struct insn* in = (struct insn*)kmalloc(n * sizeof *in);
    if (!in) return -12;
    if (copy_from_user(in, (uintptr_t)uins, n * sizeof *in) != 0) { kfree(in); return -14; }
    const char* why = 0;
    if (verify(in, n, &why) != 0) {
        kprintf("bpf: program refused: %s\n", why);
        kfree(in);
        return -22;
    }
    struct bpf_prog* p = (struct bpf_prog*)kcalloc(1, sizeof *p);
    if (!p) { kfree(in); return -12; }
    p->in = in; p->n = n; p->refs = 1;
    if (size >= 64) for (int k = 0; k < 15; k++) p->name[k] = ((const char*)a)[48 + k];
    /* A tag that identifies the instructions (Linux uses a SHA-1 of them). */
    uint64_t h = 1469598103934665603ull;
    for (uint32_t k = 0; k < n * sizeof *in; k++) { h ^= ((uint8_t*)in)[k]; h *= 1099511628211ull; }
    for (int k = 0; k < 8; k++) p->tag[k] = (uint8_t)(h >> (8 * k));
    uint32_t f = spin_lock_irqsave(&g_bpf_lock);
    p->id = g_next_id++;
    int slot = -1;
    for (int i = 0; i < BPF_MAX_PROGS; i++) if (!g_progs[i]) { g_progs[i] = p; slot = i; break; }
    spin_unlock_irqrestore(&g_bpf_lock, f);
    if (slot < 0) { kfree(in); kfree(p); return -28; }
    long fd = install_prog(p);
    bpf_prog_put(p);                                  /* the descriptor holds it now */
    return fd;
}

static long prog_attach(void* a, int detach) {
    int target = (int)rd32(a, 0), pfd = (int)rd32(a, 4);
    uint32_t type = rd32(a, 8), flags = rd32(a, 12);
    int rfd = (int)rd32(a, 16);
    if (type != BPF_CGROUP_DEVICE) return -22;
    void* cg = cgroup_of_fd(target);
    if (!cg) return -9;                               /* EBADF: not a cgroup directory */
    struct bpf_prog* p = pfd > 0 || !detach ? prog_of_fd(pfd) : NULL;
    if (!detach && !p) return -9;
    uint32_t f = spin_lock_irqsave(&g_bpf_lock);
    if (detach) {
        int done = 0;
        for (int i = 0; i < g_natt; i++)
            if (g_att[i].cg == cg && (!p || g_att[i].p == p)) {
                struct bpf_prog* q = g_att[i].p;
                g_att[i] = g_att[--g_natt];
                spin_unlock_irqrestore(&g_bpf_lock, f);
                bpf_prog_put(q);
                return 0;
            }
        spin_unlock_irqrestore(&g_bpf_lock, f);
        (void)done;
        return -2;                                    /* ENOENT */
    }
    if (flags & BPF_F_REPLACE) {
        struct bpf_prog* old = prog_of_fd(rfd);
        for (int i = 0; old && i < g_natt; i++)
            if (g_att[i].cg == cg && g_att[i].p == old) {
                p->refs++;
                g_att[i].p = p;
                spin_unlock_irqrestore(&g_bpf_lock, f);
                bpf_prog_put(old);
                return 0;
            }
        spin_unlock_irqrestore(&g_bpf_lock, f);
        return -2;
    }
    int count = 0;
    for (int i = 0; i < g_natt; i++) if (g_att[i].cg == cg) count++;
    if (count && !(flags & BPF_F_ALLOW_MULTI)) {      /* one program, replaced */
        for (int i = 0; i < g_natt; i++)
            if (g_att[i].cg == cg) {
                struct bpf_prog* old = g_att[i].p;
                p->refs++; g_att[i].p = p;
                spin_unlock_irqrestore(&g_bpf_lock, f);
                bpf_prog_put(old);
                return 0;
            }
    }
    if (count >= BPF_MAX_ATTACH || g_natt >= BPF_MAX_PROGS) {
        spin_unlock_irqrestore(&g_bpf_lock, f);
        return -7;                                    /* E2BIG */
    }
    p->refs++;
    g_att[g_natt].cg = cg; g_att[g_natt].p = p; g_natt++;
    spin_unlock_irqrestore(&g_bpf_lock, f);
    return 0;
}

static long prog_query(void* a, unsigned size) {
    if (size < 28) return -22;
    int target = (int)rd32(a, 0);
    uint32_t type = rd32(a, 4);
    uint64_t uids = rd64(a, 16);
    uint32_t cap = rd32(a, 24);
    if (type != BPF_CGROUP_DEVICE) return -22;
    void* cg = cgroup_of_fd(target);
    if (!cg) return -9;
    uint32_t ids[BPF_MAX_ATTACH];
    uint32_t n = 0;
    uint32_t f = spin_lock_irqsave(&g_bpf_lock);
    for (int i = 0; i < g_natt && n < BPF_MAX_ATTACH; i++) if (g_att[i].cg == cg) ids[n++] = g_att[i].p->id;
    spin_unlock_irqrestore(&g_bpf_lock, f);
    wr32(a, 12, n ? BPF_F_ALLOW_MULTI : 0);           /* attach_flags */
    wr32(a, 24, n);                                   /* prog_cnt: the real count */
    if (n > cap) return -28;                          /* ENOSPC: retry with room */
    if (n && uids && copy_to_user((uintptr_t)uids, ids, n * sizeof ids[0]) != 0) return -14;
    return 0;
}

static long get_fd_by_id(void* a) {
    uint32_t id = rd32(a, 0);
    struct bpf_prog* p = NULL;
    uint32_t f = spin_lock_irqsave(&g_bpf_lock);
    for (int i = 0; i < BPF_MAX_PROGS; i++) if (g_progs[i] && g_progs[i]->id == id) { p = g_progs[i]; p->refs++; break; }
    spin_unlock_irqrestore(&g_bpf_lock, f);
    if (!p) return -2;
    long fd = install_prog(p);
    bpf_prog_put(p);
    return fd;
}

static long obj_info(void* a) {
    int fd = (int)rd32(a, 0);
    uint32_t len = rd32(a, 4);
    uint64_t uinfo = rd64(a, 8);
    struct bpf_prog* p = prog_of_fd(fd);
    if (!p) return -22;
    uint8_t info[80];
    for (unsigned k = 0; k < sizeof info; k++) info[k] = 0;
    wr32(info, 0, BPF_PROG_TYPE_CGROUP_DEVICE);
    wr32(info, 4, p->id);
    for (int k = 0; k < 8; k++) info[8 + k] = p->tag[k];
    wr32(info, 20, p->n * 8);                         /* xlated_prog_len */
    for (int k = 0; k < 16; k++) info[64 + k] = (uint8_t)p->name[k];
    uint32_t w = len < sizeof info ? len : (uint32_t)sizeof info;
    if (w && copy_to_user((uintptr_t)uinfo, info, w) != 0) return -14;
    wr32(a, 4, w);
    return 0;
}

long bpf_syscall(int cmd, void* attr, unsigned size) {
    switch (cmd) {
    case 0:  return -22;                              /* BPF_MAP_CREATE: no maps */
    case 5:  return prog_load(attr, size);            /* BPF_PROG_LOAD */
    case 8:  return prog_attach(attr, 0);             /* BPF_PROG_ATTACH */
    case 9:  return prog_attach(attr, 1);             /* BPF_PROG_DETACH */
    case 13: return get_fd_by_id(attr);               /* BPF_PROG_GET_FD_BY_ID */
    case 15: return obj_info(attr);                   /* BPF_OBJ_GET_INFO_BY_FD */
    case 16: return prog_query(attr, size);           /* BPF_PROG_QUERY */
    default:
        kprintf("bpf: command %d not supported\n", cmd);
        return -22;
    }
}

/* ---- `bpftest` — the verifier, the interpreter and the attachment rule ----- */
#include "shellcmd.h"
#include "console.h"
static int bt_load(const struct insn* in, uint32_t n, struct bpf_prog** out) {
    const char* why = 0;
    if (verify(in, n, &why) != 0) return -1;
    struct bpf_prog* p = (struct bpf_prog*)kcalloc(1, sizeof *p);
    struct insn* c = (struct insn*)kmalloc(n * sizeof *c);
    if (!p || !c) { kfree(p); kfree(c); return -2; }
    for (uint32_t i = 0; i < n; i++) c[i] = in[i];
    p->in = c; p->n = n; p->refs = 1;
    *out = p;
    return 0;
}
static void cmd_bpftest(const char* args) {
    (void)args;
    /* runc's shape: r2 = ctx->access_type & 0xffff (device type), r3 = major,
     * r4 = minor; allow "c 1:3", else deny. */
    const struct insn allow_null[] = {
        { 0x61, 0x12, 0, 0 },          /* r2 = *(u32*)(r1+0)          */
        { 0x54, 0x02, 0, 0xffff },     /* w2 &= 0xffff                 */
        { 0x61, 0x13, 4, 0 },          /* r3 = *(u32*)(r1+4)  major    */
        { 0x61, 0x14, 8, 0 },          /* r4 = *(u32*)(r1+8)  minor    */
        { 0x55, 0x02, 4, 2 },          /* if r2 != 2 (char) goto deny  */
        { 0x55, 0x03, 3, 1 },          /* if r3 != 1 goto deny         */
        { 0x55, 0x04, 2, 3 },          /* if r4 != 3 goto deny         */
        { 0xb7, 0x00, 0, 1 },          /* r0 = 1                       */
        { 0x95, 0x00, 0, 0 },          /* exit                         */
        { 0xb7, 0x00, 0, 0 },          /* deny: r0 = 0                 */
        { 0x95, 0x00, 0, 0 },          /* exit                         */
    };
    const struct insn backward[] = { { 0x05, 0, -1, 0 }, { 0x95, 0, 0, 0 } };
    const struct insn call[]     = { { 0x85, 0, 0, 1 }, { 0x95, 0, 0, 0 } };
    const struct insn wild[]     = { { 0x61, 0x12, 64, 0 }, { 0xb7, 0, 0, 1 }, { 0x95, 0, 0, 0 } };
    struct bpf_prog *p = 0, *q = 0, *w = 0;
    int ld = bt_load(allow_null, 11, &p);
    int lb = bt_load(backward, 2, &q), lc = bt_load(call, 2, &q);
    int lw = bt_load(wild, 3, &w);
    uint8_t ctx[12];
    uint32_t acc = (6u << 16) | 2u;                   /* read|write, char */
    for (int k = 0; k < 4; k++) { ctx[k] = (uint8_t)(acc >> (8 * k)); ctx[4 + k] = (uint8_t)(1u >> (8 * k)); }
    ctx[8] = 3; ctx[9] = ctx[10] = ctx[11] = 0;
    int null_ok = p && run(p, ctx, 12) == 1;
    ctx[8] = 5;
    int zero_no = p && run(p, ctx, 12) == 0;
    int wild_no = w && run(w, ctx, 12) == 0;          /* out of the context: refused */
    /* attached to a cgroup: a task inside it is filtered, one outside is not */
    vfs_mkdir("/sys/fs/cgroup/bpftest");
    struct dentry* d = vfs_resolve("/sys/fs/cgroup/bpftest");
    void* cg = d && d->inode ? cgroupfs_of_inode(d->inode) : 0;
    int att = 0, in_null = 0, in_zero = 1, out_zero = 0;
    if (cg && p) {
        uint32_t f = spin_lock_irqsave(&g_bpf_lock);
        p->refs++; g_att[g_natt].cg = cg; g_att[g_natt].p = p; g_natt++;
        spin_unlock_irqrestore(&g_bpf_lock, f);
        att = 1;
        struct task fake;
        for (unsigned k = 0; k < sizeof fake; k++) ((uint8_t*)&fake)[k] = 0;
        fake.cgroup = cg;
        in_null = bpf_devcg_allowed(&fake, 1, 1, 3, 6);
        in_zero = bpf_devcg_allowed(&fake, 1, 1, 5, 2);
        out_zero = bpf_devcg_allowed(task_current(), 1, 1, 5, 2);
        bpf_cgroup_gone(cg);                          /* detach (drops the attachment ref) */
    }
    vfs_unlink("/sys/fs/cgroup/bpftest");
    kprintf("bpf: load %d, refused backward %d call %d; null %d zero %d out-of-ctx %d; "
            "attached %d: inside null %d zero %d, outside zero %d\n",
            ld, lb, lc, null_ok, zero_no, wild_no, att, in_null, in_zero, out_zero);
    int ok = ld == 0 && lb == -1 && lc == -1 && lw == 0 && null_ok && zero_no && wild_no &&
             att && in_null && !in_zero && out_zero;
    if (p) bpf_prog_put(p);
    if (w) bpf_prog_put(w);
    console_write(ok ? "bpf: ok\n" : "bpf: FAIL\n");
}
SHELL_CMD(bpftest) = { "bpftest", "", "eBPF device programs: verifier, interpreter, cgroups",
                       SHELL_G_TEST, cmd_bpftest, SHELL_P_ANY };
