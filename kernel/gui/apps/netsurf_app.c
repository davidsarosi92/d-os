/* =============================================================================
 * netsurf_app.c — Start-menu launcher for the NetSurf web browser (§M42).
 *
 * NetSurf is a ring-3 musl/Linux-ABI program.  The launcher runs it as an
 * INDEPENDENT, preemptible user task via proc_spawn_argv (NOT the synchronous
 * proc_exec_* excursion — that nests the ring-3 run inside the launcher's task
 * and is reserved for self-tests).  Running as a real user_task is what lets a
 * crashing or wedged browser be torn down on its own (the fault handler kills
 * the task; M46 force-kill reclaims a wedge) WITHOUT freezing the desktop.
 *
 * Inside, NetSurf's libnsfb "dos" surface (`-f dos`) creates a WM window via the
 * display bridge (kernel/gui/dosgui.c) and blits its frames into it, so the
 * browser appears as a normal desktop window.  The binary is embedded as a weak
 * blob; on arches/images without it the launcher just reports so.
 *
 * The blob is arch-matched to the kernel (an i386 kernel embeds the i386 PIE, an
 * x86_64 kernel the x86_64 PIE).  The x64-runs-32-bit dual-package split
 * ("NetSurf (x32)" + "NetSurf (x64)") is a separate arch-personality milestone.
 * ============================================================================= */
/* NetSurf is ported to the x86 arches (the binary is a musl PIE); compile this
 * launcher to nothing on aarch64 so no dead Start-menu entry appears there. */
#if defined(__i386__) || defined(__x86_64__)

#include "gui.h"
#include "gui_app.h"
#include "pkg.h"
#include "icons.h"
#include "task.h"
#include "proc.h"
#include "config.h"
#include "printf.h"
#include <stddef.h>

extern const unsigned char _binary_user_netsurf_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_netsurf_dynelf_end[]   __attribute__((weak));

static void netsurf_launch(void) {
    /* §M62 follow-up — the browser's resources are unpacked on first launch,
     * not at boot: they were 171 ms of every boot, and most boots never open a
     * browser (see pkg.h). */
    pkg_ensure_netsurf_res();
    if (!_binary_user_netsurf_dynelf_start) {
        kprintf("netsurf: not built into this image\n");
        return;
    }
    size_t len = (size_t)(_binary_user_netsurf_dynelf_end -
                          _binary_user_netsurf_dynelf_start);
    /* argv strings are copied into the new task's initial stack synchronously by
     * proc_spawn_argv_under, so this local array is safe.  linux_abi=1 selects
     * the musl/Linux personality.  Parent it to the DESKTOP session task, not to
     * the caller: a Start-menu launch runs on a transient "app:NetSurf" host task
     * (gui.c dispatch_launches) that exits immediately, which would leave the
     * browser an orphan re-parented to init.  gui_desktop_pid() is the long-lived
     * GUI session; fall back to the caller (-1) if the GUI is somehow not up. */
    const char* argv[] = { "netsurf", "-f", "dos", "about:welcome" };
    int sess = gui_desktop_pid();
    int pid = proc_spawn_argv_under("netsurf", _binary_user_netsurf_dynelf_start,
                                    len, 4, argv, 1 /* linux_abi */,
                                    sess > 0 ? sess : -1);
    /* §M46 — per-package runaway auto-fkill policy (opt-in, data-driven): a
     * package-specific key overrides the global default, which defaults to 0
     * (off — the manual chrome/Task-Manager force-kill still applies). */
    if (pid > 0) {
        long ms = config_get_long("package.netsurf.auto_fkill_ms",
                                  config_get_long("package.auto_fkill_ms", 0));
        if (ms > 0) task_set_auto_fkill(pid, (uint32_t)ms);
    }
}

GUI_APP_ICON("NetSurf", netsurf_launch, ICON_GLOBE);

#endif /* i386 || x86_64 */
