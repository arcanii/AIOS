/* E1 bare-metal stall reproducer -- MULTI-CORE variant (runs at EL2).
 *
 * Core 0: enable MMU+caches, bring the cluster up (cores 1-3 join the coherency
 * domain via shared MMU + SMPEN, do a coherent burst, then idle in WFE), then run
 * the same quiesce+timed-op test as the single-core trial. If a fabric op now
 * stalls ~32400ms, the multi-core coherency/DVM domain is the trigger.
 */
#include <stdint.h>

/* ---- mini-UART (AUX @ 0xFE215000), firmware-configured 115200 8N1 ---- */
#define AUX_MU_IO   (*(volatile uint32_t *)0xFE215040u)
#define AUX_MU_LSR  (*(volatile uint32_t *)0xFE215054u)
#define LSR_TX_RDY  (1u << 5)

static void uputc(char c)
{
    for (int t = 0; t < 2000000; t++)
        if (AUX_MU_LSR & LSR_TX_RDY) { AUX_MU_IO = (uint32_t)(uint8_t)c; return; }
}
static void uputs(const char *s) { while (*s) { if (*s == '\n') uputc('\r'); uputc(*s++); } }
static void udec(uint64_t v)
{
    char b[24]; int i = 0;
    if (!v) { uputc('0'); return; }
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) uputc(b[--i]);
}

#define RD(reg)      ({ uint64_t _v; __asm__ volatile("mrs %0, " #reg : "=r"(_v)); _v; })
#define WR(reg, val) __asm__ volatile("msr " #reg ", %0" :: "r"((uint64_t)(val)))
static inline uint64_t cntpct(void)
{ uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v)); return v; }

/* ---- shared identity page table (all cores use it -> coherent, inner-shareable) ---- */
#define DRAM_BLK(oa) ((uint64_t)(oa) | (1ull << 10) | (3ull << 8) | (4ull << 2) | 1ull)
#define DEV_BLK(oa)  ((uint64_t)(oa) | (1ull << 10) | (0ull << 8) | (0ull << 2) | 1ull)
static const uint64_t __attribute__((aligned(4096))) l1_table[512] = {
    [0] = DRAM_BLK(0x00000000ull),
    [1] = DRAM_BLK(0x40000000ull),
    [2] = DRAM_BLK(0x80000000ull),
    [3] = DEV_BLK(0xC0000000ull),
};
#define MAIR_VAL ((0xffull << 32) | (0x00ull << 0))

static void mmu_on(void)   /* per-core: SMPEN + L2 clock-force + MMU/caches */
{
    WR(S3_1_C15_C2_1, RD(S3_1_C15_C2_1) | (1ull << 6));   /* CPUECTLR_EL1.SMPEN */
    if (((RD(midr_el1) >> 4) & 0xfff) == 0xD08)
        WR(S3_1_C15_C0_0, RD(S3_1_C15_C0_0) | 0x0C000000ull);
    __asm__ volatile("isb");
    WR(mair_el2, MAIR_VAL);
    uint64_t pr = RD(id_aa64mmfr0_el1) & 0xf;
    WR(tcr_el2, 25ull | (1ull << 8) | (1ull << 10) | (3ull << 12) | (0ull << 14)
              | (pr << 16) | (1ull << 23) | (1ull << 31));
    WR(ttbr0_el2, (uint64_t)l1_table);
    __asm__ volatile("isb; dsb sy; tlbi alle2; dsb sy; isb");
    WR(sctlr_el2, RD(sctlr_el2) | (1ull << 0) | (1ull << 2) | (1ull << 12));
    __asm__ volatile("isb");
}

/* ---- cluster bring-up ---- */
#define NSEC 3                          /* secondaries: cores 1,2,3 */
static volatile int core_up[4];
static volatile uint64_t shared_buf[1024] __attribute__((aligned(64)));

void secondary_main(uint64_t core)
{
    mmu_on();                            /* join the coherency domain (SMPEN + shared MMU) */
    /* a coherent RMW burst so the cluster actually exchanges snoop/DVM traffic */
    for (int r = 0; r < 100; r++)
        for (int i = 0; i < 1024; i++)
            shared_buf[i] += core + 1;
    __asm__ volatile("dsb sy" ::: "memory");
    core_up[core & 3] = 1;               /* signal core 0; then boot.S parks us in WFE */
}

/* ---- the experiment: COLD INSTRUCTION FETCH vs COLD DATA LOAD after idle ----
 * Session-10 (HW, v0.4.291) localized the AIOS ~32.4s wedge to a cold INSTRUCTION-cache
 * refill of the user resume line after idle -- an I-SIDE fetch. e1 trials 1-4 + e3 NEVER
 * tested this: they timed cold DATA loads + broadcast DVM-Sync (all 0ms). This variant
 * evicts a target line, idles so the SCB fabric parks, then EITHER fetches that line
 * (jump to it) OR loads it (data), timing the first post-idle access. Decision:
 *   I-fetch hangs ~32400ms but D-load does not -> the cold-I-fetch-through-parked-fabric
 *     is a SILICON property of this BCM2711 (matches AIOS) -> cure = keep the line warm.
 *   both fast -> bare-metal does not reproduce it -> the AIOS EL1&0/eviction context matters. */
