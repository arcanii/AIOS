#include "aios.h"

int _start(aios_syscalls_t *sys) {
    puts("=== AIOS System Info ===\n");
    puts("Kernel:    seL4 14.0.0\n");
    puts("Framework: Microkit 2.1.0\n");
    puts("Arch:      AArch64 (Cortex-A53)\n");
    puts("RAM:       2 GiB\n");
    puts("Disk:      128 MiB FAT16\n");
    puts("LLM:       25M params (Linux C)\n\n");
    puts("Sandbox syscalls:\n");
    puts("  puts putc put_dec put_hex\n");
    puts("  malloc free memcpy memset\n");
    puts("  strlen strcmp strcpy strncpy\n");
    return 0;
}
