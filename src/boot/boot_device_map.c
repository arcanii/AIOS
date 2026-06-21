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
seL4_CPtr dev_genet_frame_caps[16];   /* netd Stage 3: retained GENET MMIO frame caps */
volatile uint32_t *dev_vcmbox_vaddr;
uint32_t dev_vcmbox_off;
volatile uint32_t *dev_pm_vaddr;
volatile uint32_t *dev_pcie_vaddr;
volatile uint32_t *dev_v3d_vaddr;
volatile uint32_t *dev_v3d_asb_vaddr;
volatile uint32_t *dev_armlocal_vaddr;   /* 0xFF800000 ARM-local (AXI_QUIET_TIME @ +0x30); best-effort */
volatile uint32_t *dev_dma_vaddr;        /* 0xFE007000 BCM2711 legacy DMA controller; session-8 DRAM keep-warm; best-effort */

#ifdef PLAT_RPI4

#define RPI4_GPIO_PADDR 0xFE200000UL
#define RPI4_MUART_PADDR 0xFE215000UL
#define RPI4_PM_PADDR 0xFE100000UL     /* power management / watchdog */
#define RPI4_V3D_ASB_PADDR 0xFEC11000UL /* RPiVid ASB V3D power bridges (1 page) */
#define RPI4_ARMLOCAL_PADDR 0xFF800000UL /* ARM-local block (AXI_QUIET_TIME @ +0x30); diagnostic */
#define RPI4_DMA_PADDR 0xFE007000UL       /* BCM2711 legacy DMA controller (channels 0-14, 1 page) */

struct dev_req {
    uint64_t paddr;          /* page-aligned */
    int npages;
    volatile uint32_t **out;
    const char *name;
    seL4_CPtr *caps_out;     /* netd Stage 3: if non-NULL, retain the frame caps here */
};

/* Allocate npages contiguous device frames starting at paddr and map them
 * into the root vspace (non-cacheable). Returns the vaddr, or NULL on error.
 * If out_caps is non-NULL, the npages frame caps are also stored there (so
 * spawn_netd can copy + re-map the GENET MMIO into netd). */
static void *map_dev(uint64_t paddr, int npages, seL4_CPtr *out_caps)
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
        if (out_caps) out_caps[p] = f.cptr;
    }
    return vspace_map_pages(&vspace, caps, NULL, seL4_AllRights,
                            npages, seL4_PageBits, 0);
}

void prealloc_rpi4_devices(void)
{
    struct dev_req reqs[13];
    int n = 0;

    reqs[n++] = (struct dev_req){ RPI4_GPIO_PADDR,  1, &dev_gpio_vaddr, "gpio" };
    reqs[n++] = (struct dev_req){ RPI4_MUART_PADDR, 1, &dev_uart_vaddr, "uart" };
    /* PM/watchdog block (0xFE100000) -- sits between the VC mailbox (0xFE00B)
     * and GPIO (0xFE200000); the ascending sort places it correctly. Used by
     * aios_system_reboot for the BCM2711 watchdog reset. */
    reqs[n++] = (struct dev_req){ RPI4_PM_PADDR, 1, &dev_pm_vaddr, "pm" };
    if (hw_info.has_emmc)
        reqs[n++] = (struct dev_req){ hw_info.emmc_paddr & ~0xFFFUL, 1,
                                      &dev_emmc_vaddr, "emmc" };
    if (hw_info.has_genet)
        reqs[n++] = (struct dev_req){ hw_info.genet_paddr & ~0xFFFUL, 16,
                                      &dev_genet_vaddr, "genet", dev_genet_frame_caps };
    if (hw_info.has_vc_mbox) {
        dev_vcmbox_off = (uint32_t)(hw_info.vc_mbox_paddr & 0xFFF);
        reqs[n++] = (struct dev_req){ hw_info.vc_mbox_paddr & ~0xFFFUL, 1,
                                      &dev_vcmbox_vaddr, "vcmbox" };
    }
    /* brcmstb PCIe controller regs (0xFD500000, ~0x9310 -> 10 pages). Sits BELOW
     * GENET (0xFD580000), so the ascending sort claims it first -- USB HID Phase
     * D (link bring-up + VL805 detect). NOT the MMIO window (0x6_00000000), which
     * seL4 does not expose; see docs/DESIGN_USB_HID.md "Phase D findings". */
    if (hw_info.has_pcie)
        reqs[n++] = (struct dev_req){ hw_info.pcie_ecam_paddr & ~0xFFFUL, 10,
                                      &dev_pcie_vaddr, "pcie" };
    /* V3D 4.2 GPU: hub+core0 as ONE contiguous 8-page region (0xFEC00000), plus the
     * RPiVid ASB power bridges (0xFEC11000, 1 page). Both sit ABOVE the eMMC
     * (0xFE340000, the current highest claim), so the ascending watermark is safe;
     * they still go through the sorted table (house convention). v3d_init then uses
     * these vaddrs instead of self-mapping (GENET rule). */
    if (hw_info.has_v3d) {
        reqs[n++] = (struct dev_req){ hw_info.v3d_paddr & ~0xFFFUL, 8,
                                      &dev_v3d_vaddr, "v3d" };
        reqs[n++] = (struct dev_req){ RPI4_V3D_ASB_PADDR, 1,
                                      &dev_v3d_asb_vaddr, "v3dasb" };
    }
    /* ARM-local block (0xFF800000): the AXI_QUIET_TIME fabric-quiesce detector lives at +0x30
     * (BCM2711 datasheet 6.5.2). Diagnostic only (cure space closed) -- best-effort: if it is
     * not exposed as a device untyped, map_dev returns NULL and the loop just records that. It
     * is the highest paddr, so the ascending watermark reaches it last. */
    reqs[n++] = (struct dev_req){ RPI4_ARMLOCAL_PADDR, 1, &dev_armlocal_vaddr, "armlocal" };
    /* BCM2711 legacy DMA controller (0xFE007000): session-8 autonomous DRAM keep-warm
     * (src/servers/dma_warm.c). One page covers legacy channels 0-14. Sits between the VC
     * mailbox (0xFE00B000) and PM (0xFE100000); the ascending sort places it. Best-effort:
     * if not exposed as a device untyped, map_dev returns NULL and dma_warm_init no-ops. */
    reqs[n++] = (struct dev_req){ RPI4_DMA_PADDR, 1, &dev_dma_vaddr, "dma" };

    /* Insertion sort ascending by paddr (n <= 10). This ordering is the whole
     * point -- claim low addresses before the watermark passes them. */
    for (int i = 1; i < n; i++) {
        struct dev_req k = reqs[i];
        int j = i - 1;
        while (j >= 0 && reqs[j].paddr > k.paddr) { reqs[j + 1] = reqs[j]; j--; }
        reqs[j + 1] = k;
    }

    for (int i = 0; i < n; i++) {
        void *v = map_dev(reqs[i].paddr, reqs[i].npages, reqs[i].caps_out);
        *(reqs[i].out) = (volatile uint32_t *)v;
        printf("[devmap] %-6s 0x%lx -> %p (%d pg)\n",
               reqs[i].name, (unsigned long)reqs[i].paddr, v, reqs[i].npages);
    }
}

#else  /* !PLAT_RPI4 */

void prealloc_rpi4_devices(void) { /* QEMU: drivers self-map their MMIO */ }

#endif
