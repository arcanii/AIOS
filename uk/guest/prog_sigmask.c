/* prog_sigmask.c -- a gate for sigprocmask: a BLOCKED signal stays pending (its handler does not run)
 * until it is unblocked, at which point it is delivered. This is the signal-masking dash JOBS=1 needs
 * (sigblockall/sigclearmask around its critical sections). Exit 0 iff the block/pend/unblock cycle
 * holds. No pty needed -- runs in the automated gate. */

#include <signal.h>
#include <unistd.h>
#include <stdio.h>

static volatile int got_usr1;
static void on_usr1(int s) { (void)s; got_usr1 = 1; }

int
main(void)
{
    int fail = 0;
    sigset_t set, old;

    signal(SIGUSR1, on_usr1);

    /* Block SIGUSR1, then raise it: the handler must NOT run while it is blocked (stays pending). */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, &old) != 0) { printf("FAIL sigprocmask(BLOCK)\n"); fail = 1; }

    got_usr1 = 0;
    raise(SIGUSR1);
    if (got_usr1) { printf("FAIL SIGUSR1 delivered while BLOCKED (should be pending)\n"); fail = 1; }
    else printf("SIGUSR1 raised while blocked -> pending, handler NOT run (correct)\n");

    /* Unblock: the pending SIGUSR1 is delivered now. */
    if (sigprocmask(SIG_UNBLOCK, &set, 0) != 0) { printf("FAIL sigprocmask(UNBLOCK)\n"); fail = 1; }
    if (!got_usr1) { printf("FAIL pending SIGUSR1 not delivered on UNBLOCK\n"); fail = 1; }
    else printf("SIG_UNBLOCK -> pending SIGUSR1 delivered, handler ran (correct)\n");

    /* The previous mask is returned; SETMASK back to it should leave SIGUSR1 unblocked. */
    got_usr1 = 0;
    sigprocmask(SIG_SETMASK, &old, 0);
    raise(SIGUSR1);
    if (!got_usr1) { printf("FAIL after SETMASK(old) SIGUSR1 should deliver immediately\n"); fail = 1; }
    else printf("SETMASK(old) restored -> SIGUSR1 delivers immediately (correct)\n");

    printf("sigmask: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
