/* =============================================================================
 * version.h — the single source of truth for the "current milestone" string.
 *
 * The desktop draws DOS_LABEL in the wallpaper corner.  Bump DOS_MILESTONE when
 * a milestone ships, so the on-screen label always names the most recently
 * completed M number (nothing else needs touching — the label sizes itself to
 * the string length).
 * ============================================================================= */
#ifndef VERSION_H
#define VERSION_H

#define DOS_MILESTONE  "M47"                 /* latest shipped: §M47 crash records + pluggable reporting */

/* Short architecture tag for the on-screen label.  Deliberately the FAMILIAR
 * short form ("x32"/"x64") rather than the toolchain triple — the wallpaper
 * label answers "which build am I looking at" at a glance, and a screenshot of
 * a bug report should say so without anyone having to ask.  The verbose name
 * (hal_arch_name(): "i386"/"x86_64"/"aarch64") stays what `uname` reports. */
#if   defined(__x86_64__)
#  define DOS_ARCH_TAG "x64"
#elif defined(__i386__)
#  define DOS_ARCH_TAG "x32"
#elif defined(__aarch64__)
#  define DOS_ARCH_TAG "arm64"
#else
#  define DOS_ARCH_TAG "?"
#endif

#define DOS_LABEL      "d-os " DOS_MILESTONE "  " DOS_ARCH_TAG

/* Semantic kernel version — the single source of truth for "which build am I".
 * Every built-in component (drivers, services, modules, shell providers) that
 * does not carry its own version defaults to this, so EVERYTHING is versioned
 * and you can tell what you have + whether an update applies.  0.<M>.0 tracks
 * the milestone; bump the patch for sub-milestone builds.
 * (Swappable units — packages, the runtime libc, the pkg backend — carry their
 * OWN versions; this is the baseline for the non-swappable core.) */
#define DOS_VERSION    "0.47.0"

#endif /* VERSION_H */
