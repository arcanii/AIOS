/*
 * prog_execjail.c -- M4.3 red-team: proves a GUEST can only exec binaries INSIDE its AIOS root.
 *
 * Run under AIOS_ROOT, the PAL confines a guest-issued exec (AIOS_SYS_EXEC) to the root: the target
 * path is resolved inside the root (openat2 RESOLVE_IN_ROOT), so an absolute host path or a ".."
 * escape can never launch a binary outside the root. This program forks children that try to exec
 * various paths and checks the outcome by the child's exit code: an in-root binary actually runs (and
 * exits with its own code); an out-of-root path makes the exec fail, so the child falls through to
 * exit 111. (The INIT program the operator names on the aios-uk command line is the trusted entry and
 * is exempt -- only a guest's OWN exec() is confined, which is what this proves.)
 *
 * run.sh stages /jailtrue + /jailfalse (copies of sbase true/false) inside the root. This is to exec
 * what prog_jail is to open: a concrete adversary proving the boundary by failing to cross it.
 */
#include "libaios.h"

/* Fork a child that execs `path`; return the child's exit code (111 if the exec itself was denied). */
static int try_exec(const char *path) {
    char *argv[2]; argv[0] = (char *)path; argv[1] = 0;
    long pid = aios_fork();
    if (pid == 0) { aios_exec(path, argv); aios_exit(111); }   /* exec returned -> it was denied */
    int status = 0;
    aios_waitpid(pid, &status, 0);
    return WEXITSTATUS(status);
}

int main(void) {
    int fails = 0;
    printf("prog_execjail: a guest may exec only binaries INSIDE its AIOS root\n");

    struct { const char *path; int want; const char *note; } cases[] = {
        { "/jailtrue",    0,   "in-root binary runs (exit 0)" },
        { "/jailfalse",   1,   "in-root binary runs (exit 1)" },
        { "/../jailtrue", 0,   "dotdot clamped to root -> still the in-root binary" },
        { "/bin/sh",      111, "out-of-root host path DENIED" },
        { "/usr/bin/env", 111, "out-of-root host path DENIED" },
        { "/etc/passwd",  111, "out-of-root host path DENIED" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int got = try_exec(cases[i].path);
        int ok = (got == cases[i].want);
        printf("  %s exec(%s) -> %d (want %d)  %s\n",
               ok ? "ok  " : "FAIL", cases[i].path, got, cases[i].want, cases[i].note);
        if (!ok) fails++;
    }

    printf("prog_execjail: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
