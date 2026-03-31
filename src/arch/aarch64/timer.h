/*
 * AArch64 high-resolution timer
 *
 * Uses the ARM Generic Timer (cntpct_el0 / cntfrq_el0).
 * Available from EL0 on most configurations, always from EL1.
 * Resolution: typically 1/62.5MHz = 16ns on QEMU virt.
 */
#ifndef AIOS_ARCH_AARCH64_TIMER_H
#define AIOS_ARCH_AARCH64_TIMER_H

#include <stdint.h>

/* Read the 64-bit cycle counter */
static inline uint64_t arch_timestamp(void) {
    uint64_t cnt;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    return cnt;
}

/* Read the timer frequency in Hz */
static inline uint64_t arch_timer_freq(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000; /* QEMU virt default */
    return freq;
}

/* Convert timestamp delta to nanoseconds */
static inline uint64_t arch_ticks_to_ns(uint64_t ticks, uint64_t freq) {
    /* Avoid overflow: ticks * 1000000000 / freq */
    /* For freq ~62.5MHz, ticks < 2^54 is safe with this method */
    return (ticks / freq) * 1000000000ULL +
           ((ticks % freq) * 1000000000ULL) / freq;
}

/* Convert timestamp delta to microseconds */
static inline uint64_t arch_ticks_to_us(uint64_t ticks, uint64_t freq) {
    return (ticks / freq) * 1000000ULL +
           ((ticks % freq) * 1000000ULL) / freq;
}

/* Convert timestamp delta to milliseconds */
static inline uint64_t arch_ticks_to_ms(uint64_t ticks, uint64_t freq) {
    return (ticks * 1000ULL) / freq;
}

#endif /* AIOS_ARCH_AARCH64_TIMER_H */
