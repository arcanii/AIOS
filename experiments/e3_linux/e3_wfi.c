// SPDX-License-Identifier: GPL-2.0
/*
 * E3-wfi -- the FAITHFUL idle->wake test. The stop_machine version (e3_dvm_test)
 * forced a BUSY-SPIN idle (cores executing, never WFI), which never signals
 * STANDBYWFI, so the SoC never asserts ACINACTM / quiesces the SCB. AIOS's stall
 * is the kernel-exit -> idle (cores WFI) -> IRQ-wake -> first-fabric-op path.
 *
 * Here: arm an hrtimer for idle_ms and RETURN, so the system genuinely idles
 * (cores enter WFI). When the timer IRQ fires (the wake-from-idle), its hard-IRQ
 * callback issues `tlbi vmalle1is; dsb sy` (the IS-broadcast DVM-Sync) and times
 * it. If the SCB quiesced during the WFI idle, the DVM-Sync hangs ~32.4s here.
 *
 * Run from a quiet system. Watchdog must be off (a hang freezes a core ~32.4s).
 *   sudo insmod e3_wfi.ko idle_ms=30000 ; sleep 35 ; dmesg | grep E3-wfi ; sudo rmmod e3_wfi
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

static int idle_ms = 30000;
module_param(idle_ms, int, 0444);
MODULE_PARM_DESC(idle_ms, "ms to let the system idle (cores WFI) before the TLBI (default 30000)");

static struct hrtimer t;

static inline u64 rd_cntvct(void){ u64 v; asm volatile("isb; mrs %0, cntvct_el0":"=r"(v)); return v; }
static inline u64 rd_cntfrq(void){ u64 v; asm volatile("mrs %0, cntfrq_el0":"=r"(v)); return v; }

static enum hrtimer_restart fire(struct hrtimer *h)
{
	u64 t0 = rd_cntvct();
	asm volatile("tlbi vmalle1is; dsb sy; isb" ::: "memory");   /* DVM-Sync in the wake/IRQ context */
	u64 dur = rd_cntvct() - t0;
	u64 frq = rd_cntfrq(); if (!frq) frq = 54000000ULL;
	u64 ms = dur * 1000ULL / frq;
	pr_err("[E3-wfi] after %dms WFI-idle: tlbi vmalle1is+dsb (timer hard-IRQ) dur=%llums%s\n",
	       idle_ms, ms, ms > 5000 ? "   <<<<< HANG (reproduced!)" : "  (no hang)");
	return HRTIMER_NORESTART;
}

static int __init e3wfi_init(void)
{
	pr_err("[E3-wfi] arming hard-IRQ timer for %dms; system idles (cores WFI) until it fires...\n", idle_ms);
	hrtimer_setup(&t, fire, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	hrtimer_start(&t, ms_to_ktime(idle_ms), HRTIMER_MODE_REL_HARD);
	return 0;   /* return immediately so the system can actually idle */
}
static void __exit e3wfi_exit(void) { hrtimer_cancel(&t); }

module_init(e3wfi_init);
module_exit(e3wfi_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("E3-wfi: broadcast TLBI DVM-Sync in the timer-IRQ wake after genuine WFI idle");
