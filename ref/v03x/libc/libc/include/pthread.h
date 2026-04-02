#ifndef _AIOS_PTHREAD_H
#define _AIOS_PTHREAD_H

/* AIOS POSIX Threads - stub interface
 * Single-threaded stubs. Mutexes/conds are no-ops.
 * Thread creation returns ENOSYS.
 * Real threading requires seL4 TCB management.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long pthread_t;
typedef unsigned int  pthread_key_t;

typedef struct { int __attr; } pthread_attr_t;
typedef struct { int __mutex; int __lock; } pthread_mutex_t;
typedef struct { int __attr; } pthread_mutexattr_t;
typedef struct { int __cond;  } pthread_cond_t;
typedef struct { int __attr; } pthread_condattr_t;
typedef struct { int __rwlock; int __readers; int __writer; } pthread_rwlock_t;
typedef struct { int __attr; } pthread_rwlockattr_t;

#define PTHREAD_MUTEX_INITIALIZER  { 0, 0 }
#define PTHREAD_COND_INITIALIZER   { 0 }
#define PTHREAD_RWLOCK_INITIALIZER { 0, 0, 0 }

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
void pthread_exit(void *retval);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_mutex_destroy(pthread_mutex_t *mutex);

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);
int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

#ifdef __cplusplus
}
#endif

#endif
