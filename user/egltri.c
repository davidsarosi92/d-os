/* =============================================================================
 * egltri.c — EGL + GLES2 through a Wayland surface (§M40 definition of done).
 *
 * The milestone's remaining DoD: "an EGL+GLES2 program clears + draws a triangle
 * via a software rasteriser, presented through a Wayland buffer."  This is that
 * program, written the way any Wayland/EGL application is: nothing here knows it
 * is running on d-os.
 *
 * The stack under it, bottom-up:
 *   d-os Wayland server (§M26)  ← the compositor
 *   libwayland-client (upstream, §M40 stage 1)
 *   libwayland-egl              ← turns a wl_surface into an EGLNativeWindow
 *   Mesa EGL + GLES2 (swrast)   ← the software rasteriser
 * and, underneath all of it, musl pthreads (stage 9) and the shm + fd-passing
 * path (stage 2), because Mesa's swrast presents through wl_shm buffers.
 * ============================================================================= */

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <xdg-shell-client-protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define FRAMES 500      /* ~20 s at 25 fps — long enough to see + screenshot */
#define W 200
#define H 200

static struct wl_compositor *compositor;
static struct xdg_wm_base   *wm_base;
static int configured;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver)
{
    (void)d; (void)ver;
    if (!strcmp(iface, "wl_compositor"))
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base"))
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{ (void)d; configured = 1; xdg_surface_ack_configure(xs, serial); }
static const struct xdg_surface_listener xs_listener = { xs_configure };

/* The smallest shader pair that still proves the whole pipeline ran: geometry
 * in, colour out.  A clear alone would not distinguish "GL works" from "the
 * buffer happened to be that colour". */
static const char *VS =
    "attribute vec2 pos;\n"
    "uniform float phase;\n"
    "void main() {\n"
    "  float c = cos(phase), s = sin(phase);\n"
    "  gl_Position = vec4(pos.x * c - pos.y * s, pos.x * s + pos.y * c, 0.0, 1.0);\n"
    "}\n";
static const char *FS =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(1.0, 0.55, 0.1, 1.0); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        printf("egltri: shader compile failed: %s\n", log);
        return 0;
    }
    return s;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { printf("egltri: wl_display_connect failed\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!compositor || !wm_base) {
        printf("egltri: missing wl_compositor/xdg_wm_base\n"); return 1;
    }

    struct wl_surface *surf = wl_compositor_create_surface(compositor);
    struct xdg_surface *xsurf = xdg_wm_base_get_xdg_surface(wm_base, surf);
    xdg_surface_add_listener(xsurf, &xs_listener, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_set_title(top, "EGL triangle");
    wl_surface_commit(surf);
    wl_display_roundtrip(dpy);

    /* wl_egl_window is the bridge: EGL renders into it, and libwayland-egl
     * turns each swap into a wl_surface attach + commit. */
    struct wl_egl_window *win = wl_egl_window_create(surf, W, H);
    if (!win) { printf("egltri: wl_egl_window_create failed\n"); return 1; }

    /* eglGetPlatformDisplay, NOT eglGetDisplay: the legacy call makes Mesa
     * AUTODETECT the platform and, if it does not recognise the pointer as a
     * wl_display, open a connection of its own with wl_display_connect(NULL).
     * That cannot work here — libwayland unsets WAYLAND_SOCKET once the first
     * connect has consumed it — so Mesa would end up with a NULL wl_display and
     * crash in wl_proxy_create_wrapper the moment a window surface is created.
     * Naming the platform and handing over OUR display removes the guess. */
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay ed = getPlatformDisplay
        ? getPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, dpy, NULL)
        : eglGetDisplay((EGLNativeDisplayType)dpy);
    if (ed == EGL_NO_DISPLAY) { printf("egltri: no EGL display\n"); return 1; }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(ed, &major, &minor)) {
        printf("egltri: eglInitialize failed (0x%x)\n", eglGetError()); return 1;
    }
    printf("egltri: EGL %d.%d — vendor '%s'\n", major, minor,
           eglQueryString(ed, EGL_VENDOR));
    printf("egltri: EGL_VERSION '%s'\n", eglQueryString(ed, EGL_VERSION));
    /* Which APIs the driver actually brought.  If GLES is missing here, a
     * failing eglCreateContext is a BUILD problem (the GLES frontend was not
     * compiled in), not a runtime one — and the EGL error alone cannot tell
     * those apart, since the DRI layer collapses every context failure into
     * EGL_BAD_ALLOC. */
    printf("egltri: EGL_CLIENT_APIS '%s'\n", eglQueryString(ed, EGL_CLIENT_APIS));
    printf("egltri: EGL_EXTENSIONS '%s'\n", eglQueryString(ed, EGL_EXTENSIONS));

    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(ed, cfg_attr, &cfg, 1, &n) || n < 1) {
        printf("egltri: eglChooseConfig found no config (0x%x)\n", eglGetError());
        return 1;
    }
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    /* Checked SEPARATELY and immediately.  eglGetError() reports the error of
     * the LAST call and clears it, so testing both results together and then
     * asking once will report EGL_SUCCESS whenever the first call failed and
     * the second succeeded — which is exactly what the i386 bring-up hit. */
    EGLContext ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        printf("egltri: eglCreateContext(GLES2) failed (0x%x)\n", eglGetError());
        /* Is the client simply out of memory?  The DRI layer collapses a
         * context allocation failure into the same EGL_BAD_ALLOC it uses for
         * everything else, so ask the allocator directly rather than inferring.
         * (Do NOT probe EGL again here — a second eglCreateContext after a
         * failed one walks half-initialised driver state and faults.) */
        size_t probe = 32u << 20;
        void* p32 = malloc(probe);
        printf("egltri: 32 MiB malloc after failure: %s\n", p32 ? "OK" : "FAILED");
        free(p32);
        return 1;
    }
    EGLSurface es = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)win, NULL);
    if (es == EGL_NO_SURFACE) {
        printf("egltri: eglCreateWindowSurface failed (0x%x)\n", eglGetError()); return 1;
    }
    if (!eglMakeCurrent(ed, es, es, ctx)) {
        printf("egltri: eglMakeCurrent failed (0x%x)\n", eglGetError()); return 1;
    }
    printf("egltri: GL_RENDERER '%s'\n", (const char *)glGetString(GL_RENDERER));
    printf("egltri: GL_VERSION  '%s'\n", (const char *)glGetString(GL_VERSION));

    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) return 1;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { printf("egltri: link failed\n"); return 1; }
    glUseProgram(prog);

    static const GLfloat tri[] = { 0.0f, 0.8f, -0.8f, -0.7f, 0.8f, -0.7f };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);

    /* Animated, and paced to real time: a still frame cannot distinguish "GL
     * rendered this" from "the buffer happened to be that colour", and a
     * free-running loop finishes before anyone (or any screenshot) sees it. */
    GLint uloc = glGetUniformLocation(prog, "phase");
    for (int frame = 0; frame < FRAMES; frame++) {
        glViewport(0, 0, W, H);
        glClearColor(0.05f, 0.10f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (uloc >= 0) glUniform1f(uloc, frame * 0.06f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (!eglSwapBuffers(ed, es)) {
            printf("egltri: eglSwapBuffers failed (0x%x)\n", eglGetError());
            return 1;
        }
        if (frame == 0) printf("egltri: first frame swapped\n");
        wl_display_dispatch_pending(dpy);
        struct timespec ts = { 0, 40 * 1000 * 1000 };   /* ~25 fps */
        nanosleep(&ts, NULL);
    }

    printf("egltri: PASS — GLES2 triangle rendered and presented via Wayland\n");
    eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(ed);
    wl_display_disconnect(dpy);
    return 0;
}
