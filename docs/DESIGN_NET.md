# AIOS Networking Design (virtio-net)

## Executive Summary

Add networking to AIOS via a virtio-net MMIO driver and custom IP stack, following
the established server-thread + seL4 IPC pattern. The driver detects the virtio-net
device in the same MMIO region as virtio-blk, allocates DMA for RX/TX virtqueues,
and exposes packet send/receive to a net_server thread. The net_server handles
ARP, IPv4, ICMP, UDP, and TCP, and provides a POSIX socket API to user programs
via IPC labels 90-99. First milestone: ICMP ping response.

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

The networking driver follows this same pattern but with two queues (RX=0, TX=1)
instead of one, and 64KB DMA instead of 16KB.

## Architecture

```
                User Programs (socktest, netstat, wget)
                    |  socket() / sendto() / recvfrom()
                    v
              POSIX Shim (posix_net.c)
                    |  seL4_Call(net_ep, NET_SENDTO)
                    v
              net_server thread (IPC loop)
                    |  switch(label)
                    |  NET_SOCKET: alloc socket entry
                    |  NET_SENDTO: build_ip_packet + net_send
                    |  NET_RECVFROM: check socket rxbuf
                    |  NET_POLL: rx_poll -> handle_packet
                    v
              net_stack.c (protocol processing)
                    |  Ethernet demux -> ARP / IPv4
                    |  IPv4 -> ICMP / UDP / TCP
                    |  handle_arp: reply to requests
                    |  handle_icmp: echo reply
                    v
              net_io.c (low-level I/O)
                    |  net_send: write TX descriptor, notify device
                    |  net_recv: read RX used ring, copy data
                    |  rx_replenish: refill RX descriptors
                    v
              virtio-net MMIO device (QEMU)
                    |  RX queue 0, TX queue 1
                    |  DMA ring buffers in contiguous memory
```

## File Decomposition

### New Files

| File | Purpose | LOC |
|------|---------|-----|
| `include/aios/net.h` | Constants, protocol structs, socket types | ~120 |
| `src/boot/boot_net_init.c` | MMIO init, DMA alloc, device bringup | ~200 |
| `src/boot/net_io.c` | net_send, net_recv, rx_replenish | ~150 |
| `src/servers/net_server.c` | IPC loop, socket table, packet dispatch | ~350 |
| `src/net/net_stack.c` | ETH/ARP/IPv4/ICMP handlers, checksums | ~350 |
| `src/net/net_tcp.c` | TCP state machine (Milestone 4) | ~350 |
| `src/lib/posix_net.c` | Client-side socket(), sendto(), etc. | ~250 |

### Modified Files

| File | Change |
|------|--------|
| `include/aios/root_shared.h` | NET_* labels (90-99), net globals, declarations |
| `include/virtio.h` | VIRTIO_NET_DEVICE_ID, struct virtio_net_hdr |
| `include/aios/vka_audit.h` | VKA_SUB_NET enum value |
| `src/boot/boot_fs_init.c` | Export vio_vaddr, scan for net device |
| `src/boot/boot_services.c` | Allocate net_ep, start net_server thread |
| `src/aios_root.c` | Call boot_net_init(), RX poll in main loop |
| `src/lib/posix_internal.h` | net_ep extern, is_socket in fd table |
| `src/lib/aios_posix.c` | Wire socket syscalls, extract net_ep from argv |
| `src/servers/exec_server.c` | Pass net_ep cap to child processes |
| `projects/aios/CMakeLists.txt` | Add new source files |

## IPC Protocol (Labels 90-99)

