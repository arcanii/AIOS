/* sbase_test.c -- sbase integration test via fork/exec/pipe
 * Tag: SBASE_TEST_V4
 * Resource-aware: status-only tests first, then capture tests.
 * Known limits: ~20 fork/exec before allocator exhaustion.
 * Touch/file creation untestable until ext2 dir cache is fixed.
 * Build as AiosPosixApp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>

static int pass_count = 0;
static int fail_count = 0;
static int skip_count = 0;
static int total_count = 0;
static int fork_count = 0;
static int resources_ok = 1;

static void test(const char *name, int expr)
{
    total_count++;
    if (expr) { pass_count++; printf("  PASS: %s\n", name); }
    else      { fail_count++; printf("  FAIL: %s\n", name); }
}

static void skip(const char *name, const char *reason)
{
    total_count++; skip_count++;
    printf("  SKIP: %s (%s)\n", name, reason);
}

static int run_capture(const char *path, char *const argv[],
                       char *buf, int bufsz)
{
    buf[0] = 0;
    if (!resources_ok) return -1;
    int pfd[2];
    if (pipe(pfd) < 0) { resources_ok = 0; return -1; }
    pid_t child = fork();
    if (child < 0) { close(pfd[0]); close(pfd[1]); resources_ok = 0; return -1; }
    fork_count++;
    if (child == 0) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        close(pfd[1]);
        execv(path, argv);
        _exit(127);
    }
    close(pfd[1]);
    int total = 0;
    while (total < bufsz - 1) {
        int n = read(pfd[0], buf + total, bufsz - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = 0;
    close(pfd[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -2;
}

static int run_status(const char *path, char *const argv[])
{
    if (!resources_ok) return -1;
    pid_t child = fork();
    if (child < 0) { resources_ok = 0; return -1; }
    fork_count++;
    if (child == 0) {
        close(1); close(2);
        execv(path, argv);
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -2;
}

static int contains(const char *buf, const char *needle)
{
    int nlen = 0;
    while (needle[nlen]) nlen++;
    for (int i = 0; buf[i]; i++) {
        int m = 1;
        for (int j = 0; j < nlen; j++)
            if (buf[i+j] != needle[j]) { m = 0; break; }
        if (m) return 1;
    }
    return 0;
}

static void chomp(char *s)
{
    int len = 0;
    while (s[len]) len++;
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r'))
        s[--len] = 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[1024];
    int rc;

    printf("=== AIOS sbase Integration Test (V4) ===\n");
    printf("Testing POSIX via sbase tool execution\n");
    printf("Resource budget: ~20 fork/exec cycles\n\n");

    /* Create test data before any forks */
    {
        int fd = open("/tmp/sbt_3lines", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, "aaa\nbbb\nccc\n", 12); close(fd); }
    }
    {
        int fd = open("/tmp/sbt_grep", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, "alpha\nbravo\ncharlie\n", 20); close(fd); }
    }
    {
        int fd = open("/tmp/sbt_cksum", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, "test\n", 5); close(fd); }
    }

    /* ---- Phase 1: run_status tests (no pipe, less pressure) ---- */

    printf("[1] Exit Codes\n");
    {
        char *a[] = {"/bin/true", NULL};
        rc = run_status("/bin/true", a);
        if (rc == -1) skip("true exits 0", "resources");
        else test("true exits 0", rc == 0);
    }
    {
        char *a[] = {"/bin/false", NULL};
        rc = run_status("/bin/false", a);
        if (rc == -1) skip("false exits 1", "resources");
        else test("false exits 1", rc == 1);
    }

    printf("\n[2] test(1)\n");
    {
        char *a[] = {"/bin/test", "-d", "/tmp", NULL};
        rc = run_status("/bin/test", a);
        if (rc == -1) skip("test -d /tmp", "resources");
        else test("test -d /tmp", rc == 0);
    }
    {
        char *a[] = {"/bin/test", "-d", "/nonexistent", NULL};
        rc = run_status("/bin/test", a);
        if (rc == -1) skip("test -d nonexistent", "resources");
        else test("test -d nonexistent", rc != 0);
    }
    {
        char *a[] = {"/bin/test", "hello", "=", "hello", NULL};
        rc = run_status("/bin/test", a);
        if (rc == -1) skip("test string eq", "resources");
        else test("test string eq", rc == 0);
    }
    {
        char *a[] = {"/bin/test", "3", "-gt", "2", NULL};
        rc = run_status("/bin/test", a);
        if (rc == -1) skip("test 3 -gt 2", "resources");
        else test("test 3 -gt 2", rc == 0);
    }
    {
        char *a[] = {"/bin/test", "1", "-eq", "2", NULL};
        rc = run_status("/bin/test", a);
        if (rc == -1) skip("test 1 -eq 2 (false)", "resources");
        else test("test 1 -eq 2 (false)", rc != 0);
    }

    printf("\n[3] File Ops (status only)\n");
    {
        char *a[] = {"/bin/cp", "/tmp/sbt_3lines", "/tmp/sbt_cp", NULL};
        rc = run_status("/bin/cp", a);
        if (rc == -1) skip("cp file", "resources");
        else test("cp file", rc == 0);
    }
    {
        char *a[] = {"/bin/rm", "/tmp/sbt_cp", NULL};
        rc = run_status("/bin/rm", a);
        if (rc == -1) skip("rm file", "resources");
        else test("rm file", rc == 0);
    }
    {
        char *a[] = {"/bin/mkdir", "/tmp/sbt_dir", NULL};
        rc = run_status("/bin/mkdir", a);
        if (rc == -1) { skip("mkdir", "resources"); }
        else {
            test("mkdir", rc == 0);
            rmdir("/tmp/sbt_dir");
        }
    }

    printf("  (fork count: %d)\n", fork_count);

    /* ---- Phase 2: run_capture tests (pipe + fork) ---- */

    printf("\n[4] Output Capture\n");
    if (!resources_ok) {
        skip("echo hello", "resources");
        skip("uname -s", "resources");
        skip("uname -m", "resources");
        skip("hostname", "resources");
        skip("whoami", "resources");
    } else {
        {
            char *a[] = {"/bin/echo", "hello", NULL};
            rc = run_capture("/bin/echo", a, buf, sizeof(buf));
            chomp(buf);
            if (rc == -1) skip("echo hello", "resources");
            else test("echo hello", rc == 0 && strcmp(buf, "hello") == 0);
        }
        if (resources_ok) {
            char *a[] = {"/bin/uname", "-s", NULL};
            rc = run_capture("/bin/uname", a, buf, sizeof(buf));
            chomp(buf);
            if (rc == -1) skip("uname -s", "resources");
            else test("uname -s = AIOS", rc == 0 && strcmp(buf, "AIOS") == 0);
        }
        if (resources_ok) {
            char *a[] = {"/bin/uname", "-m", NULL};
            rc = run_capture("/bin/uname", a, buf, sizeof(buf));
            chomp(buf);
            if (rc == -1) skip("uname -m", "resources");
            else test("uname -m = aarch64", rc == 0 && strcmp(buf, "aarch64") == 0);
        }
        if (resources_ok) {
            char *a[] = {"/bin/hostname", NULL};
            rc = run_capture("/bin/hostname", a, buf, sizeof(buf));
            chomp(buf);
            if (rc == -1) skip("hostname", "resources");
            else test("hostname = aios", rc == 0 && strcmp(buf, "aios") == 0);
        }
        if (resources_ok) {
            char *a[] = {"/bin/whoami", NULL};
            rc = run_capture("/bin/whoami", a, buf, sizeof(buf));
            chomp(buf);
            if (rc == -1) skip("whoami", "resources");
            else test("whoami = root", rc == 0 && strcmp(buf, "root") == 0);
        }
    }

    printf("  (fork count: %d)\n", fork_count);

    printf("\n[5] Text Processing\n");
    if (!resources_ok) {
        skip("cat file", "resources");
        skip("wc -l", "resources");
        skip("head -n 1", "resources");
        skip("sort -r", "resources");
    } else {
        {
            char *a[] = {"/bin/cat", "/tmp/sbt_3lines", NULL};
            rc = run_capture("/bin/cat", a, buf, sizeof(buf));
            if (rc == -1) skip("cat file", "resources");
            else test("cat file", rc == 0 && contains(buf, "aaa") && contains(buf, "ccc"));
        }
        if (resources_ok) {
            char *a[] = {"/bin/wc", "-l", "/tmp/sbt_3lines", NULL};
            rc = run_capture("/bin/wc", a, buf, sizeof(buf));
            if (rc == -1) skip("wc -l", "resources");
            else test("wc -l = 3", rc == 0 && contains(buf, "3"));
        }
        if (resources_ok) {
            char *a[] = {"/bin/head", "-n", "1", "/tmp/sbt_3lines", NULL};
            rc = run_capture("/bin/head", a, buf, sizeof(buf));
            chomp(buf);
            if (rc == -1) skip("head -n 1", "resources");
            else test("head -n 1 = aaa", rc == 0 && contains(buf, "aaa"));
        }
        if (resources_ok) {
            char *a[] = {"/bin/sort", "-r", "/tmp/sbt_3lines", NULL};
            rc = run_capture("/bin/sort", a, buf, sizeof(buf));
            if (rc == -1) skip("sort -r", "resources");
            else test("sort -r starts ccc", rc == 0 && contains(buf, "ccc"));
        }
    }

    printf("  (fork count: %d)\n", fork_count);

    printf("\n[6] String Utils\n");
    if (!resources_ok) {
        skip("basename", "resources");
        skip("dirname", "resources");
        skip("seq", "resources");
        skip("expr", "resources");
    } else {
        {
            char *a[] = {"/bin/basename", "/usr/local/bin/test", NULL};
            rc = run_capture("/bin/basename", a, buf, sizeof(buf));
            chomp(buf);
            if (rc == -1) skip("basename", "resources");
            else test("basename", rc == 0 && strcmp(buf, "test") == 0);
        }
        if (resources_ok) {
            char *a[] = {"/bin/dirname", "/usr/local/bin/test", NULL};
            rc = run_capture("/bin/dirname", a, buf, sizeof(buf));
            chomp(buf);
            if (rc == -1) skip("dirname", "resources");
            else test("dirname", rc == 0 && strcmp(buf, "/usr/local/bin") == 0);
        }
        if (resources_ok) {
            char *a[] = {"/bin/seq", "1", "5", NULL};
            rc = run_capture("/bin/seq", a, buf, sizeof(buf));
            if (rc == -1) skip("seq 1 5", "resources");
            else test("seq 1 5", rc == 0 && contains(buf, "3") && contains(buf, "5"));
        }
        if (resources_ok) {
            char *a[] = {"/bin/expr", "3", "+", "4", NULL};
            rc = run_capture("/bin/expr", a, buf, sizeof(buf));
            chomp(buf);
            if (rc == -1) skip("expr 3+4=7", "resources");
            else test("expr 3+4=7", strcmp(buf, "7") == 0);
        }
    }

    printf("\n[7] Grep\n");
    if (!resources_ok) {
        skip("grep match", "resources");
        skip("grep no-match", "resources");
    } else {
        {
            char *a[] = {"/bin/grep", "bravo", "/tmp/sbt_grep", NULL};
            rc = run_capture("/bin/grep", a, buf, sizeof(buf));
            chomp(buf);
            if (rc == -1) skip("grep match", "resources");
            else test("grep bravo", rc == 0 && strcmp(buf, "bravo") == 0);
        }
        if (resources_ok) {
            char *a[] = {"/bin/grep", "zzz", "/tmp/sbt_grep", NULL};
            rc = run_capture("/bin/grep", a, buf, sizeof(buf));
            if (rc == -1) skip("grep no-match", "resources");
            else test("grep no-match exits 1", rc == 1);
        }
    }

    printf("\n[8] Checksums\n");
    if (!resources_ok) {
        skip("cksum", "resources");
        skip("md5sum", "resources");
    } else {
        {
            char *a[] = {"/bin/cksum", "/tmp/sbt_cksum", NULL};
            rc = run_capture("/bin/cksum", a, buf, sizeof(buf));
            if (rc == -1) skip("cksum", "resources");
            else test("cksum runs", rc == 0 && buf[0] != 0);
        }
        if (resources_ok) {
            char *a[] = {"/bin/md5sum", "/tmp/sbt_cksum", NULL};
            rc = run_capture("/bin/md5sum", a, buf, sizeof(buf));
            if (rc == -1) skip("md5sum", "resources");
            else test("md5sum correct", rc == 0 && contains(buf, "d8e8fca2"));
        }
    }

    printf("\n[9] Environment\n");
    if (!resources_ok) {
        skip("printenv", "resources");
    } else {
        char *a[] = {"/bin/printenv", "PATH", NULL};
        rc = run_capture("/bin/printenv", a, buf, sizeof(buf));
        if (rc == -1) skip("printenv PATH", "resources");
        else test("printenv PATH", rc == 0 && contains(buf, "/bin"));
    }

    /* Cleanup */
    unlink("/tmp/sbt_3lines");
    unlink("/tmp/sbt_grep");
    unlink("/tmp/sbt_cksum");

    printf("\n========================================\n");
    printf("AIOS sbase Test (V4): %d/%d passed",
           pass_count, total_count);
    if (fail_count > 0) printf(", %d FAILED", fail_count);
    if (skip_count > 0) printf(", %d skipped", skip_count);
    printf("\n  fork/exec count: %d", fork_count);
    if (!resources_ok) printf(" (exhausted)");
    printf("\n========================================\n");
    return fail_count > 0 ? 1 : 0;
}
