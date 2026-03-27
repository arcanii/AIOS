/* aios_api.h — AIOS sandbox syscall interface
 * Programs compiled by TCC call these to interact
 * with the AIOS orchestrator via shared memory.
 */
#ifndef AIOS_API_H
#define AIOS_API_H

/* Console output */
void aios_puts(const char *s);
void aios_putc(char c);
void aios_put_dec(unsigned int n);

/* File I/O */
int aios_open(const char *name);
int aios_read(int fd, char *buf, int len);
int aios_write(int fd, const char *buf, int len);
int aios_close(int fd);

/* Directory listing */
typedef struct { int _handle; } DIR;
typedef struct { char name[12]; unsigned int size; } dirent;
int aios_opendir(const char *path, DIR *d);
int aios_readdir(DIR *d, dirent *ent);

/* Arguments (set by orchestrator before launch) */
const char *aios_get_arg(int index);

/* Exit */
void aios_exit(int code);

#endif
