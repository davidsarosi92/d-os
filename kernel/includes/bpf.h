/* bpf.h — eBPF for cgroup device control (§M90); see kernel/core/bpf.c. */
#ifndef DOS_BPF_H
#define DOS_BPF_H
#include <stdint.h>
#include <stddef.h>

struct ofile;
struct task;
struct bpf_prog;

/* bpf(2): `cmd` with the guest's `union bpf_attr` at `uattr` (`size` bytes,
 * already validated readable/writable by the caller).  0 / an fd / a negative
 * Linux errno. */
long bpf_syscall(int cmd, void* attr, unsigned size);

/* The program behind an FD_BPF descriptor's last reference: drop it. */
void bpf_prog_put(struct bpf_prog* p);

/* May `t` open the device (`chr` 1 = character, 0 = block) `major`:`minor`
 * for `access` (BPF_DEVCG_ACC_READ 2 | WRITE 4 | MKNOD 1)?  Runs every
 * program attached to t's cgroup and its ancestors (BPF_CGROUP_DEVICE); all
 * must allow.  1 = allowed (also when nothing is attached), 0 = refused. */
int bpf_devcg_allowed(const struct task* t, int chr, uint32_t major, uint32_t minor,
                      uint32_t access);

/* A cgroup is being removed: detach what was attached to it. */
void bpf_cgroup_gone(void* cg);
#endif
