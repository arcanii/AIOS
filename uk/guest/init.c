/*
 * init.c -- the AIOS system init (system layer, increment 1). This is the FIRST guest the AIOS kernel
 * launches (the appliance's /sbin/init): it owns the console and runs the system. For now that means
 * one job -- start /bin/login, and respawn it when a session ends (logout) -- the init -> login ->
 * shell -> logout -> login loop that makes AIOS feel like a system you log into, not a single shell.
 *
 * An AIOS-ABI program (libaios); the AIOS kernel reaps orphaned guests itself, so init only manages
 * its own login child. (libaios sleep() is a no-op, so the anti-thrash pause is a clock_gettime nap.)
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

/* A real short pause (libaios sleep() is a no-op): busy-wait on the monotonic clock. Keeps a
 * non-interactive (EOF) respawn loop from spinning tight; harmless in the interactive case (login
 * blocks on input, so we only nap after a genuine logout). */
static void nap_ms(long ms) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &b);
        long d = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
        if (d >= ms) return;
    }
}

int main(void) {
    fputs("\n[init] AIOS system init -- starting the console login\n", stderr);
    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            char *argv[] = { "/bin/login", (char *)0 };
            execv("/bin/login", argv);
            fputs("[init] FATAL: cannot exec /bin/login\n", stderr);
            _exit(127);
        }
        if (pid < 0) { nap_ms(500); continue; }      /* fork failed -> back off + retry */
        int st; while (waitpid(pid, &st, 0) < 0) { }  /* wait for the session (login -> shell) to end */
        nap_ms(300);                                  /* brief anti-thrash pause, then respawn login */
    }
}
