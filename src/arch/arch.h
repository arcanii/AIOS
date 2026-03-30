/*
 * Architecture abstraction layer
 *
 * Include this instead of arch-specific headers.
 * The build system defines AIOS_ARCH_AARCH64 or AIOS_ARCH_X86_64.
 */
#ifndef AIOS_ARCH_H
#define AIOS_ARCH_H

#if defined(AIOS_ARCH_AARCH64) || defined(__aarch64__)

#include "aarch64/cache.h"
#include "aarch64/context.h"

#elif defined(AIOS_ARCH_X86_64) || defined(__x86_64__)

#include "x86_64/cache.h"
#include "x86_64/context.h"

#else
#error "Unsupported architecture — define AIOS_ARCH_AARCH64 or AIOS_ARCH_X86_64"
#endif

#endif /* AIOS_ARCH_H */
