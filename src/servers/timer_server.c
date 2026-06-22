/*
 * timer_server.c -- session-11 BCM2711 system-timer service for BLOCKING timed waits.
 *
 * THE STALL CURE (see docs/NEXT_20260622_stall_session11_blocking_sleep_seed.md):
 * AIOS has historically had NO blocking sleep -- every "sleep" is a seL4_Yield
 * busy-wait. So core 0 is never idle: the prio-200 servers yield-spin continuously,
 * and each seL4_Yield is a syscall whose kernel-scheduler footprint cycles the cache.
 * Over a ~30s idle window that EVICTS a blocked thread's resume line from L2; at resume
 * the cold I-fetch misses L2, reaches the parked SCB fabric, and hangs ~32.4s (the wedge,
 * localized register-exact in s10). The principled cure is to STOP THE EVICTION by letting
 * core 0 actually idle -- which requires a real BLOCKING sleep instead of a yield-spin.
 *
 * This server provides it. The BCM2711 system timer (0xFE003000) is a free-running 1 MHz
 * counter with four compare channels; a compare match raises a peripheral IRQ. Channels 0
 * and 2 are owned by the VPU firmware, so we use channel 1 (board DTS overlay exposes only
 * channels 1 and 3 -> GIC SPI INTID 97). A dedicated core-0 server thread binds that IRQ to
 * its TCB and:
 *   - on a client request (seL4_Call(timer_ep, label=TIMER_SLEEP, MR0=delay_us)), it
 *     SaveCaller-parks the caller's reply cap in a deadline-sorted table and programs the
 *     compare register for the earliest deadline -- WITHOUT replying, so the caller BLOCKS;
 *   - on the timer IRQ (a bound-notification wake, label==0), it replies to every expired
 *     sleeper (unblocking them) and reprograms the compare for the next earliest.
 * The server itself only wakes on real client requests or real timer IRQs, so it does NOT
 * yield-spin -- its cache footprint is minimal. The classic non-MCS deferred-reply pattern
 * (seL4_CNode_SaveCaller + later seL4_Send), proven in pipe_server.c / thread_server.c.
 *
 * SELF-ADAPTING / SAFE ON QEMU: the qemu-arm-virt platform has no BCM2711 system timer, so
 * dev_systimer_vaddr is NULL there and the IRQ never arms. timer_server_init() then leaves
 * aios_timer_ready=0 and clients fall back to the old yield-spin via aios_timer_sleep_us().
 * Same fallback if the IRQ fails to arm on real HW -- a client never blocks forever.
 */
#include "aios/root_shared.h"
#include "aios/device_map.h"
#include "aios/cpuacct.h"
#include <sel4/sel4.h>
#include <sel4utils/thread.h>
#include <stdio.h>
#include <string.h>
#define LOG_MODULE "timer"
#define LOG_LEVEL  LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* BCM2711 system-timer register indices into dev_systimer_vaddr (uint32_t words). */
#define ST_CS        0      /* control/status: write 1<<n to clear channel n's match */
#define ST_CLO       1      /* counter low 32 bits (1 MHz) */
#define ST_CHI       2      /* counter high 32 bits */
#define ST_C0        3      /* compare channel 0 (C1=+1, C2=+2, C3=+3) */

#define TIMER_CHANNEL   1                       /* compare channel 1 (0/2 are VPU-owned) */
#define TIMER_CMP_IDX   (ST_C0 + TIMER_CHANNEL) /* = 4 (C1 register) */
#define TIMER_CS_BIT    (1u << TIMER_CHANNEL)   /* match-clear bit for channel 1 */
#define TIMER_INTID     97                      /* GIC SPI 0x41(65)+32; board DTS exposes ch1 & ch3 */
#define TIMER_IRQ_BADGE 0x100000u               /* badge on the IRQ notification (label==0 wake) */

#define MAX_TIMER_SLEEPERS 32
#define TIMER_MAX_DELTA    0xF0000000ULL  /* clamp one compare program (~4000s); re-arm for longer */

