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

#define DOS_MILESTONE  "M37"                 /* latest shipped: §M37 dynamic linking */
#define DOS_LABEL      "d-os " DOS_MILESTONE

#endif /* VERSION_H */
