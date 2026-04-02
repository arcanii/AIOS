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

    puts("=== test_signals: kill + signal handlers ===\n\n");

    /* kill_proc API */
    puts("--- kill ---\n");
    if (sys_ptr->kill_proc) {
        /* Invalid PID */
        int r = (sys_ptr->kill_proc)(9999, 15);
        check("kill nonexistent pid returns -1", r == -1);

        /* Invalid signal */
        int my_pid = (sys_ptr->getpid)();
        r = (sys_ptr->kill_proc)(my_pid, 0);
        check("kill with sig 0 returns -1", r == -1);
        r = (sys_ptr->kill_proc)(my_pid, 99);
        check("kill with sig 99 returns -1", r == -1);
    } else {
        fail_msg("kill_proc not in syscall table");
    }

    /* Signal handler registration */
    puts("\n--- signal handler ---\n");
    if (sys_ptr->signal_handler) {
        /* Register SIG_IGN for SIGTERM */
        unsigned long old = (sys_ptr->signal_handler)(15, 1);
        check("signal(SIGTERM, SIG_IGN) returns SIG_DFL=0", old == 0);

        /* Restore default */
        unsigned long prev = (sys_ptr->signal_handler)(15, 0);
        check("signal(SIGTERM, SIG_DFL) returns SIG_IGN=1", prev == 1);

        /* SIGKILL cannot be caught */
        unsigned long err = (sys_ptr->signal_handler)(9, 1);
        check("signal(SIGKILL) returns SIG_ERR", err == (unsigned long)(-1));

        /* SIGSTOP cannot be caught */
        err = (sys_ptr->signal_handler)(19, 1);
        check("signal(SIGSTOP) returns SIG_ERR", err == (unsigned long)(-1));
    } else {
        fail_msg("signal_handler not in syscall table");
    }

    /* Self-kill with ignored signal */
    puts("\n--- ignored signal ---\n");
    if (sys_ptr->kill_proc && sys_ptr->signal_handler) {
        int my_pid = (sys_ptr->getpid)();
        /* Set SIGTERM to SIG_IGN */
        (sys_ptr->signal_handler)(15, 1);
        int r = (sys_ptr->kill_proc)(my_pid, 15);
        check("kill self with ignored SIGTERM", r == 0);
        check("still alive after ignored signal", 1);
        /* Restore */
        (sys_ptr->signal_handler)(15, 0);
    }

    puts("\n=== test_signals: ");
    put_dec(pass_count); puts(" passed, ");
    put_dec(fail_count); puts(" failed ===\n");
    return fail_count > 0 ? 1 : 0;
}
