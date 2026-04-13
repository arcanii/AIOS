/*
 * plat.h -- Platform Abstraction Layer dispatcher
 *
 * Selects platform-specific headers based on compile-time define.
 * Modeled after src/arch/arch.h.
 */
#ifndef AIOS_PLAT_H
#define AIOS_PLAT_H

#if defined(PLAT_QEMU_VIRT)
/* QEMU virt: virtio-blk, virtio-net, ramfb */
#elif defined(PLAT_RPI4)
/* RPi4: BCM2835 SDHCI, GENET, VC mailbox (future) */
#else
#error "No platform selected. Define PLAT_QEMU_VIRT or PLAT_RPI4."
#endif

#endif /* AIOS_PLAT_H */
