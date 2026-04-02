#include "aios.h"

static aios_syscalls_t *sys_ptr;
static int pass_count = 0;
static int fail_count = 0;

static void ok(const char *name) { puts("  [PASS] "); puts(name); puts("\n"); pass_count++; }
static void fail_msg(const char *name) { puts("  [FAIL] "); puts(name); puts("\n"); fail_count++; }
static void check(const char *name, int cond) { if (cond) ok(name); else fail_msg(name); }

/* ---- Thread functions ---- */
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

static void *count_a(void *arg) { (void)arg; for (int i = 0; i < 500; i++) preempt_a++; return (void *)0; }
static void *count_b(void *arg) { (void)arg; for (int i = 0; i < 500; i++) preempt_b++; return (void *)0; }

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;
    sys_ptr = _sys;

    puts("=== test_threads: pthreads + scheduling ===\n\n");

    /* Create + Join */
    puts("--- Create/Join ---\n");
    {
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

    /* Mutex */
    puts("\n--- Mutex ---\n");
    {
        sys_ptr->pthread_mutex_init(mutex_buf, (void *)0);
        shared_counter = 0;
        unsigned long t1, t2;
        sys_ptr->pthread_create(&t1, (void *)0, mutex_thread, (void *)0);
        sys_ptr->pthread_create(&t2, (void *)0, mutex_thread, (void *)0);
        sys_ptr->pthread_join(t1, (void *)0);
        sys_ptr->pthread_join(t2, (void *)0);
        puts("  counter = "); put_dec(shared_counter); puts("\n");
        check("mutex: counter == 200", shared_counter == 200);
        sys_ptr->pthread_mutex_destroy(mutex_buf);
    }

    /* Condition Variable */
    puts("\n--- Condvar ---\n");
    {
        sys_ptr->pthread_mutex_init(cond_mtx, (void *)0);
        sys_ptr->pthread_cond_init(cond_var, (void *)0);
        cond_ready = 0;
        cond_value = 0;
        unsigned long prod_tid;
        sys_ptr->pthread_create(&prod_tid, (void *)0, cond_producer, (void *)0);
        sys_ptr->pthread_mutex_lock(cond_mtx);
        while (!cond_ready) sys_ptr->pthread_cond_wait(cond_var, cond_mtx);
        int got = cond_value;
        sys_ptr->pthread_mutex_unlock(cond_mtx);
        sys_ptr->pthread_join(prod_tid, (void *)0);
        check("cond: received 42", got == 42);
    }

    /* RWLock */
    puts("\n--- RWLock ---\n");
    {
        sys_ptr->pthread_rwlock_init(rwlock_buf, (void *)0);
        rw_shared = 0;
        unsigned long wtid;
        sys_ptr->pthread_create(&wtid, (void *)0, rw_writer, (void *)99UL);
        sys_ptr->pthread_join(wtid, (void *)0);
        check("writer set 99", rw_shared == 99);
        unsigned long rtid;
        sys_ptr->pthread_create(&rtid, (void *)0, rw_reader, (void *)0);
        void *rval;
        sys_ptr->pthread_join(rtid, &rval);
        check("reader got 99", (unsigned long)rval == 99);
    }

    /* TLS */
    puts("\n--- TLS ---\n");
    {
        unsigned int key = 0;
        int r = sys_ptr->pthread_key_create(&key, (void (*)(void *))0);
        check("key_create returns 0", r == 0);
        sys_ptr->pthread_setspecific(key, (void *)12345UL);
        void *val = sys_ptr->pthread_getspecific(key);
        check("TLS set/get", (unsigned long)val == 12345);
    }

    /* Preemption */
    puts("\n--- Preemption ---\n");
    {
        preempt_a = 0;
        preempt_b = 0;
        unsigned long ta, tb;
        sys_ptr->pthread_create(&ta, (void *)0, count_a, (void *)0);
        sys_ptr->pthread_create(&tb, (void *)0, count_b, (void *)0);
        sys_ptr->pthread_join(ta, (void *)0);
        sys_ptr->pthread_join(tb, (void *)0);
        puts("  a="); put_dec(preempt_a); puts(" b="); put_dec(preempt_b); puts("\n");
        check("both threads ran to 500", preempt_a == 500 && preempt_b == 500);
    }

    /* Yield */
    puts("\n--- Yield ---\n");
    if (sys_ptr->sched_yield) {
        (sys_ptr->sched_yield)();
        ok("sched_yield returned");
    } else {
        fail_msg("sched_yield not in syscall table");
    }

    puts("\n=== test_threads: ");
    put_dec(pass_count); puts(" passed, ");
    put_dec(fail_count); puts(" failed ===\n");
    return fail_count > 0 ? 1 : 0;
}
