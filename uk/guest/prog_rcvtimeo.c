/* prog_rcvtimeo.c -- proof for networking increment 4a: a TIMED socket read (SO_RCVTIMEO). It sets a
 * 300 ms receive timeout on a bound UDP socket and shows (1) a read with a datagram already waiting
 * returns IMMEDIATELY (the timeout never delays a ready read), and (2) a read with no data blocks up to
 * the timeout and then returns -1/EAGAIN (not hanging, not returning early). This is the groundwork for a
 * DNS resolver (inc 4b) that must time out + retry on packet loss. Self-contained -- loopback UDP only,
 * no external network. An AIOS-ABI program (libaios); the kernel honors SO_RCVTIMEO in its socket
 * park/wake (the host socket is non-blocking, so this is a kernel-side deadline).
 *
 * Exit 0 iff both the immediate read and the ~300 ms timeout behave. */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

static long ms_now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int
main(void)
{
    int rc = 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("rcvtimeo: socket"); return 1; }

    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("rcvtimeo: bind"); return 1; }
    struct sockaddr_in b; socklen_t bl = sizeof b;
    if (getsockname(s, (struct sockaddr *)&b, &bl) != 0) { perror("rcvtimeo: getsockname"); return 1; }

    struct timeval tv = { 0, 300 * 1000 };           /* 300 ms */
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) { perror("rcvtimeo: setsockopt"); return 1; }

    /* (1) a datagram is already queued -> the read must return it immediately, NOT wait for the timeout */
    int c = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = b.sin_port;
    if (c < 0 || connect(c, (struct sockaddr *)&to, sizeof to) != 0) { perror("rcvtimeo: connect"); return 1; }
    if (write(c, "hi", 2) != 2) { perror("rcvtimeo: write"); return 1; }

    char buf[64];
    long t0 = ms_now();
    int n = (int)read(s, buf, sizeof buf);
    long e1 = ms_now() - t0;
    int ready_ok = (n == 2 && buf[0] == 'h' && buf[1] == 'i' && e1 < 250);
    printf("  ready read: n=%d in %ld ms -> %s\n", n, e1, ready_ok ? "ok (immediate)" : "BAD");
    if (!ready_ok) rc = 1;

    /* (2) no more data -> the read blocks up to ~300 ms then returns -1/EAGAIN */
    errno = 0;
    t0 = ms_now();
    n = (int)read(s, buf, sizeof buf);
    long e2 = ms_now() - t0;
    int timeout_ok = (n < 0 && errno == EAGAIN && e2 >= 250 && e2 < 2000);
    printf("  timeout read: n=%d errno=%d in %ld ms -> %s\n", n, errno, e2,
           timeout_ok ? "ok (EAGAIN after ~300ms)" : "BAD");
    if (!timeout_ok) rc = 1;

    close(c); close(s);
    printf("prog_rcvtimeo: %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}
