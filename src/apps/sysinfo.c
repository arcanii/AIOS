/*
 * AIOS 0.4.x — sysinfo (POSIX version)
 *
 * Uses aios_posix shim: printf goes through IPC to serial_server.
 * argv[0] = serial_ep
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include "aios_posix.h"

static long parse_num(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int main(int argc, char *argv[]) {
    /* Initialize POSIX shim with serial endpoint */
    seL4_CPtr serial_ep = 0;
    if (argc > 0) serial_ep = (seL4_CPtr)parse_num(argv[0]);
    aios_init(serial_ep, 0);

    /* Now printf works via IPC! */
    printf("AIOS System Information\n");
    printf("=======================\n");
    printf("Kernel:    seL4 15.0.0\n");
    printf("Arch:      AArch64 (ARMv8-A)\n");
    printf("CPU:       Cortex-A53\n");
    printf("Cores:     4 (SMP)\n");
    printf("Scheduler: Classic (non-MCS)\n");
    printf("Mode:      EL2 (hypervisor)\n");
    printf("FS:        ext2 on virtio-blk\n");
    printf("IPC:       seL4 endpoints\n");
    printf("POSIX:     aios_posix shim\n");
    printf("=======================\n");

    return 0;
}
