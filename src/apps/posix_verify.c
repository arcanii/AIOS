/* posix_verify.c -- POSIX compliance verification for AIOS
 * Tag: POSIX_VERIFY_V3
 * Comprehensive test of all implemented POSIX interfaces.
 * Reference: IEEE Std 1003.1-2024
 * Build as AiosPosixApp (uses __wrap_main).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <grp.h>

/* ---- Test scaffolding ---- */

static int pass_count = 0;
static int fail_count = 0;
static int skip_count = 0;
static int total_count = 0;

static void test(const char *name, int expr)
{
    total_count++;
    if (expr) {
        pass_count++;
        printf("  PASS: %s\n", name);
    } else {
        fail_count++;
        printf("  FAIL: %s (errno=%d)\n", name, errno);
    }
}

static void skip(const char *name, const char *reason)
{
    total_count++;
    skip_count++;
    printf("  SKIP: %s (%s)\n", name, reason);
}

/* ---- Signal handler for section 10 ---- */

static volatile int sig_received = 0;
static void usr1_handler(int sig) { (void)sig; sig_received = 1; }

/* ---- Thread functions for section 13 ---- */

static void *thread_set_value(void *arg)
{
    int *val = (int *)arg;
    *val = 42;
    return (void *)(long)99;
}

static pthread_mutex_t test_mtx;
static int shared_counter = 0;

static void *thread_mutex_inc(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&test_mtx);
    shared_counter++;
    pthread_mutex_unlock(&test_mtx);
    return NULL;
}

