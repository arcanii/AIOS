/*
 * signal_test -- AIOS signal infrastructure test program
 *
 * Tests: sigaction, sigprocmask, kill(pid,0), kill(pid,sig),
 *        signal masking, and SIG_IGN/SIG_DFL behavior.
 */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

static volatile int got_signal = 0;
static volatile int last_signum = 0;

static void test_handler(int sig) {
    got_signal = 1;
    last_signum = sig;
}

static int tests_run = 0;
static int tests_pass = 0;

static void check(const char *name, int cond) {
    tests_run++;
    if (cond) {
        tests_pass++;
        printf("  PASS: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
    }
}

int main(void) {
    printf("=== AIOS Signal Test ===\n");
    printf("PID: %d\n\n", getpid());

    /* Test 1: sigaction installs handler without error */
    printf("[1] sigaction\n");
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = test_handler;
    int r = sigaction(SIGUSR1, &sa, NULL);
    check("sigaction(SIGUSR1) returns 0", r == 0);

    /* Test 2: sigaction retrieves old handler */
    struct sigaction old;
    memset(&old, 0, sizeof(old));
    r = sigaction(SIGUSR1, NULL, &old);
    check("sigaction(SIGUSR1, NULL, old) returns 0", r == 0);
    check("old handler == test_handler", old.sa_handler == test_handler);

    /* Test 3: sigaction rejects SIGKILL */
    printf("\n[2] sigaction rejects uncatchable\n");
    r = sigaction(SIGKILL, &sa, NULL);
    check("sigaction(SIGKILL) returns -1", r == -1);

    r = sigaction(SIGSTOP, &sa, NULL);
    check("sigaction(SIGSTOP) returns -1", r == -1);

    /* Test 4: sigprocmask */
    printf("\n[3] sigprocmask\n");
    sigset_t newset, oldset;
    sigemptyset(&newset);
    sigaddset(&newset, SIGUSR2);
    r = sigprocmask(SIG_BLOCK, &newset, &oldset);
    check("sigprocmask(SIG_BLOCK, {SIGUSR2}) returns 0", r == 0);

    r = sigprocmask(SIG_SETMASK, NULL, &oldset);
    check("sigprocmask read-back returns 0", r == 0);
    check("SIGUSR2 is blocked", sigismember(&oldset, SIGUSR2) == 1);

    /* Unblock */
    r = sigprocmask(SIG_UNBLOCK, &newset, NULL);
    check("sigprocmask(SIG_UNBLOCK) returns 0", r == 0);

    /* Test 5: kill(getpid(), 0) existence check */
    printf("\n[4] kill existence check\n");
    r = kill(getpid(), 0);
    check("kill(self, 0) returns 0", r == 0);

    r = kill(99999, 0);
    check("kill(99999, 0) returns -1 (no such process)", r == -1);

    /* Test 6: kill delivers SIGUSR1 to self -- cooperative dispatch */
    printf("\n[5] kill signal delivery (Phase 2)\n");
    got_signal = 0;
    last_signum = 0;
    r = kill(getpid(), SIGUSR1);
    check("kill(self, SIGUSR1) returns 0", r == 0);
    check("handler was invoked", got_signal == 1);
    check("received correct signal", last_signum == SIGUSR1);

    /* Test 6b: second signal also dispatches */
    got_signal = 0;
    last_signum = 0;
    r = kill(getpid(), SIGUSR1);
    check("second kill returns 0", r == 0);
    check("handler invoked again", got_signal == 1);

    /* Test 6c: SIG_DFL(SIGCHLD) should be ignored, not terminate */
    printf("\n[5b] SIG_DFL ignore class\n");
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &dfl, NULL);
    r = kill(getpid(), SIGCHLD);
    check("kill(self, SIGCHLD) with SIG_DFL returns 0", r == 0);
    check("process survived (SIGCHLD default=ignore)", 1);

    /* Test 7: SIG_IGN */
    printf("\n[6] SIG_IGN\n");
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    r = sigaction(SIGPIPE, &ign, NULL);
    check("sigaction(SIGPIPE, SIG_IGN) returns 0", r == 0);

    struct sigaction check_ign;
    sigaction(SIGPIPE, NULL, &check_ign);
    check("SIGPIPE handler is SIG_IGN", check_ign.sa_handler == SIG_IGN);

    /* Summary */
    printf("\n=== Results: %d/%d passed ===\n", tests_pass, tests_run);
    return (tests_pass == tests_run) ? 0 : 1;
}
