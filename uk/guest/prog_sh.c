/*
 * prog_sh.c -- a minimal shell for AIOS, the capstone of the process model.
 *
 * It composes the whole quartet -- fork, exec, wait, pipe (+ dup2) -- into the thing they exist
 * for: reading a command line and running a pipeline of real programs. It reads a line from stdin,
 * splits it on '|' into stages, tokenises each stage into argv, builds the pipes, wires each
 * stage's stdin/stdout onto the right pipe ends with dup2, fork+execs every stage, and waits for
 * all of them. So `./prog_args alpha | ./prog_wc` runs two exec'd AIOS programs connected by an
 * AIOS-kernel pipe -- an operational shell on the AIOS userspace kernel.
 *
 * Minimal on purpose (the full dash/sbase retarget is the larger goal): one builtin (exit), no
 * quoting, globbing, redirection-to-file, $vars, or PATH search (type ./name). Programs are named
 * relative to the cwd, exactly as the kernel resolves them.
 */
#include "libaios.h"

#define MAXLINE   1024
#define MAXARGS   32
#define MAXSTAGES 8

/* Read one line from stdin into buf (NUL-terminated, newline stripped). Returns length, or -1 at
 * EOF with no input. Reads a byte at a time so we never consume past the line -- stdin may be a
 * pipe carrying more commands. */
static int read_line(char *buf, int max) {
    int n = 0;
    for (;;) {
        char c;
        long r = aios_read(STDIN_FILENO, &c, 1);
        if (r <= 0) { if (n == 0) return -1; break; }   /* EOF */
        if (c == '\n') break;
        if (n < max - 1) buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* Split s into whitespace-separated tokens (argv, NULL-terminated). Mutates s. Returns argc. */
static int tokenize(char *s, char **argv, int maxargs) {
    int argc = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (argc < maxargs - 1) argv[argc++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = '\0';
    }
    argv[argc] = 0;
    return argc;
}

/* Split line into pipeline stages on '|'. Mutates line. Returns the stage count. */
static int split_pipes(char *line, char **stages, int maxstages) {
    int n = 0;
    stages[n++] = line;
    for (char *p = line; *p; p++)
        if (*p == '|') { *p = '\0'; if (n < maxstages) stages[n++] = p + 1; }
    return n;
}

/* Run a pipeline: for each stage fork a child, wiring its stdin to the previous pipe's read end and
 * (unless it is the last stage) its stdout to a fresh pipe's write end, then exec it. Wait for all. */
static void run_pipeline(char **stages, int nstages) {
    long pids[MAXSTAGES];
    int  prev_read = -1;                          /* this stage's stdin (previous pipe's read end) */
    for (int i = 0; i < nstages; i++) {
        int fds[2] = { -1, -1 };
        int have_pipe = (i < nstages - 1);
        if (have_pipe && aios_pipe(fds) < 0) { fdputs(STDERR_FILENO, "sh: pipe failed\n"); return; }

        long pid = aios_fork();
        if (pid < 0) { fdputs(STDERR_FILENO, "sh: fork failed\n"); return; }
        if (pid == 0) {                           /* child stage */
            if (prev_read >= 0) aios_dup2(prev_read, STDIN_FILENO);
            if (have_pipe)      aios_dup2(fds[1], STDOUT_FILENO);
            if (prev_read >= 0) aios_close(prev_read);
            if (have_pipe) { aios_close(fds[0]); aios_close(fds[1]); }
            char *argv[MAXARGS];
            if (tokenize(stages[i], argv, MAXARGS) == 0) aios_exit(0);
            aios_exec(argv[0], argv);
            fdputs(STDERR_FILENO, "sh: cannot exec: ");
            fdputs(STDERR_FILENO, argv[0]);
            fdputs(STDERR_FILENO, "\n");
            aios_exit(127);
        }
        pids[i] = pid;
        if (prev_read >= 0) aios_close(prev_read); /* parent done with the previous read end */
        if (have_pipe) { aios_close(fds[1]); prev_read = fds[0]; }
        else prev_read = -1;
    }
    for (int i = 0; i < nstages; i++) { int st; aios_waitpid(pids[i], &st, 0); }
}

static int is_exit(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    return strncmp(line, "exit", 4) == 0 && (line[4] == '\0' || line[4] == ' ' || line[4] == '\t');
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[MAXLINE];
    for (;;) {
        fdputs(STDERR_FILENO, "aios$ ");           /* prompt on stderr so it never pollutes a pipe */
        int n = read_line(line, sizeof line);
        if (n < 0) break;                          /* EOF -> exit */
        if (n == 0) continue;
        if (is_exit(line)) break;
        char *stages[MAXSTAGES];
        int ns = split_pipes(line, stages, MAXSTAGES);
        run_pipeline(stages, ns);
    }
    fdputs(STDERR_FILENO, "aios$ exit\n");
    return 0;
}
