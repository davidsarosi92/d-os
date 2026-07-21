/* =============================================================================
 * netsurf_app.c — Start-menu launcher for the NetSurf web browser (§M42).
 *
 * NetSurf is a ring-3 musl/Linux-ABI program, not a kernel app-host task, so the
 * launcher spawns a dedicated task that performs the ring-3 excursion
 * (proc_exec_elf_argv) with the linux-abi personality.  Inside, NetSurf's libnsfb
 * "dos" surface (`-f dos`) creates a WM window via the display bridge
 * (kernel/gui/dosgui.c) and blits its frames into it — so the browser appears as
 * a normal desktop window.  The binary is embedded as a blob and is x86_64-only
 * (the extern is weak); on arches without it the launcher just reports so.
 * ============================================================================= */
/* NetSurf is x86_64-only (the binary is a 64-bit musl PIE); compile this whole
 * launcher to nothing on the other arches so no dead Start-menu entry appears. */
#if defined(__x86_64__)

#include "gui.h"
#include "gui_app.h"
#include "task.h"
#include "proc.h"
#include "printf.h"
#include <stddef.h>

extern const unsigned char _binary_user_netsurf_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_netsurf_dynelf_end[]   __attribute__((weak));

/* Runs on its own task: become a linux-abi process and exec the browser.  The
 * excursion does not return until NetSurf exits. */
static void netsurf_task(void) {
    struct task* me = task_current();
    if (me) me->linux_abi = 1;
    const char* argv[] = { "netsurf", "-f", "dos", "about:welcome" };
    size_t len = (size_t)(_binary_user_netsurf_dynelf_end -
                          _binary_user_netsurf_dynelf_start);
    proc_exec_elf_argv(_binary_user_netsurf_dynelf_start, len, 4, argv);
}

static void netsurf_launch(void) {
    if (!_binary_user_netsurf_dynelf_start) {
        kprintf("netsurf: not built into this image (x86_64 only)\n");
        return;
    }
    /* Detached so the browser outlives the launcher click; it manages its own
     * window through the display bridge. */
    task_spawn_detached("netsurf", netsurf_task);
}

GUI_APP("NetSurf", netsurf_launch);

#endif /* __x86_64__ */