```
Label  Name           Request MRs                              Reply MRs
-----  ----           -----------                              ---------
 90    NET_SOCKET     MR0=domain MR1=type MR2=proto            MR0=sockfd (0-7 or -1)
 91    NET_BIND       MR0=sockfd MR1=port MR2=ip_addr          MR0=0 or -1
 92    NET_LISTEN     MR0=sockfd MR1=backlog                   MR0=0 or -1
 93    NET_ACCEPT     MR0=sockfd                               MR0=new_fd MR1=remote_ip MR2=remote_port
 94    NET_CONNECT    MR0=sockfd MR1=ip MR2=port               MR0=0 or -1
 95    NET_SENDTO     MR0=sockfd MR1=len MR2=dst_ip            MR0=bytes_sent
                      MR3=dst_port MR4..N=data
 96    NET_RECVFROM   MR0=sockfd MR1=max_len                   MR0=len MR1=src_ip MR2=src_port
                                                               MR3..N=data
 97    NET_CLOSE      MR0=sockfd                               MR0=0 or -1
 98    NET_GETINFO    MR0=which (0=MAC 1=IP)                   MR0..N=data
 99    NET_POLL       (none)                                   MR0=pkts_processed
```

Data packing: 8 bytes per MR, same pattern as fs_server/exec_server.

IP addresses encoded as single 32-bit word: `(a<<24)|(b<<16)|(c<<8)|d`.

## DMA Layout (64KB, size-16 untyped)

```
Offset       Size    Content
-----------  ------  --------------------------
0x00000      0x080   RX descriptors (8 entries x 16 bytes)
0x00100      0x020   RX avail ring (flags + idx + 8 ring entries)
0x01000      0x060   RX used ring (4K-aligned for legacy virtio)
0x02000      0x080   TX descriptors (8 entries)
0x02100      0x020   TX avail ring
0x03000      0x060   TX used ring
0x04000      0x4000  RX packet buffers (8 x 2048 = 16KB)
0x08000      0x4000  TX packet buffers (8 x 2048 = 16KB)
0x0C000      0x4000  Scratch: ARP table, temp packet assembly
```

Queue size = 8 (sufficient for QEMU user-mode networking).
VKA cost: 1 untyped(14) + 16 CSlots + 16 frames.

Why 64KB not 128KB: QEMU user-mode generates little traffic. 8 buffers per
queue handles ping, HTTP, and basic TCP with margin. Can increase to 16
(128KB) later if needed.

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

  2. Allocate DMA:
     vka_alloc_untyped(&vka, 16, &net_dma_ut)     // 64KB
     for i in 0..15:
       vka_cspace_alloc(&vka, &slot)
       seL4_Untyped_Retype(net_dma_ut, SmallPage, 12, ..., slot, 1)
     vspace_map_pages(&vspace, caps, NULL, AllRights, 16, PageBits, 0)
     seL4_ARM_Page_GetAddress(caps[0]) -> net_dma_pa

  3. Zero DMA region

  4. Legacy virtio init:
     STATUS = 0                         // reset
     STATUS = ACK
     STATUS = ACK | DRIVER
     Read HOST_FEATURES (ignore for now, features=0)
     GUEST_PAGE_SIZE = 4096
     DRV_FEATURES = 0

     QUEUE_SEL = 0                      // RX queue
     QUEUE_NUM = 8
     QUEUE_PFN = (net_dma_pa + 0x00000) / 4096

     QUEUE_SEL = 1                      // TX queue
     QUEUE_NUM = 8
     QUEUE_PFN = (net_dma_pa + 0x02000) / 4096

     STATUS = ACK | DRIVER | DRIVER_OK

  5. Replenish RX buffers:
     for i in 0..7:
       rx_desc[i].addr = net_dma_pa + 0x4000 + i*2048
       rx_desc[i].len  = 2048
       rx_desc[i].flags = VIRTQ_DESC_F_WRITE
       rx_avail->ring[i] = i
     rx_avail->idx = 8
     dmb sy
     QUEUE_NOTIFY = 0                   // kick RX queue

  6. Read MAC from config space:
     for i in 0..5:
       net_mac[i] = *(uint8_t*)(net_vio_base + VIRTIO_MMIO_CONFIG + i)

  7. Print: [boot] virtio-net ready, MAC=xx:xx:xx:xx:xx:xx
