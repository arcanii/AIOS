#include "aios.h"

static aios_syscalls_t *sys_ptr;
static int pass_count = 0;
static int fail_count = 0;

static void ok(const char *name) { puts("  [PASS] "); puts(name); puts("\n"); pass_count++; }
static void fail_msg(const char *name) { puts("  [FAIL] "); puts(name); puts("\n"); fail_count++; }
static void check(const char *name, int cond) { if (cond) ok(name); else fail_msg(name); }

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    sys_ptr = _sys;

    puts("=== test_fileio: file I/O + directories ===\n\n");

    /* File create/write/read */
    puts("--- File I/O ---\n");
    {
        int fd = (sys_ptr->open_flags)("/tmp/iotest.txt", 0x0041);
        check("create file", fd >= 0);
        if (fd >= 0) {
            const char *msg = "file io test data\n";
            int w = (sys_ptr->write_file)(fd, msg, 18);
            check("write_file", w > 0);
            (sys_ptr->close)(fd);
        }
        fd = (sys_ptr->open)("/tmp/iotest.txt");
        check("open read", fd >= 0);
        if (fd >= 0) {
            char buf[64];
            int r = (sys_ptr->read)(fd, buf, 63);
            check("read > 0", r > 0);
            if (r > 0) {
                buf[r] = 0;
                check("read content matches", buf[0] == 'f');
            }
            (sys_ptr->close)(fd);
        }
        {
            unsigned long sz = 0;
            int r = (sys_ptr->stat_file)("/tmp/iotest.txt", &sz);
            check("stat_file", r == 0 && sz > 0);
        }
        (sys_ptr->unlink)("/tmp/iotest.txt");
    }

    /* Directories */
    puts("\n--- Directories ---\n");
    {
        int r = (sys_ptr->mkdir)("/tmp/testdir");
        check("mkdir", r == 0);
        r = (sys_ptr->rmdir)("/tmp/testdir");
        check("rmdir", r == 0);
    }

    puts("\n=== test_fileio: ");
    put_dec(pass_count); puts(" passed, ");
    put_dec(fail_count); puts(" failed ===\n");
    return fail_count > 0 ? 1 : 0;
}
