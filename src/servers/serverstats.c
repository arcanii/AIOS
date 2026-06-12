/*
 * serverstats.c -- v0.4.121 server health probe (ping-only).
 *
 * A dedicated thread periodically sends SVC_PING to each in-process
 * server endpoint and records a per-server "last ok" timestamp plus
 * pings_ok / pings_fail counters. /proc/serverstats renders the
 * snapshot for an operator.
 *
 * Why a dedicated thread (not on-demand from procfs):
 * fs_thread serves /proc reads. Doing seL4_Call to another server
 * inside that handler clobbers fs_thread's reply cap on non-MCS seL4
 * (see feedback_sel4_nested_call.md), losing the original caller.
 * A separate thread is the safe pattern.
 *
 * Auto-restart on hang is not in scope; this is observation only.
 *
 * pings_fail is reserved for future use: seL4 non-MCS has no Call timeout,
 * so a hung server hangs the probe thread itself. The visible signal is a
 * stale last_ok_us / age_s in /proc/serverstats. With MCS timeouts, this
 * counter would track timed-out attempts.
 */
#include "aios/root_shared.h"
#include "aios/vka_audit.h"
#include "crypto_server.h"
#include <sel4/sel4.h>
#include <sel4utils/thread.h>
#include <stdio.h>
#include <string.h>
#define LOG_MODULE "srvstat"
#define LOG_LEVEL  LOG_LEVEL_INFO
#include "aios/aios_log.h"

/* Probe period in seconds. 5s is frequent enough to spot a hang within
 * a few user actions, infrequent enough that the Yield loop does not
 * dominate scheduling. */
#define PROBE_PERIOD_SEC  5

/* Servers we know how to ping. tty/auth live in CPIO and dont expose a
 * simple PING surface from root, so theyre omitted for now. */
typedef struct {
    const char *name;
    seL4_CPtr  *ep_p;        /* Pointer because endpoints are set during boot */
    int         use_op;      /* 1 = crypto-style: opcode in MR0, label 0 */
    int         enabled;
    uint32_t    pings_ok;
    uint32_t    pings_fail;
    uint64_t    last_ok_us;
    uint64_t    last_attempt_us;
    uint32_t    last_latency_us;
} srv_t;

#define SRV_PIPE   0
#define SRV_FS     1
#define SRV_THREAD 2
#define SRV_NET    3
#define SRV_DISP   4
#define SRV_CRYPTO 5
#define SRV_COUNT  6

static srv_t srv[SRV_COUNT];

static uint64_t now_us(void) {
    uint64_t cnt, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0) freq = 62500000;
    return cnt * 1000000ULL / freq;
}

static void probe_sleep(uint32_t sec) {
    uint64_t target = now_us() + (uint64_t)sec * 1000000ULL;
    while (now_us() < target) {
        seL4_Yield();
    }
}

static void ping_one(srv_t *s) {
    if (!s->enabled || !s->ep_p || *s->ep_p == 0) return;
    uint64_t t0 = now_us();
    s->last_attempt_us = t0;
    seL4_MessageInfo_t msg;
    if (s->use_op) {
        seL4_SetMR(0, CRYPTO_OP_STATUS);
        msg = seL4_MessageInfo_new(0, 0, 0, 1);
    } else {
        msg = seL4_MessageInfo_new(SVC_PING, 0, 0, 0);
    }
    seL4_Call(*s->ep_p, msg);
    uint64_t t1 = now_us();
    s->last_ok_us = t1;
    s->last_latency_us = (uint32_t)(t1 - t0);
    s->pings_ok++;
}

static void probe_thread_fn(void *a, void *b, void *c) {
    (void)a; (void)b; (void)c;
    while (1) {
        for (int i = 0; i < SRV_COUNT; i++) {
            ping_one(&srv[i]);
        }
        probe_sleep(PROBE_PERIOD_SEC);
    }
}

void serverstats_init(void) {
    memset(srv, 0, sizeof(srv));
    srv[SRV_PIPE]   = (srv_t){ "pipe",    &pipe_ep_cap,   0, 1, 0, 0, 0, 0, 0 };
    srv[SRV_FS]     = (srv_t){ "fs",      &fs_ep_cap,     0, 1, 0, 0, 0, 0, 0 };
    srv[SRV_THREAD] = (srv_t){ "thread",  &thread_ep_cap, 0, 1, 0, 0, 0, 0, 0 };
    srv[SRV_NET]    = (srv_t){ "net",     &net_ep_cap,    0, net_available, 0, 0, 0, 0, 0 };
    srv[SRV_DISP]   = (srv_t){ "display", &disp_ep_cap,   0, gpu_available, 0, 0, 0, 0, 0 };
    srv[SRV_CRYPTO] = (srv_t){ "crypto",  &crypto_ep_cap, 1, 1, 0, 0, 0, 0, 0 };

    sel4utils_thread_t thread;
    int err = sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
        simple_get_cnode(&simple), seL4_NilData, &thread);
    if (err) {
        AIOS_LOG_WARN_V("serverstats: thread configure failed err=", (unsigned long)err);
        return;
    }
    /* Run at the same priority as the servers (200). The probe spends most
     * of its time blocked in seL4_Call to a server, so it does not steal
     * CPU from real work; running at server priority avoids the long
     * starvation we saw at 180 on this scheduler. */
    int p_err = seL4_TCB_SetPriority(thread.tcb.cptr,
                                     simple_get_tcb(&simple), 200);
    if (p_err) {
        AIOS_LOG_WARN_V("serverstats: SetPriority err=", (unsigned long)p_err);
    }
    /* v0.4.178: pin to core 0 like every other root thread -- it shares the
     * global lock-free allocman/vka (see boot_services start_server_thread). */
    #if CONFIG_MAX_NUM_NODES > 1
    seL4_TCB_SetAffinity(thread.tcb.cptr, 0);
    #endif
    err = sel4utils_start_thread(&thread, probe_thread_fn, NULL, NULL, 1);
    if (err) {
        AIOS_LOG_WARN_V("serverstats: thread start failed err=", (unsigned long)err);
        return;
    }
    AIOS_LOG_INFO("serverstats probe thread started");
}

int serverstats_format(char *buf, int bufsize) {
    int w = 0;
    uint64_t now = now_us();
    w += snprintf(buf + w, bufsize - w,
        "%-8s %-8s %6s %6s %10s %10s\n",
        "name", "state", "ok", "fail", "lat_us", "age_s");
    for (int i = 0; i < SRV_COUNT; i++) {
        srv_t *s = &srv[i];
        const char *state;
        uint64_t age_s = 0;
        if (!s->enabled)              state = "off";
        else if (s->pings_ok == 0)    state = "pending";
        else                          state = "ok";
        if (s->last_ok_us)
            age_s = (now - s->last_ok_us) / 1000000ULL;
        w += snprintf(buf + w, bufsize - w,
            "%-8s %-8s %6u %6u %10u %10llu\n",
            s->name, state, s->pings_ok, s->pings_fail,
            s->last_latency_us, (unsigned long long)age_s);
    }
    return w;
}
