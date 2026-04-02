/*
 * AIOS 0.4.x — child process
 * Runs in its own VSpace + TCB.
 */
#include <stdio.h>
#include <sel4/sel4.h>

int main(int argc, char *argv[]) {
    printf("[child] Hello from child process!\n");
    printf("[child] argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("[child] argv[%d] = %s\n", i, argv[i]);
    }
    printf("[child] I have my own VSpace + TCB.\n");
    printf("[child] Exiting.\n");
    return 0;
}
