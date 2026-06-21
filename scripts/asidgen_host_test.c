/*
 * asidgen_host_test.c -- host-side correctness test for the AIOS seL4
 * ASID-generation TLB-recycling algorithm (project_stall_hunt; the cure for the
 * RPi4 idle-teardown stall, docs/NEXT_20260621_asid_generation.md).
 *
 * The kernel cannot be exercised under QEMU because the QEMU build is HYP
 * (CONFIG_ARM_HYPERVISOR_SUPPORT) and the ASID-generation code only compiles on
 * the non-hyp Pi build. So this standalone program mirrors the resolver
 * (aios_hwasid_for / aios_hwasid_current / aios_hwasid_clear from
 * deps/kernel/src/arch/arm/64/kernel/vspace.c) and a GROUND-TRUTH TLB model, and
 * proves the stale-TLB invariant under spawn/teardown/unmap/wrap stress.
 *
 * The invariant: a TLB lookup for (running vspace, va) must return the CURRENT
 * physical frame for that vspace+va. A violation = one process reading anothers
 * (or its own stale) memory = the security hole this design must never create.
 *
 * Build + run:  cc -O2 -Wall -o /tmp/asidgen_host_test scripts/asidgen_host_test.c && /tmp/asidgen_host_test
 *
 * Output is OK/FAIL per the AIOS script convention. Exit 0 iff every sub-test
 * gives its EXPECTED result (the correct modes pass; the deliberately-buggy
 * modes are detected -- proving the checker has teeth).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ---- resolver state (mirrors vspace.c armKSHwAsid / g_hwasid_gen / g_hwasid_next) ---- */
#define NASIDS 64
typedef struct { uint16_t hw; uint16_t gen; } ent_t;
static ent_t  tab[NASIDS];
static uint16_t g_gen;
static uint16_t g_next;
static uint32_t HWMAX;          /* AIOS_HWASID_MAX, tiny here to force wraps */
static long assigns, wraps;

/* ---- ground-truth TLB model (a global, multi-core-shared cache) ---- */
typedef struct { int valid; uint16_t hw; uint32_t va; uint32_t pa; int owner; } tlbent_t;
#define TLBN 8192
static tlbent_t tlb[TLBN];

/* ---- vspace model: each sel4 asid maps a small set of VAs to UNIQUE pa values ---- */
#define MAXVA 24
static struct { int alive; uint32_t va[MAXVA]; uint32_t pa[MAXVA]; int nva; } vs[NASIDS];
static uint32_t g_pa_next = 1;  /* every map() hands out a brand-new pa so staleness is visible */

/* ---- per-core current TTBR (vspace + hw asid loaded) ---- */
typedef struct { int vspace; uint16_t hw; } core_t;
#define NCORES 2
static core_t core[NCORES];

/* ---- toggles for the deliberately-buggy modes ---- */
static int CLEAR_ON_UNMAP;      /* 1 = correct (fresh hw after any live-vspace unmap) */
static int RESERVE_AT_WRAP;     /* 1 = correct (reserve active hw asids across a wrap) */

static long corruptions;        /* counted, not aborted, so a run reports a total */

static void reset_all(void)
{
    memset(tab, 0, sizeof tab);
    memset(tlb, 0, sizeof tlb);
    memset(vs, 0, sizeof vs);
    memset(core, 0, sizeof core);
    g_gen = 1; g_next = 1; g_pa_next = 1;
    assigns = wraps = corruptions = 0;
}

static void full_flush(void)    /* models tlbi vmalle1 at a generation wrap */
{
    for (int i = 0; i < TLBN; i++) tlb[i].valid = 0;
}

/* reserve every currently-active hw asid across a wrap so it is NOT reissued this
 * generation (the Linux/xv6 reserved-asid fix; design section 5). Re-stamps the
 * running vspaces entry into the new generation with the SAME hw value. */
static uint8_t reserved[1 << 16];

static uint16_t hwasid_for(int sel4_asid)
{
    if (sel4_asid == 0 || sel4_asid >= NASIDS) return (uint16_t)sel4_asid;
    ent_t *e = &tab[sel4_asid];
    if (e->gen != g_gen) {
        if (g_next > HWMAX) {
            /* wrap */
            if (++g_gen == 0) g_gen = 1;
            full_flush();
            memset(reserved, 0, sizeof reserved);
            if (RESERVE_AT_WRAP) {
                for (int c = 0; c < NCORES; c++) {
                    int rv = core[c].vspace;
                    if (rv > 0 && rv < NASIDS) {
                        uint16_t h = core[c].hw;
                        reserved[h] = 1;
                        /* carry the running vspace into the new gen with the same hw */
                        tab[rv].hw = h; tab[rv].gen = g_gen;
                    }
                }
            }
            g_next = 1;
            wraps++;
        }
        if (e->gen != g_gen) {          /* may have been re-stamped by reservation above */
            while (g_next <= HWMAX && reserved[g_next]) g_next++;
            e->hw  = (uint16_t)g_next++;
            e->gen = g_gen;
            assigns++;
        }
    }
    return e->hw;
}

