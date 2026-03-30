#include "aios.h"
#include "posix.h"

#undef strlen
#undef strcmp
#undef strcpy
#undef strncpy
#undef memcpy
#undef memset
#undef malloc
#undef free
#undef open
#undef close
#undef read
#undef write
#undef puts
#undef putc
#undef exit
#undef sleep
#undef time

#define print(s) sys->puts_direct(s)

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static unsigned int parse_ip(const char *s) {
    unsigned int octets[4] = {0,0,0,0};
    int oi = 0;
    while (*s && oi < 4) {
        if (*s >= '0' && *s <= '9') {
            octets[oi] = octets[oi] * 10 + (*s - '0');
        } else if (*s == '.') {
            oi++;
        }
        s++;
    }
    return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
}

__attribute__((section(".text._start")))
int _start(aios_syscalls_t *_sys) {
    sys = _sys;

    /* Parse args: wget <ip> [port] [path] */
    const char *args = sys->args ? sys->args : "";
    
    /* Default values */
    char ip_str[32] = "10.0.2.2";  /* QEMU host */
    int port = 80;
    char path[128] = "/";
    
    /* Parse IP from args */
    if (args && args[0]) {
        int i = 0;
        while (args[i] && args[i] != ' ' && i < 31) { ip_str[i] = args[i]; i++; }
        ip_str[i] = '\0';
        /* Parse port */
        if (args[i] == ' ') {
            i++;
            port = 0;
            while (args[i] && args[i] != ' ' && args[i] >= '0' && args[i] <= '9') {
                port = port * 10 + (args[i] - '0'); i++;
            }
            /* Parse path */
            if (args[i] == ' ') {
                i++;
                int pi = 0;
                while (args[i] && pi < 127) { path[pi++] = args[i++]; }
                path[pi] = '\0';
            }
        }
    }
    
    print("wget: connecting to ");
    print(ip_str);
    print(":");
    /* print port */
    {
        char pbuf[8]; int pi = 0;
        int p = port;
        if (p == 0) { pbuf[pi++] = '0'; }
        else { char tmp[8]; int ti = 0; while (p > 0) { tmp[ti++] = '0' + p % 10; p /= 10; } while (ti > 0) pbuf[pi++] = tmp[--ti]; }
        pbuf[pi] = '\0';
        print(pbuf);
    }
    print(path);
    print("\n");
    
    /* Create socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { print("wget: socket failed\n"); return 1; }
    
    /* Connect */
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(parse_ip(ip_str));
    
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) { print("wget: connect failed\n"); close(fd); return 1; }
    print("wget: connected\n");
    
    /* Send HTTP request */
    const char *req_start = "GET ";
    const char *req_mid = " HTTP/1.0\r\nHost: ";
    const char *req_end = "\r\nConnection: close\r\n\r\n";
    
    send(fd, req_start, 4, 0);
    send(fd, path, slen(path), 0);
    send(fd, req_mid, slen(req_mid), 0);
    send(fd, ip_str, slen(ip_str), 0);
    send(fd, req_end, slen(req_end), 0);
    
    print("wget: request sent, reading response...\n\n");
    
    /* Read response */
    char buf[512];
    ssize_t n;
    int total = 0;
    while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        print(buf);
        total += (int)n;
    }
    
    print("\n\nwget: received ");
    {
        char tbuf[12]; int ti = 0;
        int t = total;
        if (t == 0) { tbuf[ti++] = '0'; }
        else { char tmp[12]; int tt = 0; while (t > 0) { tmp[tt++] = '0' + t % 10; t /= 10; } while (tt > 0) tbuf[ti++] = tmp[--tt]; }
        tbuf[ti] = '\0';
        print(tbuf);
    }
    print(" bytes\n");
    
    close(fd);
    return 0;
}
