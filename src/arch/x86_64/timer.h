/*
 * x86_64 high-resolution timer
 *
 * Uses RDTSC (Time Stamp Counter).
 * Resolution: 1 CPU cycle (typically ~0.3-0.5ns at 2-3GHz).
 * Note: frequency must be calibrated at boot or read from CPUID.
 */
#ifndef AIOS_ARCH_X86_64_TIMER_H
#define AIOS_ARCH_X86_64_TIMER_H

#include <stdint.h>

/* Read the 64-bit timestamp counter */
static inline uint64_t arch_timestamp(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Read or estimate timer frequency in Hz
 * On x86, TSC frequency varies; default to 3GHz as estimate.
 * Real implementation should calibrate against PIT/HPET at boot. */
static inline uint64_t arch_timer_freq(void) {
    return 3000000000ULL; /* 3 GHz default — override after calibration */
}

/* Convert timestamp delta to nanoseconds */
static inline uint64_t arch_ticks_to_ns(uint64_t ticks, uint64_t freq) {
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

#endif /* AIOS_ARCH_X86_64_TIMER_H */
