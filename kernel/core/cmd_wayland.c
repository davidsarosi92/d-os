/* =============================================================================
 * cmd_wayland.c — Wayland and GL demo commands (§M70).
 *
 * Split out of shell.c.  These are the §M26/§M40 ladder in order: the
 * hand-marshalled self-test, the visible demo, the in-window surface, the real
 * ring-3 client, the mini client library, the UPSTREAM libwayland client, an
 * unmodified `weston-simple-shm`, and Mesa EGL/GLES on softpipe.
 *
 * Each rung is kept as its own verb rather than folded into one `waytest
 * <stage>`, because they fail in different LAYERS — a broken wire handshake
 * and a broken shm pool look identical from a single pass/fail, and the whole
 * value of the ladder is that the first rung that fails names the layer.
 * =========================================================================== */

#include "shellcmd.h"
#include "cmd_util.h"
#include "console.h"
#include "printf.h"
#include "wayland.h"
#include "fd.h"
#include "task.h"
#include "proc.h"
#include "gui.h"
#include "vfs.h"
#include "kmalloc.h"
#include "config.h"
#include "hal_api.h"
#include <stdint.h>
#include <stddef.h>

/* Every blob symbol here is WEAK: a tree built without `make musl`, without the
 * Wayland cross-build or without Mesa still links, and the command reports
 * "not embedded" rather than failing at the linker.  That is also why each one
 * is CHECKED before use — an absent weak symbol is NULL. */
extern const unsigned char _binary_user_wlclient_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_wlclient_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_wlapp_elf_start[]     __attribute__((weak));
extern const unsigned char _binary_user_wlapp_elf_end[]       __attribute__((weak));
extern const unsigned char _binary_user_wlupstream_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_wlupstream_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_simpleshm_muslelf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_simpleshm_muslelf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_wlclip_muslelf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_wlclip_muslelf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_egltri_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_egltri_dynelf_end[]   __attribute__((weak));

/* Run a ring-3 Wayland client blob: hand it fd 3 = one end of a usock_pair, run
 * the server on its own task on the other end, exec the client. */
static void run_wayland_client(const char* what, const unsigned char* s,
                               const unsigned char* e) {
    if (!s) { console_write("wayland: client not embedded\n"); return; }
    struct task* me = task_current();
    if (me->fds[3]) { console_write("wayland: fd 3 already in use\n"); return; }

    struct usock *srv, *cli;
    if (usock_pair(&srv, &cli) != 0) { console_write("wayland: usock_pair failed\n"); return; }
    struct ofile* cli_of = ofile_from_sock(cli);
    struct wl_conn* conn = (struct wl_conn*)kmalloc(sizeof *conn);
    if (!cli_of || !conn) { console_write("wayland: out of memory\n");
        if (cli_of) ofile_unref(cli_of); else usock_close(cli);
        usock_close(srv); return; }

    me->fds[3] = cli_of;                          /* client socket = fd 3 */
    wl_conn_init(conn, srv);
    task_spawn_arg("wl-server", wl_server_task, conn);   /* server on its own task */

    kprintf("%s: launching a ring-3 Wayland client (fd 3)...\n", what);
    int rc = proc_exec_elf(s, (size_t)(e - s));
    kprintf("%s: client exited rc=%d\n", what, rc);      /* fd 3 auto-closed → server tears down */
}

static void run_upstream_wl(const char* what, const unsigned char* sp,
                            const unsigned char* ep, int windowed,
                            int argc, const char* const* argv) {
    struct task* me = task_current();
    if (me->fds[3]) { kprintf("%s: fd 3 already in use\n", what); return; }

    struct usock *srv, *cli;
    if (usock_pair(&srv, &cli) != 0) { kprintf("%s: usock_pair failed\n", what); return; }
    struct ofile* cli_of = ofile_from_sock(cli);
    struct wl_conn* conn = (struct wl_conn*)kmalloc(sizeof *conn);
    if (!cli_of || !conn) {
        kprintf("%s: out of memory\n", what);
        if (cli_of) ofile_unref(cli_of); else usock_close(cli);
        usock_close(srv); return;
    }
    me->fds[3] = cli_of;
    wl_conn_init(conn, srv);
    if (windowed) { gui_start(); task_msleep(300); conn->wm_mode = 1; }
    task_spawn_arg("wl-server", wl_server_task, conn);

    kprintf("%s: WAYLAND_SOCKET=3%s\n", what,
            windowed ? " — surface becomes a desktop window" : "");
    proc_set_exec_env("WAYLAND_SOCKET=3");
    int prev = me->linux_abi;
    me->linux_abi = 1;
    int rc = proc_exec_elf_argv(sp, (size_t)(ep - sp), argc, argv);
    me->linux_abi = prev;
    kprintf("%s: client exited rc=%d\n", what, rc);
}


