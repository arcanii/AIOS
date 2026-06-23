#ifndef AIOS_SCHED_STRATEGY_H
#define AIOS_SCHED_STRATEGY_H
/*
 * sched_strategy.h -- pluggable core-placement strategies, EXTRACTED as pure functions of
 * the per-core load so they can be A/B'd to find the best (select via /proc/placement.sN)
 * AND host-tested with no kernel dependencies (scripts/placement_strategy_host_test.c).
 *
 * The caller supplies the per-core load via a callback (aios_load_fn) so the same code runs
 * in the kernel (load = aios_core_load, a scan of the live proc table) and in the host test
 * (load = a fed array). Adding a strategy = one function + one table row; nothing else.
 *
 * NOT a stall cure (s12: a busy core still wedges on teardown) -- these trade off locality,
 * balance, parallelism, and blast-radius. The experiment harness picks the winner.
 */

typedef unsigned (*aios_load_fn)(unsigned core, void *cookie);

typedef struct {
    unsigned     n_cores;     /* number of cores to consider */
    int          threshold;   /* spill threshold (open a cold core only past this load) */
    int          hint_core;   /* parent/session core for locality, -1 if none (reserved) */
    aios_load_fn load;        /* per-core load accessor */
    void        *cookie;      /* opaque, passed to load() */
    unsigned    *rr;          /* round-robin counter (caller-owned); may be NULL */
} aios_place_ctx_t;

/* HYBRID: least-loaded among ACTIVE cores; spill to the globally-coldest core (open a new
 * one) only when the least-loaded active core is already at/over the threshold. */
static inline unsigned aios_strat_hybrid(const aios_place_ctx_t *x) {
    unsigned best_active = 0, best_active_load = ~0u, n_active = 0;
    unsigned best_any = 0, best_any_load = ~0u;
    for (unsigned c = 0; c < x->n_cores; c++) {
        unsigned l = x->load(c, x->cookie);
        if (l < best_any_load) { best_any_load = l; best_any = c; }
        if (l > 0) { n_active++; if (l < best_active_load) { best_active_load = l; best_active = c; } }
    }
    if (n_active == 0) return 0;
    if (best_active_load < (unsigned)x->threshold) return best_active;
    return best_any;
}

/* SPREAD: always the globally-coldest core -> maximum distribution / parallelism. */
static inline unsigned aios_strat_spread(const aios_place_ctx_t *x) {
    unsigned best = 0, best_load = ~0u;
    for (unsigned c = 0; c < x->n_cores; c++) {
        unsigned l = x->load(c, x->cookie);
        if (l < best_load) { best_load = l; best = c; }
    }
    return best;
}

/* PACK: fill the MOST-loaded core still under threshold before opening a new one ->
 * maximum locality / fewest cores woken (the "use a core already running" extreme). */
static inline unsigned aios_strat_pack(const aios_place_ctx_t *x) {
    unsigned best = 0, best_load = 0, coldest = 0, coldest_load = ~0u; int found = 0;
    for (unsigned c = 0; c < x->n_cores; c++) {
        unsigned l = x->load(c, x->cookie);
        if (l < coldest_load) { coldest_load = l; coldest = c; }
        if (l < (unsigned)x->threshold && (!found || l > best_load)) { best = c; best_load = l; found = 1; }
    }
    return found ? best : coldest;
}

/* RR: simple round-robin over all cores (naive baseline for the experiment). */
static inline unsigned aios_strat_rr(const aios_place_ctx_t *x) {
    unsigned v = x->rr ? (*x->rr)++ : 0;
    return v % x->n_cores;
}

static const struct { const char *name; unsigned (*fn)(const aios_place_ctx_t *); }
AIOS_PLACE_STRATEGIES[] = {
    { "hybrid", aios_strat_hybrid },   /* 0: least-loaded among active, spill past threshold */
    { "spread", aios_strat_spread },   /* 1: globally coldest (max parallelism) */
    { "pack",   aios_strat_pack },     /* 2: most-loaded under threshold (max locality) */
    { "rr",     aios_strat_rr },       /* 3: round-robin (naive baseline) */
};
#define AIOS_N_PLACE_STRATEGIES ((int)(sizeof(AIOS_PLACE_STRATEGIES) / sizeof(AIOS_PLACE_STRATEGIES[0])))

#endif /* AIOS_SCHED_STRATEGY_H */
