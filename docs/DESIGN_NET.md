# AIOS Networking Design (virtio-net)

## Executive Summary

Add networking to AIOS via a virtio-net MMIO driver and custom IP stack using
a two-thread architecture: a dedicated **net_driver** thread for hardware I/O
and a **net_server** thread for protocol processing and socket IPC. The driver
is IRQ-driven via seL4 notification binding, communicates with the server
through a lock-free shared ring buffer, and achieves zero-copy RX where
possible. The server exposes a POSIX socket API to user programs via IPC
labels 90-109. First milestone: ICMP ping response.

## Why Networking

| Use case | Requires | Status |
|---|---|---|
| ICMP ping (reachability test) | virtio-net + ARP + IPv4 + ICMP | Target M2 |
| UDP datagram send/recv | + socket API | Target M3 |
| TCP echo / HTTP status page | + TCP state machine | Target M4 |
| wget / curl from inside AIOS | + DNS (stub) + TCP client | Future |
| SSH / remote login | + PTY + TCP server | Future |
| NFS / network filesystem | + UDP/TCP + NFS client | Future |

## Current State: virtio-blk Pattern (Template)

The block device is the reference implementation for virtio MMIO on AIOS:

```
boot_fs_init.c                blk_io.c
  Map 4 pages at 0xa000000      blk_read_sector()
  Scan 32 slots for magic+ID    blk_write_sector()
  Alloc 16KB DMA (untyped)       3-descriptor chain
  Legacy init sequence           avail/used rings
  Store blk_vio, blk_dma         dmb sy barriers
```

The networking driver follows this same MMIO/DMA pattern but with two
virtqueues (RX=0, TX=1), larger DMA (128KB), and a multi-thread design.

## Architecture: Two-Thread Model

```
    User Programs (socktest, netstat, wget)
        |  socket() / sendto() / recvfrom()
        v
    POSIX Shim (posix_net.c)
        |  seL4_Call(net_ep, NET_SENDTO / NET_RECVFROM)
        v
    net_server thread                          net_driver thread
    ==================                         ==================
    seL4_Recv(net_ep, &badge)                  seL4_Wait(drv_ntfn)
    bound notification: srv_ntfn               IRQ fires -> wake
        |                                          |
    if badge == ntfn:                          drain RX used ring
      process rx_ring packets                  copy to rx_ring
      through protocol stack                   replenish RX descs
    if badge == ipc:                           seL4_Signal(srv_ntfn)
      handle socket API                        seL4_IRQHandler_Ack()
      (socket/bind/send/recv)                  loop
        |
    TX path: build frame
      write TX desc, kick device
      (server owns TX queue directly)
```

### Why Two Threads

| Concern | Single-thread problem | Two-thread solution |
|---|---|---|
| RX latency | IPC handler blocks RX processing | Driver drains RX independently |
| TX blocking | net_send polls used ring, stalls IPC | Server writes TX desc, immediate return |
| Packet loss | While handling IPC, RX buffers fill | Driver continuously drains into ring |
| Separation | One 700+ LOC monolith | Driver ~200 LOC, server ~400 LOC |
| Future IRQ | Polling baked into main loop | Clean IRQ binding, no main loop changes |

### Thread Responsibilities

**net_driver thread** (RX-only hardware interface):
- Owns the RX virtqueue exclusively
- Waits for IRQ notification (or polling signal in Phase 1)
- Drains RX used ring: copies packet data into rx_ring
- Replenishes RX descriptors with fresh DMA buffer pointers
- Signals srv_ntfn to wake net_server
- Acks the IRQ handler

**net_server thread** (protocol + IPC + TX):
- Bound notification (srv_ntfn) wakes it for RX delivery
- seL4_Recv on net_ep: handles both notification and IPC
- Processes rx_ring packets through Ethernet/ARP/IPv4/ICMP/UDP/TCP
- Manages socket table (8 sockets)
- Handles user IPC: NET_SOCKET, NET_BIND, NET_SENDTO, NET_RECVFROM, etc.
- Owns the TX virtqueue: builds frames, writes TX descriptors, kicks device
- Blocked-reader support: SaveCaller for recvfrom on empty socket

### Inter-Thread Communication

Both threads share the root VSpace. Communication uses a lock-free
single-producer single-consumer ring buffer:

```c
#define NET_RX_RING_SIZE  32
#define NET_RX_PKT_MAX    1518   /* max Ethernet frame */

struct rx_pkt_entry {
    uint16_t len;
    uint8_t  data[NET_RX_PKT_MAX];
};

struct net_rx_ring {
    volatile uint32_t head;      /* driver writes (produces) */
    volatile uint32_t tail;      /* server reads (consumes) */
    struct rx_pkt_entry pkts[NET_RX_RING_SIZE];
};
```

