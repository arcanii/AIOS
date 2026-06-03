/*
 * boot_device_map.c -- RPi4 peripheral MMIO pre-mapping in ascending
 * physical-address order. See include/aios/device_map.h for the why.
 *
 * Fixes the v0.4.98 GENET + display disable: both peripherals sit at LOWER
 * physical addresses than the eMMC/UART/GPIO that were mapped first, so their
 * sel4platsupport_alloc_frame_at landed behind the device-untyped watermark
 * and failed. Claiming every region in one low->high pass removes that.
 */
#include "aios/device_map.h"
#include "aios/root_shared.h"      /* vka, vspace globals */
#include "aios/hw_info.h"
#include <sel4platsupport/device.h>
#include <stdio.h>

volatile uint32_t *dev_gpio_vaddr;
volatile uint32_t *dev_uart_vaddr;
volatile uint32_t *dev_emmc_vaddr;
volatile uint32_t *dev_genet_vaddr;
volatile uint32_t *dev_vcmbox_vaddr;
uint32_t dev_vcmbox_off;

#ifdef PLAT_RPI4

#define RPI4_GPIO_PADDR 0xFE200000UL
#define RPI4_MUART_PADDR 0xFE215000UL

struct dev_req {
    uint64_t paddr;          /* page-aligned */
    int npages;
    volatile uint32_t **out;
    const char *name;
};

/* Allocate npages contiguous device frames starting at paddr and map them
 * into the root vspace (non-cacheable). Returns the vaddr, or NULL on error. */
static void *map_dev(uint64_t paddr, int npages)
{
    seL4_CPtr caps[16];
    if (npages > 16) npages = 16;
    for (int p = 0; p < npages; p++) {
        vka_object_t f;
        int err = sel4platsupport_alloc_frame_at(
            &vka, paddr + (uint64_t)p * 0x1000, seL4_PageBits, &f);
        if (err) {
            printf("[devmap] alloc 0x%lx pg%d failed: %d\n",
                   (unsigned long)paddr, p, err);
            return NULL;
        }
        caps[p] = f.cptr;
    }
    return vspace_map_pages(&vspace, caps, NULL, seL4_AllRights,
                            npages, seL4_PageBits, 0);
}

void prealloc_rpi4_devices(void)
{
    struct dev_req reqs[5];
    int n = 0;

    reqs[n++] = (struct dev_req){ RPI4_GPIO_PADDR,  1, &dev_gpio_vaddr, "gpio" };
    reqs[n++] = (struct dev_req){ RPI4_MUART_PADDR, 1, &dev_uart_vaddr, "uart" };
    if (hw_info.has_emmc)
        reqs[n++] = (struct dev_req){ hw_info.emmc_paddr & ~0xFFFUL, 1,
                                      &dev_emmc_vaddr, "emmc" };
    if (hw_info.has_genet)
        reqs[n++] = (struct dev_req){ hw_info.genet_paddr & ~0xFFFUL, 16,
                                      &dev_genet_vaddr, "genet" };
    if (hw_info.has_vc_mbox) {
        dev_vcmbox_off = (uint32_t)(hw_info.vc_mbox_paddr & 0xFFF);
        reqs[n++] = (struct dev_req){ hw_info.vc_mbox_paddr & ~0xFFFUL, 1,
                                      &dev_vcmbox_vaddr, "vcmbox" };
    }

    /* Insertion sort ascending by paddr (n <= 5). This ordering is the whole
     * point -- claim low addresses before the watermark passes them. */
    for (int i = 1; i < n; i++) {
        struct dev_req k = reqs[i];
        int j = i - 1;
        while (j >= 0 && reqs[j].paddr > k.paddr) { reqs[j + 1] = reqs[j]; j--; }
        reqs[j + 1] = k;
    }

    for (int i = 0; i < n; i++) {
        void *v = map_dev(reqs[i].paddr, reqs[i].npages);
        *(reqs[i].out) = (volatile uint32_t *)v;
        printf("[devmap] %-6s 0x%lx -> %p (%d pg)\n",
               reqs[i].name, (unsigned long)reqs[i].paddr, v, reqs[i].npages);
    }
}

#else  /* !PLAT_RPI4 */

void prealloc_rpi4_devices(void) { /* QEMU: drivers self-map their MMIO */ }

#endif
