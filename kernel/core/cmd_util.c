/* =============================================================================
 * cmd_util.c — helpers shared by the shell command files (§M70).
 * See cmd_util.h for why these are shared rather than copied.
 * =========================================================================== */

#include "cmd_util.h"
#include "vfs.h"
#include "kmalloc.h"
#include "task.h"
#include "proc.h"
#include <stddef.h>

int cmd_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int cmd_starts_with(const char* s, const char* p) {
    while (*p) {
        if (*s != *p) return 0;
        s++; p++;
    }
    return 1;
}

int cmd_parse_uint(const char* s, uint32_t* out) {
    if (!s || !*s) return -1;
    uint32_t v = 0;
    int any = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        any = 1;
        s++;
    }
    if (!any || *s != 0) return -1;
    *out = v;
    return 0;
}

/* Hex, with or without an 0x prefix.  0xFF = any of CPUs 0..7 for `taskset`. */
int cmd_parse_hex(const char* s, uint32_t* out) {
    if (!s || !*s) return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    int any = 0;
    while (*s) {
        int d;
        if      (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = 10 + (*s - 'a');
        else if (*s >= 'A' && *s <= 'F') d = 10 + (*s - 'A');
        else return -1;
        v = (v << 4) | (uint32_t)d;
        any = 1;
        s++;
    }
    if (!any) return -1;
    *out = v;
    return 0;
}

int dos_run_elf(const char* path) { return dos_run_elf_cap(path, NULL, 0); }

int dos_run_elf_cap(const char* path, char* cap, int caplen) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;
    size_t sz = f->inode ? (size_t)f->inode->size : 0;
    if (sz == 0 || sz > (16u << 20)) { vfs_close(f); return -1; }
    uint8_t* img = (uint8_t*)kmalloc(sz);
    if (!img) { vfs_close(f); return -1; }
    ssize_t rd = vfs_read(f, img, sz);
    vfs_close(f);
    if (rd < (ssize_t)sz) { kfree(img); return -1; }
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    if (me && cap && caplen > 0) {            /* §M43 capture stdout */
        cap[0] = '\0';
        me->cap_buf = cap; me->cap_len = 0; me->cap_cap = caplen;
    }
    int rc = proc_exec_elf(img, sz);
    if (me) { me->cap_buf = NULL; me->cap_len = me->cap_cap = 0; me->linux_abi = prev; }
    kfree(img);
    return rc;
}
