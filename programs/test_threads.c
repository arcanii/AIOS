#include "aios.h"

/* ============================================================
 * test_threads.c - Comprehensive pthread + system test suite
 *
 * Tests: thread create/join, mutex, cond var, rwlock, TLS,
 *        basic I/O, process info, memory, scheduler preemption
 *
 * Uses aios.h macros for standard calls (puts, malloc, etc.)
 * Uses sys_ptr for pthread calls (no conflicting macros)
 * ============================================================ */

/* sys_ptr must be file-scope so thread functions can use it */
static aios_syscalls_t *sys_ptr;

static int pass_count = 0;
static int fail_count = 0;

static void ok(const char *name) {
    puts("  [PASS] "); puts(name); puts("\n");
    pass_count++;
}
static void fail_msg(const char *name) {
    puts("  [FAIL] "); puts(name); puts("\n");
    fail_count++;
}
static void check(const char *name, int cond) {
    if (cond) ok(name); else fail_msg(name);
}

/* ---- Thread functions (use sys_ptr for pthread, macros for I/O) ---- */

static volatile int thread_ran = 0;

static void *simple_thread(void *arg) {
    int val = (int)(unsigned long)arg;
    thread_ran = val;
    return (void *)(unsigned long)(val * 2);
}

/* Mutex contention */
static int mutex_buf[4];
static volatile int shared_counter = 0;

static void *mutex_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) {
        sys_ptr->pthread_mutex_lock(mutex_buf);
        shared_counter++;
        sys_ptr->pthread_mutex_unlock(mutex_buf);
    }
    return (void *)0;
}

/* Condition variable */
static int cond_mtx[4];
static int cond_var[4];
static volatile int cond_ready = 0;
static volatile int cond_value = 0;

static void *cond_producer(void *arg) {
    (void)arg;
    sys_ptr->pthread_mutex_lock(cond_mtx);
    cond_value = 42;
    cond_ready = 1;
    sys_ptr->pthread_cond_signal(cond_var);
    sys_ptr->pthread_mutex_unlock(cond_mtx);
    return (void *)0;
}

/* RWLock */
static int rwlock_buf[4];
static volatile int rw_shared = 0;

static void *rw_reader(void *arg) {
    (void)arg;
    sys_ptr->pthread_rwlock_rdlock(rwlock_buf);
    int val = rw_shared;
    sys_ptr->pthread_rwlock_unlock(rwlock_buf);
    return (void *)(unsigned long)val;
}

static void *rw_writer(void *arg) {
    int val = (int)(unsigned long)arg;
    sys_ptr->pthread_rwlock_wrlock(rwlock_buf);
    rw_shared = val;
    sys_ptr->pthread_rwlock_unlock(rwlock_buf);
    return (void *)0;
}

/* Preemption */
static volatile int preempt_a = 0;
static volatile int preempt_b = 0;

static void *count_a(void *arg) {
    (void)arg;
    for (int i = 0; i < 500; i++) preempt_a++;
    return (void *)0;
}
static void *count_b(void *arg) {
    (void)arg;
    for (int i = 0; i < 500; i++) preempt_b++;
    return (void *)0;
}

