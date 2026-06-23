/*
 * placement_strategy_host_test.c -- deterministic host test of the pluggable core-placement
 * strategies in include/aios/sched_strategy.h (the SAME header the kernel uses, so there is
 * no replication drift). Feeds known per-core load arrays and checks each strategy picks the
 * expected core. Prints OK / FAIL per case and a final tally.
 *
 * Build + run:
 *   cc -I include -o /tmp/place_test scripts/placement_strategy_host_test.c && /tmp/place_test
 */
#include <stdio.h>
#include "aios/sched_strategy.h"

static unsigned g_load[8];
static unsigned load_fn(unsigned c, void *ck) { (void)ck; return g_load[c]; }

static int g_pass, g_fail;

static unsigned pick(int strat, const unsigned *loads, unsigned n, int thr, unsigned *rr) {
    for (unsigned i = 0; i < n; i++) g_load[i] = loads[i];
    aios_place_ctx_t x;
    x.n_cores = n; x.threshold = thr; x.hint_core = -1;
    x.load = load_fn; x.cookie = 0; x.rr = rr;
    return AIOS_PLACE_STRATEGIES[strat].fn(&x);
}

static void check(const char *label, unsigned got, unsigned want) {
    if (got == want) { g_pass++; printf("  OK   %-46s -> %u\n", label, got); }
    else { g_fail++; printf("  FAIL %-46s -> %u (want %u)\n", label, got, want); }
}

int main(void) {
    printf("=== placement strategy host test (%d strategies) ===\n", AIOS_N_PLACE_STRATEGIES);

    /* HYBRID (idx 0): least-loaded among ACTIVE, spill to coldest past threshold (=2). */
    unsigned L0[4] = {0,0,0,0}, L1[4] = {11,0,0,0}, L2[4] = {11,1,0,0},
             L3[4] = {11,2,0,0}, L4[4] = {11,2,2,2};
    check("hybrid all-cold -> home core 0",            pick(0, L0, 4, 2, 0), 0);
    check("hybrid loaded-core0 -> spill cold core1",   pick(0, L1, 4, 2, 0), 1);
    check("hybrid core1 under thr -> core1",           pick(0, L2, 4, 2, 0), 1);
    check("hybrid core1 at thr -> spill cold core2",   pick(0, L3, 4, 2, 0), 2);
    check("hybrid all-active>=thr -> coldest (core1)", pick(0, L4, 4, 2, 0), 1);

    /* SPREAD (idx 1): globally coldest. */
    unsigned S0[4] = {11,2,0,5}, S1[4] = {3,3,3,3};
    check("spread -> coldest core2",                   pick(1, S0, 4, 2, 0), 2);
    check("spread all-equal -> first (core0)",         pick(1, S1, 4, 2, 0), 0);

    /* PACK (idx 2): most-loaded under threshold (=2), else coldest. */
    unsigned P0[4] = {1,0,0,0}, P1[4] = {2,1,0,0}, P2[4] = {3,3,3,3}, P3[4] = {0,0,0,0};
    check("pack most-loaded-under-thr (core0)",        pick(2, P0, 4, 2, 0), 0);
    check("pack core0 at thr -> next most-loaded c1",  pick(2, P1, 4, 2, 0), 1);
    check("pack none-under-thr -> coldest (core0)",    pick(2, P2, 4, 2, 0), 0);
    check("pack all-cold -> core0",                    pick(2, P3, 4, 2, 0), 0);

    /* RR (idx 3): round-robin over all cores using a shared counter. */
    unsigned rr = 0, any[4] = {9,9,9,9};
    check("rr #1 -> core0", pick(3, any, 4, 2, &rr), 0);
    check("rr #2 -> core1", pick(3, any, 4, 2, &rr), 1);
    check("rr #3 -> core2", pick(3, any, 4, 2, &rr), 2);
    check("rr #4 -> core3", pick(3, any, 4, 2, &rr), 3);
    check("rr #5 -> core0 (wrap)", pick(3, any, 4, 2, &rr), 0);

    printf("=== placement strategy host test: %d/%d passed ===\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
