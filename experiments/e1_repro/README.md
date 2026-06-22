# E1 — bare-metal stall reproducer (RPi4 / BCM2711 / A72)

Tests whether the ~32.4s idle→wake fabric freeze reproduces with **no seL4** — a minimal
standalone program using AIOS's exact low-level config (SMPEN, L2ACTLR clock-force,
Normal-WB inner-shareable DRAM, mini-UART) but none of its scheduler/servers/BKL. Runs on
core 0 at EL2; cores 1–3 parked. See `docs/NEXT_20260622_linux_uboot_stall_experiments.md`.

**What it does, per trial:** evict a target DRAM line → spin ~30s polling `CNTPCT` only (no
memory/MMIO traffic, so the SCB fabric quiesces) → time ONE fabric op. It cycles four ops to
pin the trigger: cold cacheable DRAM **load** (the s8 "cold-load" hypothesis), `tlbi+dsb`
(DVM path), `dsb` alone, and a Device read (expected NOT to hang).

## Files
- `boot.S` `repro.c` `repro.ld` — the reproducer (links at 0x80000, "ARMd" header, no relocator).
- `build.sh` — compile → `kernel8.img` (+ structural sanity checks).
- `config.txt` — identical to AIOS (core_freq=250, enable_uart=1 → mini-UART 115200).
- `mkbootimg.py` — builds `e1_boot.img` (Etcher-ready: MBR + FAT32 + firmware + kernel8.img).
- `e1_boot.img` — **flash this** (already built).

## Run (spare SD — the AIOS SD is untouched)
1. **Flash** `e1_boot.img` to the spare SD with **balenaEtcher**.
2. **Serial**: USB–TTL adapter on GPIO 14/15 (mini-UART), 115200 8N1. GND↔GND, Pi TXD(GPIO14)→adapter RX.
3. **Capture** on the Mac: `python3 scripts/sercap.py /tmp/e1.log` (one serial reader only).
4. **Insert the SD into the same Pi4** (same silicon) and power on. The test runs autonomously
   (~6–7 min: 4 ops × 3 trials × 30s) then hangs. Swap back to the AIOS SD when done.

## Expected output + interpretation
```
[E1] booted EL2, MMU+caches ON, core0, A72. cntfrq=54000000 Hz
[E1] quiesce=30s trials=3 -- watch for dur ~32400ms (STALL).
[E1] op=coldload trial=0 dur=____ms ...
...
```
- **Any op shows `dur` ≈ 32400ms (`<<<<< STALL`)** → the freeze **reproduces bare-metal** →
  it is silicon+firmware, *not* anything seL4 does, and the named op is the trigger (cold load
  vs DVM — which our 4 mixed AIOS sites could never cleanly separate). Cure axis becomes E2/E4
  (config/power), and this is now a tiny, single-core, flash-free-iterable testbed.
- **All ops ≈ 0ms** → single-core bare-metal does **not** reproduce → the freeze needs OS/SMP
  state (the multi-core coherency/DVM domain). Next: a 2-core variant (bring up core 1 to
  generate real DVM traffic) and retest.
- **No serial at all** → check wiring/baud; firmware rejects a bad image with "no compatible
  kernel" (magic is verified at 0x30, so unlikely).

## Tweak / rebuild
- Edit `repro.c` (e.g. `QUIESCE_S`, `TRIALS`, add ops) → `./build.sh` → `python3 mkbootimg.py` → re-flash.
- Quick-iterate without re-flashing the whole card: replace just `kernel8.img` on the SD's FAT
  partition (it is the only thing that changes), like AIOS's flash-free kernel swap.
- If 30s doesn't quiesce, try `QUIESCE_S` 35–60.

## Faithfulness notes / variables to vary later
- EL2 (not EL1) — fine for the EL-independent cold-load; drop to EL1 as a follow-up for `tlbi vae1`.
- Single core — intentional (matches core-0 pinning) but is itself a variable → the 2-core follow-up.
- L2ACTLR clock-force is ON (matches AIOS) — toggling it off is its own mini-experiment.
