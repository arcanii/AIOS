/*
 * aios_init.c -- PID 1 for the minimal AIOS-on-Linux appliance.
 *
 * The minimal Linux 6.18 kernel boots, unpacks the initramfs, and execs THIS as /init (PID 1). Its
 * only job is to stand up just enough host state for the AIOS userspace kernel and then hand the
 * machine to AIOS: it mounts /proc (aios-uk's M4.2/M4.3 confinement canonicalizes via /proc/self/fd),
 * gives the console a controlling terminal (so AIOS job control -- ^C/^Z, tcsetpgrp -- works), sets the
 * confinement env, and execs `aios-uk /aiosroot/sbin/init` with AIOS_ROOT=/aiosroot. The AIOS system
 * init (which runs the console login) becomes the AIOS init guest; the host filesystem is unreachable
 * (jailed to /aiosroot). So the appliance boots into an AIOS LOGIN, not a bare shell.
 *
 * Statically linked (build_appliance.sh: cc -static) so the initramfs needs no libc at all -- it holds
 * exactly three things: /init, /aios-uk, and /aiosroot (the AIOS userland). PID 1 must never return,
 * so when the AIOS session ends we respawn it (a kiosk).
 *
 * This file is HOST (Linux) code -- it is the substrate's init, not an AIOS-ABI program.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>   /* RB_POWER_OFF / RB_AUTOBOOT -- the host power transition */
#include <termios.h>      /* TIOCSCTTY */
#include "aios_abi.h"     /* AIOS_EXIT_* -- the shutdown exit codes aios-uk returns (host-agnostic) */

static void msg(const char *s) { (void)write(2, s, strlen(s)); }

int main(void) {
    /* aios-uk resolves confined paths through /proc/self/fd, so /proc must be mounted. devtmpfs gives
     * us /dev/console etc.; both are best-effort (a static /dev/console in the initramfs also works). */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("dev",  "/dev",  "devtmpfs", 0, NULL);

    setenv("AIOS_ROOT", "/aiosroot", 1);   /* jail every guest path + exec inside the AIOS root */
    setenv("PATH", "/bin", 1);
    setenv("HOME", "/", 1);
    setenv("TERM", "vt100", 1);

    msg("\n[aios-init] minimal Linux up; launching the AIOS userspace kernel (host fs is jailed away)\n");

    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            /* New session + controlling terminal on the console, so the AIOS shell can do job control
             * (PID 1 itself cannot setsid -- it is already a session leader -- so we do it in the child). */
            setsid();
            int tty = open("/dev/console", O_RDWR);
            if (tty >= 0) {
                ioctl(tty, TIOCSCTTY, 1);
                dup2(tty, 0); dup2(tty, 1); dup2(tty, 2);
                if (tty > 2) close(tty);
            }
            char *argv[] = { "/aios-uk", "/aiosroot/sbin/init", (char *)0 };
            execv("/aios-uk", argv);
            msg("[aios-init] FATAL: cannot exec /aios-uk\n");
            _exit(127);
        }
        int st;
        while (waitpid(pid, &st, 0) < 0) { }
        /* A clean AIOS shutdown: aios-uk exits with a distinguished code (a root `poweroff`/`halt`/
         * `reboot` reached the AIOS kernel). Map it to a REAL host power transition instead of
         * respawning -- this is how "shutdown AIOS" powers off the machine. */
        if (WIFEXITED(st) && AIOS_EXIT_IS_SHUTDOWN(WEXITSTATUS(st))) {
            int c = WEXITSTATUS(st);
            msg(c == AIOS_EXIT_REBOOT ? "[aios-init] AIOS requested REBOOT; rebooting the host\n"
                                      : "[aios-init] AIOS requested POWEROFF; powering off the host\n");
            sync();
            reboot(c == AIOS_EXIT_REBOOT ? RB_AUTOBOOT : RB_POWER_OFF);
            /* reboot() should not return; if it does (unprivileged / no perm), fall through to respawn */
        }
        msg("[aios-init] AIOS session ended; respawning the shell...\n");
        sleep(1);
    }
}
