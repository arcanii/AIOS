/*
 * cpu_gov.c -- RPi4 load-driven DVFS governor (the only power lever compatible
 * with the no-WFI stall cure; core-parking re-triggers the 32.4s tlbi stall).
 *
 * MECHANISM (HW-verified 2026-06-14, see feedback_rpi4_thermal_clock):
 *   - actuator: hw_arm_clock_set (VC mailbox SET_CLOCK_RATE id 3). Confirmed it
 *     PINS the requested MHz and the firmware does NOT auto-boost back under load,
 *     so explicit control is required. The firmware clamps the request to
 *     [arm_freq_min, arm_freq] from config.txt, so the governor range below
 *     (GOV_MIN_MHZ..GOV_MAX_MHZ = 600..1000) only takes full effect with
 *     arm_freq=1000 + arm_freq_min=600 (mksdcard.py); with the old arm_freq=600
 *     every set clamps to 600 and the governor is a harmless no-op.
 *   - metric: aios_acct_busy_permille (src/cpuacct.c, seL4 track_utilisation).
 *     The fraction of core-0 cycles spent on the event-driven WORK servers (pipe /
 *     fs / exec / net / ...), summed positively. NOT total-minus-background: that
 *     reads the no-WFI idle spin as work, because track_utilisation books a TCB
 *     only at switch-out so the always-running root/tlbi spinners under-report
 *     (HW-confirmed: bmin stuck ~440 over a disconnected idle). The work servers
 *     block on seL4_Recv, so they are booked accurately and read ~0 at idle; a
 *     forked tcc shows via its pipe/fs/exec syscall traffic. This also REPLACES
 *     the original root-loop-rate metric, which the same spin confounded.
 *
 * POLICY: sample every GOV_TICK_MS. busy > HI => jump to MAX now. busy < LO for
 * GOV_IDLE_TICKS in a row => settle to MIN. Raise fast, lower gently. MAX is the
 * firmware ceiling (~65C, well under the 85C throttle) so no separate thermal cap
 * is needed. cpu_gov_tick() runs from the root main loop; it is cheap (a cntpct
 * read, a few benchmark syscalls once a second) and only Calls the mailbox on a
 * transition. The observer effect (a connected netconsole relay shows as work)
 * makes any connected idle read unreliable -- verify cooling by TEMPERATURE.
 */
#include <stdint.h>
#include <stdio.h>
#include "aios/hw_info.h"    /* hw_arm_clock_set */
#include "aios/cpuacct.h"    /* aios_acct_busy_permille */

#ifdef PLAT_RPI4

/* v0.4.262: raised the ceiling 600->1000 (interactive perf) and the floor
 * 300->600 (idle-teardown-freeze severity mitigation). The 32.4s TLBI/DVM freeze
 * scales inversely with clock (HW-proven: 600MHz->~33s = one quantum, 300MHz->
 * ~164s = ~5 quanta -- see project_stall_hunt), so never idle-downclocking below
 * 600 caps the worst-case disconnected-idle freeze at ~33s instead of ~164s. This
 * is the seed's "raise the DVFS floor" fallback (the BCM2711 UBUS-timeout-register
 * cure is a confirmed dead end -- project_stall_ubus_deadend). 600 idle is
 * thermally proven-stable (it was the old fixed arm_freq cap). The firmware
 * clamps SET_CLOCK_RATE to [arm_freq_min, arm_freq] in config.txt, so this only
 * takes full effect with arm_freq=1000 + arm_freq_min=600 (mksdcard.py). */
#define GOV_MIN_MHZ    600u
#define GOV_MAX_MHZ    1000u
#define GOV_BUSY_HI    200     /* permille: > 20% non-background work => boost to MAX */
#define GOV_BUSY_LO     80     /* permille: < 8% for IDLE_TICKS in a row => settle MIN */
#define GOV_IDLE_TICKS 3       /* consecutive idle windows before downclock (~3s) */
/* 1s window: under the ~7s CCNT wrap (so the busy% is computed from a single
 * non-wrapped CCNT delta), and it dilutes any sub-second background blip to a
 * small fraction of the busy metric -- which already subtracts the background, so
 * the dominant idle confound is gone and this window only smooths the remainder. */
#define GOV_TICK_MS    1000u

static uint64_t rd_cntpct(void) {
    uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
}

static int      gov_enabled  = 1;            /* auto-DVFS ON by default */
static unsigned gov_target   = GOV_MAX_MHZ;  /* gentle start at MAX; settle down when idle */
static uint64_t gov_frq      = 0;            /* cntpct frequency (cached) */
static uint64_t gov_span     = 0;            /* GOV_TICK_MS in cntpct ticks */
static uint64_t gov_last_ct  = 0;
static int      gov_idle_run = 0;
/* runtime-tunable thresholds (defaults above) so the governor can be tuned live
 * over /proc/cpufreq.tune.LO.HI.IT -- no reflash per attempt (HW iteration
 * discipline: tune over netconsole, bake the final values in at commit). */
static unsigned gov_busy_hi  = GOV_BUSY_HI;
static unsigned gov_busy_lo  = GOV_BUSY_LO;
static int      gov_it       = GOV_IDLE_TICKS;
/* diagnostics on /proc/cpufreq: busy is the metric the governor acts on (permille
 * of core-0 cycles on the work servers); bmin/bmax bound its observed range (read
 * bmin after a disconnected idle to see the true idle floor despite the relay
 * observer effect); total/work are the last raw CCNT total and work-server cycle
 * sum; set is the clock last actuated. */
