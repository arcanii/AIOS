/*
 * posix_thread.c -- AIOS POSIX pthread + passwd + group wrappers
 * pthread_create/join/exit/detach, pthread_mutex_*,
 * getpwuid, getpwnam
 */
#include "posix_internal.h"
#include <grp.h>

static struct passwd _pw_buf;
static char _pw_name[32];
static char _pw_dir[64];
static char _pw_shell[64];

/* Unpack a string from MRs starting at given index, return next MR index */
static int _unpack_mr_string(int start_mr, char *buf, int maxlen) {
    seL4_Word slen = seL4_GetMR(start_mr);
    int len = (int)slen;
    if (len > maxlen - 1) len = maxlen - 1;
    int mr = start_mr + 1;
    for (int i = 0; i < len; i++) {
        if (i % 8 == 0 && i > 0) mr++;
        buf[i] = (char)((seL4_GetMR(mr) >> ((i % 8) * 8)) & 0xFF);
    }
    buf[len] = '\0';
    return start_mr + 1 + (len + 7) / 8;
}

struct passwd *__wrap_getpwuid(uid_t uid) {
    if (!auth_ep) {
        /* Fallback: return minimal entry from stored uid */
        _pw_buf.pw_uid = aios_uid;
        _pw_buf.pw_gid = aios_gid;
        _pw_buf.pw_name = "root";
        _pw_buf.pw_passwd = "x";
        _pw_buf.pw_gecos = "";
        _pw_buf.pw_dir = "/";
        _pw_buf.pw_shell = "/bin/sh";
        return &_pw_buf;
    }

    seL4_SetMR(0, (seL4_Word)uid);
    seL4_Call(auth_ep, seL4_MessageInfo_new(51, 0, 0, 1));

    if (seL4_GetMR(0) != 0) return NULL;

    _pw_buf.pw_uid = uid;
    _pw_buf.pw_gid = (gid_t)seL4_GetMR(1);
    int mr = _unpack_mr_string(2, _pw_name, 32);
    mr = _unpack_mr_string(mr, _pw_dir, 64);
    _unpack_mr_string(mr, _pw_shell, 64);

    _pw_buf.pw_name = _pw_name;
    _pw_buf.pw_passwd = "x";
    _pw_buf.pw_gecos = _pw_name;
    _pw_buf.pw_dir = _pw_dir;
    _pw_buf.pw_shell = _pw_shell;
    return &_pw_buf;
}

struct passwd *__wrap_getpwnam(const char *name) {
    /* Simple approach: try uid 0 and 1000 (common defaults) */
    /* A proper implementation would need a NAME_LOOKUP IPC */
    struct passwd *pw;
    pw = __wrap_getpwuid(0);
    if (pw && pw->pw_name[0]) {
        int match = 1;
        const char *a = pw->pw_name, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) return pw;
    }
    for (uid_t uid = 1000; uid < 1016; uid++) {
        pw = __wrap_getpwuid(uid);
        if (pw && pw->pw_name[0]) {
            int match = 1;
            const char *a = pw->pw_name, *b = name;
            while (*a && *b && *a == *b) { a++; b++; }
            if (*a == *b) return pw;
        }
    }
    return NULL;
}

/* ---- pthreads (IPC to thread_server + userspace spinlocks) ---- */

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg) {
    (void)attr;
    if (!thread_ep) return 11; /* EAGAIN — no thread server */

    seL4_SetMR(0, (seL4_Word)(uintptr_t)start_routine);
    seL4_SetMR(1, (seL4_Word)(uintptr_t)arg);
    seL4_MessageInfo_t reply = seL4_Call(thread_ep,
        seL4_MessageInfo_new(AIOS_THREAD_CREATE, 0, 0, 2));

    seL4_Word tid = seL4_GetMR(0);
    if ((long)tid <= 0) return 11; /* EAGAIN */
    if (thread) *thread = (pthread_t)tid;
    return 0;
}

int __wrap_pthread_join(pthread_t th, void **retval) {
    if (!thread_ep) return 3; /* ESRCH */

    seL4_SetMR(0, (seL4_Word)th);
    seL4_Call(thread_ep,
        seL4_MessageInfo_new(AIOS_THREAD_JOIN, 0, 0, 1));

    if (retval) *retval = (void *)(uintptr_t)seL4_GetMR(1);
    return (int)(long)seL4_GetMR(0);
}

void __wrap_pthread_exit(void *retval) {
    (void)retval;
    /* Trigger VM fault — thread_server catches on fault ep */
    volatile int *null_ptr = (volatile int *)0;
    *null_ptr = 0;
    __builtin_unreachable();
}

int __wrap_pthread_detach(pthread_t th) {
    (void)th;
    return 0; /* stub — detached threads still cleaned up on process exit */
}

int __wrap_pthread_mutex_init(pthread_mutex_t *mutex,
                               const pthread_mutexattr_t *attr) {
    (void)attr;
    if (!mutex) return 22; /* EINVAL */
    /* Zero the struct — __lock is the first int field */
    char *p = (char *)mutex;
    for (int i = 0; i < (int)sizeof(pthread_mutex_t); i++) p[i] = 0;
    return 0;
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return 22;
    volatile int *lock = (volatile int *)mutex;
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) {
        seL4_Yield(); /* yield to avoid burning CPU at equal priority */
    }
    return 0;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return 22;
    volatile int *lock = (volatile int *)mutex;
    __atomic_clear(lock, __ATOMIC_RELEASE);
    return 0;
}

int __wrap_pthread_mutex_destroy(pthread_mutex_t *mutex) {
    (void)mutex;
    return 0;
}

/* ---- Group database (v0.4.62) ---- */

static struct group _gr_buf;
static char _gr_name[32];

struct group *__wrap_getgrgid(gid_t gid) {
    _gr_buf.gr_gid = gid;
    if (gid == 0) {
        _gr_name[0] = 'r'; _gr_name[1] = 'o'; _gr_name[2] = 'o';
        _gr_name[3] = 't'; _gr_name[4] = 0;
    } else {
        /* Format GID as decimal string */
        int v = (int)gid, pos = 0;
        char tmp[16];
        if (v == 0) { tmp[pos++] = '0'; }
        else { while (v > 0 && pos < 15) { tmp[pos++] = '0' + (v % 10); v /= 10; } }
        for (int i = 0; i < pos; i++) _gr_name[i] = tmp[pos - 1 - i];
        _gr_name[pos] = 0;
    }
    _gr_buf.gr_name   = _gr_name;
    _gr_buf.gr_passwd = "x";
    _gr_buf.gr_mem    = (char **)0;
    return &_gr_buf;
}

struct group *__wrap_getgrnam(const char *name) {
    if (!name) return (struct group *)0;
    /* Match "root" -> gid 0 */
    if (name[0] == 'r' && name[1] == 'o' && name[2] == 'o'
        && name[3] == 't' && name[4] == 0) {
        return __wrap_getgrgid(0);
    }
    /* Default: return gid 0 with requested name */
    _gr_buf.gr_gid = 0;
    str_copy(_gr_name, name, sizeof(_gr_name));
    _gr_buf.gr_name   = _gr_name;
    _gr_buf.gr_passwd = "x";
    _gr_buf.gr_mem    = (char **)0;
    return &_gr_buf;
}
