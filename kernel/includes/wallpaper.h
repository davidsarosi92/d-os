/* =============================================================================
 * wallpaper.h — what fills the desktop background (§M60).
 *
 * Until this milestone the background was one call to `gfx_vgradient` in
 * `gui_start`: not configurable, not changeable, not a thing anybody could
 * point at.  Here it becomes a SOURCE chosen by config:
 *
 *     gui.wallpaper      = gradient            (the built-in, and the fallback)
 *                        | solid:RRGGBB        (a flat colour, hex, no '#')
 *                        | /path/to/image.bmp  (a file on the VFS)
 *     gui.wallpaper_fit  = fill | stretch | center | tile      (default: fill)
 *
 * Two contracts worth stating up front, because both are the kind of thing
 * that is painful to retrofit:
 *
 *   1. `wallpaper_render` ALWAYS leaves the destination fully painted.  A
 *      missing file, an unsupported format, a truncated read — every failure
 *      falls back to the gradient and records WHY (`wallpaper_status`).  A
 *      desktop that refuses to come up because an image moved is a worse
 *      desktop than one with a gradient, and a silent black screen is worse
 *      than either.
 *
 *   2. Decoding NEVER holds the whole image in memory.  Rows are read from
 *      the file and sampled straight into the destination, so the peak cost
 *      of a 1920×1200 wallpaper is one source row (~7.7 KB), not 9 MB of
 *      decoded pixels plus a 6.9 MB file buffer.  The destination surface
 *      the compositor already owns is the only large allocation involved.
 *
 * Format support is deliberately ONE format — uncompressed BMP (24/32 bpp,
 * both row orders).  It is what any tool can produce, it needs no allocator
 * games, and anything richer (PNG/JPEG/GIF) belongs in ring 3 where the
 * decoders already are (§M42 ships nsgif/nsbmp as store packages): a
 * kernel-resident image codec is an attack surface with a mouse attached to
 * it.  The ring-3 path hands over already-decoded pixels; that is §M63's
 * Personalisation panel, not this file.
 * ============================================================================= */

#ifndef WALLPAPER_H
#define WALLPAPER_H

struct gfx_surface;

/* Where the embedded default image is written on first GUI start, and the
 * default value of `gui.wallpaper`.  A path rather than a special value, so
 * the shipped picture is an ordinary file the user can replace, copy or
 * delete — an embedded default that cannot be replaced is a hardcoded
 * background wearing a config key. */
#define WALLPAPER_DEFAULT_PATH  "/usr/share/wallpapers/default.bmp"

/* Write the embedded default image to WALLPAPER_DEFAULT_PATH if it is not
 * already there.  Called once from gui_start, before the first render.
 * Returns 0 when the file exists afterwards. */
int wallpaper_provision(void);

/* Draw a BMP into `dst` — centred, or scaled to cover.  Exposed so §M62's boot
 * splash reuses THIS decoder instead of growing a second one; anything that
 * cannot be decoded leaves `dst` untouched and returns non-zero, so the caller
 * keeps its fallback. */
int wallpaper_draw_bmp(struct gfx_surface* dst, const char* path, int centered);

/* Paint the desktop background into `dst` according to the config keys above.
 * Returns 0 if the requested source was used, -1 if it fell back to the
 * gradient (the surface is painted either way). */
int wallpaper_render(struct gfx_surface* dst);

/* Change the source at runtime.  Accepts the same spellings as the config
 * value.  Updates the key, repaints the live wallpaper and damages the whole
 * screen when the GUI is running.  Returns as `wallpaper_render`; -2 means
 * the spec itself was rejected (nothing changed). */
int wallpaper_set(const char* spec);

/* Change the fit mode ("fill" | "stretch" | "center" | "tile").  Returns as
 * `wallpaper_set`. */
int wallpaper_set_fit(const char* fit);

/* One line describing what is actually on screen, and — when that is not what
 * was asked for — why.  Owned by this module; do not free. */
const char* wallpaper_status(void);

/* The `wallpaper` shell command, implemented HERE rather than in a shell.
 * §M24's lesson: a command that lives in `shell.c` can only run on the arches
 * that build it, and aarch64 runs its own `serial_shell.c` — so a wallpaper
 * test written in the x86 shell would silently never run on ARM, which is
 * exactly where the framebuffer differs most (virtio-gpu, not a linear LFB).
 * `args` is everything after the command word (may be empty).
 *
 * Sub-commands: (none) status | gradient|solid:…|<path> | fit <mode> |
 * testimg <path> [w h] | check [w h].  `check` renders the current
 * configuration into an OFF-SCREEN surface and prints corner pixels plus a
 * checksum — the only way to verify what was drawn on a machine with no
 * framebuffer, which is exactly the situation of the aarch64 test harness. */
void wallpaper_cmd(const char* args);

/* Test scaffolding (`wallpaper testimg <path>`): write a generated w×h BMP to
 * the VFS.  The automated path needs an image that came through the real file
 * system and the real decoder; without this, testing the decoder means
 * provisioning a file from the host, which the headless boot cannot do.
 * Returns 0 on success. */
int wallpaper_write_test_bmp(const char* path, int w, int h);

#endif
