/* =============================================================================
 * wedgewin.c — a GUI client that opens a window and then FREEZES (§M46/§M47.1).
 *
 * `wedge` proves a wedged ring-3 task can be reclaimed.  This proves the thing
 * the user actually experiences: **the window chrome still works when the app
 * behind it is frozen.**  That guarantee had no automated test, which is how a
 * regression in the opposite direction slipped in — the compositor started
 * force-killing healthy clients on the very first pass, so an ordinary "close
 * the browser" was recorded as a crash.
 *
 * Sequence: DOSGUI_CREATE a window, present one frame so it is visibly there,
 * then spin forever WITHOUT ever polling for events.  The close event can
 * therefore never be observed by this program: clicking the title-bar X must
 * still make the window go away, via the compositor's force-kill fallback after
 * the grace period.
 *
 * Built as a musl/Linux-ABI program because the dosgui bridge is reached through
 * the Linux personality's syscall range (0xD05x).
 * ============================================================================= */

#include <stdint.h>
#include <unistd.h>

#define DOSGUI_CREATE  0xD050
#define DOSGUI_PRESENT 0xD051

#define W 320
#define H 200

static uint32_t fb[W * H];

int main(void) {
    long h = syscall(DOSGUI_CREATE, (long)W, (long)H, (long)"Wedged App");
    if (h < 0) {
        write(1, "wedgewin: DOSGUI_CREATE failed\n", 31);
        return 1;
    }

    /* One frame, so the window is unmistakably on screen before we freeze. */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            fb[y * W + x] = ((x ^ y) & 32) ? 0xFF902020u : 0xFF401010u;
    syscall(DOSGUI_PRESENT, h, (long)fb, (long)W, (long)H, (long)W);

    write(1, "wedgewin: window up — now freezing forever (close it with the X)\n", 65);

    /* No poll, no yield, no syscall: from here on this program is unreachable
     * by anything cooperative.  Only the force-kill safe point can end it. */
    for (;;)
        for (volatile long j = 0; j < 1000000; j++) { }

    return 0;
}
