/* prog_pwgrp.c -- a gate for the libaios passwd/group database (getpwuid/getpwnam/getgrgid/getgrnam,
 * parsed from /etc/passwd and /etc/group). Asserts the universal Linux entries: uid 0 is "root", a
 * name lookup round-trips to uid 0, gid 0 resolves to a name, and an absent uid yields NULL (the
 * numeric-id fallback ls -l relies on). Exit 0 iff all pass -- the run.sh gate keys on it. */

#include <pwd.h>
#include <grp.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
    int fail = 0;

    struct passwd *pw = getpwuid(0);
    if (!pw || strcmp(pw->pw_name, "root") != 0 || pw->pw_uid != 0) {
        printf("FAIL getpwuid(0) -> root\n");
        fail = 1;
    } else {
        printf("getpwuid(0)      -> %s (uid %u gid %u shell %s)\n",
               pw->pw_name, pw->pw_uid, pw->pw_gid, pw->pw_shell);
    }

    struct passwd *pw2 = getpwnam("root");
    if (!pw2 || pw2->pw_uid != 0) {
        printf("FAIL getpwnam(root) -> uid 0\n");
        fail = 1;
    } else {
        printf("getpwnam(\"root\") -> uid %u\n", pw2->pw_uid);
    }

    struct group *gr = getgrgid(0);
    if (!gr || gr->gr_name[0] == '\0' || gr->gr_gid != 0) {
        printf("FAIL getgrgid(0) -> a named group\n");
        fail = 1;
    } else {
        printf("getgrgid(0)      -> %s (gid %u)\n", gr->gr_name, gr->gr_gid);
    }

    /* an unassigned uid must return NULL so ls -l keeps the numeric fallback */
    if (getpwuid(4000000)) {
        printf("FAIL getpwuid(4000000) should be NULL\n");
        fail = 1;
    } else {
        printf("getpwuid(4000000) -> NULL (numeric fallback) OK\n");
    }

    printf("pwgrp: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
