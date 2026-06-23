# Decision Record: Linux/BSD driver reuse on seL4 for AIOS

- **Date:** 2026-06-23 (session 14)
- **Status:** ANALYSIS / not-yet-decided. Recommendation recorded; revisit at x86-64 bring-up.
- **Context:** AIOS is a from-scratch OS on seL4 (BKL config), RPi4/BCM2711 aarch64 only, with
  hand-written drivers (xHCI/USB, GENET, eMMC, ext2, FAT32, USB-MSC, V3D) over a userspace
  server/IPC model + SHM-ring transport. Goals: reach **x86-64** and gain **broad firmware/driver
  coverage** without hand-writing every driver. Backdrop: the ~32.4s idle->wake core-0 stall
  ([[project_stall_session12]], s14 prewarm confirmation in `docs/s14_results.md`) and the open
  question of whether to stay on seL4 or move to a Linux base.
- **Question reviewed this session:** the **API-shim / DDE / rump-kernel** model -- running Linux/BSD
  drivers as NATIVE userspace components on seL4 by emulating the kernel driver API (vs a driver-VM
  or a Linux base). Web-verified via a research+adversarial-verify workflow.

## The mechanism (what a shim is)
Re-implement enough of the kernel's INTERNAL driver-facing API as a userspace shim, link unmodified
upstream driver source against it, host the result as an seL4 component talking over your own IPC.
You emulate: kmalloc/DMA allocators, the device/bus model + probe/initcall ordering, request_irq
(-> seL4 IRQHandler caps + Notifications), timers/delays/workqueues/tasklets, spinlocks/mutexes/RCU,
and the per-subsystem core (USB core, net core, MMC core). **Hardest parts:** (a) DMA + physical
memory (drivers want real bus addresses; this is where isolation breaks without an IOMMU); (b)
initcall/probe + device-tree/ACPI/PCI enumeration; (c) **concurrency-model impedance** (Linux
preemptible/per-CPU/RCU/softirq vs AIOS's BKL + the core-0 stall -- the shim fights exactly our
weakest seam); (d) the long #include symbol tail (Genode's create_dummies/extract_initcall_order
tooling exists to manage it).

## The four routes (corrected by adversarial verification)
| Route | What | Runs on RAW seL4? | License | Maturity 2024-2026 |
|-------|------|-------------------|---------|--------------------|
| Genode dde_linux (lx_emul) | Linux drivers as native components via shim | **No -- only inside Genode** (Genode CAN sit on base-sel4, but you adopt the whole framework) | driver src GPLv2; Genode AGPLv3 | Active; re-based to Linux 6.12 LTS in 25.08 (Aug 2025), **annual** cadence |
| rump / rumprun | Unmodified **NetBSD** drivers in userspace | **Yes -- real seL4 port** (McLeod/Data61, CAmkES rumprun_ethernet) | **2-clause BSD, no copyleft** | **Stale x86-ONLY prototype**; only e1000 + NetBSD net stack tested; dormant since ~2022; zero aarch64 |
| sDDF (seL4 Device Driver Framework) | seL4-**native** hand-written drivers over shm rings | Yes (native) | permissive | Active/experimental; **NOT Linux reuse** -- the "rewrite" path, closest cousin to AIOS's SHM-ring |
| Linux driver-VM | unmodified Linux driver in a minimal Linux VM (UIO bridge) | Yes (seL4 VMM) | driver GPLv2 (isolated in VM) | The Trustworthy-Systems Linux-reuse path; needs IOMMU for containment |

**Key corrections to the loose "DDE vs VM" framing:** dde_linux means adopting Genode, not a raw-seL4
library. rump-on-seL4 is real but x86-only/stale/BSD/NetBSD-drivers. sDDF is native, not reuse.
Trustworthy-Systems "Linux driver reuse" is a driver-VM, a 4th path.

