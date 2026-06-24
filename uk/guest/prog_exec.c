/*
 * prog_exec.c -- exercises AIOS_SYS_EXEC (M3d step 1: the process model begins).
 *
 * Replaces its own image with the AIOS program named in argv[1], passing the rest of argv on.
 * The AIOS kernel injects a real execve into the (stopped) guest; on success this image is gone
 * and the new one runs under the same kernel (its syscalls keep trapping). exec only returns on
 * failure -- like POSIX execv -- so the "exec failed" line proves the error path too.
 *
 *   aios-uk prog_exec ./prog_args one two   -> becomes prog_args, prints argv [prog_args one two]
 *   aios-uk prog_exec /no/such/program       -> "exec failed", exit 1
 */
#include "libaios.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: prog_exec <program> [args...]\n"); return 2; }
    printf("prog_exec: replacing my image with '%s' via AIOS_SYS_EXEC\n", argv[1]);
    long rc = aios_exec(argv[1], &argv[1]);   /* &argv[1] is NULL-terminated (argv's own NULL) */
    printf("prog_exec: exec failed (rc=%d)\n", (int)rc);
    return 1;
}
