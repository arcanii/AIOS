#ifndef _AIOS_VERSION_H
#define _AIOS_VERSION_H

#define AIOS_VERSION_MAJOR  0
#define AIOS_VERSION_MINOR  4
#define AIOS_VERSION_PATCH  26

/* Build number — auto-generated, do NOT edit by hand */
#include "build_number.h"

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
