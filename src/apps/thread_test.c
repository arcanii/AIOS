/*
 * AIOS 0.4.x — Thread Test
 * Root task creates additional threads in this process's VSpace.
 * This process prints from main thread, root spawns worker threads.
 */
#include <stdio.h>
#include <sel4/sel4.h>

/* Shared variable — proves threads share address space */
volatile int shared_counter = 0;

int main(int argc, char *argv[]) {
    printf("[thread_test] Main thread started\n");
    printf("[thread_test] Initial counter = %d\n", shared_counter);

    /* Worker threads will increment shared_counter.
     * Main thread polls it. */
    printf("[thread_test] Waiting for workers to increment counter...\n");
    int last = 0;
    for (int i = 0; i < 100000; i++) {
        int cur = shared_counter;
        if (cur != last) {
            printf("[thread_test] counter = %d\n", cur);
            last = cur;
        }
        if (cur >= 4) break;
        seL4_Yield();
    }

    printf("[thread_test] Final counter = %d\n", shared_counter);
    if (shared_counter >= 4) {
        printf("[thread_test] SUCCESS: %d threads ran concurrently!\n", shared_counter);
    } else {
        printf("[thread_test] PARTIAL: counter=%d (expected 4)\n", shared_counter);
    }
    return 0;
}
