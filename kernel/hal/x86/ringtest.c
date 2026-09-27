/* =============================================================================
 * ringtest.c — x86 ring-3 self-test (the `ringtest` shell command's arch half).
 *
 * Moved out of shell.c (M21) so shell.c stays arch-portable: the user-mode
 * plumbing (vmm USER mappings, hand-coded machine code, the ring-3 drop) is
 * inherently x86.  aarch64 provides its own arch_ringtest() (drops to EL0).
 * shell.c just calls arch_ringtest().
 *
 * Allocates two frames, USER-maps them at 0x40000000 (code) / 0x40001000
 * (stack), hand-codes a tiny i386 program that SYS_PRINTs a message then
 * SYS_EXITs, and drops to ring 3.  The i386 encoding runs in both the 32-bit
 * and (compat) 64-bit ring-3 paths.
 * ============================================================================= */

#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "usermode.h"
#include "proc.h"
#include "syscall.h"
#include "task.h"
#include "hal_api.h"
#include <stdint.h>
#include <stddef.h>

/* Emit the tiny i386 ring-3 program used by the M25 exec path: code at
 * offset 0, message at offset 0x100.  The program issues
 * `write(1, msg, len)` (M25 stage-3 fd syscall) then `exit` — exercising the
 * ring-3 → fd path end-to-end.  `base` is the VA the image loads at, so the
 * absolute message pointer is base + 0x100.  Returns the payload length.  The
 * encoding runs in both the 32-bit and (compat) 64-bit ring-3 paths. */
static size_t build_hello(uint8_t* code, uintptr_t base) {
    const char* src = "hello from ring 3 (ELF, write syscall)!\n";
    uint32_t len = 0;
    while (src[len]) len++;
    uint32_t msg_va = (uint32_t)base + 0x100;

    int o = 0;
    code[o++] = 0xBB; *(uint32_t*)&code[o] = 1;         o += 4;  /* mov ebx, 1 (fd) */
    code[o++] = 0xB9; *(uint32_t*)&code[o] = msg_va;    o += 4;  /* mov ecx, msg    */
    code[o++] = 0xBA; *(uint32_t*)&code[o] = len;       o += 4;  /* mov edx, len    */
    code[o++] = 0xB8; *(uint32_t*)&code[o] = SYS_WRITE; o += 4;  /* mov eax, WRITE  */
    code[o++] = 0xCD; code[o++] = 0x80;                          /* int 0x80        */
    code[o++] = 0xB8; *(uint32_t*)&code[o] = SYS_EXIT;  o += 4;  /* mov eax, EXIT   */
    code[o++] = 0xCD; code[o++] = 0x80;                          /* int 0x80        */
    code[o++] = 0xEB; code[o++] = 0xFE;                          /* jmp $           */

    uint8_t* msg = code + 0x100;
    for (uint32_t i = 0; i < len; i++) msg[i] = (uint8_t)src[i];
    msg[len] = 0;
    return 0x100 + len + 1;                                      /* code + gap + msg */
}

/* usermode.h — arch hook for the portable exec path (proc.c). */
size_t arch_user_hello(uint8_t* buf, size_t cap, uintptr_t base) {
    if (cap < 0x100 + 48) return 0;
    return build_hello(buf, base);
}

/* A PRIVATE ADDRESS SPACE, NOT THE KERNEL'S (2026-09-26).
 *
 * This used to vmm_map the program straight into the KERNEL directory at the
 * user base (0x40000000) and vmm_unmap it afterwards.  Unmapping removes the
 * PAGES and leaves the page TABLES the map created — and every address space
 * created later copies the kernel's top-level entries, so from then on every
 * process shared one set of page tables for the bottom of user space.  On
 * x86_64 that made `pipetest` after `ringtest` read 0 bytes every time: the
 * forked child's pages landed in tables its parent was using too.  A test that
 * leaves the machine different from how it found it is not a test of the
 * machine.  So the program gets its own space, exactly like proc_exec does,
 * and the space's destruction takes the tables with it. */
int arch_ringtest(void) {
    struct vmm_space* sp = vmm_space_create();
    pmm_phys_t code_phys  = pmm_alloc_frame();
    pmm_phys_t stack_phys = pmm_alloc_frame();
    if (!sp || !code_phys || !stack_phys) {
        console_write("ringtest: out of memory\n");
        if (code_phys)  pmm_free_frame(code_phys);
        if (stack_phys) pmm_free_frame(stack_phys);
        if (sp) vmm_space_destroy(sp);
        return -1;
    }

    /* Build the user program through the kernel's view of the frame.  Layout:
     *   0x40000000 [21B]  code: mov ebx,msg; mov eax,SYS_PRINT; int 0x80;
     *                           mov eax,SYS_EXIT; int 0x80; jmp $
     *   0x40000100        msg:  "hello from ring 3!\n\0"
     */
    uint8_t* code = (uint8_t*)phys_to_virt(code_phys);
    for (int i = 0; i < 4096; i++) code[i] = 0;
    code[0]  = 0xBB;                              /* mov ebx, imm32 */
    *(uint32_t*)&code[1]  = 0x40000100u;          /* msg address */
    code[5]  = 0xB8;                              /* mov eax, imm32 */
    *(uint32_t*)&code[6]  = SYS_PRINT;
    code[10] = 0xCD; code[11] = 0x80;             /* int 0x80 */
    code[12] = 0xB8;                              /* mov eax, imm32 */
    *(uint32_t*)&code[13] = SYS_EXIT;
    code[17] = 0xCD; code[18] = 0x80;             /* int 0x80 */
    code[19] = 0xEB; code[20] = 0xFE;             /* jmp $ */

    char* msg = (char*)code + 0x100;
    const char* src = "hello from ring 3!\n";
    int i = 0;
    while (src[i]) { msg[i] = src[i]; i++; }
    msg[i] = 0;

    /* The space owns both frames from here: destroying it frees them. */
    /* §M86 — the code page must ASK to be executable: with no-execute on, a
     * user page without VMM_EXEC is data, and ring 3 would fault on its
     * first instruction. */
    if (vmm_space_map(sp, 0x40000000, code_phys,  VMM_WRITABLE | VMM_USER | VMM_EXEC) != 0 ||
        vmm_space_map(sp, 0x40001000, stack_phys, VMM_WRITABLE | VMM_USER) != 0) {
        console_write("ringtest: vmm_space_map failed\n");
        vmm_space_destroy(sp);
        return -1;
    }

    struct task* me = task_current();
    struct vmm_space* prev = me ? me->mm : NULL;
    if (me) me->mm = sp;                 /* the scheduler keeps CR3 across a switch */
    vmm_space_switch(sp);

    /* Drop to ring 3.  Stack top is 0x40002000 (top of stack frame). */
    console_write("ringtest: dropping to ring 3...\n");
    enter_user_mode_wrap(0x40000000u, 0x40002000u);
    /* §M71 — the SYS_EXIT teleport does not unwind the dispatcher, so the gate
     * would stay armed on whatever task ran `ringtest`.  See proc.h. */
    user_excursion_end();
    console_write("ringtest: back in ring 0\n");

    vmm_space_switch(prev);
    if (me) me->mm = prev;
    vmm_space_destroy(sp);
    return 0;
}
