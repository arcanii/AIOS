/*
 * cpu_gov.c -- RPi4 load-driven DVFS governor (the only power lever compatible
 * with the no-WFI stall cure; core-parking re-triggers the 32.4s tlbi stall).
 *
 * MECHANISM (HW-verified 2026-06-14, see feedback_rpi4_thermal_clock):
 *   - actuator: hw_arm_clock_set (VC mailbox SET_CLOCK_RATE id 3). Confirmed it
 *     PINS 300/450/600 and the firmware does NOT auto-boost back under load, so
 *     explicit control is required. Needs config.txt `arm_freq_min=300` to open
 *     the floor (arm_freq stays the heat-capped 600 ceiling); without it the set
 *     clamps to 600 and the governor is a harmless no-op.
 *   - metric: the root main-loop iteration rate (g_root_loop_iters). The root
 *     SPINS on RPi4 (no-WFI), so the rate collapses ~45x under load (idle
 *     ~12000/s -> busy ~260/s at 300 MHz; ~2x both at 600). cntpct is a fixed-rate
 *     timer (clock-independent), so the rate is computed over real wall-clock and
 *     the huge idle/busy gap makes one threshold work at any clock.
 *
 * POLICY: sample every GOV_TICK_MS. rate < LOW => BUSY => jump to MAX now. rate >
 * HIGH for GOV_IDLE_TICKS in a row => settle to MIN. Raise fast, lower gently. MAX
 * is the firmware ceiling (~65C, well under the 85C throttle) so no separate
 * thermal cap is needed. cpu_gov_tick() runs from the root main loop; it is cheap
 * (a cntpct read + a compare) and only Calls the mailbox on a transition.
 */
#include <stdint.h>
#include <stdio.h>
#include "aios/hw_info.h"   /* g_root_loop_iters, hw_arm_clock_set */

#ifdef PLAT_RPI4

#define GOV_MIN_MHZ    300u
#define GOV_MAX_MHZ    600u
#define GOV_LOW_RATE   2000u   /* loop rate below this => BUSY -> boost to MAX */
#define GOV_HIGH_RATE  6000u   /* above this => idle candidate -> may drop to MIN */
#define GOV_IDLE_TICKS 2       /* consecutive idle windows before downclock (~2s) */
/* 1s averaging window. The per-250ms loop rate is wildly noisy on HW (HW-measured
 * rmin=95 .. rmax=75903) -- a single ~250ms core-0 stall (scheduler / serverstats /
 * periodic) reads as fully busy and, with the immediate single-tick upclock, kept
 * bouncing the clock off 300. A 1s window dilutes any sub-second stall to a small
 * fraction, so genuine idle reads >>HIGH and the downclock sticks. */
#define GOV_TICK_MS    1000u

static uint64_t rd_cntpct(void) {
    uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
}

static int      gov_enabled   = 1;            /* auto-DVFS ON by default */
static unsigned gov_target    = GOV_MAX_MHZ;  /* gentle start at MAX; settle down when idle */
static uint64_t gov_frq       = 0;            /* cntpct frequency (cached) */
static uint64_t gov_span      = 0;            /* GOV_TICK_MS in cntpct ticks */
static uint64_t gov_last_ct   = 0;
static unsigned long gov_last_iters = 0;
static int      gov_idle_run  = 0;
static unsigned gov_last_rate = 0;
/* diagnostics: why does the governor hold 600 in idle? frq/span check the tick
 * timing math; ticks confirms it fires ~4/s; sets/setfail show transitions vs
 * mailbox failures; rmin/rmax show the rate distribution it actually measures
 * (rmax ~37000 => it sees idle but will not act; rmax ~270 => never measures idle). */
static uint64_t gov_ticks    = 0;
static uint64_t gov_sets     = 0;
static uint64_t gov_setfail  = 0;
static unsigned gov_rate_min = 0xFFFFFFFFu;
static unsigned gov_rate_max = 0;

void cpu_gov_tick(void) {
    if (!gov_frq) {
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(gov_frq));
        if (!gov_frq) gov_frq = 54000000u;          /* RPi4 generic-timer default */
        gov_span = (gov_frq * GOV_TICK_MS) / 1000u;
    }
    uint64_t now = rd_cntpct();
    if (gov_last_ct == 0) { gov_last_ct = now; gov_last_iters = g_root_loop_iters; return; }
    if (now - gov_last_ct < gov_span) return;       /* not a tick boundary yet */

    unsigned long iters = g_root_loop_iters;
    uint64_t dct = now - gov_last_ct;
    unsigned rate = (unsigned)(((uint64_t)(iters - gov_last_iters) * gov_frq) / dct);
    gov_last_rate = rate;
    gov_ticks++;
    if (rate < gov_rate_min) gov_rate_min = rate;
    if (rate > gov_rate_max) gov_rate_max = rate;
    gov_last_ct = now;
    gov_last_iters = iters;

    unsigned want = gov_target;
    if (rate < GOV_LOW_RATE) {                       /* busy: raise immediately */
        want = GOV_MAX_MHZ; gov_idle_run = 0;
    } else if (rate > GOV_HIGH_RATE) {               /* idle: lower after a run */
        if (++gov_idle_run >= GOV_IDLE_TICKS) want = GOV_MIN_MHZ;
    } else {
        gov_idle_run = 0;                            /* mid-band: hold */
    }

    if (want != gov_target) {
        if (gov_enabled) {
            unsigned got = 0;
            if (hw_arm_clock_set(want, &got) == 0) { gov_target = want; gov_sets++; }
            else gov_setfail++;   /* mailbox busy / clamped: keep target, retry next tick */
        } else {
            gov_target = want;                       /* track intent, do not actuate */
        }
    }
}

int cpu_gov_enable(int on) { gov_enabled = on ? 1 : 0; return gov_enabled; }

int cpu_gov_status(char *buf, int sz) {
    return snprintf(buf, sz,
        "governor: %s  target_mhz: %u  loop_rate: %u/s  idle_run: %d\n"
        "gov_dbg: frq=%llu span=%llu ticks=%llu sets=%llu setfail=%llu rmin=%u rmax=%u\n",
        gov_enabled ? "on" : "off", gov_target, gov_last_rate, gov_idle_run,
        (unsigned long long)gov_frq, (unsigned long long)gov_span,
        (unsigned long long)gov_ticks, (unsigned long long)gov_sets,
        (unsigned long long)gov_setfail, gov_rate_min, gov_rate_max);
}

#else  /* !PLAT_RPI4 -- no VC mailbox clock control; governor is RPi4-only */

void cpu_gov_tick(void) {}
int  cpu_gov_enable(int on) { (void)on; return 0; }
int  cpu_gov_status(char *buf, int sz) {
    return snprintf(buf, sz, "governor: n/a (not RPi4)\n");
}

#endif
