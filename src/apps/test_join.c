/*
 * test_join -- v0.4.191 thread_server deferred-join regression test.
 *
 * Exercises both orderings of the new non-blocking join:
 *   - DEFERRED: a slow worker is joined before it exits (the join is parked,
 *     the server keeps serving, and replies when the thread faults).
 *   - ZOMBIE: a quick worker exits before it is joined (the server records the
 *     exit + retval; the later join returns immediately).
 * Also checks pthread return-value propagation (x0 captured at the exit fault).
 */
#include <stdio.h>
#include <pthread.h>

static void spin(volatile long n) { while (n-- > 0) { __asm__ volatile(""); } }

static void *slow_worker(void *arg) {
    spin(40000000);                       /* outlast the main thread's join */
    return (void *)(long)(0x1230 + (long)arg);
}
static void *quick_worker(void *arg) {
    return (void *)(long)(0x5670 + (long)arg);
}

int main(void) {
    printf("=== thread join test ===\n");
    int ok = 1;

    /* 1. DEFERRED: create then immediately join a slow worker. */
    pthread_t a;
    if (pthread_create(&a, NULL, slow_worker, (void *)5)) {
        printf("FAIL: create slow\n"); return 1;
    }
    void *ra = (void *)-1;
    pthread_join(a, &ra);
    printf("deferred retval=0x%lx (expect 0x1235)\n", (long)ra);
    ok &= ((long)ra == 0x1235);

    /* 2. ZOMBIE: create a quick worker, let it exit, then join. */
    pthread_t b;
    if (pthread_create(&b, NULL, quick_worker, (void *)9)) {
        printf("FAIL: create quick\n"); return 1;
    }
    spin(40000000);                       /* let the quick worker exit first */
    void *rb = (void *)-1;
    pthread_join(b, &rb);
    printf("zombie retval=0x%lx (expect 0x5679)\n", (long)rb);
    ok &= ((long)rb == 0x5679);

    /* 3. Reuse: a third create+join after both prior threads were reaped,
     *    proving slots free correctly. */
    pthread_t c;
    if (pthread_create(&c, NULL, quick_worker, (void *)1)) {
        printf("FAIL: create reuse\n"); return 1;
    }
    void *rc = (void *)-1;
    pthread_join(c, &rc);
    printf("reuse retval=0x%lx (expect 0x5671)\n", (long)rc);
    ok &= ((long)rc == 0x5671);

    /* 4. CONCURRENT: two slow workers alive at once (no mutex), then join
     *    both. Exercises two live threads + two near-simultaneous exit faults
     *    in the event loop -- the case test_threads also hits. */
    pthread_t d, e;
    if (pthread_create(&d, NULL, slow_worker, (void *)1) ||
        pthread_create(&e, NULL, slow_worker, (void *)2)) {
        printf("FAIL: create concurrent\n"); return 1;
    }
    void *rd = (void *)-1, *re = (void *)-1;
    pthread_join(d, &rd);
    pthread_join(e, &re);
    printf("concurrent retvals=0x%lx,0x%lx (expect 0x1231,0x1232)\n",
           (long)rd, (long)re);
    ok &= ((long)rd == 0x1231 && (long)re == 0x1232);

    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
