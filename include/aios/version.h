#ifndef _AIOS_VERSION_H
#define _AIOS_VERSION_H

/* ── AIOS Semantic Version ──
 *   X.Y.Z   where:
 *     X = major (architecture changes)
 *     Y = minor (major milestones — new subsystem, new PD, etc.)
 *     Z = patch (each git checkin / milestone)
 *
 *   Build number increments on EVERY successful compile.
 *   It is NOT reset when Z bumps.
 */

#define AIOS_VERSION_MAJOR  0
#define AIOS_VERSION_MINOR  2
#define AIOS_VERSION_PATCH  33

/* Build number — auto-generated, do NOT edit by hand */
#include "build_number.h"

/* String helpers (two-level macro trick to stringify a number) */
#define _AIOS_STR(x)  #x
#define _AIOS_XSTR(x) _AIOS_STR(x)

#define AIOS_VERSION_STR \
    _AIOS_XSTR(AIOS_VERSION_MAJOR) "." \
    _AIOS_XSTR(AIOS_VERSION_MINOR) "." \
    _AIOS_XSTR(AIOS_VERSION_PATCH)

#define AIOS_VERSION_FULL \
    "AIOS v" AIOS_VERSION_STR \
    " (build " _AIOS_XSTR(AIOS_BUILD_NUMBER) ")"

/* Packed 32-bit version for runtime comparison:
 *   bits 31-24 = major, 23-16 = minor, 15-0 = patch */
#define AIOS_VERSION_CODE \
    ((AIOS_VERSION_MAJOR << 24) | (AIOS_VERSION_MINOR << 16) | AIOS_VERSION_PATCH)

#endif
