/* poweroff.c -- the AIOS shutdown command (system layer, increment 2). One binary installed under three
 * names -- /sbin/poweroff, /sbin/halt, /sbin/reboot -- it picks the action from argv[0]. It asks the
 * AIOS kernel to bring the system down via reboot() (root only); on success the whole AIOS system goes
 * down and this never returns, so reaching the end means the request was denied (not root). An AIOS-ABI
 * program (libaios); never a host call. */
#include <sys/reboot.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

int main(int argc, char **argv) {
    char name[32] = "poweroff";
    if (argc > 0 && argv[0]) {                          /* basename of argv[0], without mutating it */
        const char *s = argv[0], *b = s;
        for (; *s; s++) if (*s == '/') b = s + 1;
        size_t i = 0; for (; b[i] && i < sizeof name - 1; i++) name[i] = b[i]; name[i] = '\0';
    }
    int cmd = RB_POWER_OFF;
    if      (!strcmp(name, "reboot")) cmd = RB_AUTOBOOT;
    else if (!strcmp(name, "halt"))   cmd = RB_HALT_SYSTEM;

    reboot(cmd);                                        /* on success: the system goes down, no return */
    perror(name);                                       /* only reached on failure (EPERM -- not root) */
    return 1;
}