/* ---- Entry point ---- */

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    sys_ptr = _sys;

    puts("============================================================\n");
    puts("  AIOS Comprehensive Test Suite\n");
    puts("============================================================\n\n");

    /* ---- 1. Basic Syscalls ---- */
    puts("--- 1. Basic Syscalls ---\n");
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

    /* ---- 2. Memory ---- */
    puts("\n--- 2. Memory ---\n");
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

    /* ---- 3. Strings ---- */
    puts("\n--- 3. Strings ---\n");
    check("strlen", strlen("abc") == 3);
    check("strcmp equal", strcmp("foo", "foo") == 0);
    check("strcmp less", strcmp("abc", "abd") < 0);
    {
        char buf[16];
        strcpy(buf, "test");
        check("strcpy", buf[0] == 't' && buf[3] == 't' && buf[4] == 0);
    }

    /* ---- 4. File I/O ---- */
    puts("\n--- 4. File I/O ---\n");
    {
        int fd = (sys_ptr->open_flags)("/tmp/ttest.txt", 0x0041);
        check("create file", fd >= 0);
        if (fd >= 0) {
            const char *msg = "thread test data\n";
            int w = (sys_ptr->write_file)(fd, msg, 17);
            check("write_file", w > 0);
            (sys_ptr->close)(fd);
        }
        fd = (sys_ptr->open)("/tmp/ttest.txt");
        check("open read", fd >= 0);
        if (fd >= 0) {
            char buf[64];
            int r = (sys_ptr->read)(fd, buf, 63);
            check("read > 0", r > 0);
            if (r > 0) {
                buf[r] = 0;
                check("read content", buf[0] == 't');
            }
            (sys_ptr->close)(fd);
        }
        {
            unsigned long sz = 0;
            int r = (sys_ptr->stat_file)("/tmp/ttest.txt", &sz);
            check("stat_file", r == 0 && sz > 0);
        }
        (sys_ptr->unlink)("/tmp/ttest.txt");
    }

    /* ---- 5. Directory Ops ---- */
    puts("\n--- 5. Directory Ops ---\n");
    {
        int r = (sys_ptr->mkdir)("/tmp/testdir");
        check("mkdir", r == 0);
        r = (sys_ptr->rmdir)("/tmp/testdir");
        check("rmdir", r == 0);
    }

    /* ---- 6. Thread Create + Join ---- */
    puts("\n--- 6. Thread Create/Join ---\n");
    if (!sys_ptr->pthread_create) {
        fail_msg("pthread_create not in syscall table");
    } else {
        unsigned long tid = 0;
        thread_ran = 0;
        int r = sys_ptr->pthread_create(&tid, (void *)0, simple_thread, (void *)42UL);
        check("pthread_create returns 0", r == 0);
        check("thread id assigned", tid > 0);
        void *retval = (void *)0;
        r = sys_ptr->pthread_join(tid, &retval);
        check("pthread_join returns 0", r == 0);
        check("thread ran (flag=42)", thread_ran == 42);
        check("thread retval (84)", (unsigned long)retval == 84);
    }

    /* ---- 7. Mutex ---- */
    puts("\n--- 7. Mutex Contention ---\n");
    if (!sys_ptr->pthread_mutex_init) {
        fail_msg("pthread_mutex_init not in syscall table");
    } else {
        sys_ptr->pthread_mutex_init(mutex_buf, (void *)0);
        shared_counter = 0;
        unsigned long t1, t2;
        sys_ptr->pthread_create(&t1, (void *)0, mutex_thread, (void *)0);
        sys_ptr->pthread_create(&t2, (void *)0, mutex_thread, (void *)0);
        sys_ptr->pthread_join(t1, (void *)0);
        sys_ptr->pthread_join(t2, (void *)0);
        puts("  shared_counter = "); put_dec(shared_counter); puts("\n");
        check("mutex: counter == 200", shared_counter == 200);
        sys_ptr->pthread_mutex_destroy(mutex_buf);
    }

    /* ---- 8. Condition Variable ---- */
    puts("\n--- 8. Condition Variable ---\n");
    if (!sys_ptr->pthread_cond_init) {
        fail_msg("pthread_cond_init not in syscall table");
    } else {
        sys_ptr->pthread_mutex_init(cond_mtx, (void *)0);
        sys_ptr->pthread_cond_init(cond_var, (void *)0);
        cond_ready = 0;
        cond_value = 0;
        unsigned long prod_tid;
        sys_ptr->pthread_create(&prod_tid, (void *)0, cond_producer, (void *)0);
        sys_ptr->pthread_mutex_lock(cond_mtx);
        while (!cond_ready) {
            sys_ptr->pthread_cond_wait(cond_var, cond_mtx);
        }
        int got = cond_value;
        sys_ptr->pthread_mutex_unlock(cond_mtx);
        sys_ptr->pthread_join(prod_tid, (void *)0);
        check("cond: received value 42", got == 42);
    }

    /* ---- 9. Read-Write Lock ---- */
    puts("\n--- 9. Read-Write Lock ---\n");
    if (!sys_ptr->pthread_rwlock_init) {
        fail_msg("pthread_rwlock_init not in syscall table");
    } else {
        sys_ptr->pthread_rwlock_init(rwlock_buf, (void *)0);
        rw_shared = 0;
        unsigned long wtid;
        sys_ptr->pthread_create(&wtid, (void *)0, rw_writer, (void *)99UL);
        sys_ptr->pthread_join(wtid, (void *)0);
        check("rwlock: writer set 99", rw_shared == 99);
        unsigned long rtid;
        sys_ptr->pthread_create(&rtid, (void *)0, rw_reader, (void *)0);
        void *rval;
        sys_ptr->pthread_join(rtid, &rval);
        check("rwlock: reader got 99", (unsigned long)rval == 99);
    }

    /* ---- 10. Thread-Local Storage ---- */
    puts("\n--- 10. Thread-Local Storage ---\n");
    if (!sys_ptr->pthread_key_create) {
        fail_msg("pthread_key_create not in syscall table");
    } else {
        unsigned int key = 0;
        int r = sys_ptr->pthread_key_create(&key, (void (*)(void *))0);
        check("key_create returns 0", r == 0);
        sys_ptr->pthread_setspecific(key, (void *)12345UL);
        void *val = sys_ptr->pthread_getspecific(key);
        check("TLS set/get", (unsigned long)val == 12345);
    }

    /* ---- 11. Preemptive Scheduling ---- */
    puts("\n--- 11. Preemptive Scheduling ---\n");
    {
        preempt_a = 0;
        preempt_b = 0;
        unsigned long ta, tb;
        sys_ptr->pthread_create(&ta, (void *)0, count_a, (void *)0);
        sys_ptr->pthread_create(&tb, (void *)0, count_b, (void *)0);
        sys_ptr->pthread_join(ta, (void *)0);
        sys_ptr->pthread_join(tb, (void *)0);
        puts("  thread_a count = "); put_dec(preempt_a); puts("\n");
        puts("  thread_b count = "); put_dec(preempt_b); puts("\n");
        check("both threads ran to 500", preempt_a == 500 && preempt_b == 500);
    }

    /* ---- 12. Process Info ---- */
    puts("\n--- 12. Process Info ---\n");
    {
        int pid = (sys_ptr->getpid)();
        int ppid = (sys_ptr->getppid)();
        puts("  pid="); put_dec(pid); puts(" ppid="); put_dec(ppid); puts("\n");
        check("pid > 0", pid > 0);
        check("ppid >= 0", ppid >= 0);
    }

    /* ---- 13. sched_yield ---- */
    puts("\n--- 13. sched_yield ---\n");
    if (sys_ptr->sched_yield) {
        sys_ptr->sched_yield();
        ok("sched_yield returned");
    } else {
        fail_msg("sched_yield not in syscall table");
    }

    /* ---- Summary ---- */
    puts("\n============================================================\n");
    puts("  Results: ");
    put_dec(pass_count); puts(" passed, ");
    put_dec(fail_count); puts(" failed, ");
    put_dec(pass_count + fail_count); puts(" total\n");
    puts("============================================================\n");

    return fail_count > 0 ? 1 : 0;
}
