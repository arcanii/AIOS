#include "aios.h"

/* ── Utility ──────────────────────────────────────────── */
static void put_num(unsigned long n) {
    char buf[20];
    int i = 0;
    if (n == 0) { sys->puts_direct("0"); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    char out[20];
    int j = 0;
    while (i > 0) out[j++] = buf[--i];
    out[j] = 0;
    sys->puts_direct(out);
}

static void print(const char *s) { sys->puts_direct(s); }

static unsigned long long read_timer(void) {
    unsigned long long t;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t;
}

/* ── Benchmark 1: Sieve of Eratosthenes ──────────────── */
#define SIEVE_SIZE 100000
static unsigned char sieve_buf[SIEVE_SIZE];

static int bench_sieve(void) {
    for (int i = 0; i < SIEVE_SIZE; i++) sieve_buf[i] = 1;
    sieve_buf[0] = sieve_buf[1] = 0;
    for (int i = 2; i * i < SIEVE_SIZE; i++) {
        if (sieve_buf[i]) {
            for (int j = i * i; j < SIEVE_SIZE; j += i)
                sieve_buf[j] = 0;
        }
    }
    int count = 0;
    for (int i = 0; i < SIEVE_SIZE; i++)
        if (sieve_buf[i]) count++;
    return count;
}

/* ── Benchmark 2: Pi via Leibniz (integer scaled) ────── */
static long bench_pi_leibniz(int iterations) {
    long pi = 0;
    long scale = 1000000000L;
    for (int i = 0; i < iterations; i++) {
        long term = scale / (2 * i + 1);
        if (i & 1) pi -= term;
        else pi += term;
    }
    return pi * 4;
}

/* ── Benchmark 3: Fibonacci (iterative) ──────────────── */
static unsigned long bench_fib(int n) {
    unsigned long a = 0, b = 1;
    for (int i = 0; i < n; i++) {
        unsigned long t = a + b;
        a = b;
        b = t;
    }
    return b;
}

/* ── Benchmark 4: Memory throughput ──────────────────── */
#define MEM_SIZE 65536
static unsigned char mem_buf[MEM_SIZE];

static unsigned long bench_memory(void) {
    for (int pass = 0; pass < 100; pass++) {
        for (int i = 0; i < MEM_SIZE; i++)
            mem_buf[i] = (unsigned char)(i + pass);
    }
    unsigned long checksum = 0;
    for (int i = 0; i < MEM_SIZE; i++)
        checksum += mem_buf[i];
    return checksum;
}

/* ── Benchmark 5: Integer matrix multiply ────────────── */
#define MAT_SIZE 64
static int mat_a[MAT_SIZE][MAT_SIZE];
static int mat_b[MAT_SIZE][MAT_SIZE];
static int mat_c[MAT_SIZE][MAT_SIZE];

static long bench_matmul(void) {
    for (int i = 0; i < MAT_SIZE; i++)
        for (int j = 0; j < MAT_SIZE; j++) {
            mat_a[i][j] = i + j;
            mat_b[i][j] = i - j + 1;
            mat_c[i][j] = 0;
        }
    for (int i = 0; i < MAT_SIZE; i++)
        for (int j = 0; j < MAT_SIZE; j++)
            for (int k = 0; k < MAT_SIZE; k++)
                mat_c[i][j] += mat_a[i][k] * mat_b[k][j];
    return mat_c[MAT_SIZE/2][MAT_SIZE/2];
}

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    unsigned long long freq = get_timer_freq();
    unsigned long long t0, t1, ms;

    print("AIOS Benchmark Suite v1.0\n");
    print("Timer: "); put_num((unsigned long)(freq / 1000000)); print(" MHz\n\n");

    /* 1. Sieve */
    print("[1/5] Sieve (100K)... ");
    t0 = read_timer();
    int primes = bench_sieve();
    t1 = read_timer();
    ms = ((t1 - t0) * 1000) / freq;
    put_num((unsigned long)primes); print(" primes in ");
    put_num((unsigned long)ms); print(" ms\n");

    /* 2. Pi */
    print("[2/5] Pi Leibniz (10M iter)... ");
    t0 = read_timer();
    long pi = bench_pi_leibniz(10000000);
    t1 = read_timer();
    ms = ((t1 - t0) * 1000) / freq;
    print("pi*10^9 = ");
    if (pi < 0) { print("-"); pi = -pi; }
    put_num((unsigned long)pi); print(" in ");
    put_num((unsigned long)ms); print(" ms\n");

    /* 3. Fibonacci */
    print("[3/5] Fibonacci (1M iter)... ");
    t0 = read_timer();
    unsigned long fib = bench_fib(1000000);
    t1 = read_timer();
    ms = ((t1 - t0) * 1000) / freq;
    put_num(fib); print(" in ");
    put_num((unsigned long)ms); print(" ms\n");

    /* 4. Memory */
    print("[4/5] Memory (64K x 100)... ");
    t0 = read_timer();
    unsigned long cksum = bench_memory();
    t1 = read_timer();
    ms = ((t1 - t0) * 1000) / freq;
    print("cksum="); put_num(cksum); print(" in ");
    put_num((unsigned long)ms); print(" ms\n");

    /* 5. Matrix multiply */
    print("[5/5] Matrix (64x64)... ");
    t0 = read_timer();
    long mat = bench_matmul();
    t1 = read_timer();
    ms = ((t1 - t0) * 1000) / freq;
    print("C[32][32]=");
    if (mat < 0) { print("-"); mat = -mat; }
    put_num((unsigned long)mat); print(" in ");
    put_num((unsigned long)ms); print(" ms\n");

    print("\nBenchmark complete.\n");
    return 0;
}