```

## RX Strategy: Polling

AIOS currently has zero IRQ handlers. The main loop in aios_root.c polls
UART for keyboard input. Network RX uses the same polling approach:

```c
/* In main() loop, after UART poll: */
if (net_available) {
    uint32_t isr = net_vio[VIRTIO_MMIO_INTERRUPT_STATUS / 4];
    if (isr) {
        net_vio[VIRTIO_MMIO_INTERRUPT_ACK / 4] = isr;
        seL4_SetMR(0, 0);
        seL4_Call(net_ep_cap, seL4_MessageInfo_new(NET_POLL, 0, 0, 1));
    }
}
```

This is ~5 lines added to the existing main loop. The NET_POLL handler in
net_server processes all pending RX packets, then replies.

Future optimization: bind virtio-net IRQ to a notification object, bind
notification to net_server TCB, use seL4_NBRecv. This eliminates polling
but requires new IRQ handling infrastructure.

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

### ARP

Static ARP cache (16 entries). Responds to ARP requests for our IP.
Sends ARP requests when sending to unknown MACs.

### IPv4

Parse: verify version/IHL, check dst matches my_ip, dispatch by protocol.
Build: construct header with checksum, prepend Ethernet header.

No fragmentation support (MTU=1500 assumed, QEMU user-mode respects this).

### ICMP

Echo reply only: receive type=8, reply with type=0 and same payload.

### UDP (Milestone 3)

Deliver received datagrams to matching socket rxbuf (by local port).
Send datagrams with IP/UDP header construction.

### TCP (Milestone 4)

Minimal state machine: CLOSED -> LISTEN -> SYN_RCVD -> ESTABLISHED -> FIN_WAIT.
Single connection per local port. No retransmit timer (QEMU local link is
reliable). No window scaling. Port from v0.3.x net_server.c handle_tcp().

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

TCP pseudo-header checksum same as v0.3.x.

## Socket Table

```c
#define MAX_NET_SOCKETS 8

struct net_socket {
    int      in_use;
    int      type;            /* SOCK_STREAM=1, SOCK_DGRAM=2 */
    int      state;           /* TCP state or 0 for UDP */
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t  remote_ip[4];
    uint8_t  remote_mac[6];
    uint8_t  rxbuf[2048];     /* receive buffer */
    uint32_t rxlen;
    uint32_t snd_nxt;         /* TCP sequence state */
    uint32_t rcv_nxt;
};

