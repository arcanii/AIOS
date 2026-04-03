/*
 * AIOS 0.4.x — sysinfo
 * Demonstrates POSIX shim: printf via IPC.
 */
#include <stdio.h>
#include "aios_posix.h"

int main(int argc, char *argv[]) {
    AIOS_INIT(argc, argv);

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