/* §M40 — `waykeymap`: print the xkb keymap this system would hand a Wayland
 * client.  It exists to be VERIFIED, not admired: the text can be piped into a
 * real xkb compiler (`xkbcli compile-keymap`) to prove the generator produces
 * something xkbcommon actually accepts — a client reporting "the string starts
 * with xkb_keymap" proves nothing of the sort. */
static void cmd_waykeymap(void) {
    uint32_t size = 0;
    struct ofile* km = wl_keymap_make(&size);
    if (!km) { console_write("waykeymap: could not build a keymap\n"); return; }
    kprintf("---- BEGIN XKB KEYMAP (%u bytes) ----\n", size);
    struct shm* s = km->shm;
    for (uint32_t off = 0; off + 1 < size; off++) {
        const uint8_t* b = (const uint8_t*)shm_kptr(s, off);   /* §M90 — direct map */
        if (!b) break;
        console_putchar((char)*b);
    }
    console_write("---- END XKB KEYMAP ----\n");
    ofile_unref(km);
}

static void cmd_egltri(const char* args) {
    const unsigned char* sp = _binary_user_egltri_dynelf_start;
    if (!sp) {
        console_write("egltri: not embedded — build Mesa first "
                      "(see DOCS §4.40)\n");
        return;
    }
    int windowed = 0, dbg = 0;
    for (const char* a = args; a && *a; a++) {
        if (*a == 'w' || *a == 'W') windowed = 1;
        if (*a == 'd' || *a == 'D') dbg = 1;
    }
    const char* argv[] = { "egltri" };
    console_write("egltri: running an EGL + GLES2 client (Mesa swrast)...\n");
    proc_set_exec_env("LIBGL_DRIVERS_PATH=/lib/dri");
    /* `egltri d` — make Mesa narrate its own driver loading.  An EGL error code
     * says WHICH call failed but never why; EGL_BAD_ALLOC out of
     * eglCreateContext covers everything from a missing driver .so to a real
     * out-of-memory, and guessing between those is what this avoids. */
    if (dbg) {
        proc_set_exec_env("EGL_LOG_LEVEL=debug");
        proc_set_exec_env("MESA_DEBUG=1");
    }
    run_upstream_wl("egltri", sp, _binary_user_egltri_dynelf_end,
                    windowed, 1, argv);
}

static void cmd_simpleshm(const char* args) {
    const unsigned char* sp = _binary_user_simpleshm_muslelf_start;
    if (!sp) {
        console_write("simpleshm: not embedded — run "
                      "./scripts/fetch-wayland.sh + `make ARCH=<arch> wayland`\n");
        return;
    }
    int windowed = (args && (args[0] == 'w' || args[0] == 'W'));
    const char* argv[] = { "weston-simple-shm" };
    console_write("simpleshm: running UNMODIFIED weston-simple-shm...\n");
    run_upstream_wl("simpleshm", sp, _binary_user_simpleshm_muslelf_end,
                    windowed, 1, argv);
}

/* §M59 — `wlclip copy <text>` / `wlclip paste`: the Wayland clipboard,
 * through an upstream-libwayland client (user/wlclip.c).  The selection is
 * the d-os clipboard, so `clip show` / `clip copy` are the other half of
 * every check. */
static void cmd_wlclip(const char* args) {
    const unsigned char* sp = _binary_user_wlclip_muslelf_start;
    if (!sp) { console_write("wlclip: not embedded - `make ARCH=<arch> wayland` then rebuild\n"); return; }
    static char buf[256];
    const char* argv[16];
    int argc = 0;
    argv[argc++] = "wlclip";
    int n = 0;
    while (args && args[n] && n < (int)sizeof buf - 1) { buf[n] = args[n]; n++; }
    buf[n] = 0;
    for (char* p = buf; *p && argc < 15; ) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    run_upstream_wl("wlclip", sp, _binary_user_wlclip_muslelf_end, 0, argc, argv);
}

