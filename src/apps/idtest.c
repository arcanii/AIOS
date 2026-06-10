/*
 * idtest -- v0.4.190 privilege-escalation regression probe.
 *
 * Attempts the two historical privesc vectors and checks whether they gained
 * root filesystem access (creating a file under /etc, which fs_check_path_write
 * permits only for uid 0). Run as a NON-ROOT user, "BLOCKED" is the pass; run as
 * root, the /etc write succeeds, confirming the probe actually discriminates.
 *
 *   VECTOR 1: send PIPE_SET_IDENTITY(0) on our own pipe_ep (now root-gated).
 *   (VECTOR 2, forged CWD in EXEC_RUN, is unreachable -- user procs have no
 *    exec_ep -- so it is not probed here.)
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sel4/sel4.h>

#define PIPE_SET_IDENTITY 74

seL4_CPtr aios_get_pipe_ep(void);

int main(void) {
    int before = (int)getuid();
    printf("idtest: uid before = %d\n", before);

    /* Vector 1: try to set our own server-side uid to 0. */
    long rc = -1;
    seL4_CPtr pep = aios_get_pipe_ep();
    if (pep) {
        seL4_SetMR(0, 0);
        seL4_SetMR(1, 0);
        seL4_Call(pep, seL4_MessageInfo_new(PIPE_SET_IDENTITY, 0, 0, 2));
        rc = (long)seL4_GetMR(0);
    }
    printf("idtest: PIPE_SET_IDENTITY(0) rc = %ld (0=accepted, -1=denied)\n", rc);

    /* Did we actually gain root fs access? mkdir under /etc is root-only and
     * FS_MKDIR returns a real, fs_check_path_write-gated status (unlike open
     * O_CREAT, which hands back a usable fd even when the create is denied). */
    int r = mkdir("/etc/.idtestd", 0755);
    int can_write = (r == 0);
    if (r == 0) rmdir("/etc/.idtestd");

    if (before == 0) {
        /* Root: the probe should be able to write /etc (sanity for the test). */
        printf("idtest: RESULT=%s\n", can_write ? "ROOT-OK" : "ROOT-DENIED-UNEXPECTED");
        return can_write ? 0 : 1;
    }
    /* Non-root: if we could write /etc, the privesc succeeded. */
    printf("idtest: RESULT=%s\n", can_write ? "VULNERABLE" : "BLOCKED");
    return can_write ? 1 : 0;
}