#define QUIESCE_S 30
#define TRIALS    6                       /* alternating: even = I-fetch, odd = D-load control */
static volatile uint64_t test_buf[8] __attribute__((aligned(64)));
volatile uint64_t g_cold_entry;           /* cold_target stamps its post-fetch entry cntpct here */

/* The cold-fetch target: line-aligned, never called before the timed test, so evicting its
 * line(s) makes the ENTRY fetch a genuine cold refill from DRAM. Its first act reads cntpct
 * (the post-fetch timestamp). noinline -> the call is a real `blr` (the I-fetch we time). */
void cold_target(void);
__attribute__((noinline, aligned(64)))
void cold_target(void)
{
    uint64_t t; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
    g_cold_entry = t;
    __asm__ volatile("nop;nop;nop;nop;nop;nop;nop;nop");
}

static void evict_iline(uint64_t a)        /* make an INSTRUCTION line cold: L1D/L2 -> DRAM, then drop L1I */
{
    __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");   /* clean+invalidate unified L2/L1D to PoC(DRAM) */
    __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");    /* invalidate L1I to PoU (IS-broadcast) */
}

void repro_main(void)
{
    mmu_on();
    uint64_t frq = RD(cntfrq_el0);
    if (!frq) frq = 54000000ull;

    /* let the firmware's 2nd-stage UART output drain so the banner is clean */
    uint64_t d = cntpct();
    while (cntpct() - d < frq / 5) { }

    uputs("\n[E1-mc] booted EL2, MMU+caches ON, core0, A72. cntfrq=");
    udec(frq); uputs(" Hz\n");

    /* wait (bounded ~3s) for cores 1-3 to join the coherency domain */
    uint64_t w = cntpct();
    while ((cntpct() - w < 3ull * frq) &&
           !(core_up[1] && core_up[2] && core_up[3])) { }
    int up = core_up[1] + core_up[2] + core_up[3];
    uputs("[E1-mc] secondaries up: "); udec(up); uputs("/3");
    uputs((up == NSEC) ? "  -> 4 cores in coherency domain\n"
                       : "  -> WARNING: not all cores joined (spin-table?)\n");
    uputs("[E1-mc] COLD I-FETCH vs D-LOAD after "); udec(QUIESCE_S);
    uputs("s idle. I-fetch hang ~32400ms => silicon I-path (matches AIOS s10).\n");

    for (int trial = 0; trial < TRIALS; trial++) {
        int do_ifetch = ((trial & 1) == 0);            /* even = I-fetch, odd = D-load control */

        /* Evict the target so the first post-idle access is a genuine cold refill from DRAM. */
        if (do_ifetch) {
            uint64_t base = (uint64_t)&cold_target;
            evict_iline(base);                          /* entry line */
            evict_iline(base + 64);                     /* + next line (in case it spills) */
        } else {
            __asm__ volatile("dc civac, %0" :: "r"((uint64_t)&test_buf[0]) : "memory");
        }
        __asm__ volatile("dsb ish; isb" ::: "memory");

        /* Idle: busy-spin on CNTPCT only (no memory/MMIO) so the SCB fabric parks. Faithful to
         * AIOS core 0 (no-WFI, but generates no external bus traffic -> the fabric quiesces). */
        uint64_t s = cntpct();
        while (cntpct() - s < (uint64_t)QUIESCE_S * frq) { }

        uint64_t ms;
        if (do_ifetch) {
            uint64_t t0 = cntpct();
            cold_target();                              /* <-- THE COLD INSTRUCTION FETCH (blr to evicted line) */
            ms = ((g_cold_entry - t0) * 1000ull) / frq;
            uputs("[E1-mc] trial "); udec(trial); uputs(" COLD-IFETCH dur="); udec(ms); uputs("ms");
        } else {
            uint64_t t0 = cntpct();
            volatile uint64_t v = test_buf[0]; (void)v; /* cold DATA load (control; e1 baseline = 0ms) */
            __asm__ volatile("dsb sy" ::: "memory");
            ms = ((cntpct() - t0) * 1000ull) / frq;
            uputs("[E1-mc] trial "); udec(trial); uputs(" COLD-DLOAD  dur="); udec(ms); uputs("ms");
        }
        if (ms > 5000) uputs("   <<<<< HANG");
        uputc('\n');
    }
    uputs("[E1-mc] cold-fetch test complete.\n");
}
