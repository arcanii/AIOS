/* prog_stop.c -- a gate for the STOP/CONTINUE machinery (job control, increment 2): a process can be
 * stopped (SIGSTOP/SIGTSTP -> PS_STOPPED, not resumed) and continued (SIGCONT), and the parent learns
 * of both through wait(WUNTRACED)/wait(WCONTINUED). No terminal/pty needed -- driven by explicit kill,
 * so it runs in the automated gate. Exit 0 iff the whole stop -> continue -> terminate cycle holds. */

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>

int
main(void)
{
    int fail = 0;

    int kid = fork();
    if (kid == 0) {
        /* A running child making syscalls (so a pending stop signal can be delivered at a syscall
         * exit). Bounded so a broken mechanism fails the test instead of hanging; normally the parent
         * stops/terminates it within the first few iterations. */
        for (volatile long i = 0; i < 2000000; i++) (void)getpid();
        _exit(0);
    }

    int st = 0;

    /* SIGSTOP -> the child stops; wait(WUNTRACED) reports it (and does NOT reap it). */
    kill(kid, SIGSTOP);
    if (waitpid(kid, &st, WUNTRACED) != kid || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGSTOP) {
        printf("FAIL stop: waitpid(WUNTRACED) status=0x%x (want WIFSTOPPED, WSTOPSIG=SIGSTOP)\n", st);
        fail = 1;
    } else {
        printf("SIGSTOP -> child STOPPED; wait(WUNTRACED) reported WSTOPSIG=%d (SIGSTOP)\n", WSTOPSIG(st));
    }

    /* SIGCONT -> the child resumes; wait(WCONTINUED) reports the continue. */
    kill(kid, SIGCONT);
    if (waitpid(kid, &st, WCONTINUED) != kid || !WIFCONTINUED(st)) {
        printf("FAIL continue: waitpid(WCONTINUED) status=0x%x (want WIFCONTINUED)\n", st);
        fail = 1;
    } else {
        printf("SIGCONT -> child CONTINUED; wait(WCONTINUED) reported it\n");
    }

    /* The child is running again (only a running child receives a posted signal at a syscall): prove
     * it by terminating it and reaping. A default-terminate surfaces as WIFSIGNALED/WTERMSIG (POSIX). */
    kill(kid, SIGTERM);
    if (waitpid(kid, &st, 0) != kid || !WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) {
        printf("FAIL terminate: waitpid status=0x%x (want WIFSIGNALED, WTERMSIG %d)\n", st, SIGTERM);
        fail = 1;
    } else {
        printf("SIGTERM after continue -> child took it (signal %d) and was reaped\n", WTERMSIG(st));
    }

    printf("stop: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
