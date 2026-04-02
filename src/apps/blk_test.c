/*
 * AIOS 0.4.x — Block device test
 *
 * Probes virtio-mmio slots for a block device,
 * initializes virtqueue, reads sector 0.
 *
 * argv[0] = virtio mmio frame cap
 * argv[1] = DMA frame cap
 * argv[2] = DMA physical address (for virtqueue setup)
 */
#include <stdio.h>
#include <stdint.h>
#include <sel4/sel4.h>
#include "virtio.h"

/* ── MMIO access ── */
static volatile uint32_t *vio_base;

static uint32_t vio_read32(uint32_t off) {
    return vio_base[off / 4];
}
static void vio_write32(uint32_t off, uint32_t val) {
    vio_base[off / 4] = val;
}

/* ── DMA layout within one 4K page ── */
/*
 * Offset 0x000: virtq_desc[16]  (16 * 16 = 256 bytes)
 * Offset 0x100: virtq_avail     (6 + 16*2 = 38 bytes)
 * Offset 0x200: padding
 * Offset 0x800: virtq_used      (6 + 16*8 = 134 bytes)
 * Offset 0xC00: virtio_blk_req  (16 + 512 + 1 = 529 bytes)
 */
#define DMA_DESC_OFF    0x000
#define DMA_AVAIL_OFF   0x100
#define DMA_USED_OFF    0x800
#define DMA_REQ_OFF     0xC00

static uint8_t *dma_vaddr;
static uint64_t dma_paddr;

static struct virtq_desc  *desc;
static struct virtq_avail *avail;
static struct virtq_used  *used;
static struct virtio_blk_req *blk_req;