typedef struct {
    int       active;
    uint64_t  deadline;    /* absolute 1 MHz ticks (== microseconds) */
    seL4_CPtr reply_cap;   /* saved caller reply cap (pre-allocated cslot) */
} sleeper_t;

static sleeper_t   sleepers[MAX_TIMER_SLEEPERS];
static cspacepath_t reply_slots[MAX_TIMER_SLEEPERS];  /* pre-allocated SaveCaller slots */
static seL4_CPtr   timer_irq_handler;                 /* IRQHandler cap for INTID 97 */

/* Observability (/proc/timer): the HW canary -- if wakes climbs and active stays bounded,
 * the timer is firing and replying. A stuck "active>0, wakes flat" means the IRQ is dead. */
static uint64_t st_parked;    /* total sleep requests parked */
static uint64_t st_wakes;     /* total timer-IRQ wakes */
static uint64_t st_replied;   /* total sleepers replied (woken at deadline) */
static uint64_t st_full;      /* requests rejected because the table was full */
static uint64_t st_unknown;   /* Recv wakes that matched neither IRQ nor TIMER_SLEEP (diag) */
static seL4_Word st_last_badge; /* badge from the most recent Recv (diag) */
static seL4_Word st_last_label; /* label from the most recent Recv (diag) */
static seL4_Word st_selftest_badge; /* badge the self-test saw on the IRQ (diag) */

/* /proc/timersleep knob -- DEFAULT 0 (Phase-2 userspace blocking dormant until armed). The
 * spawner (pipe_server PIPE_EXEC) only conveys the timer cap to a child when this is 1 AND
 * the timer armed; otherwise the child's timer_ep stays 0 and nanosleep yield-falls-back. */
volatile int g_timer_userspace_enable = 0;

int timersleep_cmd(const char *args, char *buf, int bufsize) {
    if (args[0] == '.' && (args[1] == '0' || args[1] == '1'))
        g_timer_userspace_enable = (args[1] == '1');
    return snprintf(buf, bufsize,
        "timersleep: userspace=%d (newly-spawned procs %s the timer cap -> nanosleep %s); timer_ready=%d\n",
        g_timer_userspace_enable,
        g_timer_userspace_enable ? "GET" : "omit",
        g_timer_userspace_enable ? "BLOCK" : "yield",
        aios_timer_ready);
}