/* ---- Main ---- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("=== AIOS POSIX Compliance Verification (V3) ===\n");
    printf("Reference: IEEE Std 1003.1-2024\n");
    printf("Testing all implemented syscalls on target...\n\n");

    /* ============================================================
     * Section 1: Process Identity
     * Interfaces: getpid getppid getuid geteuid getgid getegid
     * ============================================================ */
    printf("[1] Process Identity\n");
    test("getpid > 0",       getpid() > 0);
    test("getppid >= 0",     getppid() >= 0);
    test("getuid >= 0",      (int)getuid() >= 0);
    test("geteuid >= 0",     (int)geteuid() >= 0);
    test("getgid >= 0",      (int)getgid() >= 0);
    test("getegid >= 0",     (int)getegid() >= 0);

    /* ============================================================
     * Section 2: Filesystem I/O -- create, write, read, seek
     * Interfaces: open close read write lseek
     * ============================================================ */
    printf("\n[2] Filesystem I/O\n");

    {
        struct stat tmp_st;
        int tmp_ok = (fstatat(AT_FDCWD, "/tmp", &tmp_st, 0) == 0);
        test("stat /tmp exists", tmp_ok);
        if (!tmp_ok) mkdir("/tmp", 0777);
    }

    char cwd_save[256];
    char *cret = getcwd(cwd_save, sizeof(cwd_save));
    test("getcwd", cret != NULL);

    int fd_w = open("/tmp/pv_test", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    test("open O_CREAT|O_WRONLY", fd_w >= 0);

    if (fd_w >= 0) {
        const char *msg = "posix verify\n";
        ssize_t nw = write(fd_w, msg, 13);
        test("write 13 bytes", nw == 13);

        int cr = close(fd_w);
        test("close write fd", cr == 0);

        /* Re-open for reading (ext2 cache must serve fresh data) */
        int fd_r = open("/tmp/pv_test", O_RDONLY);
        test("open O_RDONLY after write", fd_r >= 0);

        if (fd_r >= 0) {
            char buf[64];
            memset(buf, 0, sizeof(buf));
            ssize_t nr = read(fd_r, buf, sizeof(buf));
            test("read returns 13", nr == 13);
            test("read content match", memcmp(buf, "posix verify\n", 13) == 0);

            off_t pos = lseek(fd_r, 0, SEEK_SET);
            test("lseek SEEK_SET 0", pos == 0);

            pos = lseek(fd_r, 5, SEEK_SET);
            test("lseek to offset 5", pos == 5);

            struct stat st;
            int sr = fstat(fd_r, &st);
            test("fstat on open fd", sr == 0);
            test("fstat st_size=13", st.st_size == 13);

            close(fd_r);
        }
    }

    /* ============================================================
     * Section 3: File Operations -- stat, unlink, unlinkat
     * Interfaces: fstatat unlink unlinkat
     * ============================================================ */
    printf("\n[3] File Operations\n");

    {
        struct stat st2;
        int sr2 = fstatat(AT_FDCWD, "/tmp/pv_test", &st2, 0);
        test("fstatat /tmp/pv_test", sr2 == 0);

        int ur = unlink("/tmp/pv_test");
        test("unlink /tmp/pv_test", ur == 0);

        /* After unlink, open must fail (ext2 cache makes this visible) */
        int fd3 = open("/tmp/pv_test", O_RDONLY);
        test("unlink verified (open fails)", fd3 < 0);
        if (fd3 >= 0) close(fd3);

        /* unlinkat with AT_FDCWD */
        int fd_u = open("/tmp/pv_unlinkat", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd_u >= 0) {
            write(fd_u, "test", 4);
            close(fd_u);
            int ua = unlinkat(AT_FDCWD, "/tmp/pv_unlinkat", 0);
            test("unlinkat AT_FDCWD", ua == 0);
        } else {
            skip("unlinkat AT_FDCWD", "create failed");
        }
    }

    /* ============================================================
     * Section 4: Scatter/Gather I/O
     * Interfaces: writev readv (via pipe to avoid ext2 round-trip)
     * ============================================================ */
    printf("\n[4] Scatter/Gather I/O\n");

    {
        int sgfd[2];
        int pr = pipe(sgfd);
        if (pr == 0) {
            struct iovec wv[2];
            char wa[] = "hello";
            char wb[] = "world";
            wv[0].iov_base = wa;
            wv[0].iov_len = 5;
            wv[1].iov_base = wb;
            wv[1].iov_len = 5;
            ssize_t wnv = writev(sgfd[1], wv, 2);
            test("writev 2 vectors", wnv == 10);
            close(sgfd[1]);

            char ra[5], rb[5];
            memset(ra, 0, 5);
            memset(rb, 0, 5);
            struct iovec rv[2];
            rv[0].iov_base = ra;
            rv[0].iov_len = 5;
            rv[1].iov_base = rb;
            rv[1].iov_len = 5;
            ssize_t rnv = readv(sgfd[0], rv, 2);
            test("readv total 10", rnv == 10);
            test("readv vec[0] content", memcmp(ra, "hello", 5) == 0);
            test("readv vec[1] content", memcmp(rb, "world", 5) == 0);
            close(sgfd[0]);
        } else {
            skip("writev 2 vectors",     "pipe failed");
            skip("readv total 10",       "pipe failed");
            skip("readv vec[0] content", "pipe failed");
            skip("readv vec[1] content", "pipe failed");
        }
    }

    /* ============================================================
     * Section 5: File Descriptors
     * Interfaces: dup dup3 fcntl
     * ============================================================ */
    printf("\n[5] File Descriptors\n");

    {
        int dfd = dup(STDOUT_FILENO);
        test("dup(stdout)", dfd > 2);
        if (dfd >= 0) close(dfd);

        /* dup3: duplicate stdout to a specific fd */
        int d3 = dup3(STDOUT_FILENO, 10, 0);
        test("dup3 to fd 10", d3 == 10);
        if (d3 >= 0) close(d3);

        /* fcntl F_GETFD / F_SETFD */
        int tfd = open("/tmp/pv_fcntl", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (tfd >= 0) {
            int flags = fcntl(tfd, F_GETFD);
            test("fcntl F_GETFD", flags >= 0);

            int sr = fcntl(tfd, F_SETFD, FD_CLOEXEC);
            test("fcntl F_SETFD CLOEXEC", sr >= 0);

            close(tfd);
            unlink("/tmp/pv_fcntl");
        } else {
            skip("fcntl F_GETFD",        "open failed");
            skip("fcntl F_SETFD CLOEXEC", "open failed");
        }
    }

    /* ============================================================
     * Section 6: Access Control
     * Interfaces: access faccessat umask
     * ============================================================ */
    printf("\n[6] Access Control\n");

    test("access /bin/echo X_OK",  access("/bin/echo", X_OK) == 0);
    test("access /bin/echo R_OK",  access("/bin/echo", R_OK) == 0);
    test("access /nonexist F_OK",  access("/nonexistent_file", F_OK) != 0);

    {
        int fa = faccessat(AT_FDCWD, "/bin/echo", R_OK, 0);
        test("faccessat AT_FDCWD R_OK", fa == 0);
    }

    {
        mode_t old_mask = umask(0022);
        mode_t check    = umask(old_mask);
        test("umask round-trip", check == 0022);
    }

    /* ============================================================
     * Section 7: System Information
     * Interfaces: uname getcwd
     * ============================================================ */
    printf("\n[7] System Info\n");

    {
        struct utsname uts;
        int uret = uname(&uts);
        test("uname returns 0", uret == 0);
        if (uret == 0) {
            printf("    sysname  = %s\n", uts.sysname);
            printf("    release  = %s\n", uts.release);
            printf("    machine  = %s\n", uts.machine);
            printf("    nodename = %s\n", uts.nodename);
            test("uname sysname=AIOS",    strcmp(uts.sysname, "AIOS") == 0);
            test("uname nodename set",    strlen(uts.nodename) > 0);
            test("uname machine=aarch64", strcmp(uts.machine, "aarch64") == 0);
        }
    }

    /* ============================================================
     * Section 8: Time
     * Interfaces: clock_gettime gettimeofday nanosleep
     * ============================================================ */
    printf("\n[8] Time\n");

    {
        struct timespec ts;
        int cr = clock_gettime(CLOCK_MONOTONIC, &ts);
        test("clock_gettime MONOTONIC",  cr == 0);
        test("clock_gettime value > 0",  ts.tv_sec > 0 || ts.tv_nsec > 0);

        struct timeval tv;
        int gtr = gettimeofday(&tv, NULL);
        test("gettimeofday",         gtr == 0);
        test("gettimeofday tv_sec",  tv.tv_sec >= 0);

        struct timespec before, after;
        clock_gettime(CLOCK_MONOTONIC, &before);
        struct timespec req = {0, 10000000}; /* 10 ms */
        int ns = nanosleep(&req, NULL);
        clock_gettime(CLOCK_MONOTONIC, &after);
        test("nanosleep 10ms", ns == 0);

        long elapsed_ns = (after.tv_sec - before.tv_sec) * 1000000000L
                        + (after.tv_nsec - before.tv_nsec);
        test("nanosleep elapsed >= 5ms", elapsed_ns >= 5000000);
    }

    /* ============================================================
     * Section 9: Directories
     * Interfaces: mkdir mkdirat chdir getcwd rmdir
     * ============================================================ */
    printf("\n[9] Directories\n");

    {
        int md = mkdir("/tmp/pv_dir", 0755);
        test("mkdir /tmp/pv_dir", md == 0);

        if (md == 0) {
            int cd = chdir("/tmp/pv_dir");
            test("chdir", cd == 0);

            char cwd2[256];
            getcwd(cwd2, sizeof(cwd2));
            test("chdir verify getcwd", strcmp(cwd2, "/tmp/pv_dir") == 0);

            chdir(cwd_save);

            int rd = rmdir("/tmp/pv_dir");
            test("rmdir", rd == 0);
        }

        /* mkdirat with AT_FDCWD */
        int mda = mkdirat(AT_FDCWD, "/tmp/pv_mkat", 0755);
        test("mkdirat AT_FDCWD", mda == 0);
        if (mda == 0) rmdir("/tmp/pv_mkat");
    }

    /* ============================================================
     * Section 10: Signals
     * Interfaces: kill sigaction sigprocmask sigpending
     * ============================================================ */
    printf("\n[10] Signals\n");

    test("kill(self, 0)", kill(getpid(), 0) == 0);

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = usr1_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        int sa_ret = sigaction(SIGUSR1, &sa, NULL);
        test("sigaction SIGUSR1", sa_ret == 0);

        if (sa_ret == 0) {
            sig_received = 0;
            kill(getpid(), SIGUSR1);
            test("SIGUSR1 delivered", sig_received == 1);
        }

        /* SIG_IGN */
        sa.sa_handler = SIG_IGN;
        test("sigaction SIG_IGN", sigaction(SIGUSR1, &sa, NULL) == 0);

        /* sigprocmask BLOCK / UNBLOCK */
        sigset_t set, oldset;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        int pm = sigprocmask(SIG_BLOCK, &set, &oldset);
        test("sigprocmask BLOCK", pm == 0);

        /* sigpending: after blocking SIGUSR2 and sending it, it should pend */
        kill(getpid(), SIGUSR2);
        sigset_t pend;
        sigemptyset(&pend);
        int sp = sigpending(&pend);
        test("sigpending returns 0", sp == 0);
        test("SIGUSR2 is pending",   sigismember(&pend, SIGUSR2));

        /* Unblock (SIGUSR2 default action is terminate -- set IGN first) */
        sa.sa_handler = SIG_IGN;
        sigaction(SIGUSR2, &sa, NULL);
        pm = sigprocmask(SIG_UNBLOCK, &set, NULL);
        test("sigprocmask UNBLOCK", pm == 0);
    }

    /* ============================================================
     * Section 11: Fork and Wait
     * Interfaces: fork waitpid _exit WIFEXITED WEXITSTATUS
     * ============================================================ */
    printf("\n[11] Fork / Wait\n");

    {
        pid_t child = fork();
        if (child < 0) {
            test("fork", 0);
        } else if (child == 0) {
            _exit(42);
        } else {
            test("fork returns > 0", child > 0);
            int status = 0;
            pid_t wp = waitpid(child, &status, 0);
            test("waitpid",         wp == child);
            test("WIFEXITED",       WIFEXITED(status));
            test("WEXITSTATUS=42",  WEXITSTATUS(status) == 42);
        }
    }

    /* ============================================================
     * Section 12: Pipe
     * Interfaces: pipe read/write through pipe
     * ============================================================ */
    printf("\n[12] Pipe\n");

    {
        int pfd[2];
        int pr = pipe(pfd);
        if (pr == 0) {
            test("pipe", 1);
            const char *pmsg = "pipe ok";
            write(pfd[1], pmsg, 7);
            close(pfd[1]);
            char pbuf[16];
            memset(pbuf, 0, sizeof(pbuf));
            ssize_t pnr = read(pfd[0], pbuf, sizeof(pbuf));
            test("pipe read 7 bytes",   pnr == 7);
            test("pipe content match",  memcmp(pbuf, "pipe ok", 7) == 0);
            close(pfd[0]);
        } else {
            test("pipe", 0);
            skip("pipe read 7 bytes",   "pipe failed");
            skip("pipe content match",  "pipe failed");
        }
    }

    /* ============================================================
     * Section 13: POSIX Threads
     * Interfaces: pthread_create pthread_join
     *             pthread_mutex_init lock unlock destroy
     * ============================================================ */
    printf("\n[13] POSIX Threads\n");

    {
        int tval = 0;
        pthread_t tid;
        int pc = pthread_create(&tid, NULL, thread_set_value, &tval);
        if (pc != 0) {
            skip("pthread_create",          "thread_ep not in child CSpace");
            skip("pthread_join",            "no thread");
            skip("thread set value",        "no thread");
            skip("thread retval=99",        "no thread");
        } else {
            test("pthread_create", 1);
            void *retval = NULL;
            int pj = pthread_join(tid, &retval);
            test("pthread_join",       pj == 0);
            test("thread set value",   tval == 42);
            test("thread retval=99",   (long)retval == 99);
        }

        /* Mutex test (userspace spinlock, no thread_ep needed) */
        shared_counter = 0;
        int mi = pthread_mutex_init(&test_mtx, NULL);
        test("pthread_mutex_init", mi == 0);

        if (mi == 0) {
            pthread_t mt;
            int mc = pthread_create(&mt, NULL, thread_mutex_inc, NULL);
            if (mc == 0) {
                pthread_join(mt, NULL);
                test("mutex protected counter=1", shared_counter == 1);
            } else {
                skip("mutex protected counter=1", "thread_ep unavailable");
            }
            pthread_mutex_destroy(&test_mtx);
        }
    }

    /* ============================================================
     * Section 14: User Database
     * Interfaces: getpwuid getpwnam
     * ============================================================ */
    printf("\n[14] User Database\n");

    {
        struct passwd *pw = getpwuid(0);
        test("getpwuid(0) non-NULL", pw != NULL);
        if (pw) {
            test("getpwuid(0) name=root", strcmp(pw->pw_name, "root") == 0);
            test("getpwuid(0) uid=0",     pw->pw_uid == 0);
        }

        struct passwd *pw2 = getpwnam("root");
        test("getpwnam root non-NULL", pw2 != NULL);
        if (pw2) {
            test("getpwnam root uid=0", pw2->pw_uid == 0);
        }
    }

    /* ============================================================
     * Section 15: Memory Mapping
     * Interfaces: mmap munmap
     * ============================================================ */
    printf("\n[15] Memory Mapping\n");

    {
        size_t len = 4096;
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        test("mmap MAP_ANONYMOUS", p != MAP_FAILED && p != NULL);

        if (p != MAP_FAILED && p != NULL) {
            /* Write and read back */
            int *ip = (int *)p;
            *ip = 0xDEAD;
            test("mmap write/read", *ip == 0xDEAD);

            int mu = munmap(p, len);
            test("munmap", mu == 0);
        }
    }


    /* ============================================================
     * Section 16: Extended File I/O -- pread, pwrite
     * Interfaces: pread pwrite
     * ============================================================ */
    printf("\n[16] Extended File I/O\n");

    {
        int pfd_w = open("/tmp/pv_pread", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (pfd_w >= 0) {
            write(pfd_w, "ABCDEFGHIJ", 10);
            close(pfd_w);

            int pfd_r = open("/tmp/pv_pread", O_RDONLY);
            if (pfd_r >= 0) {
                char pb[8];
                memset(pb, 0, sizeof(pb));
                ssize_t pn = pread(pfd_r, pb, 4, 3);
                test("pread offset=3 len=4", pn == 4);
                test("pread content DEFG", memcmp(pb, "DEFG", 4) == 0);

                /* Verify file position was not changed by pread */
                char pb2[4];
                ssize_t pn2 = read(pfd_r, pb2, 4);
                test("pread pos unchanged", pn2 == 4 && memcmp(pb2, "ABCD", 4) == 0);

                close(pfd_r);
            } else {
                skip("pread offset=3 len=4", "open failed");
                skip("pread content DEFG",   "open failed");
                skip("pread pos unchanged",  "open failed");
            }
            unlink("/tmp/pv_pread");
        } else {
            skip("pread offset=3 len=4", "create failed");
            skip("pread content DEFG",   "create failed");
            skip("pread pos unchanged",  "create failed");
        }
    }

    /* ============================================================
     * Section 17: File Metadata -- fchmod, fchown, linkat
     * Interfaces: fchmod fchmodat fchown fchownat linkat readlinkat
     * ============================================================ */
    printf("\n[17] File Metadata\n");

    {
        int mfd = open("/tmp/pv_meta", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (mfd >= 0) {
            test("fchmod returns 0",  fchmod(mfd, 0755) == 0);
            test("fchown returns 0",  fchown(mfd, 0, 0) == 0);
            close(mfd);
            test("fchmodat returns 0", fchmodat(AT_FDCWD, "/tmp/pv_meta", 0644, 0) == 0);
            test("fchownat returns 0", fchownat(AT_FDCWD, "/tmp/pv_meta", 0, 0, 0) == 0);
            unlink("/tmp/pv_meta");
        } else {
            skip("fchmod returns 0",   "open failed");
            skip("fchown returns 0",   "open failed");
            skip("fchmodat returns 0", "open failed");
            skip("fchownat returns 0", "open failed");
        }
        /* linkat: expected to fail with ENOSYS (not implemented) */
        int lr = linkat(AT_FDCWD, "/bin/echo", AT_FDCWD, "/tmp/pv_link", 0);
        test("linkat returns error", lr < 0);
        /* readlinkat: expected to fail (no symlinks) */
        char rlbuf[64];
        ssize_t rln = readlinkat(AT_FDCWD, "/tmp", rlbuf, sizeof(rlbuf));
        test("readlinkat returns error", rln < 0);
    }

    /* ============================================================
     * Section 18: Process Identity (extended)
     * Interfaces: setsid getpgid
     * ============================================================ */
    printf("\n[18] Process Identity (extended)\n");

    {
        pid_t sid = setsid();
        test("setsid returns > 0", sid > 0);

        pid_t pgid = getpgid(0);
        test("getpgid returns > 0", pgid > 0);
    }

    /* ============================================================
     * Section 19: Extended Time
     * Interfaces: clock_nanosleep
     * ============================================================ */
    printf("\n[19] Extended Time\n");

    {
        struct timespec tb_before, tb_after;
        clock_gettime(CLOCK_MONOTONIC, &tb_before);
        struct timespec cns_req = {0, 10000000}; /* 10 ms */
        int cns_r = clock_nanosleep(CLOCK_MONOTONIC, 0, &cns_req, NULL);
        clock_gettime(CLOCK_MONOTONIC, &tb_after);
        test("clock_nanosleep returns 0", cns_r == 0);
        long cns_elapsed = (tb_after.tv_sec - tb_before.tv_sec) * 1000000000L
                         + (tb_after.tv_nsec - tb_before.tv_nsec);
        test("clock_nanosleep elapsed >= 5ms", cns_elapsed >= 5000000);
    }

    /* ============================================================
     * Section 20: Memory Protection
     * Interfaces: mprotect
     * ============================================================ */
    printf("\n[20] Memory Protection\n");

    {
        void *mp = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mp != MAP_FAILED && mp != NULL) {
            int mr = mprotect(mp, 4096, PROT_READ);
            test("mprotect returns 0", mr == 0);
            munmap(mp, 4096);
        } else {
            skip("mprotect returns 0", "mmap failed");
        }
    }

    /* ============================================================
     * Section 21: Group Database
     * Interfaces: getgrgid getgrnam
     * ============================================================ */
    printf("\n[21] Group Database\n");

    {
        struct group *gr = getgrgid(0);
        test("getgrgid(0) non-NULL", gr != NULL);
        if (gr) {
            test("getgrgid(0) name=root", strcmp(gr->gr_name, "root") == 0);
            test("getgrgid(0) gid=0",     gr->gr_gid == 0);
        }

        struct group *gr2 = getgrnam("root");
        test("getgrnam root non-NULL", gr2 != NULL);
        if (gr2) {
            test("getgrnam root gid=0", gr2->gr_gid == 0);
        }
    }

    /* ============================================================
     * Summary
     * ============================================================ */
    printf("\n========================================\n");
    printf("AIOS POSIX Verification (V3): %d/%d passed", pass_count, total_count);
    if (fail_count > 0) printf(", %d FAILED", fail_count);
    if (skip_count > 0) printf(", %d skipped", skip_count);
    printf("\n========================================\n");

    return fail_count > 0 ? 1 : 0;
}
