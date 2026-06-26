/* prog_loop.c -- a guest that runs forever (making a syscall each iteration so a signal can be
 * delivered). Used as a foreground job by the interactive ^C test: pressing ^C must terminate it and
 * return dash to its prompt. It has no signal handler, so the default action of SIGINT (terminate)
 * applies. */
#include <unistd.h>

int
main(void)
{
    for (;;)
        (void)getpid();
    return 0;
}
