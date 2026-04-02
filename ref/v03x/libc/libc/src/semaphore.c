/* semaphore.c - AIOS POSIX Semaphores stubs
 *
 * Unnamed semaphores as counters (single-threaded safe).
 * Named semaphores return SEM_FAILED / -1.
 * TODO: Real impl needs IPC / shared memory support.
 */

#include <semaphore.h>

#ifndef EINVAL
#define EINVAL 22
#endif

/* ---- Unnamed semaphores ---- */

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    (void)pshared;
    if (!sem) return -1;
    sem->__value = (int)value;
    sem->__valid = 1;
    return 0;
}

int sem_wait(sem_t *sem)
{
    if (!sem || !sem->__valid) return -1;
    if (sem->__value <= 0) return -1;
    sem->__value--;
    return 0;
}

int sem_post(sem_t *sem)
{
    if (!sem || !sem->__valid) return -1;
    sem->__value++;
    return 0;
}

int sem_destroy(sem_t *sem)
{
    if (!sem || !sem->__valid) return -1;
    sem->__valid = 0;
    sem->__value = 0;
    return 0;
}

/* ---- Named semaphores (not supported yet) ---- */

sem_t *sem_open(const char *name, int oflag, ...)
{
    (void)name; (void)oflag;
    return SEM_FAILED;
}

int sem_close(sem_t *sem)
{
    (void)sem;
    return -1;
}

int sem_unlink(const char *name)
{
    (void)name;
    return -1;
}
