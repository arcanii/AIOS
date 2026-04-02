/* sem_stubs.c
 * Minimal POSIX semaphore stubs for AIOS libc.
 * All blocking operations return ENOSYS.
 * Allows programs using sem_* to link cleanly.
 */

#include <errno.h>
#include <stddef.h>

typedef struct { int count; } sem_t;

int sem_init(sem_t *s, int pshared, unsigned int val)
{ (void)s; (void)pshared; (void)val; return ENOSYS; }

int sem_wait(sem_t *s)
{ (void)s; return ENOSYS; }

int sem_post(sem_t *s)
{ (void)s; return ENOSYS; }

int sem_destroy(sem_t *s)
{ (void)s; return 0; }

sem_t *sem_open(const char *name, int oflag, ...)
{ (void)name; (void)oflag; errno = ENOSYS; return ((sem_t *)0); }

int sem_close(sem_t *s)
{ (void)s; return ENOSYS; }

int sem_unlink(const char *name)
{ (void)name; return ENOSYS; }
