/*
 * AIOS 0.4.x — Crash Test
 * Deliberately accesses invalid memory.
 * Root task should catch the fault and keep running.
 */
#include <stdio.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[]) {
    printf("[crash] I'm alive in my own VSpace.\n");
    printf("[crash] About to dereference NULL...\n");

    /* This should trigger a VM fault.
     * In 0.3.x this would kill the entire sandbox.
     * In 0.4.x the kernel delivers a fault to root task. */
    volatile int *bad = (volatile int *)0x0;
    int x = *bad;  /* BOOM */

    /* Should never reach here */
    printf("[crash] ERROR: should have faulted! x=%d\n", x);
    return -1;
}