static int virtio_blk_init(void) {
    uint32_t magic = vio_read32(VIRTIO_MMIO_MAGIC);
    uint32_t ver   = vio_read32(VIRTIO_MMIO_VERSION);
    uint32_t devid = vio_read32(VIRTIO_MMIO_DEVICE_ID);

    printf("[blk] magic=0x%x ver=%u devid=%u\n",
           (unsigned)magic, (unsigned)ver, (unsigned)devid);

    if (magic != VIRTIO_MAGIC) {
        printf("[blk] Bad magic\n");
        return -1;
    }
    if (devid != VIRTIO_BLK_DEVICE_ID) {
        printf("[blk] Not a block device (id=%u)\n", (unsigned)devid);
        return -1;
    }
    if (ver != 1 && ver != 2) {
        printf("[blk] Unsupported version %u\n", (unsigned)ver);
        return -1;
    }

    /* Reset */
    vio_write32(VIRTIO_MMIO_STATUS, 0);
    /* Acknowledge */
    vio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    /* Driver */
    vio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    /* Guest page size (legacy v1) */
    if (ver == 1) {
        vio_write32(VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    }
    /* No special features */
    vio_write32(VIRTIO_MMIO_DRV_FEATURES, 0);

    /* Select queue 0 */
    vio_write32(VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = vio_read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    printf("[blk] Queue max size: %u\n", (unsigned)qmax);
    if (qmax == 0) return -1;

    uint32_t qsz = (qmax < VIRTQ_SIZE) ? qmax : VIRTQ_SIZE;
    vio_write32(VIRTIO_MMIO_QUEUE_NUM, qsz);

    /* Set up virtqueue in DMA memory */
    desc     = (struct virtq_desc  *)(dma_vaddr + DMA_DESC_OFF);
    avail    = (struct virtq_avail *)(dma_vaddr + DMA_AVAIL_OFF);
    used     = (struct virtq_used  *)(dma_vaddr + DMA_USED_OFF);
    blk_req  = (struct virtio_blk_req *)(dma_vaddr + DMA_REQ_OFF);

    /* Zero DMA page */
    for (int i = 0; i < 4096; i++) dma_vaddr[i] = 0;

    /* Tell device where the queue is (legacy: PFN) */
    if (ver == 1) {
        vio_write32(VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(dma_paddr / 4096));
    }

    /* Driver OK */
    vio_write32(VIRTIO_MMIO_STATUS,
                VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    printf("[blk] virtio-blk initialized (qsz=%u)\n", (unsigned)qsz);
    return 0;
}

static int virtio_blk_read(uint64_t sector, uint8_t *out) {
    /* Set up request */
    blk_req->type = VIRTIO_BLK_T_IN;
    blk_req->reserved = 0;
    blk_req->sector = sector;
    blk_req->status = 0xFF;

    uint64_t req_pa = dma_paddr + DMA_REQ_OFF;

    /* Descriptor 0: request header (type + reserved + sector = 16 bytes) */
    desc[0].addr  = req_pa;
    desc[0].len   = 16;
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next  = 1;

    /* Descriptor 1: data buffer (512 bytes, device writes) */
    desc[1].addr  = req_pa + 16;
    desc[1].len   = VIRTIO_BLK_SECTOR_SIZE;
    desc[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
    desc[1].next  = 2;

    /* Descriptor 2: status byte (1 byte, device writes) */
    desc[2].addr  = req_pa + 16 + VIRTIO_BLK_SECTOR_SIZE;
    desc[2].len   = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next  = 0;

    /* Add to available ring */
    uint16_t avail_idx = avail->idx;
    avail->ring[avail_idx % VIRTQ_SIZE] = 0;
    __asm__ volatile("dmb sy" ::: "memory");
    avail->idx = avail_idx + 1;

    /* Notify device */
    vio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* Poll for completion */
    uint16_t used_idx = used->idx;
    for (int i = 0; i < 1000000; i++) {
        __asm__ volatile("dmb sy" ::: "memory");
        if (used->idx != used_idx) break;
    }
    if (used->idx == used_idx) {
        printf("[blk] Timeout waiting for read\n");
        return -1;
    }

    /* Ack interrupt */
    vio_read32(VIRTIO_MMIO_INTERRUPT_STATUS);
    vio_write32(VIRTIO_MMIO_INTERRUPT_ACK, 1);

    if (blk_req->status != 0) {
        printf("[blk] Read failed, status=%u\n", blk_req->status);
        return -1;
    }

    /* Copy data out */
    for (int i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++) {
        out[i] = blk_req->data[i];
    }
    return 0;
}

static long parse_num(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int main(int argc, char *argv[]) {
    printf("[blk_test] Started\n");

    seL4_CPtr vio_cap = 0, dma_cap = 0;
    dma_paddr = 0;

    if (argc > 0) vio_cap = (seL4_CPtr)parse_num(argv[0]);
    if (argc > 1) dma_cap = (seL4_CPtr)parse_num(argv[1]);
    if (argc > 2) dma_paddr = (uint64_t)parse_num(argv[2]);
    unsigned long vio_offset = 0;
    if (argc > 3) vio_offset = (unsigned long)parse_num(argv[3]);

    printf("[blk_test] vio_cap=%lu dma_cap=%lu dma_pa=0x%lx\n",
           (unsigned long)vio_cap, (unsigned long)dma_cap,
           (unsigned long)dma_paddr);

    /* Map virtio MMIO registers */
    void *vio_vaddr = (void *)0x10000000UL;
    seL4_Error err = seL4_ARM_Page_Map(vio_cap,
        seL4_CapInitThreadVSpace, (seL4_Word)vio_vaddr,
        seL4_AllRights, seL4_ARM_Default_VMAttributes | seL4_ARM_ExecuteNever);
    if (err) {
        printf("[blk_test] MMIO map failed: %d\n", err);
        return -1;
    }
    vio_base = (volatile uint32_t *)((uintptr_t)vio_vaddr + vio_offset);
    printf("[blk_test] MMIO mapped at %p + offset 0x%lx\n", vio_vaddr, vio_offset);

    /* Map DMA page */
    void *dma_va = (void *)0x10001000UL;
    err = seL4_ARM_Page_Map(dma_cap,
        seL4_CapInitThreadVSpace, (seL4_Word)dma_va,
        seL4_AllRights, seL4_ARM_Default_VMAttributes | seL4_ARM_ExecuteNever);
    if (err) {
        printf("[blk_test] DMA map failed: %d\n", err);
        return -1;
    }
    dma_vaddr = (uint8_t *)dma_va;
    printf("[blk_test] DMA mapped at %p (pa=0x%lx)\n",
           dma_va, (unsigned long)dma_paddr);

    /* Init virtio */
    if (virtio_blk_init() != 0) {
        printf("[blk_test] Init failed\n");
        return -1;
    }

    /* Read sector 0 */
    printf("[blk_test] Reading sector 0...\n");
    uint8_t sector_buf[512];
    if (virtio_blk_read(0, sector_buf) != 0) {
        printf("[blk_test] Read failed\n");
        return -1;
    }

    /* Print first 64 bytes as hex */
    printf("[blk_test] Sector 0 (first 64 bytes):\n  ");
    for (int i = 0; i < 64; i++) {
        static const char hex[] = "0123456789abcdef";
        printf("%c%c ", hex[sector_buf[i] >> 4], hex[sector_buf[i] & 0xf]);
        if ((i & 15) == 15) printf("\n  ");
    }

    /* Check for ext2 superblock at sector 2 (offset 1024) */
    printf("[blk_test] Reading sector 2 (ext2 superblock)...\n");
    if (virtio_blk_read(2, sector_buf) == 0) {
        uint16_t magic = sector_buf[0x38] | (sector_buf[0x39] << 8);
        printf("[blk_test] ext2 magic: 0x%04x %s\n",
               magic, magic == 0xEF53 ? "(VALID!)" : "(not ext2)");
    }

    printf("[blk_test] Done.\n");
    return 0;
}
