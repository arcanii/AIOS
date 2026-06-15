/* sntp.c -- minimal SNTP client: fetch real time over UDP and set the clock.
 *
 * The RPi4 has no RTC, so AIOS comes up with wall-clock = uptime-since-boot
 * (~1970). This queries an NTP server (port 123) for the real time and calls
 * settimeofday(), which pushes a wall-clock epoch offset to the root task so
 * every process (and the fs, for mtimes) sees correct calendar time.
 *
 * The AIOS net stack routes off-subnet IPs through the DHCP gateway, so a
 * public NTP IP works (no DNS needed). Tries a short list, or one given as
 * argv[1] (dotted IP). One-shot: run it once at boot (or by hand).
 *
 * Build: AiosPosixApp(sntp)  ->  /bin/aios/sntp
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NTP_PORT       123
#define NTP_LOCAL_PORT 50123
#define NTP_UNIX_DELTA 2208988800U   /* seconds between 1900 (NTP) and 1970 */
#define POLL_NS        (10 * 1000 * 1000)   /* 10ms */
#define POLL_TRIES     400                  /* ~4s per server */

/* Default public NTP servers (anycast / reliable), tried in order. */
static const char *DEFAULT_SERVERS[] = {
    "162.159.200.123",  /* Cloudflare time.cloudflare.com */
    "216.239.35.0",     /* Google   time.google.com       */
    "129.6.15.28",      /* NIST     time-a-g.nist.gov      */
};

/* Query one server. Returns Unix seconds on success, 0 on failure. */
static uint32_t sntp_query(const char *server_ip)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(NTP_LOCAL_PORT);
    local.sin_addr.s_addr = 0;
    bind(fd, (struct sockaddr *)&local, sizeof(local));

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(NTP_PORT);
    srv.sin_addr.s_addr = inet_addr(server_ip);

    uint8_t req[48];
    memset(req, 0, sizeof(req));
    req[0] = 0x1B;   /* LI=0, VN=3, Mode=3 (client) */

    if (sendto(fd, req, sizeof(req), 0,
               (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        close(fd);
        return 0;
    }

    uint8_t resp[48];
    int got = -1;
    for (int i = 0; i < POLL_TRIES; i++) {
        int n = (int)recvfrom(fd, resp, sizeof(resp), 0, (void *)0, (void *)0);
        if (n >= 48) { got = n; break; }
        struct timespec ts = { 0, POLL_NS };
        nanosleep(&ts, (void *)0);
    }
    close(fd);
    if (got < 48)
        return 0;

    /* Transmit timestamp seconds = bytes 40..43, big-endian, NTP epoch. */
    uint32_t ntp_sec = ((uint32_t)resp[40] << 24) | ((uint32_t)resp[41] << 16)
                     | ((uint32_t)resp[42] << 8)  |  (uint32_t)resp[43];
    if (ntp_sec < NTP_UNIX_DELTA)
        return 0;
    return ntp_sec - NTP_UNIX_DELTA;
}

int main(int argc, char **argv)
{
    /* v0.4.254: QUIET by default -- getty auto-spawns sntp with fd1=tty, so any
     * output lands on the HDMI console (the "sntp:" clutter). Pass -v for the
     * diagnostic line; date(1) confirms the sync either way. */
    int verbose = 0;
    const char *single = (void *)0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'v') verbose = 1;
        else if (!single) single = argv[i];
    }
    int n_servers = single ? 1 : (int)(sizeof(DEFAULT_SERVERS) / sizeof(DEFAULT_SERVERS[0]));

    for (int s = 0; s < n_servers; s++) {
        const char *server = single ? single : DEFAULT_SERVERS[s];
        uint32_t unix_sec = sntp_query(server);
        if (unix_sec > 0) {
            struct timeval tv;
            tv.tv_sec = (long)unix_sec;
            tv.tv_usec = 0;
            settimeofday(&tv, (void *)0);
            if (verbose) printf("sntp: time set from %s -> epoch %u\n",
                                server, (unsigned)unix_sec);
            return 0;
        }
        if (verbose) printf("sntp: no reply from %s\n", server);
    }
    if (verbose) printf("sntp: failed to reach any NTP server\n");
    return 1;
}