static unsigned gov_last_busy  = 0;
static uint64_t gov_ticks      = 0;
static uint64_t gov_sets       = 0;
static uint64_t gov_setfail    = 0;
static unsigned gov_busy_min   = 1000;
static unsigned gov_busy_max   = 0;
static uint64_t gov_last_total = 0;
static uint64_t gov_last_work  = 0;
static unsigned gov_set_mhz    = 0;   /* clock last pushed to the mailbox (0 = none yet) */

void cpu_gov_tick(void) {
    if (!gov_frq) {
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(gov_frq));
        if (!gov_frq) gov_frq = 54000000u;          /* RPi4 generic-timer default */
        gov_span = (gov_frq * GOV_TICK_MS) / 1000u;
    }
    uint64_t now = rd_cntpct();
    if (gov_last_ct == 0) {
        gov_last_ct = now;
        aios_acct_busy_permille(0, 0);              /* prime the load baseline */
        return;
    }
    if (now - gov_last_ct < gov_span) return;       /* not a tick boundary yet */
    gov_last_ct = now;

    uint64_t total = 0, work = 0;
    int busy = aios_acct_busy_permille(&total, &work);
    gov_ticks++;
    if (busy < 0) return;                           /* accounting unavailable / priming */
    gov_last_busy  = (unsigned)busy;
    gov_last_total = total;
    gov_last_work  = work;
    if ((unsigned)busy < gov_busy_min) gov_busy_min = busy;
    if ((unsigned)busy > gov_busy_max) gov_busy_max = busy;

    unsigned want = gov_target;
    if ((unsigned)busy > gov_busy_hi) {              /* busy: raise immediately */
        want = GOV_MAX_MHZ; gov_idle_run = 0;
    } else if ((unsigned)busy < gov_busy_lo) {       /* idle: lower after a run */
        if (++gov_idle_run >= gov_it) want = GOV_MIN_MHZ;
    } else {
        gov_idle_run = 0;                            /* mid-band: hold */
    }
    gov_target = want;                               /* the decision (display + intent) */

    /* Actuate against the clock we LAST set (gov_set_mhz, 0 = nothing yet), not
     * the prior decision -- the firmware boots at its own clock (300), so the
     * governor must push the first decision to reach a known state. Comparing
     * against gov_target instead leaves want==target forever when the load never
     * changes, so the mailbox is never Called (HW-confirmed sets=0, clock stuck at
     * the boot default). When disabled we track the decision but do not actuate;
     * on re-enable the mismatch re-establishes governor control on the next tick. */
    if (gov_enabled && want != gov_set_mhz) {
        unsigned got = 0;
        if (hw_arm_clock_set(want, &got) == 0) { gov_set_mhz = want; gov_sets++; }
        else gov_setfail++;   /* mailbox busy / clamped: retry next tick */
    }
}

int cpu_gov_enable(int on) { gov_enabled = on ? 1 : 0; return gov_enabled; }

/* Live threshold override (/proc/cpufreq.tune.LO.HI.IT). A zero field is left
 * unchanged, so .tune.0.0.4 retunes only the idle-tick count. LO/HI are permille
 * (0..1000); HI is forced above LO. Lets the governor be dialled in over
 * netconsole against the HW busy/temp response without a reflash per attempt. */
void cpu_gov_tune(unsigned lo, unsigned hi, int it) {
    if (lo > 0 && lo <= 1000) gov_busy_lo = lo;
    if (hi > 0 && hi <= 1000) gov_busy_hi = hi;
    if (gov_busy_hi <= gov_busy_lo) gov_busy_hi = gov_busy_lo + 1;
    if (it >= 1 && it <= 60) gov_it = it;
}

int cpu_gov_status(char *buf, int sz) {
    return snprintf(buf, sz,
        "governor: %s  target_mhz: %u  busy: %u/1000  idle_run: %d\n"
        "gov_dbg: frq=%llu span=%llu ticks=%llu sets=%llu setfail=%llu "
        "bmin=%u bmax=%u total=%llu work=%llu set=%u lo=%u hi=%u it=%d\n",
        gov_enabled ? "on" : "off", gov_target, gov_last_busy, gov_idle_run,
        (unsigned long long)gov_frq, (unsigned long long)gov_span,
        (unsigned long long)gov_ticks, (unsigned long long)gov_sets,
        (unsigned long long)gov_setfail, gov_busy_min, gov_busy_max,
        (unsigned long long)gov_last_total, (unsigned long long)gov_last_work,
        gov_set_mhz, gov_busy_lo, gov_busy_hi, gov_it);
}

#else  /* !PLAT_RPI4 -- no VC mailbox clock control; governor is RPi4-only */

void cpu_gov_tick(void) {}
int  cpu_gov_enable(int on) { (void)on; return 0; }
void cpu_gov_tune(unsigned lo, unsigned hi, int it) { (void)lo; (void)hi; (void)it; }
int  cpu_gov_status(char *buf, int sz) {
    return snprintf(buf, sz, "governor: n/a (not RPi4)\n");
}

#endif
