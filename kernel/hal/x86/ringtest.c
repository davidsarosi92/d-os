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
#include "syscall.h"
#include <stdint.h>

int arch_ringtest(void) {
    uint32_t code_phys  = pmm_alloc_frame();
    uint32_t stack_phys = pmm_alloc_frame();
    if (!code_phys || !stack_phys) {
        console_write("ringtest: pmm OOM\n");
        if (code_phys)  pmm_free_frame(code_phys);
        if (stack_phys) pmm_free_frame(stack_phys);
        return -1;
    }
    if (vmm_map(0x40000000, code_phys,  VMM_WRITABLE | VMM_USER) != 0 ||
        vmm_map(0x40001000, stack_phys, VMM_WRITABLE | VMM_USER) != 0) {
        console_write("ringtest: vmm_map failed\n");
        return -1;
    }

    /* Build the user program.  Layout:
     *   0x40000000 [21B]  code: mov ebx,msg; mov eax,SYS_PRINT; int 0x80;
     *                           mov eax,SYS_EXIT; int 0x80; jmp $
     *   0x40000100        msg:  "hello from ring 3!\n\0"
     */
    uint8_t* code = (uint8_t*)0x40000000;
    code[0]  = 0xBB;                              /* mov ebx, imm32 */
    *(uint32_t*)&code[1]  = 0x40000100u;          /* msg address */
    code[5]  = 0xB8;                              /* mov eax, imm32 */
    *(uint32_t*)&code[6]  = SYS_PRINT;
    code[10] = 0xCD; code[11] = 0x80;             /* int 0x80 */
    code[12] = 0xB8;                              /* mov eax, imm32 */
    *(uint32_t*)&code[13] = SYS_EXIT;
    code[17] = 0xCD; code[18] = 0x80;             /* int 0x80 */
    code[19] = 0xEB; code[20] = 0xFE;             /* jmp $ */

    char* msg = (char*)0x40000100;
    const char* src = "hello from ring 3!\n";
    int i = 0;
    while (src[i]) { msg[i] = src[i]; i++; }
    msg[i] = 0;

    /* Drop to ring 3.  Stack top is 0x40002000 (top of stack frame). */
    console_write("ringtest: dropping to ring 3...\n");
    enter_user_mode_wrap(0x40000000u, 0x40002000u);
    console_write("ringtest: back in ring 0\n");

    /* Cleanup. */
    vmm_unmap(0x40000000); pmm_free_frame(code_phys);
    vmm_unmap(0x40001000); pmm_free_frame(stack_phys);
    return 0;
}
