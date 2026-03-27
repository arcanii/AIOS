/* hello.c — AIOS sandbox program */
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
    sys->puts("Hello from AIOS sandbox!\n");
    sys->puts("This program was written by Claude,\n");
    sys->puts("compiled on the host, and executed\n");
    sys->puts("inside an seL4 protection domain.\n");
    return 0;
}
