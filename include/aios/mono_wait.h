/* mono_wait.h -- time-based poll bound using the ARM generic timer (cntpct_el0).
 *
 * Replaces the fixed iteration-count "timeouts" in polled drivers. An iteration
 * count is NOT a real timeout: its duration is CPU-speed dependent, so on the
 * 1.5GHz A72 a loop bounded by 10,000,000 spun ~32.6s when a status bit was
 * missed (a known reality of polled hardware), while QEMU finished instantly.
 * That was the v0.4.176 eMMC stall (the netconsole relay hang); the same
 * anti-pattern lived in the VC mailbox + GENET + virtio probe loops.
 *
 * Usage (the body, breaks, continues, and timeout fall-through stay unchanged):
 *     for (uint64_t dl = mono_deadline_ms(2000); mono_before(dl); ) {
 *         arch_dmb();
 *         if (done) break;
 *     }
 *     // fell through -> timed out; existing error handling runs
 *
 * aarch64 only (these are arm platform drivers). The cntpct read is ~ns, so the
 * poll cadence is unchanged -- only the bound becomes real wall-clock time.
 */
#ifndef _AIOS_MONO_WAIT_H
#define _AIOS_MONO_WAIT_H

#include <stdint.h>

static inline uint64_t mono_ticks(void)
{
    uint64_t c;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c));
    return c;
}

static inline uint64_t mono_deadline_ms(int ms)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 54000000;   /* BCM2711 generic-timer fallback */
    return mono_ticks() + (uint64_t)ms * f / 1000;
}

static inline int mono_before(uint64_t deadline)
{
    return mono_ticks() < deadline;
}

#endif /* _AIOS_MONO_WAIT_H */
