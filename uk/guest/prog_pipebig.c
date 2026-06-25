/*
 * prog_pipebig.c -- stress the pipe past its buffer to exercise WRITER parking.
 *
 * The child writes a large, deterministic stream (one big write() of BYTES bytes) down the pipe;
 * the parent reads it all and verifies the byte count + a checksum. Because BYTES far exceeds the
 * pipe buffer, the child's single write fills the pipe and the kernel PARKS the writer, resuming it
 * as the parent drains -- classic bounded-buffer producer/consumer. Pass iff every byte arrives
 * intact, which can only happen if both writer- and reader-parking + the wake fixpoint are correct.
 */
#include "libaios.h"

#define BYTES (200 * 1000)

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fds[2];
    if (aios_pipe(fds) < 0) { printf("prog_pipebig: pipe failed\n"); return 1; }

    long pid = aios_fork();
    if (pid < 0) { printf("prog_pipebig: fork failed\n"); return 1; }
    if (pid == 0) {                          /* child: write BYTES in one big blocking write */
        aios_close(fds[0]);
        unsigned char *buf = malloc(BYTES);
        if (!buf) aios_exit(3);
        for (int i = 0; i < BYTES; i++) buf[i] = (unsigned char)(i & 0xff);
        long off = 0;
        while (off < BYTES) {                /* loop in case the kernel returns a short write */
            long w = aios_write(fds[1], buf + off, BYTES - off);
            if (w <= 0) break;
            off += w;
        }
        aios_close(fds[1]);
        aios_exit(off == BYTES ? 0 : 4);
    }

    aios_close(fds[1]);                        /* parent: drain + verify */
    unsigned char buf[8192];
    long total = 0, n;
    unsigned long sum = 0, expect = 0;
    while ((n = aios_read(fds[0], buf, sizeof buf)) > 0)
        for (long i = 0; i < n; i++) sum += buf[i], total++;
    aios_close(fds[0]);
    for (long i = 0; i < BYTES; i++) expect += (unsigned char)(i & 0xff);

    int status = 0;
    aios_waitpid(pid, &status, 0);
    int ok = (total == BYTES && sum == expect && WEXITSTATUS(status) == 0);
    printf("prog_pipebig: got %d/%d bytes, checksum %s, child exit %d -> %s\n",
           (int)total, BYTES, (sum == expect) ? "ok" : "BAD",
           WEXITSTATUS(status), ok ? "PASS" : "FAIL");
    return ok ? 0 : 2;
}
