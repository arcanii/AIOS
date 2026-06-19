#ifndef _AIOS_VERSION_H
#define _AIOS_VERSION_H

#define AIOS_VERSION_MAJOR  0
#define AIOS_VERSION_MINOR  4
#define AIOS_VERSION_PATCH  265

/* Build number — auto-generated, do NOT edit by hand */
#include "build_number.h"

/* Build date -- set at compile time */
#define AIOS_BUILD_DATE __DATE__

/* Build time -- full "Wkd Mon DD HH:MM:SS TZ YYYY" stamp written each build by
 * scripts/bump-build.sh (weekday + timezone are not available from the C
 * __DATE__/__TIME__ macros, so it captures the host date command). Gitignored and
 * created by setup-linux.py, like build_number.h. Fall back to the compiler macros
 * if the generated header is absent (fresh tree before the first build). */
#if defined(__has_include)
#  if __has_include("build_time.h")
#    include "build_time.h"
#  endif
#endif
#ifndef AIOS_BUILD_TIME
#  define AIOS_BUILD_TIME __DATE__ " " __TIME__
#endif

#define _AIOS_STR(x)  #x
#define _AIOS_XSTR(x) _AIOS_STR(x)

#define AIOS_VERSION_STR \
    _AIOS_XSTR(AIOS_VERSION_MAJOR) "." \
    _AIOS_XSTR(AIOS_VERSION_MINOR) "." \
    _AIOS_XSTR(AIOS_VERSION_PATCH)

#define AIOS_VERSION_FULL \
    "AIOS v" AIOS_VERSION_STR \
    " (build " _AIOS_XSTR(AIOS_BUILD_NUMBER) ")"

#define AIOS_VERSION_CODE \
    ((AIOS_VERSION_MAJOR << 24) | (AIOS_VERSION_MINOR << 16) | AIOS_VERSION_PATCH)

#endif
