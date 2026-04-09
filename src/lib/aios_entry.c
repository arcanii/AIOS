/*
 * aios_entry.c -- Entry point for tcc-compiled AIOS programs
 *
 * __aios_entry does the same job as __wrap_main: parse seL4
 * capability endpoints from argv, call aios_init, then invoke
 * the user main with clean POSIX argc/argv.
 *
 * argv from exec_thread:
 *   [serial_ep, fs_ep, thread_ep, auth_ep, pipe_ep, CWD, progname, arg1...]
 *
 * Separate from aios_posix.c to avoid --wrap symbol conflicts
 * when linked in the aios-cc path (where __wrap_main is used).
 */
#include "posix_internal.h"
#include <stdio.h>

extern int main(int argc, char **argv);

static long _entry_parse(const char *s) {
    if (!s) return 0;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int __aios_entry(int argc, char **argv, char **envp) {
    (void)envp;

    /* v0.4.72: tcc linker does not process .init_array constructors.
     * Two critical initialisations are missing in tcc-compiled binaries:
     *   1. __sysinfo (musl syscall dispatch pointer) stays NULL
     *      -- every musl syscall hits raw svc #0 and faults on seL4
     *   2. muslcsys_init_muslc (musl libc internal init) never runs
     *      -- stdio FILE structures not fully initialised
     * Fix both here before any musl function is called. Safe for
     * aios-cc binaries: __sysinfo is already set by the constructor
     * so the if-guard skips the block. */
    {
        extern long __sysinfo;
        extern long sel4_vsyscall(long, ...);
        if (!__sysinfo) {
            __sysinfo = (long)sel4_vsyscall;
            extern void muslcsys_init_muslc(void);
            muslcsys_init_muslc();
        }
    }

    seL4_CPtr serial = 0, fs = 0, thr = 0, ath = 0, pip = 0;
    if (argc > 0 && argv[0]) serial = (seL4_CPtr)_entry_parse(argv[0]);
    if (argc > 1 && argv[1]) fs     = (seL4_CPtr)_entry_parse(argv[1]);
    if (argc > 2 && argv[2]) thr    = (seL4_CPtr)_entry_parse(argv[2]);
    if (argc > 3 && argv[3]) ath    = (seL4_CPtr)_entry_parse(argv[3]);
    if (argc > 4 && argv[4]) pip    = (seL4_CPtr)_entry_parse(argv[4]);
    thread_ep = thr;
    auth_ep = ath;
    pipe_ep = pip;

    aios_init(serial, fs);

    /* Parse uid:gid:spipe:rpipe:/path from argv[5] */
    if (argc > 5 && argv[5]) {
        const char *s = argv[5];
        if (s[0] >= '0' && s[0] <= '9') {
            uint32_t uid = 0;
            while (*s >= '0' && *s <= '9') {
                uid = uid * 10 + (*s - '0'); s++;
            }
            if (*s == ':') s++;
            uint32_t gid = 0;
            while (*s >= '0' && *s <= '9') {
                gid = gid * 10 + (*s - '0'); s++;
            }
            if (*s == ':') s++;
            aios_uid = uid;
            aios_gid = gid;
            if (*s >= '0' && *s <= '9') {
                int sp = 0;
                while (*s >= '0' && *s <= '9') {
                    sp = sp * 10 + (*s - '0'); s++;
                }
                if (*s == ':') s++;
                int rp = 0;
                while (*s >= '0' && *s <= '9') {
                    rp = rp * 10 + (*s - '0'); s++;
                }
                if (*s == ':') s++;
                if (sp != 99) stdout_pipe_id = sp;
                if (rp != 99) stdin_pipe_id = rp;
            }
            if (*s == '/') aios_set_cwd(s);
        } else if (s[0] == '/') {
            aios_set_cwd(s);
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    return main(argc - 6, argv + 6);
}
