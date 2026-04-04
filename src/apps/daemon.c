/*
 * AIOS daemon — background test process
 * Prints a tick message every 5 seconds.
 * Useful for testing kill, Ctrl-C, process management.
 */
#include <stdio.h>
#include <unistd.h>
#include <time.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    int tick = 0;
    while (1) {
        struct timespec ts = { .tv_sec = 5, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        tick++;
        printf("[daemon] tick %d\n", tick);
    }
    return 0;
}
