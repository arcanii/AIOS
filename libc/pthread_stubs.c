/* pthread_stubs.c
 * Minimal POSIX thread stubs for AIOS libc.
 * All operations return ENOSYS.
 * Allows programs using pthread symbols to link cleanly.
 * Full implementation belongs in the 0.3.x sandbox kernel layer.
 */

#include <errno.h>
#include <stddef.h>

typedef unsigned long pthread_t;
typedef struct { int v; } pthread_mutex_t;
typedef struct { int v; } pthread_cond_t;
typedef struct { int v; } pthread_rwlock_t;
typedef unsigned int  pthread_key_t;

typedef struct {
    int detachstate;
    int schedpolicy;
    int inheritsched;
    int scope;
    size_t stacksize;
} pthread_attr_t;

typedef struct { int v; } pthread_mutexattr_t;
typedef struct { int v; } pthread_condattr_t;
typedef struct { int v; } pthread_rwlockattr_t;

int pthread_create(pthread_t *t, const pthread_attr_t *a,
                   void *(*fn)(void *), void *arg)
{ (void)t; (void)a; (void)fn; (void)arg; return ENOSYS; }

int pthread_join(pthread_t t, void **ret)
{ (void)t; (void)ret; return ENOSYS; }

int pthread_detach(pthread_t t)
{ (void)t; return ENOSYS; }

void pthread_exit(void *v)
{ (void)v; while(1); }

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{ (void)m; (void)a; return 0; }

int pthread_mutex_lock(pthread_mutex_t *m)
{ (void)m; return 0; }

int pthread_mutex_unlock(pthread_mutex_t *m)
{ (void)m; return 0; }

int pthread_mutex_destroy(pthread_mutex_t *m)
{ (void)m; return 0; }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{ (void)c; (void)a; return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{ (void)c; (void)m; return ENOSYS; }

int pthread_cond_signal(pthread_cond_t *c)
{ (void)c; return 0; }

int pthread_cond_broadcast(pthread_cond_t *c)
{ (void)c; return 0; }

int pthread_rwlock_init(pthread_rwlock_t *r, const pthread_rwlockattr_t *a)
{ (void)r; (void)a; return 0; }

int pthread_rwlock_rdlock(pthread_rwlock_t *r)
{ (void)r; return 0; }

int pthread_rwlock_wrlock(pthread_rwlock_t *r)
{ (void)r; return 0; }

int pthread_rwlock_unlock(pthread_rwlock_t *r)
{ (void)r; return 0; }

int pthread_key_create(pthread_key_t *k, void (*d)(void *))
{ (void)k; (void)d; return ENOSYS; }

int pthread_setspecific(pthread_key_t k, const void *v)
{ (void)k; (void)v; return ENOSYS; }

void *pthread_getspecific(pthread_key_t k)
{ (void)k; return NULL; }