/* mirrors the kernel flush-path helper (step 2). Not exercised by the full-model
 * sub-tests (the full ASID-gen model issues no per-asid flush), kept for fidelity. */
__attribute__((unused))
static uint16_t hwasid_current(int sel4_asid)   /* flush path: 0 = no live entries this gen */
{
    if (sel4_asid > 0 && sel4_asid < NASIDS)
        return (tab[sel4_asid].gen == g_gen) ? tab[sel4_asid].hw : 0;
    return (uint16_t)sel4_asid;
}

static void hwasid_clear(int sel4_asid)         /* abandon binding -> fresh hw next use */
{
    if (sel4_asid > 0 && sel4_asid < NASIDS) tab[sel4_asid].gen = 0;
}

/* table invariant: no two live (current-gen) sel4 asids share a hw asid value */
static int check_table_unique(void)
{
    for (int a = 1; a < NASIDS; a++) {
        if (tab[a].gen != g_gen) continue;
        for (int b = a + 1; b < NASIDS; b++) {
            if (tab[b].gen != g_gen) continue;
            if (tab[a].hw == tab[b].hw) {
                printf("  [INV-TABLE] asids %d and %d both bound to hw=%u gen=%u\n",
                       a, b, tab[a].hw, g_gen);
                return 0;
            }
        }
    }
    return 1;
}

/* ground truth: the current pa for (vspace,va), or 0 if unmapped */
static uint32_t gt_pa(int v, uint32_t va)
{
    if (v <= 0 || v >= NASIDS || !vs[v].alive) return 0;
    for (int i = 0; i < vs[v].nva; i++) if (vs[v].va[i] == va) return vs[v].pa[i];
    return 0;
}

/* the running vspace V (under hw H on core c) touches all its mapped VAs.  For
 * each: a TLB hit MUST return the current pa; a stale pa = corruption.  A miss
 * fills the entry from the page table (ground truth). This is the end-to-end
 * stale-TLB detector. */
static void touch_all(int c)
{
    int v = core[c].vspace;
    uint16_t h = core[c].hw;
    if (v <= 0 || v >= NASIDS || !vs[v].alive) return;
    for (int i = 0; i < vs[v].nva; i++) {
        uint32_t va = vs[v].va[i], pa = vs[v].pa[i];
        int hit = -1;
        for (int t = 0; t < TLBN; t++)
            if (tlb[t].valid && tlb[t].hw == h && tlb[t].va == va) { hit = t; break; }
        if (hit >= 0) {
            if (tlb[hit].pa != pa) {
                corruptions++;
                if (corruptions <= 3)
                    printf("  [CORRUPT] vspace=%d hw=%u va=%u got pa=%u (owner %d) want pa=%u\n",
                           v, h, va, tlb[hit].pa, tlb[hit].owner, pa);
            }
        } else {
            for (int t = 0; t < TLBN; t++) if (!tlb[t].valid) {
                tlb[t].valid = 1; tlb[t].hw = h; tlb[t].va = va; tlb[t].pa = pa; tlb[t].owner = v;
                break;
            }
        }
    }
}

/* ---- vspace operations (mirror the kernel side effects) ---- */
static void op_run(int c, int v)            /* context switch v onto core c, then run */
{
    uint16_t h = hwasid_for(v);
    if (!check_table_unique()) corruptions++;
    core[c].vspace = v; core[c].hw = h;
    touch_all(c);
}
static void op_map(int v, uint32_t va)
{
    if (gt_pa(v, va)) return;               /* already mapped */
    if (vs[v].nva >= MAXVA) return;
    vs[v].va[vs[v].nva] = va;
    vs[v].pa[vs[v].nva] = g_pa_next++;
    vs[v].nva++;
}
static void op_unmap(int v, uint32_t va)
{
    int found = 0;
    for (int i = 0; i < vs[v].nva; i++) if (vs[v].va[i] == va) {
        vs[v].va[i] = vs[v].va[vs[v].nva - 1];
        vs[v].pa[i] = vs[v].pa[vs[v].nva - 1];
        vs[v].nva--; found = 1; break;
    }
    if (found && CLEAR_ON_UNMAP) hwasid_clear(v);  /* live-vspace unmap -> fresh hw next run */
}
static void op_delete(int v)
{
    vs[v].alive = 0; vs[v].nva = 0;
    hwasid_clear(v);                        /* dead vspace -> reuse gets fresh hw */
}
static int op_spawn(void)                   /* find a free sel4 asid slot */
{
    for (int v = 1; v < NASIDS; v++) if (!vs[v].alive) {
        vs[v].alive = 1; vs[v].nva = 0; return v;
    }
    return 0;
}

/* deterministic LCG so runs are reproducible without <stdlib.h> rand seeding */
static uint64_t rng_state = 0x12345678abcdef01ull;
static uint32_t rnd(void) { rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(rng_state >> 33); }