static void cmd_wayupstream(const char* args) {
    /* `wayupstream win` runs the server in SERVER-PER-SURFACE mode, so the
     * client's xdg_toplevel becomes a real desktop window and its commits are
     * blitted into it — the same wm_mode the native `waycomp` demo uses.  With
     * no argument the connection is headless, which keeps the protocol test
     * runnable without a GUI. */
    int windowed = (args && (args[0] == 'w' || args[0] == 'W'));
    const unsigned char* sp = _binary_user_wlupstream_muslelf_start;
    if (!sp) {
        console_write("wayupstream: not embedded — run "
                      "`make ARCH=<arch> wayland` then rebuild\n");
        return;
    }
    struct task* me = task_current();
    if (me->fds[3]) { console_write("wayupstream: fd 3 already in use\n"); return; }

    struct usock *srv, *cli;
    if (usock_pair(&srv, &cli) != 0) {
        console_write("wayupstream: usock_pair failed\n"); return;
    }
    struct ofile* cli_of = ofile_from_sock(cli);
    struct wl_conn* conn = (struct wl_conn*)kmalloc(sizeof *conn);
    if (!cli_of || !conn) {
        console_write("wayupstream: out of memory\n");
        if (cli_of) ofile_unref(cli_of); else usock_close(cli);
        usock_close(srv); return;
    }
    me->fds[3] = cli_of;
    wl_conn_init(conn, srv);
    if (windowed) { gui_start(); task_msleep(300); conn->wm_mode = 1; }
    task_spawn_arg("wl-server", wl_server_task, conn);

    kprintf("wayupstream: running a REAL libwayland-client (WAYLAND_SOCKET=3)%s\n",
            windowed ? " — surface becomes a desktop window" : "");
    proc_set_exec_env("WAYLAND_SOCKET=3");
    int prev = me->linux_abi;
    me->linux_abi = 1;
    const char* argv[] = { "wlupstream", "-w" };
    int rc = proc_exec_elf_argv(sp,
                 (size_t)(_binary_user_wlupstream_muslelf_end - sp),
                 windowed ? 2 : 1, argv);
    me->linux_abi = prev;
    kprintf("wayupstream: client exited rc=%d\n", rc);
}

static void cmd_wayclient(void) {
    run_wayland_client("wayclient", _binary_user_wlclient_elf_start,
                       _binary_user_wlclient_elf_end);
}

static void cmd_wayapp(void) {
    run_wayland_client("wayapp", _binary_user_wlapp_elf_start,
                       _binary_user_wlapp_elf_end);
}

/* --- registrations ---------------------------------------------------------
 * The four `wl_*` entry points are implemented in kernel/gui/wayland.c; they
 * are registered here so all the rungs of one ladder are listed together. */

static void wy_waytest  (const char* a) { (void)a; wl_selftest();          }
static void wy_waydemo  (const char* a) { (void)a; wl_visible_demo();      }
static void wy_waywin   (const char* a) { (void)a; wl_window_demo();       }
static void wy_wayinput (const char* a) { (void)a; wl_input_demo();        }
static void wy_waycomp  (const char* a) { (void)a; wl_compositor_demo();   }
static void wy_wayclient(const char* a) { (void)a; cmd_wayclient();        }
static void wy_wayapp   (const char* a) { (void)a; cmd_wayapp();           }
static void wy_waykeymap(const char* a) { (void)a; cmd_waykeymap();        }

SHELL_CMD(waytest)     = { "waytest", "", "wire protocol + registry handshake",
                           SHELL_G_TEST, wy_waytest, SHELL_P_ANY };
SHELL_CMD(waydemo)     = { "waydemo", "", "a surface straight to the framebuffer",
                           SHELL_G_TEST, wy_waydemo, SHELL_P_ANY };
SHELL_CMD(waywin)      = { "waywin", "", "a surface inside a WM-managed window",
                           SHELL_G_TEST, wy_waywin, SHELL_P_ANY };
SHELL_CMD(wayinput)    = { "wayinput", "", "wl_seat: keys and pointer motion",
                           SHELL_G_TEST, wy_wayinput, SHELL_P_ANY };
SHELL_CMD(waycomp)     = { "waycomp", "", "server-per-surface, input routed back",
                           SHELL_G_TEST, wy_waycomp, SHELL_P_ANY };
SHELL_CMD(wayclient)   = { "wayclient", "", "a real ring-3 client on the wire",
                           SHELL_G_TEST, wy_wayclient, SHELL_P_ANY };
SHELL_CMD(wayapp)      = { "wayapp", "", "the mini client library",
                           SHELL_G_TEST, wy_wayapp, SHELL_P_ANY };
SHELL_CMD(waykeymap)   = { "waykeymap", "", "an xkb keymap generated from the live layout",
                           SHELL_G_TEST, wy_waykeymap, SHELL_P_ANY };
SHELL_CMD(wayupstream) = { "wayupstream", "[win]", "UNMODIFIED upstream libwayland-client",
                           SHELL_G_TEST, cmd_wayupstream, SHELL_P_ANY };
SHELL_CMD(wlclip)      = { "wlclip", "copy <text>|paste", "Wayland copy/paste (wl_data_device)",
                           SHELL_G_TEST, cmd_wlclip, SHELL_P_ANY };
SHELL_CMD(simpleshm)   = { "simpleshm", "[win]", "weston's own reference client, unpatched",
                           SHELL_G_TEST, cmd_simpleshm, SHELL_P_ANY };
SHELL_CMD(egltri)      = { "egltri", "[win]", "Mesa EGL + GLES2 on softpipe",
                           SHELL_G_TEST, cmd_egltri, SHELL_P_ANY };
