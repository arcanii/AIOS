#ifndef AIOS_X86_64_BARRIERS_H
#define AIOS_X86_64_BARRIERS_H

/* x86_64 memory barriers for device I/O and ordering.
 * x86 has a stronger memory model than ARM, but explicit
 * barriers are still needed for device MMIO. */

#define arch_dmb()  __asm__ volatile("mfence" ::: "memory")
#define arch_dsb()  __asm__ volatile("mfence" ::: "memory")
#define arch_isb()  __asm__ volatile(""       ::: "memory")

/* Combined barrier + serialize */
#define arch_dsb_isb() __asm__ volatile("mfence" ::: "memory")

#endif /* AIOS_X86_64_BARRIERS_H */
