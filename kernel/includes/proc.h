/* =============================================================================
 * proc.h — run a loaded ELF as a user program (M25 stage 2b).
 *
 * `proc_exec_elf` ties the stage-2a ELF loader to the existing ring-3 / EL0
 * entry path: it creates a private address space, loads the image into it,
 * maps a user stack, binds the space to the calling task (so the scheduler
 * keeps the right CR3/TTBR0 if the program is preempted), and drops to user
 * mode at the entry point.  It returns when the program issues SYS_EXIT.
 *
 * This is a *synchronous excursion* on the caller's task — the ancestor of a
 * real spawned process.  The hello programs it runs today are short enough
 * that a timer tick never lands mid-user; fully independent, long-running,
 * preemptible user processes (per-task kernel stack in TSS.esp0, robust
 * IRQ-from-user handling) arrive with the scheduling work in later M25 stages.
 * ============================================================================= */

#ifndef PROC_H
#define PROC_H

#include <stddef.h>

/* Load `image` (`len` bytes, a static ELF) and run it in ring 3 / EL0 in a
 * fresh private address space; returns 0 once the program SYS_EXITs, or a
 * negative code if loading / setup failed. */
int proc_exec_elf(const void* image, size_t len);

/* M34 — like proc_exec_elf, but builds a System V initial stack carrying
 * argc/argv (+ empty envp + AT_NULL auxv) that the program's crt0 reads.
 * `argv` holds `argc` NUL-terminated strings.  Same synchronous-excursion
 * model + return contract as proc_exec_elf. */
int proc_exec_elf_argv(const void* image, size_t len,
                       int argc, const char* const argv[]);

/* Tier B — spawn `image` as an INDEPENDENT, preemptible user process on its own
 * task, named `name`.  Returns immediately with the new task's pid (or negative
 * on failure); the program runs concurrently at ring 3/EL0 until it SYS_EXITs
 * (→ task_exit), and init reaps it (freeing its address space).  Several may
 * run at once.  The caller can task_wait(pid) to await completion. */
int proc_spawn(const char* name, const void* image, size_t len);

#endif