**Producer (driver):**
```
entry = &ring->pkts[ring->head % NET_RX_RING_SIZE]
memcpy(entry->data, dma_buf + VIRTIO_NET_HDR_SIZE, frame_len)
entry->len = frame_len
dmb sy
ring->head++
```

**Consumer (server):**
```
while (ring->tail != ring->head):
    entry = &ring->pkts[ring->tail % NET_RX_RING_SIZE]
    handle_packet(entry->data, entry->len)
    ring->tail++
```

No locks needed: single writer (driver), single reader (server), with
memory barriers between write and index update. The ring is sized at 32
entries (~48KB) to absorb bursts while the server processes IPC.

## seL4 Notification and IRQ Binding

### Phase 1: Polling (initial implementation)

The main loop in aios_root.c checks the virtio interrupt status register
and signals the driver notification:

```c
/* In main() loop, after UART poll: */
if (net_available) {
    uint32_t isr = net_vio[VIRTIO_MMIO_INTERRUPT_STATUS / 4];
    if (isr) {
        net_vio[VIRTIO_MMIO_INTERRUPT_ACK / 4] = isr;
        seL4_Signal(drv_ntfn_cap);
    }
}
```

The driver thread uses `seL4_Wait(drv_ntfn, &badge)` and wakes on the
signal. This is functionally identical to IRQ-driven but adds main-loop
coupling. Good enough for M1-M2.

### Phase 2: IRQ-Driven (target)

```c
/* Boot setup: */
vka_object_t drv_ntfn_obj;
vka_alloc_notification(&vka, &drv_ntfn_obj);
seL4_CPtr drv_ntfn = drv_ntfn_obj.cptr;

/* Get IRQ handler for virtio-net device */
seL4_CPtr irq_handler;
vka_cspace_alloc(&vka, &irq_handler);
/* QEMU arm-virt: virtio MMIO IRQs are SPI 48 + slot_index */
int irq_num = 48 + net_vio_slot;
seL4_IRQControl_Get(simple_get_IRQ_ctrl(&simple), irq_num,
    seL4_CapInitThreadCNode, irq_handler, seL4_WordBits);

/* Bind IRQ to driver notification */
seL4_IRQHandler_SetNotification(irq_handler, drv_ntfn);

/* Driver thread loop: */
while (1) {
    seL4_Word badge;
    seL4_Wait(drv_ntfn, &badge);

    /* Drain RX */
    while (rx_used->idx != rx_last_used) {
        /* copy packet to rx_ring */
        /* replenish RX descriptor */
    }
    seL4_Signal(srv_ntfn);
    seL4_IRQHandler_Ack(irq_handler);
}
```

No main-loop polling needed. The driver wakes only when the device has
packets. This removes the tight-loop interrupt-status check.

### Server Notification Binding

```c
/* Boot setup: */
vka_object_t srv_ntfn_obj;
vka_alloc_notification(&vka, &srv_ntfn_obj);
seL4_CPtr srv_ntfn = srv_ntfn_obj.cptr;

/* Bind to net_server TCB */
seL4_TCB_BindNotification(net_server_tcb, srv_ntfn);

/* Server thread loop: */
while (1) {
    seL4_Word badge;
    seL4_MessageInfo_t msg = seL4_Recv(net_ep, &badge);

    if (badge == srv_ntfn_badge) {
        /* Notification: packets in rx_ring */
        while (rx_ring.tail != rx_ring.head) {
            handle_packet(rx_ring.pkts[rx_ring.tail % SIZE].data, ...);
            rx_ring.tail++;
        }
        continue; /* no reply for notifications */
    }

    /* IPC: handle socket API */
    seL4_Word label = seL4_MessageInfo_get_label(msg);
    switch (label) { ... }
    seL4_Reply(reply);
}
```

