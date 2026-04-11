#ifndef AIOS_ARCH_H
#define AIOS_ARCH_H

/* Architecture dispatcher -- select the right headers at compile time.
 * seL4 sets __aarch64__ or __x86_64__ via the toolchain. */

#if defined(__aarch64__)
#  include "aarch64/barriers.h"
#  include "aarch64/page.h"
#elif defined(__x86_64__)
#  include "x86_64/barriers.h"
#  include "x86_64/page.h"
#else
#  error "Unsupported architecture"
#endif

#endif /* AIOS_ARCH_H */
