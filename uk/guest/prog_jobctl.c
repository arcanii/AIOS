/* prog_jobctl.c -- a gate for the job-control FOUNDATION: kernel-tracked process groups + the
 * controlling-terminal foreground group + kill-to-a-process-group. This increment lands the state
 * and the syscalls (setpgid/getpgid/getpgrp/tcsetpgrp/tcgetpgrp + kill(-pgid)); terminal-signal
 * ROUTING and stop/continue come in a later increment, so this asserts the plumbing, not ^Z.
 *
 * Exit 0 iff every check passes -- the run.sh gate keys on it. */

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>

static volatile int got_usr1;
static void on_usr1(int s) { (void)s; got_usr1 = 1; }

int
main(void)
{
    int fail = 0;
    int pid = getpid();

    /* init is its own process-group leader: getpgrp() == getpid(). */
    if (getpgrp() != pid) { printf("FAIL getpgrp() != getpid() (%d != %d)\n", getpgrp(), pid); fail = 1; }
    else printf("getpgrp() == getpid() == %d (init is its own group leader)\n", pid);

    /* getpgid(0) is getpgrp(); getpgid(pid) is the same. */
    if (getpgid(0) != pid || getpgid(pid) != pid) { printf("FAIL getpgid mismatch\n"); fail = 1; }

    /* pgid is INHERITED across fork; a child can then become its own leader with setpgid(0,0). */
    int kid = fork();
    if (kid == 0) {
        int cfail = 0;
        if (getpgrp() != pid) cfail = 1;                 /* inherited the parent's group */
        if (setpgid(0, 0) != 0) cfail = 1;               /* become my own group leader */
        if (getpgrp() != getpid()) cfail = 1;            /* ... pgid now == my pid */
        _exit(cfail);
    }
    int status = 0;
    waitpid(kid, &status, 0);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) { printf("FAIL child pgid checks\n"); fail = 1; }
    else printf("fork inherits pgid; setpgid(0,0) makes a new leader (child OK)\n");

    /* kill to a PROCESS GROUP: a pid < 0 targets the group -pid; pid 0 targets the caller's group.
     * Signal our own group (just us now -- the child left it) and confirm the handler fires. */
    signal(SIGUSR1, on_usr1);
    got_usr1 = 0;
    if (kill(-getpgrp(), SIGUSR1) != 0) { printf("FAIL kill(-pgrp) returned error\n"); fail = 1; }
    if (!got_usr1) { printf("FAIL kill(-pgrp, SIGUSR1) did not reach the group\n"); fail = 1; }
    else printf("kill(-pgrp, SIGUSR1) delivered to the group (handler ran)\n");

    /* killpg(0, ...) is kill(-pgrp, ...); existence-probe a real pid with signal 0. */
    got_usr1 = 0;
    if (killpg(0, SIGUSR1) != 0 || !got_usr1) { printf("FAIL killpg(0, SIGUSR1)\n"); fail = 1; }
    else printf("killpg(0, SIGUSR1) delivered to the caller's group\n");

    /* tcsetpgrp/tcgetpgrp operate on the controlling terminal. In the gate stdin is NOT a tty, so
     * they must fail with ENOTTY; on a real tty they round-trip. Either way the syscall is wired. */
    if (isatty(0)) {
        if (tcsetpgrp(0, pid) != 0 || tcgetpgrp(0) != pid) { printf("FAIL tcsetpgrp/tcgetpgrp round-trip\n"); fail = 1; }
        else printf("tcsetpgrp(0,%d); tcgetpgrp(0) == %d (tty round-trip)\n", pid, pid);
    } else {
        errno = 0;
        if (tcgetpgrp(0) != -1 || errno != ENOTTY) { printf("FAIL tcgetpgrp(non-tty) should be ENOTTY\n"); fail = 1; }
        else printf("tcgetpgrp(non-tty) -> -1 ENOTTY (correct)\n");
    }

    printf("jobctl: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