seL4_Recv with a bound notification wakes on either:
- An IPC message on net_ep (badge from sender's badged cap)
- A signal on srv_ntfn (badge = notification word)

The server distinguishes them by badge value.

## File Decomposition

### New Files

| File | Purpose | LOC |
|------|---------|-----|
| `include/aios/net.h` | Constants, protocol structs, rx_ring, socket types | ~150 |
| `src/boot/boot_net_init.c` | MMIO init, DMA alloc, IRQ setup, device bringup | ~220 |
| `src/boot/net_io.c` | net_send, net_recv, rx_replenish, TX kick | ~180 |
| `src/servers/net_driver.c` | Driver thread: IRQ wait, RX drain, rx_ring produce | ~200 |
| `src/servers/net_server.c` | Server thread: IPC loop, socket table, RX dispatch | ~400 |
| `src/net/net_stack.c` | ETH/ARP/IPv4/ICMP/UDP handlers, checksums | ~350 |
| `src/net/net_tcp.c` | TCP state machine (Milestone 4) | ~350 |
| `src/lib/posix_net.c` | Client-side socket/bind/sendto/recvfrom/SHM path | ~300 |

### Modified Files

| File | Change |
|------|--------|
| `include/aios/root_shared.h` | NET_* labels (90-109), net globals, thread declarations |
| `include/virtio.h` | VIRTIO_NET_DEVICE_ID, virtio_net_hdr, feature bits |
| `include/aios/vka_audit.h` | VKA_SUB_NET enum value |
| `src/boot/boot_fs_init.c` | Export vio_vaddr, scan for net device |
| `src/boot/boot_services.c` | Allocate endpoints + notifications, start both threads |
| `src/aios_root.c` | Call boot_net_init(); Phase 1: poll in main loop |
| `src/lib/posix_internal.h` | net_ep extern, is_socket/socket_id in fd table, net SHM |
| `src/lib/aios_posix.c` | Wire socket syscalls, extract net_ep from argv |
| `src/servers/exec_server.c` | Pass net_ep cap to child processes |
| `projects/aios/CMakeLists.txt` | Add new source files |

## IPC Protocol (Labels 90-109)

```
Label  Name           Request MRs                              Reply MRs
-----  ----           -----------                              ---------
 90    NET_SOCKET     MR0=domain MR1=type MR2=proto            MR0=sockfd (0-7 or -1)
 91    NET_BIND       MR0=sockfd MR1=port MR2=ip_addr          MR0=0 or -1
 92    NET_LISTEN     MR0=sockfd MR1=backlog                   MR0=0 or -1
 93    NET_ACCEPT     MR0=sockfd                               MR0=new_fd MR1=ip MR2=port
 94    NET_CONNECT    MR0=sockfd MR1=ip MR2=port               MR0=0 or -1
 95    NET_SENDTO     MR0=sockfd MR1=len MR2=dst_ip            MR0=bytes_sent
                      MR3=dst_port MR4..N=data (MR path)
 96    NET_RECVFROM   MR0=sockfd MR1=max_len                   MR0=len MR1=src_ip MR2=src_port
                                                               MR3..N=data (MR path)
 97    NET_CLOSE      MR0=sockfd                               MR0=0 or -1
 98    NET_GETINFO    MR0=which (0=MAC 1=IP 2=stats)           MR0..N=data
 99    NET_SETSOCKOPT MR0=sockfd MR1=level MR2=optname MR3=val MR0=0 or -1
100    NET_SENDTO_SHM MR0=sockfd MR1=len MR2=dst_ip            MR0=bytes_sent
                      MR3=dst_port (data in SHM page)
101    NET_RECVFROM_SHM MR0=sockfd MR1=max_len                 MR0=len MR1=src_ip MR2=src_port
                                                               (data in SHM page)
102    NET_MAP_SHM    MR0=sockfd                                MR0=0 or -1
```

### Data Transfer Paths

**MR path (labels 95-96):** Data packed 8 bytes per MR, up to ~900 bytes
per IPC. Suitable for small UDP datagrams, DNS queries, HTTP headers.

**SHM path (labels 100-102):** For payloads >900 bytes. Client maps a
shared page (NET_MAP_SHM), writes data to it, sends IPC with offset+length.
Server reads directly from shared page. Avoids MR packing overhead.
Follows the same pattern as PIPE_MAP_SHM/PIPE_WRITE_SHM/PIPE_READ_SHM.

The SHM page is allocated lazily on first NET_MAP_SHM request (one page
per socket, max 8 pages = 32KB). The page table leak fix from SHM pipes
(Priority 1) applies here too -- wait for that fix before enabling
client-side mapping.

IP addresses encoded as single 32-bit word: `(a<<24)|(b<<16)|(c<<8)|d`.

## DMA Layout (128KB, size-17 untyped, 32 frames)

```
Offset       Size    Content
-----------  ------  --------------------------
0x00000      0x100   RX descriptors (16 entries x 16 bytes)
0x00100      0x040   RX avail ring (flags + idx + 16 ring entries)
0x01000      0x100   RX used ring (4K-aligned for legacy virtio)
0x02000      0x100   TX descriptors (16 entries)
0x02100      0x040   TX avail ring
0x03000      0x100   TX used ring
0x04000      0x8000  RX packet buffers (16 x 2048 = 32KB)
0x0C000      0x8000  TX packet buffers (16 x 2048 = 32KB)
0x14000      0xC000  Reserved (future: more buffers, jumbo frames)
```

Queue size = 16 (matching VIRTQ_SIZE in virtio.h).
VKA cost: 1 untyped(17) + 32 CSlots + 32 frames.

Why 128KB/16 buffers:
- 16 RX buffers absorbs burst traffic while server processes IPC
- 16 TX buffers allows batched sends (TCP segments, ARP + data)
- Matches existing VIRTQ_SIZE constant in virtio.h
- Cost: 32 pages from ~3994 available (<1% of pool)

## Performance Optimizations

### P1: Zero-Copy RX (Phase 2)

Initial implementation copies from DMA to rx_ring (safe, simple).
Phase 2 optimization: pass DMA buffer index through rx_ring instead of
copying data. Server processes packet directly from DMA memory. After
processing, returns buffer index to driver for replenishment.

```c
/* Zero-copy rx_ring entry: */
struct rx_entry {
    uint16_t dma_idx;     /* index into DMA RX buffer pool */
    uint16_t len;         /* frame length (excluding virtio header) */
};
```

Tradeoff: server must finish processing before buffer can be replenished.
With 16 buffers this is safe unless the server is stalled for >16 packets.

### P2: TX Batching

When the server builds multiple TX frames (e.g., ARP reply + ICMP reply
from same RX burst), accumulate TX descriptors without kicking the device:

```c
/* Queue TX descriptor without kick: */
net_tx_enqueue(frame, len);
net_tx_enqueue(frame2, len2);
/* Single kick for all queued: */
net_tx_flush();   /* writes QUEUE_NOTIFY once */
```

Reduces MMIO write overhead from N writes to 1.

### P3: Checksum Offload

virtio-net supports TX checksum offload via VIRTIO_NET_F_CSUM (bit 0).
When negotiated, the driver sets csum_start and csum_offset in the
virtio_net_hdr and the device computes the checksum. This skips
software ip_checksum() and tcp_checksum() on the TX path.

```c
/* Feature negotiation in boot_net_init: */
uint32_t host_features = VIO_R(VIRTIO_MMIO_HOST_FEATURES);
uint32_t drv_features = 0;
if (host_features & VIRTIO_NET_F_CSUM)
    drv_features |= VIRTIO_NET_F_CSUM;
if (host_features & VIRTIO_NET_F_MAC)
    drv_features |= VIRTIO_NET_F_MAC;
VIO_W(VIRTIO_MMIO_DRV_FEATURES, drv_features);
```

Feature bits (include/aios/net.h):
```
VIRTIO_NET_F_CSUM       0    /* TX checksum offload */
VIRTIO_NET_F_MAC        5    /* Device has MAC in config */
VIRTIO_NET_F_STATUS    16    /* Link status in config */
VIRTIO_NET_F_MRG_RXBUF 15    /* Mergeable RX buffers */
```

### P4: Blocked Receiver (SaveCaller pattern)

When recvfrom() finds an empty socket rxbuf, use the SaveCaller pattern
(same as pipe_server blocked reader) to avoid busy-wait:

```c
/* In net_server, NET_RECVFROM handler: */
if (sock->rxlen == 0) {
    /* Save client reply cap */
    seL4_CNode_SaveCaller(cnode, blocked_cap_slot, seL4_WordBits);
    sock->blocked_reader_cap = blocked_cap_slot;
    sock->has_blocked_reader = 1;
    continue;  /* no reply -- client blocks */
}

/* Later, when packet arrives for this socket: */
if (sock->has_blocked_reader) {
    /* Wake blocked reader with data */
    pack_data_into_mrs(sock->rxbuf, sock->rxlen);
    seL4_Send(sock->blocked_reader_cap, reply_info);
    sock->has_blocked_reader = 0;
    sock->rxlen = 0;
}
```

This eliminates polling loops in recvfrom(). The client blocks in the
kernel until data arrives, consuming zero CPU.

### P5: Scatter-Gather TX

Use multi-descriptor chains on TX to avoid copying header + payload
into a single contiguous buffer:

```c
/* Descriptor 0: virtio_net_hdr (10 bytes, in scratch area) */
tx_desc[idx].addr = net_dma_pa + HDR_SCRATCH_OFF;
tx_desc[idx].len  = VIRTIO_NET_HDR_SIZE;
tx_desc[idx].flags = VIRTQ_DESC_F_NEXT;
tx_desc[idx].next = idx + 1;

/* Descriptor 1: Ethernet + IP + payload (in TX buffer) */
tx_desc[idx+1].addr = net_dma_pa + TX_BUF_OFF + slot * 2048;
tx_desc[idx+1].len  = frame_len;
tx_desc[idx+1].flags = 0;
```

Saves one memcpy per TX packet (10-byte header no longer prepended).

### P6: Per-Socket Receive Ring

Replace the single 2KB rxbuf per socket with a ring buffer to handle
burst traffic without dropping:

```c
#define SOCK_RX_RING_SIZE   4
#define SOCK_RX_BUF_SIZE    2048

struct net_socket {
    ...
    struct {
        uint8_t  data[SOCK_RX_BUF_SIZE];
        uint16_t len;
    } rxring[SOCK_RX_RING_SIZE];
    uint8_t  rx_head;    /* stack writes (produces) */
    uint8_t  rx_tail;    /* recvfrom reads (consumes) */
};
```

4 entries per socket allows the protocol stack to buffer up to 4 packets
while the user program is processing the previous one. Total memory per
socket: ~8KB (4 x 2048). With 8 sockets: 64KB.

### P7: Statistics Counters

Track performance metrics for debugging and /proc/net:

```c
struct net_stats {
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_dropped;    /* rx_ring full */
    uint32_t tx_dropped;    /* TX queue full */
    uint32_t arp_requests;
    uint32_t arp_replies;
    uint32_t icmp_echo;
    uint32_t tcp_conns;
    uint32_t udp_datagrams;
};
```

Exposed via NET_GETINFO (MR0=2) and /proc/net pseudo-file.

## Device Discovery and Init

### Phase 1: Detection (in boot_fs_init.c)

The existing MMIO probe loop scans 32 device slots at 0xa000000.
Currently it stops after finding VIRTIO_BLK_DEVICE_ID (2). Extend to
also record VIRTIO_NET_DEVICE_ID (1):

```c
/* In boot_fs_init.c MMIO scan loop: */
int net_slot = -1;
for (int i = 0; i < VIRTIO_NUM_SLOTS; i++) {
    volatile uint32_t *slot = vio_vaddr + i * VIRTIO_SLOT_SIZE;
    if (slot[0] != VIRTIO_MAGIC) continue;
    uint32_t devid = slot[VIRTIO_MMIO_DEVICE_ID/4];
    if (devid == VIRTIO_BLK_DEVICE_ID && blk_slot < 0) blk_slot = i;
    if (devid == VIRTIO_NET_DEVICE_ID && net_slot < 0) net_slot = i;
}
/* Store for boot_net_init: */
net_vio_slot = net_slot;
net_available = (net_slot >= 0);
```

The vio_vaddr pointer is promoted from local to global so boot_net_init
can use it without remapping.

### Phase 2: Init (in boot_net_init.c)

Called from aios_root.c after boot_fs_init(), before boot_start_services().

```
boot_net_init():
  if (!net_available) return

  1. Calculate net_vio pointer from vio_vaddr + net_vio_slot * 0x200

  2. Allocate DMA (128KB, size-17 untyped):
     vka_alloc_untyped(&vka, 17, &net_dma_ut)
     for i in 0..31:
       vka_cspace_alloc(&vka, &slot)
       seL4_Untyped_Retype(net_dma_ut, SmallPage, 12, ..., slot, 1)
     vspace_map_pages(&vspace, caps, NULL, AllRights, 32, PageBits, 0)
     seL4_ARM_Page_GetAddress(caps[0]) -> net_dma_pa

  3. Zero DMA region (128KB)

  4. Legacy virtio init with feature negotiation:
     STATUS = 0                             // reset
     STATUS = ACK
     STATUS = ACK | DRIVER
     host_features = Read HOST_FEATURES
     drv_features = negotiate(host_features) // CSUM, MAC, STATUS
     DRV_FEATURES = drv_features
     GUEST_PAGE_SIZE = 4096

     QUEUE_SEL = 0                          // RX queue
     QUEUE_NUM = 16
     QUEUE_PFN = (net_dma_pa + 0x00000) / 4096

     QUEUE_SEL = 1                          // TX queue
     QUEUE_NUM = 16
     QUEUE_PFN = (net_dma_pa + 0x02000) / 4096

     STATUS = ACK | DRIVER | DRIVER_OK

  5. Replenish all 16 RX buffers:
     for i in 0..15:
       rx_desc[i].addr = net_dma_pa + 0x4000 + i*2048
       rx_desc[i].len  = 2048
       rx_desc[i].flags = VIRTQ_DESC_F_WRITE
       rx_avail->ring[i] = i
     rx_avail->idx = 16
     dmb sy
     QUEUE_NOTIFY = 0                       // kick RX queue

  6. Read MAC from config space:
     for i in 0..5:
       net_mac[i] = *(uint8_t*)(net_vio_base + VIRTIO_MMIO_CONFIG + i)

  7. Allocate notification objects:
     vka_alloc_notification(&vka, &drv_ntfn_obj)   // driver wakeup
     vka_alloc_notification(&vka, &srv_ntfn_obj)   // server wakeup

  8. Phase 2: Setup IRQ binding:
     irq_num = 48 + net_vio_slot  // QEMU arm-virt SPI mapping
     seL4_IRQControl_Get(irq_ctrl, irq_num, cnode, slot, bits)
     seL4_IRQHandler_SetNotification(irq_handler, drv_ntfn)

  9. Print: [boot] virtio-net ready, MAC=xx:xx:xx:xx:xx:xx
```

### Phase 3: Thread Startup (in boot_services.c)

```c
/* Allocate net IPC endpoint */
vka_object_t net_ep_obj;
vka_alloc_endpoint(&vka, &net_ep_obj);
net_ep_cap = net_ep_obj.cptr;

/* Start driver thread */
sel4utils_thread_t drv_thread;
sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
    simple_get_cnode(&simple), seL4_NilData, &drv_thread);
seL4_TCB_SetPriority(drv_thread.tcb.cptr, simple_get_tcb(&simple), 200);
/* No notification binding for driver -- it uses seL4_Wait directly */
sel4utils_start_thread(&drv_thread, net_driver_fn,
    (void*)(uintptr_t)drv_ntfn_cap, NULL, 1);

/* Start server thread */
sel4utils_thread_t srv_thread;
sel4utils_configure_thread(&vka, &vspace, &vspace, 0,
    simple_get_cnode(&simple), seL4_NilData, &srv_thread);
seL4_TCB_SetPriority(srv_thread.tcb.cptr, simple_get_tcb(&simple), 200);
seL4_TCB_BindNotification(srv_thread.tcb.cptr, srv_ntfn_cap);
sel4utils_start_thread(&srv_thread, net_server_fn,
    (void*)(uintptr_t)net_ep_cap, NULL, 1);
```

## Network Stack

Custom stack (no lwIP). Ported from the working v0.3.x implementation
(ref/v03x/src/net_server.c) with Microkit APIs replaced by direct calls.

### Ethernet

```c
struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;        /* ETH_ARP=0x0806, ETH_IP=0x0800 */
} __attribute__((packed));
```

Packed structs used for protocol headers. On AArch64, unaligned access
works (may trap to EL1 on some implementations). Packets are naturally
aligned in DMA buffers (each starts at 2048-byte boundary). The ext2
code avoids packed structs because on-disk structures have different
alignment constraints.

### virtio-net Header

Every packet sent/received through virtio-net has a 10-byte header:

```c
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;       /* 0 = no GSO */
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;     /* TX csum offload: offset to start */
    uint16_t csum_offset;    /* TX csum offload: offset to write */
} __attribute__((packed));
```

RX: skip first 10 bytes to get Ethernet frame.
TX without offload: prepend 10 zero bytes.
TX with CSUM offload: set csum_start/offset, device computes checksum.

### ARP

Static ARP cache (16 entries). Responds to ARP requests for our IP.
Sends ARP requests when sending to unknown MACs. Cache entries have
a valid flag; no timeout (static network, QEMU user-mode).

### IPv4

Parse: verify version/IHL, check dst matches my_ip, dispatch by protocol.
Build: construct header with checksum, prepend Ethernet header.
No fragmentation support (MTU=1500 assumed, QEMU respects this).

### ICMP

Echo reply only: receive type=8 (echo request), reply with type=0
(echo reply) using same ID, sequence, and payload.

### UDP (Milestone 3)

Match received datagrams to socket table by destination port.
Deliver to socket rx_ring. Wake blocked reader if present.
Send: build UDP header (8 bytes) + IP header + Ethernet header.

### TCP (Milestone 4)

Minimal state machine: CLOSED -> LISTEN -> SYN_RCVD -> ESTABLISHED ->
FIN_WAIT -> CLOSED. One connection per socket. Port from v0.3.x
handle_tcp(). No retransmit timer (QEMU local link is reliable).
No window scaling. Window size = 8192 (matches socket rxring capacity).

### Checksum

```c
static uint16_t ip_checksum(const void *data, int len) {
    const uint16_t *p = data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}
```

TCP/UDP: pseudo-header checksum over src_ip, dst_ip, proto, length.
When CSUM offload negotiated, software checksum skipped on TX.

## Socket Table

```c
#define MAX_NET_SOCKETS  8
#define SOCK_RX_RING_SZ  4
#define SOCK_RX_BUF_SZ   2048

struct net_socket {
    int      in_use;
    int      type;               /* SOCK_STREAM=1, SOCK_DGRAM=2 */
    int      state;              /* TCP state or 0 for UDP */
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t  remote_ip[4];
    uint8_t  remote_mac[6];

    /* Per-socket receive ring (4 entries x 2KB) */
    struct {
        uint8_t  data[SOCK_RX_BUF_SZ];
        uint16_t len;
    } rxring[SOCK_RX_RING_SZ];
    uint8_t  rx_head;            /* stack writes */
    uint8_t  rx_tail;            /* recvfrom reads */

    /* TCP sequence state */
    uint32_t snd_nxt;
    uint32_t rcv_nxt;

    /* Blocked reader (SaveCaller) */
    int       has_blocked_reader;
    seL4_CPtr blocked_reader_cap;
    int       blocked_max_len;

    /* SHM data page (lazy alloc) */
    int             shm_valid;
    vka_object_t    shm_frame;
    char           *shm_buf;
};

static struct net_socket net_sockets[MAX_NET_SOCKETS];
```

Socket IDs (0-7) returned to user programs, mapped into fd table as
is_socket=1, socket_id=N.

## Network Configuration

Static configuration for QEMU user-mode networking:

```
IP address:   10.0.2.15       (QEMU user-mode guest default)
Gateway:      10.0.2.2        (QEMU user-mode gateway)
Netmask:      255.255.255.0
DNS:          10.0.2.3        (QEMU user-mode DNS, future)
```

MAC address read from virtio config space at init.
Future: DHCP client, configurable via /etc/network.conf.

## POSIX Integration

### fd Table Extension

```c
/* In posix_internal.h, aios_fd_t: */
int is_socket;       /* 1 if this fd is a network socket */
int socket_id;       /* index into net_server socket table (0-7) */
void *net_shm;       /* SHM page vaddr (NULL until NET_MAP_SHM) */
```

### Endpoint Propagation

net_ep capability is passed to child processes in the same argv format
as pipe_ep, fs_ep, etc. In exec_server.c, net_ep is copied into the
child CSpace and its slot number is appended to the argv string.

argv format becomes:
```
serial_ep fs_ep thread_ep auth_ep pipe_ep net_ep CWD progname [args...]
```

The POSIX shim __wrap_main extracts net_ep at position 5 (after pipe_ep).

### Syscall Mapping (AArch64 musl)

```
__NR_socket     198
__NR_bind       200
__NR_listen     201
__NR_accept     202     (accept4 on aarch64)
__NR_connect    203
__NR_sendto     206
__NR_recvfrom   207
__NR_setsockopt 208
__NR_getsockopt 209
__NR_shutdown   210
```

### Automatic Path Selection

sendto/recvfrom automatically choose MR or SHM path based on payload size:

```c
int aios_sys_sendto(...) {
    if (len <= 900 && !fd->net_shm) {
        /* MR path: pack into message registers */
        return sendto_mr(sockfd, buf, len, dst_ip, dst_port);
    }
    /* SHM path: copy to shared page, send offset+length */
    if (!fd->net_shm) net_map_shm(sockfd);
    memcpy(fd->net_shm, buf, len);
    return sendto_shm(sockfd, len, dst_ip, dst_port);
}
```

## Milestones

### M1: Device Detection + Multi-Thread Skeleton

Files: net.h, boot_net_init.c, net_driver.c (stub), net_server.c (stub),
       virtio.h, root_shared.h, vka_audit.h, boot_fs_init.c,
       boot_services.c, aios_root.c, CMakeLists.txt

Deliverables:
- virtio-net detected and initialized with 128KB DMA
- Driver thread running, waiting on notification
- Server thread running, waiting on endpoint
- Notification objects allocated and bound
- Phase 1 polling in main loop signals driver

Test: Boot with/without `-device virtio-net-device`. Both clean.
Expected output: `[boot] virtio-net ready, MAC=52:54:00:xx:xx:xx`

### M2: Ping Response

Files: net_io.c, net_stack.c, net_driver.c (RX drain), net_server.c (RX dispatch)

Deliverables:
- Driver drains RX used ring into rx_ring
- Server processes rx_ring: ETH demux, ARP reply, ICMP echo reply
- TX path: server builds frame, writes TX descriptor, kicks device
- Statistics counters

Test: `ping 10.0.2.15` from host -- replies received.
QEMU args: `-device virtio-net-device,netdev=net0 -netdev user,id=net0`

### M3: POSIX Sockets + UDP

Files: posix_net.c, posix_internal.h, aios_posix.c, exec_server.c

Deliverables:
- Socket API: socket, bind, sendto, recvfrom, close
- fd table integration (is_socket, socket_id)
- net_ep propagated to child processes
- Blocked reader support (SaveCaller)
- UDP datagram delivery to socket rxring

Test: User program creates UDP socket, sends/receives datagram.

### M4: TCP + HTTP Status Page

Files: net_tcp.c, extended net_server.c

Deliverables:
- TCP state machine (SYN/ACK/FIN)
- connect, listen, accept, send, recv
- HTTP status page (port from v0.3.x)

Test: `curl localhost:8080` via QEMU port forward.
QEMU: `-netdev user,id=net0,hostfwd=tcp::8080-:80`

### M5: SHM Data Path + Performance

Files: extended posix_net.c, extended net_server.c

Deliverables:
- NET_MAP_SHM / NET_SENDTO_SHM / NET_RECVFROM_SHM
- Auto MR/SHM path selection in posix_net.c
- TX batching (net_tx_enqueue + net_tx_flush)
- /proc/net stats

Test: Large file transfer via TCP, throughput measurement.

### M6: IRQ-Driven RX (Phase 2)

Files: boot_net_init.c (IRQ setup), net_driver.c (remove poll dependency)

Deliverables:
- IRQ handler cap for virtio-net
- IRQ bound to driver notification
- Remove polling from main loop
- IRQ ack in driver loop

Test: Same as M2 but without main-loop polling. Verify with printk.

### M7: Test Programs

Files: net_test.c, netstat.c, wget.c (basic)

Test: Programs run inside AIOS shell.

## Design Decisions

### D1: Two-thread model (driver + server)

Separating hardware I/O from protocol processing prevents IPC handling
from stalling RX drain. The driver continuously absorbs packets into
the rx_ring regardless of server load. The two threads communicate via
a lock-free ring buffer (48KB) in shared root VSpace memory.

The driver thread adds ~200 LOC, one notification object, and one TCB.
This is modest overhead for clean separation and future IRQ support.

### D2: Notification-bound IPC (not polling)

seL4_TCB_BindNotification + seL4_Recv gives the server thread a single
wait point for both IPC requests and RX notifications. No busy-wait,
no polling loop, no wasted CPU cycles. The driver signals the server
notification after placing packets in rx_ring.

Phase 1 uses main-loop polling to signal the driver notification as a
stepping stone. Phase 2 (M6) replaces this with IRQ binding.

### D3: 128KB DMA with 16 buffers/queue

16 buffers provides headroom for burst traffic and matches the existing
VIRTQ_SIZE constant in virtio.h. The 128KB untyped allocation is a
single VKA call. Cost: 32 pages from ~3994 available (<1% of pool).

### D4: Custom IP stack (not lwIP)

The v0.3.x custom stack handles ARP/IPv4/ICMP/TCP in ~600 LOC. lwIP
adds ~100KB code + ~50KB RAM with complex seL4 integration. For a
research OS, understanding every packet path matters. The protocol
parsing code (structs, checksums, handlers) ports directly from v0.3.x.

### D5: SaveCaller blocked readers (not polling recv)

recvfrom() on empty socket uses the SaveCaller pattern from pipe_server.
The client blocks in the kernel until data arrives, consuming zero CPU.
The server wakes the blocked reader with seL4_Send on the saved reply
cap when a matching packet arrives. This matches the established AIOS
pattern for blocking I/O.

### D6: SHM data path (large payloads)

MR packing is limited to ~900 bytes per IPC call. For TCP bulk transfer,
file downloads, or HTTP response bodies, the SHM path avoids re-packing
overhead. Each socket can have one lazy-allocated shared page (4KB).
This reuses the PIPE_MAP_SHM infrastructure and shares the page table
leak fix dependency.

### D7: Feature negotiation (CSUM offload)

Negotiate VIRTIO_NET_F_CSUM when available. This moves TX checksum
computation to the hypervisor, saving CPU cycles on every outgoing
packet. Software checksum remains as fallback when offload is not
available or for RX path verification.

### D8: Packed structs for protocol headers

Network protocol headers (eth_hdr, ip_hdr, tcp_hdr) use
__attribute__((packed)). On AArch64, unaligned access works but may
trap. Packets are naturally aligned in DMA buffers (2048-byte boundary).
The performance impact is negligible compared to IPC and DMA overhead.

## QEMU Command

```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G \
    -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -device virtio-net-device,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::8080-:80 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

Without networking (must still boot cleanly):

```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 -smp 4 -m 2G \
    -nographic -serial mon:stdio \
    -drive file=disk/disk_ext2.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -kernel build-04/images/aios_root-image-arm-qemu-arm-virt
```

## Dependencies and Ordering

Networking is independent of the other v0.4.67 priorities:

- SHM pipes (P1): touches pipe_server.c, exec_server.c -- net touches
  exec_server.c at a different code point (net_ep cap). Merge-safe.
  SHM page table leak fix benefits NET_MAP_SHM too.
- Dash interactive (P2): touches mini_shell.c, posix_file.c -- no overlap.
- Allocator (P3): changes pool size in aios_root.c -- net adds DMA.
  Different locations. Merge-safe.

M1-M2 can proceed in parallel with P1-P3. M3 (POSIX sockets) coordinates
with P1 at exec_server.c. M5 (SHM data path) depends on P1 page table fix.

## Parallel Development Notes

Reserve IPC label ranges in root_shared.h before branching:

```
/* SER:     1-9    */
/* FS:      10-19  */
/* EXEC:    20-29  */
/* THREAD:  30-39  */
/* AUTH:    40-59  */
/* PIPE:    60-89  */
/* NET:     90-109 */
/* (future) 110+   */
```

Each stream adds defines only within its range and appends globals
in marked sections. This prevents merge conflicts on shared headers.
