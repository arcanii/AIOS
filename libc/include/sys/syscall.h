#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

/* AIOS syscall numbers — passed in MR0 via PPC to vfs_server */
#define SYS_OPEN        1
#define SYS_CLOSE       2
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_LSEEK       5
#define SYS_STAT        6
#define SYS_FSTAT       7
#define SYS_UNLINK      8
#define SYS_MKDIR       9
#define SYS_RMDIR      10
#define SYS_READDIR    11
#define SYS_OPENDIR    12
#define SYS_CLOSEDIR   13
#define SYS_GETCWD     14
#define SYS_CHDIR      15
#define SYS_RENAME     16

/* Process syscalls — to proc_server */
#define SYS_EXIT       20
#define SYS_GETPID     21
#define SYS_SLEEP      22
#define SYS_EXEC       23
#define SYS_EXEC_DONE  24

/* Console I/O — to serial_server */
#define SYS_PUTC       30
#define SYS_GETC       31

/* Network syscalls — to net_server */
#define SYS_SOCKET     40
#define SYS_BIND       41
#define SYS_LISTEN     42
#define SYS_ACCEPT     43
#define SYS_CONNECT    44
#define SYS_SEND       45
#define SYS_RECV       46

/* Memory */
#define SYS_BRK        50
#define SYS_MMAP       51


/* POSIX identity */
#define SYS_GETUID     25
#define SYS_GETGID     26
#define SYS_GETEUID    27
#define SYS_GETEGID    28
#define SYS_GETPPID    29

/* POSIX file */
#define SYS_ACCESS     17
#define SYS_UMASK      18
#define SYS_DUP        19
#define SYS_DUP2       60
#define SYS_TRUNCATE   61
#define SYS_PIPE       62
#define SYS_TIME       63
#define SYS_SPAWN      64
#define SYS_WAITPID    65
#define SYS_KILL_PROC  66

/* Signals */
#define SYS_KILL       67   /* kill(pid, sig) */
#define SYS_SIGNAL     68   /* signal(sig, handler) - register handler */

/* Additional POSIX file ops */
#define SYS_CHMOD      70
#define SYS_CHOWN      71
#define SYS_LINK       72
#define SYS_FCNTL      73
#define SYS_FTRUNCATE  74

/* Process creation */
#define SYS_FORK       75
#define SYS_SBRK       76

#endif
