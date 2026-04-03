#ifndef AIOS_PROCFS_H
#define AIOS_PROCFS_H

#include "vfs.h"

extern fs_ops_t procfs_ops;

/* Process table entry */
#define PROC_MAX 32
typedef struct {
    int active;
    int pid;
    int priority;
    int nice;
    const char *name;
    int state;  /* 0=free, 1=running, 2=sleeping, 3=zombie */
} proc_entry_t;

extern proc_entry_t proc_table[PROC_MAX];

/* Process management */
void proc_init(void);
int proc_add(const char *name, int priority);
void proc_remove(int pid);
void proc_set_nice(int pid, int nice);
int proc_get_priority(int nice);

#endif
