/*
 * prog_signal.c -- exercises real signal DELIVERY (M5): install a handler, raise the signal, and
 * confirm the handler actually RAN (the kernel saved the guest's regs, ran the handler, and returned
 * via the trampoline). Also checks SIG_IGN. Real C via the -nostdinc shadow headers.
 */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static volatile int got;
static void handler(int s) { got = s; printf("  handler ran: caught signal %d\n", s); }

int main(void) {
    int fails = 0;

    printf("prog_signal: install a SIGUSR1 handler, then raise(SIGUSR1)\n");
    if (signal(SIGUSR1, handler) == SIG_ERR) { printf("signal() failed\n"); return 1; }
    raise(SIGUSR1);
    printf("  back from raise: got=%d (expect %d)\n", got, SIGUSR1);
    if (got != SIGUSR1) fails++;

    /* SIG_IGN: raising an ignored signal must neither run anything nor terminate us */
    got = 0;
    signal(SIGUSR2, SIG_IGN);
    raise(SIGUSR2);
    printf("  raised an ignored SIGUSR2: still alive, got=%d (expect 0)\n", got);
    if (got != 0) fails++;

    /* the handler fires again on a second raise */
    raise(SIGUSR1);
    if (got != SIGUSR1) fails++;

    printf("prog_signal: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