## The decisive constraint: IOMMU / isolation (the inversion)
A reused driver -- shim OR VM -- that drives a bus-mastering device can program **DMA to arbitrary
physical memory**. seL4 caps govern CPU access, not what a device writes. So **without an IOMMU the
reused driver is inside the TCB** regardless of how it's wrapped -- it's code reuse, not containment.
- **RPi4/BCM2711: no usable SMMU** -> reuse buys ZERO isolation; and no mature aarch64 reuse tooling
  exists anyway.
- **x86-64: VT-d** -> real containment; the isolation argument only becomes true there.
- **Inversion:** the real reuse tooling (rump-on-seL4) is x86-only, the isolation guarantee is
  x86-only, but AIOS's drivers are aarch64 -- where both are absent. **Reuse pays off on the platform
  AIOS hasn't reached, and poorly on the one it's on.**

## Effort + recurring tax + license
- Shim infra on raw seL4 = engineer-**years**; **1-3 person-months per subsystem** (USB, NIC, GPU each
  separate); **recurring ~annual re-base tax** (lx_emul pins to a kernel version; Genode re-bases yearly
  and abandoned a shared shim as unmaintainable -> per-driver lx_emul). A permanent line item.
- Hand-written/sDDF = high up-front, but **no foreign-version tax** (you own the ABI). This is why
  AIOS's existing drivers carry no external maintenance dependency.
- **License:** linking GPLv2 Linux driver source into a component makes it a **GPLv2 derivative** (the
  syscall-boundary exception does NOT apply -- you link kernel code). Viral but survivable for OSS AIOS.
  **rump/NetBSD (BSD) avoids copyleft** -- its one real edge.

## RECOMMENDATION
1. **aarch64/RPi4 NOW: do NOT build a DDE/lx_emul or rump shim.** No isolation payoff (no SMMU), no
   aarch64 tooling, real recurring tax, and it fights the BKL/stall. **Keep hand-writing drivers
   (sDDF-style)** -- the model AIOS already executes well; strictly better ROI here than reuse.
2. **Driver reuse is an x86-64-phase decision.** When AIOS reaches x86-64 (VT-d), **prefer the Linux
   driver-VM over a shim** (real containment + no per-version shim tax). Reserve **rump** only for
   license-sensitive, BSD-sufficient device classes.
3. **Cheapest validating experiment (at x86 bring-up):** stand up the existing seL4 CAmkES
   `rumprun_ethernet` / e1000 app on x86-seL4 with VT-d. The one reuse path with a real pre-built seL4
   integration; exercises DMA + IRQ-cap->driver + a real net stack as a component; BSD-clean. If that
   is painful, it's decisive evidence to stay native. (Verify the stale port still builds first.)

## Tie to the seL4-vs-Linux meta-decision
On seL4, *practical* driver reuse is essentially an **x86 + driver-VM** story; the shim is not a
shortcut on AIOS's hardware. So "stay seL4 and reuse Linux drivers" really means "reach x86, run Linux
driver-VMs under seL4" -- justified ONLY if seL4's isolation/verification is the research thesis. If it
is not, the fact that the realistic reuse path is running Linux-in-a-box anyway is itself an argument
for a Linux-native base. The DDE/rump review did not open a new door -- it confirmed the real door is
**x86 + driver-VM**, and that the kernel-thesis question still decides everything.

## Sources
- Genode dde_linux experiments: https://genodians.org/skalk/2021-04-06-dde-linux-experiments
- Genode 25.08 release notes (Linux 6.12 LTS rebase): https://genode.org/documentation/release-notes/25.08
- Genode porting device drivers: https://genode.org/documentation/developer-resources/porting_device_drivers
- rump-on-seL4 (Trustworthy Systems blog): https://research.csiro.au/tsblog/using-rump-kernels-to-run-unmodified-netbsd-drivers-on-sel4/
- McLeod thesis: https://trustworthy.systems/publications/theses_public/16/McLeod:be.abstract
- seL4 Device Driver Framework (sDDF): https://trustworthy.systems/projects/drivers/ , https://github.com/au-ts/sDDF
- seL4 CAmkES (rumprun_ethernet): https://github.com/seL4/camkes