/* ---- sub-test 1: single-core soak (the DEFAULT config) ---- */
static long soak_single_core(int ops)
{
    reset_all();
    HWMAX = 7;                              /* tiny -> many wraps */
    int v0 = op_spawn();
    op_map(v0, 0x1000); op_run(0, v0);
    for (int i = 0; i < ops; i++) {
        /* keep a handful of live vspaces, pinned to core 0 */
        int live[NASIDS], nlive = 0;
        for (int v = 1; v < NASIDS; v++) if (vs[v].alive) live[nlive++] = v;
        if (nlive < 4 || (nlive < 8 && (rnd() & 3) == 0)) { int v = op_spawn(); if (v) { op_map(v, 0x1000 + (rnd()%8)*0x1000); op_run(0, v); } continue; }
        int v = live[rnd() % nlive];
        switch (rnd() % 6) {
            case 0: op_map(v, 0x1000 + (rnd()%8)*0x1000); break;
            case 1: if (vs[v].nva) op_unmap(v, vs[v].va[rnd()%vs[v].nva]); break;   /* self-munmap-ish */
            case 2: case 3: op_run(0, v); break;                                    /* run (the check) */
            case 4: { uint32_t va = 0x1000 + (rnd()%8)*0x1000; op_unmap(v, va); op_map(v, va); op_run(0, v); break; } /* remap same va */
            case 5: if (nlive > 4) op_delete(v); break;
        }
    }
    return corruptions;
}

/* ---- sub-test 2: 2-core coresched wrap (the reserved-asid scenario) ----
 * core 1 keeps RUNNING vspace V under its old hw across a wrap driven by core 0;
 * without reservation, core 0 eventually reassigns Vs old hw to W -> stale hit. */
static long coresched_wrap(void)
{
    reset_all();
    HWMAX = 6;
    int V = op_spawn();
    op_map(V, 0x4000); op_map(V, 0x5000);
    op_run(1, V);                           /* V lives on core 1 (coresched) */
    /* core 0 churns many short-lived vspaces to drive the hw-asid space to a wrap,
     * while core 1 keeps re-touching V under its (old) hw -- modelling a thread
     * that does not re-resolve across the wrap. */
    for (int round = 0; round < 200; round++) {
        int w = op_spawn();
        if (!w) { /* free the oldest non-V slot */
            for (int v = 1; v < NASIDS; v++) if (v != V && vs[v].alive && core[1].vspace != v && core[0].vspace != v) { op_delete(v); break; }
            w = op_spawn();
        }
        if (w) { op_map(w, 0x4000); op_run(0, w); }   /* note: SAME va 0x4000 as V -> collision bait */
        touch_all(1);                                  /* core 1 keeps running V under old hw */
    }
    return corruptions;
}

int main(void)
{
    int fails = 0;
    printf("AIOS ASID-generation host correctness test\n");

    /* 1. single-core, correct mechanism: expect 0 corruptions */
    CLEAR_ON_UNMAP = 1; RESERVE_AT_WRAP = 1;
    long c1 = soak_single_core(300000);
    printf("%s  single-core soak (clear-on-unmap=ON): corruptions=%ld wraps=%ld assigns=%ld\n",
           c1 == 0 ? "OK  " : "FAIL", c1, wraps, assigns);
    if (c1 != 0) fails++;

    /* 2. single-core, BUGGY (no clear on unmap): expect corruption DETECTED (>0) */
    CLEAR_ON_UNMAP = 0; RESERVE_AT_WRAP = 1;
    long c2 = soak_single_core(300000);
    printf("%s  single-core soak (clear-on-unmap=OFF, buggy): corruptions=%ld (expected >0, checker has teeth)\n",
           c2 > 0 ? "OK  " : "FAIL", c2);
    if (c2 == 0) fails++;

    /* 3. coresched wrap, BUGGY (no reservation): expect corruption DETECTED (>0) */
    CLEAR_ON_UNMAP = 1; RESERVE_AT_WRAP = 0;
    long c3 = coresched_wrap();
    printf("%s  coresched wrap (reserve-at-wrap=OFF, buggy): corruptions=%ld (expected >0, proves reservation is NEEDED)\n",
           c3 > 0 ? "OK  " : "FAIL", c3);
    if (c3 == 0) fails++;

    /* 4. coresched wrap, correct (reservation ON): expect 0 corruptions */
    CLEAR_ON_UNMAP = 1; RESERVE_AT_WRAP = 1;
    long c4 = coresched_wrap();
    printf("%s  coresched wrap (reserve-at-wrap=ON): corruptions=%ld wraps=%ld (proves reservation is SUFFICIENT)\n",
           c4 == 0 ? "OK  " : "FAIL", c4, wraps);
    if (c4 != 0) fails++;

    printf("\n%s  %d/4 sub-tests gave the expected result\n", fails == 0 ? "PASS" : "FAIL", 4 - fails);
    return fails ? 1 : 0;
}