static struct net_socket net_sockets[MAX_NET_SOCKETS];
```

Managed by net_server. Socket IDs (0-7) are returned to user programs and
mapped into the fd table as is_socket=1, socket_id=N.

## Network Configuration

Static configuration for QEMU user-mode networking:

```
IP address:   10.0.2.15       (QEMU user-mode guest default)
Gateway:      10.0.2.2        (QEMU user-mode gateway)
Netmask:      255.255.255.0
DNS:          10.0.2.3        (QEMU user-mode DNS, future)
```

MAC address read from virtio config space at init.

## virtio-net Header

Every packet sent/received through virtio-net has a 10-byte header prepended:

```c
struct virtio_net_hdr {
    uint8_t  flags;           /* 0 for basic operation */
    uint8_t  gso_type;        /* 0 = no GSO */
    uint16_t hdr_len;         /* 0 */
    uint16_t gso_size;        /* 0 */
    uint16_t csum_start;      /* 0 */
    uint16_t csum_offset;     /* 0 */
} __attribute__((packed));
```

For our use case (no checksum offload, no GSO), this is always zeroed.
net_send prepends 10 zero bytes before each frame. net_recv strips 10
bytes from the front of received data.

## POSIX Integration

### fd Table Extension

```c
/* In posix_internal.h, aios_fd_t: */
int is_socket;       /* 1 if this fd is a network socket */
int socket_id;       /* index into net_server socket table (0-7) */
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
__NR_setsockopt 208     (stub: return 0)
__NR_getsockopt 209     (stub: return 0)
```

## Milestones

### M1: Device Detection

Files: net.h, boot_net_init.c, virtio.h, root_shared.h, vka_audit.h,
       boot_fs_init.c, aios_root.c, CMakeLists.txt

Test: Boot with/without `-device virtio-net-device`. Both clean.
Expected output: `[boot] virtio-net ready, MAC=52:54:00:xx:xx:xx`

### M2: Ping Response

Files: net_io.c, net_stack.c, net_server.c, boot_services.c, aios_root.c

Test: From host terminal: `ping 10.0.2.15` -- replies received.
QEMU args: `-device virtio-net-device,netdev=net0 -netdev user,id=net0`

### M3: POSIX Sockets + UDP

Files: posix_net.c, posix_internal.h, aios_posix.c, exec_server.c

Test: User program creates UDP socket, sends datagram.

### M4: TCP + HTTP

Files: net_tcp.c, extended net_server.c

Test: `curl localhost:8080` via QEMU port forward.
QEMU args: `-netdev user,id=net0,hostfwd=tcp::8080-:80`

### M5: Test Programs

Files: net_test.c, netstat.c

Test: Programs run inside AIOS shell.

## Design Decisions

### D1: Single-thread net server (driver + stack combined)

The v0.3.x separated net_driver and net_server as Microkit PDs. In v0.4.x,
both run in the root VSpace. Combining into one thread eliminates shared-memory
notification complexity. The server directly accesses DMA ring buffers and
protocol state. For QEMU user-mode traffic volumes, this is sufficient.

### D2: Polling-based RX (no IRQ handler)

AIOS has zero IRQ handlers today. Adding one requires seL4_IRQ_Get,
seL4_IRQ_SetNotification, and notification-to-TCB binding -- significant
new infrastructure. Polling in the main loop adds ~5 lines and works now.
IRQ-driven RX is a future optimization when traffic increases.

### D3: 64KB DMA (not 128KB)

8 buffers per queue is sufficient for QEMU user-mode. This uses a size-16
untyped (16 frames) vs size-17 (32 frames), halving VKA pool impact.

### D4: Custom IP stack (not lwIP)

The v0.3.x custom stack handles ARP/IPv4/ICMP/TCP in ~600 LOC. lwIP would
add ~100KB code + ~50KB RAM with complex seL4 integration. For a research
OS, understanding every packet path matters more than production features.
The protocol parsing code (structs, checksums, handlers) ports directly.

### D5: Packed structs for protocol headers

Network protocol headers (eth_hdr, ip_hdr, tcp_hdr) use __attribute__((packed)).
On AArch64, unaligned access to packed structs works but may trap to EL1.
For network code this is standard practice and the structs are naturally
aligned in DMA buffers (each packet at a 2048-byte boundary). Using rd16/rd32
byte-by-byte helpers would add ~50% more code for zero practical benefit here.

The ext2 code avoids packed structs because on-disk structures have
different alignment constraints than in-memory packet buffers.

### D6: No DNS (yet)

DNS requires UDP client + query/response parsing. Deferred until after TCP
is working. User programs use IP addresses directly until then.

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

- SHM pipes (P1): touches pipe_server.c, exec_server.c -- net touches exec_server.c
  at a different code point (adding net_ep cap propagation). Merge-safe.
- Dash interactive (P2): touches mini_shell.c, posix_file.c -- no overlap with net.
- Allocator (P3): changes pool size in aios_root.c -- net adds DMA allocation.
  Both changes are at different locations. Merge-safe.

Networking M1-M2 can proceed in parallel with P1-P3. M3 (POSIX sockets) needs
exec_server.c changes that overlap with P1, so coordinate or sequence after P1.

## Parallel Development Notes

To enable parallel work across networking and other priorities, reserve
IPC label ranges in root_shared.h before branching:

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
