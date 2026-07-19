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

#define DOS_MILESTONE  "M43"                 /* latest shipped: §M43 on-device tcc compiler */
#define DOS_LABEL      "d-os " DOS_MILESTONE

/* Semantic kernel version — the single source of truth for "which build am I".
 * Every built-in component (drivers, services, modules, shell providers) that
 * does not carry its own version defaults to this, so EVERYTHING is versioned
 * and you can tell what you have + whether an update applies.  0.<M>.0 tracks
 * the milestone; bump the patch for sub-milestone builds.
 * (Swappable units — packages, the runtime libc, the pkg backend — carry their
 * OWN versions; this is the baseline for the non-swappable core.) */
#define DOS_VERSION    "0.43.0"

#endif /* VERSION_H */
