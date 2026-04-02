/* pthread.c - AIOS POSIX Threads stubs
 *
 * Single-threaded stubs. Mutexes/conds/rwlocks are no-ops (safe).
 * Thread creation returns ENOSYS. TLS via static array.
 * TODO: Real threads need seL4 TCB + Microkit scheduling.
 */

#include <pthread.h>

#ifndef ENOSYS
#define ENOSYS 38
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

/* ---- Thread management ---- */

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    (void)thread; (void)attr; (void)start_routine; (void)arg;
    return ENOSYS;
}

int pthread_join(pthread_t thread, void **retval)
{
    (void)thread; (void)retval;
    return ENOSYS;
}

int pthread_detach(pthread_t thread)
{
    (void)thread;
    return ENOSYS;
}

void pthread_exit(void *retval)
{
    (void)retval;
    while (1) { }
}

/* ---- Mutex (no-ops, single-threaded safe) ---- */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    if (!mutex) return EINVAL;
    mutex->__lock = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;
    mutex->__lock = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;
    mutex->__lock = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    if (!mutex) return EINVAL;
    mutex->__lock = 0;
    return 0;
}

/* ---- Condition variables (no-ops, single-threaded safe) ---- */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    (void)attr;
    if (!cond) return EINVAL;
    cond->__cond = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    (void)cond; (void)mutex;
    return ENOSYS;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

/* ---- Read-write locks (no-ops, single-threaded safe) ---- */

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr)
{
    (void)attr;
    if (!rwlock) return EINVAL;
    rwlock->__readers = 0;
    rwlock->__writer = 0;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    rwlock->__readers++;
    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    rwlock->__writer = 1;
    return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
    if (!rwlock) return EINVAL;
    if (rwlock->__writer)
        rwlock->__writer = 0;
    else if (rwlock->__readers > 0)
        rwlock->__readers--;
    return 0;
}

/* ---- Thread-local storage ---- */

#define PTHREAD_KEYS_MAX 128
static const void *_tls_values[PTHREAD_KEYS_MAX];
static int _tls_next_key = 0;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    (void)destructor;
    if (!key) return EINVAL;
    if (_tls_next_key >= PTHREAD_KEYS_MAX) return ENOMEM;
    *key = (pthread_key_t)_tls_next_key++;
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;
    _tls_values[key] = value;
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    if (key >= PTHREAD_KEYS_MAX) return (void *)0;
    return (void *)_tls_values[key];
}
