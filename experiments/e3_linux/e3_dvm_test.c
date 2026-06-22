// SPDX-License-Identifier: GPL-2.0
/*
 * E3 -- does a broadcast TLBI DVM-Sync hang after idle under a FULL Linux runtime?
 *
 * The bare-metal E1 repro (minimal runtime) did NOT reproduce the AIOS ~32.4s freeze,
 * even doing the exact broadcast DVM-Sync after 240s of 4-core idle. Hypothesis: the
 * BCM2711 SCB only enters the quiesced state under a full VideoCore-firmware runtime.
 * Linux is the reference FULL runtime that is immune to the freeze -- so: force all
 * cores quiet for `idle_s` seconds (via stop_machine; CNTVCT-only spin, no memory/fabric
 * traffic), then CPU 0 times `tlbi vmalle1is; dsb sy` (the EL1 IS-broadcast DVM-Sync,
 * the analog of AIOS's tlbi vae1is). If it hangs ~32.4s -> the SCB DOES quiesce under a
 * full runtime and the TLBI triggers it (a reference reproducer + a silicon/firmware
 * property). If it stays ~0ms -> Linux keeps the SCB warm even under forced idle, which
 * is its immunity, and the cure is to keep the SCB warm the Linux way.
 *
 * WARNING: holds all cores with IRQs off for idle_s (and, if it reproduces, +32.4s).
 * Disable the lockup detector first:  echo 0 | sudo tee /proc/sys/kernel/watchdog
 * Build on the Pi (raspberrypi-kernel-headers); load:  sudo insmod e3_dvm_test.ko idle_s=20
 * Output goes to dmesg (and the serial console).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/stop_machine.h>
#include <linux/smp.h>
#include <linux/cpumask.h>

static int idle_s = 20;
module_param(idle_s, int, 0444);
MODULE_PARM_DESC(idle_s, "seconds to hold all cores idle before the TLBI (default 20)");

static inline u64 rd_cntvct(void){ u64 v; asm volatile("isb; mrs %0, cntvct_el0":"=r"(v)); return v; }
static inline u64 rd_cntfrq(void){ u64 v; asm volatile("mrs %0, cntfrq_el0":"=r"(v)); return v; }

/* Runs on EVERY online cpu simultaneously (stop_machine, IRQs off). */
static int quiet_then_tlbi(void *unused)
{
	u64 frq = rd_cntfrq();
	u64 start = rd_cntvct();

	/* all cores spin on CNTVCT only -- NO memory/MMIO -> the fabric goes quiet */
	while (rd_cntvct() - start < (u64)idle_s * frq)
		cpu_relax();

	/* exactly one core issues the timed broadcast DVM-Sync; others keep spinning */
	if (smp_processor_id() == 0) {
		u64 t0 = rd_cntvct();
		asm volatile("tlbi vmalle1is; dsb sy; isb" ::: "memory");  /* IS broadcast -> DVM-Sync */
		u64 dur = rd_cntvct() - t0;
		u64 ms = dur * 1000ULL / frq;
		pr_err("[E3] idle=%ds  tlbi vmalle1is+dsb  dur=%llums%s\n",
		       idle_s, ms, ms > 5000 ? "   <<<<< HANG (reproduced!)" : "  (no hang)");
	}
	return 0;
}

static int __init e3_init(void)
{
	pr_err("[E3] forcing %ds all-core idle (cpus=%d), then a broadcast TLBI DVM-Sync...\n",
	       idle_s, num_online_cpus());
	stop_machine(quiet_then_tlbi, NULL, cpu_online_mask);
	pr_err("[E3] done. (rmmod, then re-insmod with a different idle_s to sweep)\n");
	return 0;
}
static void __exit e3_exit(void) { }

module_init(e3_init);
module_exit(e3_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("E3: broadcast TLBI DVM-Sync after forced all-core idle (stall repro probe)");