/* Atomic 64-bit read of the split 1 MHz counter (re-read CHI to catch a low-word wrap). */
static uint64_t systimer_now(void) {
    volatile uint32_t *t = dev_systimer_vaddr;
    uint32_t hi, lo, hi2;
    do {
        hi  = t[ST_CHI];
        lo  = t[ST_CLO];
        hi2 = t[ST_CHI];
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

/* Program compare C1 for the earliest active deadline (clamped to a 32-bit window). */
static void timer_program_next(void) {
    volatile uint32_t *t = dev_systimer_vaddr;
    uint64_t now = systimer_now();
    uint64_t earliest = 0;
    int found = 0;
    for (int i = 0; i < MAX_TIMER_SLEEPERS; i++) {
        if (sleepers[i].active && (!found || sleepers[i].deadline < earliest)) {
            earliest = sleepers[i].deadline;
            found = 1;
        }
    }
    if (!found) return;   /* nothing armed; a later spurious match is harmless (CS re-cleared) */
    uint64_t delta = (earliest > now) ? (earliest - now) : 1;
    if (delta > TIMER_MAX_DELTA) delta = TIMER_MAX_DELTA;  /* re-arm on the spurious wake */
    t[ST_CS] = TIMER_CS_BIT;             /* clear any stale match before re-arming */
    t[TIMER_CMP_IDX] = (uint32_t)(now + delta);
}

/* Timer IRQ (bound-notification wake): reply to expired sleepers, reprogram, ack. */
static void timer_handle_irq(void) {
    volatile uint32_t *t = dev_systimer_vaddr;
    st_wakes++;
    t[ST_CS] = TIMER_CS_BIT;             /* deassert the level IRQ at the source FIRST */
    uint64_t now = systimer_now();
    for (int i = 0; i < MAX_TIMER_SLEEPERS; i++) {
        if (sleepers[i].active && now >= sleepers[i].deadline) {
            seL4_Send(sleepers[i].reply_cap, seL4_MessageInfo_new(0, 0, 0, 0));
            seL4_CNode_Delete(seL4_CapInitThreadCNode, sleepers[i].reply_cap, seL4_WordBits);
            sleepers[i].active = 0;
            st_replied++;
        }
    }
    timer_program_next();
    seL4_IRQHandler_Ack(timer_irq_handler);   /* re-arm the GIC line */
}

/* Park the current caller (seL4_Recv must have just returned a TIMER_SLEEP request).
 * SaveCaller is the FIRST action so no nested invocation clobbers the reply cap. */
static void timer_park(uint64_t delay_us) {
    int si = -1;
    for (int i = 0; i < MAX_TIMER_SLEEPERS; i++)
        if (!sleepers[i].active) { si = i; break; }
    if (si < 0) {
        st_full++;
        seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));   /* table full: wake immediately */
        return;
    }
    seL4_CNode_Delete(seL4_CapInitThreadCNode, reply_slots[si].capPtr, seL4_WordBits);
    seL4_CNode_SaveCaller(seL4_CapInitThreadCNode, reply_slots[si].capPtr, seL4_WordBits);
    sleepers[si].active    = 1;
    sleepers[si].deadline  = systimer_now() + delay_us;   /* 1 tick == 1 us at 1 MHz */
    sleepers[si].reply_cap = reply_slots[si].capPtr;
    st_parked++;
    timer_program_next();
    /* no reply -> the caller blocks until timer_handle_irq() replies at the deadline */
}

static void timer_server_fn(void *arg0, void *a1, void *a2) {
    (void)a1; (void)a2;
    seL4_CPtr ep = (seL4_CPtr)(uintptr_t)arg0;
    for (;;) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t msg = seL4_Recv(ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(msg);
        st_last_badge = badge; st_last_label = label;   /* diag: what did we actually receive? */
        /* Discriminate by BADGE, not label: a bound-notification (timer IRQ) wake carries the
         * notification's badge (TIMER_IRQ_BADGE) but a GARBAGE label (HW showed label=4, not 0
         * -- a notification's MessageInfo is not a real IPC). Client Calls use the unbadged
         * timer_ep (badge 0) with label==TIMER_SLEEP. So badge==TIMER_IRQ_BADGE => the IRQ. */
        if (badge == TIMER_IRQ_BADGE) {
            timer_handle_irq();
        } else if (label == TIMER_SLEEP) {
            timer_park(seL4_GetMR(0));
        } else {
            st_unknown++;
            seL4_Reply(seL4_MessageInfo_new(0, 0, 0, 0));  /* never hang an unknown caller */
        }
    }
}

