#ifndef _AIOS_SEMAPHORE_H
#define _AIOS_SEMAPHORE_H

/* AIOS POSIX Semaphores - stub interface
 * Unnamed semaphores work as counters (single-threaded safe).
 * Named semaphores return SEM_FAILED / -1.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int __value;
    int __valid;
} sem_t;

#define SEM_FAILED ((sem_t *) 0)

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_wait(sem_t *sem);
int sem_post(sem_t *sem);
int sem_destroy(sem_t *sem);

sem_t *sem_open(const char *name, int oflag, ...);
int sem_close(sem_t *sem);
int sem_unlink(const char *name);

#ifdef __cplusplus
}
#endif

#endif
