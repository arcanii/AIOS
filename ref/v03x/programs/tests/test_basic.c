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

    puts("=== test_basic: syscalls + memory + strings ===\n\n");

    /* Syscalls */
    puts("--- Syscalls ---\n");
    check("getpid > 0", (sys_ptr->getpid)() > 0);
    check("getuid >= 0", (sys_ptr->getuid)() >= 0);
    check("getgid >= 0", (sys_ptr->getgid)() >= 0);
    {
        char buf[64];
        (sys_ptr->getcwd)(buf, sizeof(buf));
        check("getcwd returns path", buf[0] == '/');
    }
    {
        long t = (sys_ptr->time)();
        check("time() > 0", t > 0);
    }

    /* Memory */
    puts("\n--- Memory ---\n");
    {
        void *p = malloc(128);
        check("malloc(128) != NULL", p != (void *)0);
        memset(p, 0xAB, 128);
        unsigned char *cp = (unsigned char *)p;
        check("memset verified", cp[0] == 0xAB && cp[127] == 0xAB);
        char *dst = (char *)malloc(32);
        memcpy(dst, "hello", 6);
        check("memcpy verified", dst[0] == 'h' && dst[4] == 'o');
    }

    /* Strings */
    puts("\n--- Strings ---\n");
    check("strlen", strlen("abc") == 3);
    check("strcmp equal", strcmp("foo", "foo") == 0);
    check("strcmp less", strcmp("abc", "abd") < 0);
    {
        char buf[16];
        strcpy(buf, "test");
        check("strcpy", buf[0] == 't' && buf[3] == 't' && buf[4] == 0);
    }

    puts("\n=== test_basic: ");
    put_dec(pass_count); puts(" passed, ");
    put_dec(fail_count); puts(" failed ===\n");
    return fail_count > 0 ? 1 : 0;
}