void timer_server_init(void) {
    memset(sleepers, 0, sizeof(sleepers));
    aios_timer_ready = 0;

    /* On qemu-arm-virt (or if the device untyped was not exposed) the MMIO is absent;
     * stay in yield-fallback mode rather than blocking clients forever. */
    if (!dev_systimer_vaddr) {
        AIOS_LOG_INFO("timer: no system-timer MMIO -- yield-fallback (expected on QEMU)");
        return;
    }

    /* Pre-allocate every SaveCaller reply slot now, before any request can arrive, so the
     * park handler is invocation-free between seL4_Recv and seL4_CNode_SaveCaller. */
    for (int i = 0; i < MAX_TIMER_SLEEPERS; i++) {
        if (vka_cspace_alloc_path(&vka, &reply_slots[i])) {
            AIOS_LOG_WARN("timer: reply-slot alloc failed -- yield-fallback");
            return;
        }
    }

    /* Request endpoint (root threads Call timer_ep_cap directly; published global). */
    vka_object_t ep_obj;
    if (vka_alloc_endpoint(&vka, &ep_obj)) {
        AIOS_LOG_WARN("timer: endpoint alloc failed -- yield-fallback");
        return;
    }
    timer_ep_cap = ep_obj.cptr;

    /* Notification: bind the UNBADGED cap to the server TCB (so its blocking seL4_Recv wakes
     * on the IRQ), and bind a BADGE=TIMER_IRQ_BADGE mint to the IRQ handler (so the wake
     * carries a recognizable badge). The net_server / display-server bound-notification idiom. */
    vka_object_t ntfn_obj;
    if (vka_alloc_notification(&vka, &ntfn_obj)) {
        AIOS_LOG_WARN("timer: notification alloc failed -- yield-fallback");
        return;
    }
    seL4_CPtr ntfn_base = ntfn_obj.cptr;
    seL4_CPtr irq_ntfn  = ntfn_base;
    {
        cspacepath_t src, dst;
        vka_cspace_make_path(&vka, ntfn_base, &src);
        if (!vka_cspace_alloc_path(&vka, &dst) &&
            !seL4_CNode_Mint(dst.root, dst.capPtr, dst.capDepth,
                             src.root, src.capPtr, src.capDepth,
                             seL4_AllRights, (seL4_Word)TIMER_IRQ_BADGE))
            irq_ntfn = dst.capPtr;
    }

    /* Claim IRQHandler for the compare-1 SPI and bind it to the (badged) notification. */
    cspacepath_t irq_path;
    if (vka_cspace_alloc_path(&vka, &irq_path) ||
        simple_get_IRQ_handler(&simple, TIMER_INTID, irq_path)) {
        AIOS_LOG_WARN_V("timer: IRQ handler claim failed intid=", (unsigned long)TIMER_INTID);
        return;
    }
    timer_irq_handler = irq_path.capPtr;
    if (seL4_IRQHandler_SetNotification(timer_irq_handler, irq_ntfn)) {
        AIOS_LOG_WARN("timer: IRQ SetNotification failed -- yield-fallback");
        return;
    }
    dev_systimer_vaddr[ST_CS] = TIMER_CS_BIT;        /* clear any stale channel-1 match */
    seL4_IRQHandler_Ack(timer_irq_handler);          /* arm */

    /* HW SELF-TEST (s11 fix): the bind/arm above SUCCEEDING does NOT prove the compare-1 IRQ
     * actually FIRES on this silicon -- on the first HW flash it armed (ready=1) but wakes=0,
     * so serverstats/flush blocked FOREVER. QEMU can't catch this (no BCM system timer). So:
     * program a 10ms compare, busy-poll the notification (root thread holds the cap; not bound
     * to a TCB yet), and only trust the timer if the IRQ arrives. On failure, LOG the live
     * registers (clo advanced? c1 set? cs match bit?) to pinpoint counter-vs-match-vs-routing,
     * and leave aios_timer_ready=0 so every sleeper yield-falls-back (NEVER hangs). */
    {
        volatile uint32_t *t = dev_systimer_vaddr;
        uint32_t clo0 = t[ST_CLO];
        t[ST_CS] = TIMER_CS_BIT;                      /* clear stale match */
        t[TIMER_CMP_IDX] = clo0 + 10000;              /* fire in ~10ms (10000 ticks @ 1MHz) */
        int fired = 0;
        uint32_t clo1 = clo0, cs = 0;
        for (long i = 0; i < 6000000L; i++) {         /* hard cap: a stuck counter can't hang boot */
            seL4_Word b = 0;
            seL4_Poll(ntfn_base, &b);                 /* non-blocking notification check */
            clo1 = t[ST_CLO];
            cs   = t[ST_CS];
            if (b != 0) { fired = 1; st_selftest_badge = b; break; }
            if ((uint32_t)(clo1 - clo0) > 100000) break;   /* waited ~100ms of counter time */
        }
        if (fired) {
            t[ST_CS] = TIMER_CS_BIT;
            seL4_IRQHandler_Ack(timer_irq_handler);
            AIOS_LOG_INFO("timer: self-test OK -- compare-1 IRQ fires");
        } else {
            printf("[timer] SELF-TEST FAILED: clo0=%u clo1=%u advanced=%u c1=%u cs=0x%x intid=%d"
                   " -> yield-fallback (advanced~0=counter stuck; cs&2=match-but-no-IRQ; else no-match)\n",
                   clo0, clo1, (uint32_t)(clo1 - clo0), clo0 + 10000, cs, TIMER_INTID);
            return;   /* aios_timer_ready stays 0 -> serverstats/flush/nanosleep yield-fallback */
        }
    }

    /* Dedicated blocking server thread: prio 200, pinned core 0, IRQ ntfn bound to its TCB. */
    sel4utils_thread_t th;
    if (sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
            simple_get_cnode(&simple), seL4_NilData, &th)) {
        AIOS_LOG_WARN("timer: thread configure failed -- yield-fallback");
        return;
    }
    seL4_TCB_SetPriority(th.tcb.cptr, simple_get_tcb(&simple), 200);
    #if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(th.tcb.cptr, 0);
    #endif
    int berr = seL4_TCB_BindNotification(th.tcb.cptr, ntfn_base);
    if (berr) {
        AIOS_LOG_WARN_V("timer: BindNotification err=", (unsigned long)berr);
        return;
    }
    if (sel4utils_start_thread(&th, timer_server_fn,
                               (void *)(uintptr_t)timer_ep_cap, NULL, 1)) {
        AIOS_LOG_WARN("timer: thread start failed -- yield-fallback");
        return;
    }
    aios_acct_register("timer", th.tcb.cptr);
    aios_timer_ready = 1;
    AIOS_LOG_INFO_V("timer: blocking-sleep service armed, intid=", (unsigned long)TIMER_INTID);
}

