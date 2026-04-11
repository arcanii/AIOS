#ifndef AIOS_AARCH64_BARRIERS_H
#define AIOS_AARCH64_BARRIERS_H

/* AArch64 memory barriers for device I/O and ordering */

#define arch_dmb()  __asm__ volatile("dmb sy" ::: "memory")
#define arch_dsb()  __asm__ volatile("dsb sy" ::: "memory")
#define arch_isb()  __asm__ volatile("isb"    ::: "memory")

/* Combined DSB+ISB for cache/instruction coherence */
#define arch_dsb_isb() __asm__ volatile("dsb sy\n\tisb" ::: "memory")

#endif /* AIOS_AARCH64_BARRIERS_H */
