/* info.c — prints system information */
typedef struct {
    void (*puts)(const char *s);
    void (*putc)(char c);
    void (*put_dec)(unsigned int n);
    void (*put_hex)(unsigned int n);
    void *(*malloc)(unsigned long size);
    void  (*free)(void *ptr);
    void *(*memcpy)(void *dst, const void *src, unsigned long n);
    void *(*memset)(void *dst, int c, unsigned long n);
    int   (*strlen)(const char *s);
    int   (*strcmp)(const char *a, const char *b);
    char *(*strcpy)(char *dst, const char *src);
    char *(*strncpy)(char *dst, const char *src, unsigned long n);
} aios_syscalls_t;

int _start(aios_syscalls_t *sys) {
    sys->puts("=== AIOS System Info ===\n");
    sys->puts("Kernel:    seL4 14.0.0\n");
    sys->puts("Framework: Microkit 2.1.0\n");
    sys->puts("Arch:      AArch64 (Cortex-A53)\n");
    sys->puts("RAM:       2 GiB\n");
    sys->puts("Disk:      128 MiB FAT16\n");
    sys->puts("LLM:       25M params (Linux C)\n\n");

    sys->puts("Sandbox capabilities:\n");
    sys->puts("  puts, putc, put_dec, put_hex\n");
    sys->puts("  malloc, free\n");
    sys->puts("  memcpy, memset, strlen, strcmp\n");
    sys->puts("  strcpy, strncpy\n");
    return 0;
}
