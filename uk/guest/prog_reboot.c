/* prog_reboot.c -- gate for the shutdown PRIVILEGE (system layer inc 2). A directly-launched guest is
 * the seeded AIOS root (uid 0); it drops to a normal user and asserts reboot() is then DENIED (-EPERM)
 * and the system stays up. This is self-verifying: if REBOOT were NOT root-gated, the unprivileged
 * reboot would succeed and aios-uk would exit 200 (a shutdown code) instead of this program's 0 -- so
 * the gate keys on exit 0. The actual poweroff (root -> aios-uk exits 200) is checked separately.
 *
 * Exit 0 iff the privilege check holds. */

#include <sys/reboot.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

int
main(void)
{
    int fail = 0;

    if (getuid() != 0) { printf("FAIL not launched as root (uid %d)\n", getuid()); return 1; }
    if (setuid(1000) != 0) { printf("FAIL setuid(1000) drop\n"); return 1; }

    errno = 0;
    int r = reboot(RB_POWER_OFF);   /* if this were NOT gated, the system would go down here (exit 200) */
    if (r == 0 || errno != EPERM) {
        printf("FAIL unprivileged reboot not denied (r=%d errno=%d)\n", r, errno); fail = 1;
    } else {
        printf("unprivileged reboot -> EPERM (the system stays up)\n");
    }

    printf(fail ? "prog_reboot: FAIL\n" : "prog_reboot: shutdown is root-only (verified)\n");
    return fail;
}
