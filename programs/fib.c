/* fib.c — Fibonacci sequence calculator */
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
    sys->puts("Fibonacci sequence (first 20 terms):\n");

    unsigned int a = 0, b = 1;
    for (int i = 0; i < 20; i++) {
        sys->puts("  fib(");
        sys->put_dec(i);
        sys->puts(") = ");
        sys->put_dec(a);
        sys->putc('\n');

        unsigned int next = a + b;
        a = b;
        b = next;
    }

    sys->puts("\nDone. Computed entirely inside seL4 sandbox.\n");
    return 0;
}