/* Block for `us` microseconds. Uses the timer service when armed; otherwise falls back to
 * the historical yield-spin on the EL0 arch counter (QEMU / pre-arm / table-full). */
void aios_timer_sleep_us(uint64_t us) {
    if (aios_timer_ready && timer_ep_cap) {
        seL4_SetMR(0, (seL4_Word)us);
        seL4_Call(timer_ep_cap, seL4_MessageInfo_new(TIMER_SLEEP, 0, 0, 1));
        return;
    }
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    uint64_t target = cnt + us * freq / 1000000ULL;
    do {
        seL4_Yield();
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    } while (cnt < target);
}

int timer_server_format(char *buf, int bufsize) {
    int active = 0;
    for (int i = 0; i < MAX_TIMER_SLEEPERS; i++) if (sleepers[i].active) active++;
    /* live system-timer registers (read twice so clo0!=clo1 confirms the counter advances). */
    uint32_t clo0 = 0, clo1 = 0, chi = 0, c1 = 0, cs = 0;
    if (dev_systimer_vaddr) {
        clo0 = dev_systimer_vaddr[ST_CLO];
        chi  = dev_systimer_vaddr[ST_CHI];
        c1   = dev_systimer_vaddr[TIMER_CMP_IDX];
        cs   = dev_systimer_vaddr[ST_CS];
        clo1 = dev_systimer_vaddr[ST_CLO];
    }
    return snprintf(buf, bufsize,
        "ready=%d intid=%d slots=%d active=%d parked=%llu wakes=%llu replied=%llu full=%llu unknown=%llu\n"
        "clo0=%u clo1=%u advancing=%d chi=%u c1=%u cs=0x%x\n"
        "last_badge=0x%lx last_label=%lu selftest_badge=0x%lx\n",
        aios_timer_ready, TIMER_INTID, MAX_TIMER_SLEEPERS, active,
        (unsigned long long)st_parked, (unsigned long long)st_wakes,
        (unsigned long long)st_replied, (unsigned long long)st_full,
        (unsigned long long)st_unknown,
        clo0, clo1, (clo1 != clo0), chi, c1, cs,
        (unsigned long)st_last_badge, (unsigned long)st_last_label,
        (unsigned long)st_selftest_badge);
}
