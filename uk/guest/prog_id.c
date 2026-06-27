/* prog_id.c -- a gate for PROCESS IDENTITY (system layer, increment 2): per-process real/effective/
 * saved uid+gid + the setuid/setgid privilege model. A directly-launched guest is the seeded AIOS
 * root (uid 0), so this exercises: the getters, a privileged setgid/setuid (drop to a normal user),
 * the EPERM that proves the drop is real (an unprivileged caller cannot regain uid 0), and that
 * identity is INHERITED across fork yet a child's switch does NOT leak back to the parent.
 *
 * Numeric only (no /etc/passwd dependency) so it is environment-independent; the name lookup
 * (geteuid -> getpwuid) is shown separately by the sbase `whoami` util in the gate.
 *
 * Exit 0 iff every check passes -- the gate keys on it. */

#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>

int
main(void)
{
    int fail = 0;

    /* a directly-launched guest is the seeded AIOS root: real == effective == 0 for both uid and gid. */
    if (getuid() != 0 || geteuid() != 0 || getgid() != 0 || getegid() != 0) {
        printf("FAIL not seeded as root (uid %d/%d gid %d/%d)\n",
               getuid(), geteuid(), getgid(), getegid());
        fail = 1;
    } else printf("seeded identity: uid 0/0 gid 0/0 (AIOS root)\n");

    /* privileged (euid 0) setgid sets real + effective + saved gid. */
    if (setgid(100) != 0 || getgid() != 100 || getegid() != 100) {
        printf("FAIL privileged setgid(100) (gid %d/%d)\n", getgid(), getegid()); fail = 1;
    } else printf("setgid(100): gid now 100/100\n");

    /* privileged setuid drops real + effective + saved uid to a normal user -- privilege is GONE. */
    if (setuid(1000) != 0 || getuid() != 1000 || geteuid() != 1000) {
        printf("FAIL privileged setuid(1000) (uid %d/%d)\n", getuid(), geteuid()); fail = 1;
    } else printf("setuid(1000): uid now 1000/1000 (dropped root)\n");

    /* the drop is REAL: unprivileged + 0 is neither our real nor saved uid -> EPERM. */
    errno = 0;
    if (setuid(0) == 0 || errno != EPERM) {
        printf("FAIL setuid(0) after drop should be EPERM (rc errno=%d)\n", errno); fail = 1;
    } else printf("setuid(0) after drop -> EPERM (cannot regain root)\n");

    /* an unprivileged setuid back to the real uid is allowed (no-op effective switch). */
    if (setuid(1000) != 0) { printf("FAIL setuid(1000)==real should succeed\n"); fail = 1; }

    /* identity is INHERITED across fork; a child's further drop does NOT leak into the parent. */
    int kid = fork();
    if (kid == 0) {
        int cfail = 0;
        if (getuid() != 1000 || getgid() != 100) cfail = 1;   /* inherited the parent's identity */
        if (setgid(200) != 0 || getgid() != 200) cfail = 1;   /* unprivileged: 200 is real? no -> EPERM */
        _exit(cfail);
    }
    int status = 0;
    waitpid(kid, &status, 0);
    /* the child's setgid(200) must FAIL (unprivileged, 200 is neither real 100 nor saved 100), so the
     * child exits nonzero -- and the PARENT's gid is still 100 (no leak). */
    if (!(WIFEXITED(status) && WEXITSTATUS(status) != 0)) {
        printf("FAIL child unprivileged setgid(200) should have failed\n"); fail = 1;
    } else printf("child cannot escalate gid; parent gid still %d (no leak)\n", getgid());
    if (getgid() != 100) { printf("FAIL parent gid leaked to %d\n", getgid()); fail = 1; }

    printf(fail ? "prog_id: FAIL\n" : "prog_id: all identity checks passed\n");
    return fail;
}
