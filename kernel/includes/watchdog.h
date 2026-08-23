/* =============================================================================
 * watchdog.h — heartbeat-based freeze detection (M31).
 *
 * M27 handles a task that *dies*; the watchdog handles a task that is alive but
 * *wedged* (an infinite loop that never yields, a stuck state machine, a
 * spinlock deadlock).  Death and freeze are different failure modes — you
 * cannot reap what has not exited — so this is separate machinery, in layers:
 *
 *   Layer 1 — per-task heartbeat (opt-in, the systemd WatchdogSec= model).  A
 *     task registers a deadline and periodically "pets" it (watchdog_kick).  A
 *     sweep flags any task that missed its deadline as hung, logs it, and
 *     kill-trees it (a supervised M29 service is then restarted by its
 *     supervisor — the two subsystems compose).  Opt-in: a task that never
 *     registers is never watched, so a legitimately long compute is not a
 *     "freeze".
 *
 *   Layer 2 — per-CPU softlockup detector.  Each CPU's timer tick advances a
 *     per-CPU counter (percpu.ticks, bumped in schedule_request).  The sweep,
 *     running on a healthy CPU, notices a CPU whose counter stopped advancing —
 *     a core wedged with IRQs off (spinlock deadlock / IRQ storm), which a
 *     per-task heartbeat can't catch because the sweep itself may be starved on
 *     that core.
 *
 *   Layer 3 — hardware watchdog (deferred).  Arming an emulated/real watchdog
 *     timer (QEMU -watchdog) that resets the box if the whole system wedges is
 *     the only recovery when software is too dead to help itself.  It needs a
 *     per-platform device driver (i6300esb on x86, SP805 on ARM) and is left
 *     as a future addition; layers 1–2 are the detection substrate.
 *
 * The hard truth (cooperative-kill model, §M22.3): the watchdog can DETECT a
 * freeze at any layer, but a wedged kernel thread can only be *force-killed* if
 * it reaches a yield/poll point (it may hold a spinlock).  So layer-1 "kill +
 * restart" works for a task that yields but stopped petting (a logical hang); a
 * truly wedged kthread that never yields is only recoverable by layer 3 / a
 * reboot.  Genuine force-kill of any frozen task arrives with §M25 user
 * processes (a frozen ring-3 process can't corrupt kernel state).
 * ============================================================================= */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

/* Boot: register /proc/watchdog and spawn the sweep task (child of init). */
void watchdog_init(void);

/* Layer 1 opt-in.  The CALLING task asks to be watched with a `timeout_ms`
 * deadline: if it goes longer than that between kicks, the sweep declares it
 * hung.  Re-registering updates the deadline.  A short timeout on a task that
 * legitimately blocks a long time (e.g. in a blocking read) would false-fire —
 * only register on tasks whose liveness means "kicking regularly". */
void watchdog_register(uint32_t timeout_ms);

/* Pet the calling task's watchdog — call it inside the task's healthy loop. */
void watchdog_kick(void);

/* Stop watching the calling task (e.g. before a long legitimate block). */
void watchdog_unregister(void);

/* §M31 L3 — hardware watchdog (ib700.c).  A device that counts independently of
 * the CPU: if the kernel stops petting it (hard lockup — spinning / hlt with IRQs
 * off, which the task-based L1/L2 can't catch), it fires an NMI.  init arms it,
 * the watchdog task pets it; the NMI lockup handler (idt.c) logs + recovers. */
void hw_watchdog_init(void);
void hw_watchdog_pet(void);
/* Non-zero once hw_watchdog_init() has armed the device.  The TICK pets it
 * (task.c) and must not touch the ports before it is armed — nor at all when
 * `kernel.hw_watchdog` is off. */
int  hw_watchdog_armed(void);

/* Deliberately wedge the boot CPU with interrupts off, so the hardware
 * watchdog's NMI is the only thing that can end it.  `hardlock` in both
 * shells.  The box REBOOTS on success — that is the pass condition. */
void watchdog_hardlock_test(void);
void hw_watchdog_disable(void);

/* NMI lockup escalation counter, shared with the arch NMI handler (idt.c) so a
 * repeated fire (recovery failed) can escalate to reboot.  Defined in watchdog.c. */
extern volatile uint32_t g_nmi_lockups;


/* NMI-safe: layer 2's per-CPU scheduler-tick counter, now vs. at the last
 * sweep.  Equal values = that CPU has stopped taking ticks, which is what a
 * hard lockup IS.  Used by the NMI report so it names the wedged CPU rather
 * than whichever CPU the alarm happened to interrupt (§4.67). */
int watchdog_cpu_tick_state(int cpu, uint64_t* now_ticks, uint64_t* last_seen);

#endif
