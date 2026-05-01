/*
 * aios_tcc_anchor.c -- TCC self-host libc/runtime anchors
 *
 * Each symbol referenced here forces the linker to include the .o
 * that defines it (and its transitive dependencies) when building
 * the pre-linked libaios_tcc.o blob with -Wl,-r.
 *
 * The blob is consumed by the on-AIOS tcc to link compiled C
 * programs without going through TCC's archive parser, which
 * cannot handle libc.a (1418 members, duplicate names).
 *
 * Add new entries here when a libc function is needed by user
 * programs but the linker is not pulling it in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

extern int __aios_entry(int, char **, char **);
extern void __sel4runtime_load_env(int, char **, char **, void *);
extern void __sel4_start_c(void const *);
extern void muslcsys_init_muslc(void);
extern char __vsyscall_ptr;

/* Weak fallbacks for symbols TCC will not auto-resolve.
 * GNU ld auto-creates __executable_start at link time from the linker
 * script; TCC does not. fsl_avic_ptr is a freescale driver pointer
 * that the platsupport irq dispatcher references but never assigns
 * on qemu-arm-virt; declaring it weak keeps the dispatcher null-safe. */
char __executable_start[1] __attribute__((section(".text"), weak)) = {0};
void *fsl_avic_ptr __attribute__((weak)) = 0;

volatile void *aios_tcc_anchors[] = {
    /* AIOS init chain -- must be first so the strong refs survive */
    (void *)&__sel4_start_c,
    (void *)&__sel4runtime_load_env,
    (void *)&__aios_entry,
    (void *)&muslcsys_init_muslc,
    (void *)&__vsyscall_ptr,

    /* stdio */
    (void *)&printf, (void *)&fprintf, (void *)&sprintf, (void *)&snprintf,
    (void *)&vprintf, (void *)&vfprintf, (void *)&vsprintf, (void *)&vsnprintf,
    (void *)&scanf, (void *)&fscanf, (void *)&sscanf,
    (void *)&fopen, (void *)&fdopen, (void *)&freopen,
    (void *)&fclose, (void *)&fread, (void *)&fwrite,
    (void *)&fseek, (void *)&ftell, (void *)&rewind, (void *)&fflush,
    (void *)&fputs, (void *)&fputc, (void *)&fgets, (void *)&fgetc,
    (void *)&getchar, (void *)&putchar, (void *)&puts,
    (void *)&perror, (void *)&clearerr, (void *)&feof, (void *)&ferror,
    (void *)&setbuf, (void *)&setvbuf, (void *)&fileno,

    /* stdlib */
    (void *)&malloc, (void *)&free, (void *)&calloc, (void *)&realloc,
    (void *)&exit, (void *)&_Exit, (void *)&atexit, (void *)&abort,
    (void *)&getenv, (void *)&setenv, (void *)&unsetenv,
    (void *)&atoi, (void *)&atol, (void *)&atoll, (void *)&atof,
    (void *)&strtol, (void *)&strtoll, (void *)&strtoul, (void *)&strtoull,
    (void *)&strtof, (void *)&strtod,
    (void *)&qsort, (void *)&bsearch, (void *)&abs, (void *)&labs,
    (void *)&rand, (void *)&srand, (void *)&system,
    (void *)&mkstemp, (void *)&mkdtemp,

    /* string */
    (void *)&strlen, (void *)&strnlen,
    (void *)&strcmp, (void *)&strncmp, (void *)&strcasecmp, (void *)&strncasecmp,
    (void *)&strcpy, (void *)&strncpy, (void *)&strcat, (void *)&strncat,
    (void *)&strchr, (void *)&strrchr, (void *)&strstr, (void *)&strtok, (void *)&strtok_r,
    (void *)&strdup, (void *)&strndup,
    (void *)&memcpy, (void *)&memmove, (void *)&memset, (void *)&memcmp, (void *)&memchr,
    (void *)&strerror,

    /* unistd / fcntl */
    (void *)&open, (void *)&close, (void *)&read, (void *)&write,
    (void *)&lseek, (void *)&dup, (void *)&dup2,
    (void *)&unlink, (void *)&rmdir, (void *)&mkdir,
    (void *)&getpid, (void *)&getuid, (void *)&getgid,
    (void *)&isatty, (void *)&access,
    (void *)&fork, (void *)&execvp, (void *)&execve,
    (void *)&pipe,
    (void *)&stat, (void *)&fstat, (void *)&lstat,

    /* time */
    (void *)&time, (void *)&clock, (void *)&clock_gettime,
    (void *)&nanosleep,

    /* ctype */
    (void *)&isalpha, (void *)&isdigit, (void *)&isspace,
    (void *)&isupper, (void *)&islower, (void *)&isalnum,
    (void *)&toupper, (void *)&tolower,

    /* errno */
    (void *)&__errno_location,

    NULL
};
