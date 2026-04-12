/*
 * posix_net.c -- AIOS POSIX socket syscall handlers (M3: UDP)
 *
 * Implements socket, bind, sendto, recvfrom via IPC to net_server.
 * Data packed into message registers (MR path, up to ~900 bytes).
 */
#include "posix_internal.h"

/* ---- socket(domain, type, protocol) ---- */
long aios_sys_socket(va_list ap) {
    int domain = va_arg(ap, long);
    int type = va_arg(ap, long);
    int protocol = va_arg(ap, long);

    if (!net_ep) return -ENOTSUP;

    seL4_SetMR(0, (seL4_Word)domain);
    seL4_SetMR(1, (seL4_Word)type);
    seL4_SetMR(2, (seL4_Word)protocol);
    seL4_Call(net_ep, seL4_MessageInfo_new(NET_SOCKET_L, 0, 0, 3));
    int sock_id = (int)(long)seL4_GetMR(0);
    if (sock_id < 0) return -ENOMEM;

    int idx = aios_fd_alloc();
    if (idx < 0) {
        /* No fd: close server-side socket */
        seL4_SetMR(0, (seL4_Word)sock_id);
        seL4_Call(net_ep, seL4_MessageInfo_new(NET_CLOSE_SOCK_L, 0, 0, 1));
        return -EMFILE;
    }
    aios_fd_t *f = &aios_fds[idx];
    f->active = 1;
    f->is_socket = 1;
    f->socket_id = sock_id;
    return AIOS_FD_BASE + idx;
}

/* ---- bind(sockfd, addr, addrlen) ---- */
long aios_sys_bind(va_list ap) {
    int fd = va_arg(ap, long);
    const uint8_t *sa = va_arg(ap, const uint8_t *);
    va_arg(ap, long); /* addrlen */

    if (fd < AIOS_FD_BASE) return -ENOTSOCK;
    aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
    if (!f->active || !f->is_socket || !net_ep) return -ENOTSOCK;

    /* sockaddr_in: family(2) port(2,NBO) addr(4,NBO) zero(8) */
    uint16_t port = ((uint16_t)sa[2] << 8) | sa[3];
    uint32_t ip = ((uint32_t)sa[4] << 24) | ((uint32_t)sa[5] << 16) |
                  ((uint32_t)sa[6] << 8) | sa[7];

    seL4_SetMR(0, (seL4_Word)f->socket_id);
    seL4_SetMR(1, (seL4_Word)port);
    seL4_SetMR(2, (seL4_Word)ip);
    seL4_Call(net_ep, seL4_MessageInfo_new(NET_BIND_L, 0, 0, 3));
    return (long)seL4_GetMR(0);
}

/* ---- sendto(sockfd, buf, len, flags, dest_addr, addrlen) ---- */
long aios_sys_sendto(va_list ap) {
    int fd = va_arg(ap, long);
    const uint8_t *buf = va_arg(ap, const uint8_t *);
    size_t len = va_arg(ap, size_t);
    va_arg(ap, long); /* flags */
    const uint8_t *sa = va_arg(ap, const uint8_t *);
    va_arg(ap, long); /* addrlen */

    if (fd < AIOS_FD_BASE) return -ENOTSOCK;
    aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
    if (!f->active || !f->is_socket || !net_ep) return -ENOTSOCK;
    if (len > 900) return -EMSGSIZE;

    uint16_t port = 0;
    uint32_t ip = 0;
    if (sa) {
        port = ((uint16_t)sa[2] << 8) | sa[3];
        ip = ((uint32_t)sa[4] << 24) | ((uint32_t)sa[5] << 16) |
             ((uint32_t)sa[6] << 8) | sa[7];
    }

    seL4_SetMR(0, (seL4_Word)f->socket_id);
    seL4_SetMR(1, (seL4_Word)len);
    seL4_SetMR(2, (seL4_Word)ip);
    seL4_SetMR(3, (seL4_Word)port);

    /* Pack data into MR4+ (8 bytes per MR) */
    int mr = 4;
    seL4_Word w = 0;
    for (int i = 0; i < (int)len; i++) {
        w |= ((seL4_Word)(uint8_t)buf[i]) << ((i % 8) * 8);
        if (i % 8 == 7 || i == (int)len - 1) {
            seL4_SetMR(mr++, w);
            w = 0;
        }
    }
    seL4_Call(net_ep, seL4_MessageInfo_new(NET_SENDTO_L, 0, 0, mr));
    return (long)seL4_GetMR(0);
}

/* ---- recvfrom(sockfd, buf, len, flags, src_addr, addrlen) ---- */
long aios_sys_recvfrom(va_list ap) {
    int fd = va_arg(ap, long);
    uint8_t *buf = va_arg(ap, uint8_t *);
    size_t maxlen = va_arg(ap, size_t);
    va_arg(ap, long); /* flags */
    uint8_t *src_addr = va_arg(ap, uint8_t *);
    int *addrlen_ptr = va_arg(ap, int *);

    if (fd < AIOS_FD_BASE) return -ENOTSOCK;
    aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
    if (!f->active || !f->is_socket || !net_ep) return -ENOTSOCK;
    if (maxlen > 900) maxlen = 900;

    seL4_SetMR(0, (seL4_Word)f->socket_id);
    seL4_SetMR(1, (seL4_Word)maxlen);
    /* v0.4.85: pass O_NONBLOCK flag so server returns EAGAIN */
    seL4_SetMR(2, (seL4_Word)(f->is_nonblock ? 1 : 0));
    seL4_Call(net_ep, seL4_MessageInfo_new(NET_RECVFROM_L, 0, 0, 3));

    int recv_len = (int)(long)seL4_GetMR(0);
    if (recv_len < 0) return recv_len;  /* v0.4.85: -EAGAIN or error */
    if (recv_len == 0) return 0;

    uint32_t src_ip = (uint32_t)seL4_GetMR(1);
    uint16_t src_port = (uint16_t)seL4_GetMR(2);

    /* Unpack data from MR3+ */
    int mr = 3;
    seL4_Word w = 0;
    for (int i = 0; i < recv_len; i++) {
        if (i % 8 == 0) w = seL4_GetMR(mr++);
        buf[i] = (uint8_t)(w & 0xFF);
        w >>= 8;
    }

    /* Fill src_addr (sockaddr_in) if provided */
    if (src_addr) {
        src_addr[0] = 2; src_addr[1] = 0; /* AF_INET */
        src_addr[2] = (src_port >> 8) & 0xFF;
        src_addr[3] = src_port & 0xFF;
        src_addr[4] = (src_ip >> 24) & 0xFF;
        src_addr[5] = (src_ip >> 16) & 0xFF;
        src_addr[6] = (src_ip >> 8) & 0xFF;
        src_addr[7] = src_ip & 0xFF;
        for (int i = 8; i < 16; i++) src_addr[i] = 0;
    }
    if (addrlen_ptr) *addrlen_ptr = 16;
    return recv_len;
}

/* ---- listen(sockfd, backlog) ---- */
long aios_sys_listen(va_list ap) {
    int fd = va_arg(ap, long);
    int backlog = va_arg(ap, long);
    (void)backlog;

    if (fd < AIOS_FD_BASE) return -ENOTSOCK;
    aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
    if (!f->active || !f->is_socket || !net_ep) return -ENOTSOCK;

    seL4_SetMR(0, (seL4_Word)f->socket_id);
    seL4_SetMR(1, (seL4_Word)backlog);
    seL4_Call(net_ep, seL4_MessageInfo_new(NET_LISTEN_L, 0, 0, 2));
    return (long)seL4_GetMR(0);
}

/* ---- accept4(sockfd, addr, addrlen, flags) ---- */
long aios_sys_accept4(va_list ap) {
    int fd = va_arg(ap, long);
    va_arg(ap, void *);  /* addr (filled later) */
    va_arg(ap, void *);  /* addrlen */
    va_arg(ap, long);    /* flags */

    if (fd < AIOS_FD_BASE) return -ENOTSOCK;
    aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
    if (!f->active || !f->is_socket || !net_ep) return -ENOTSOCK;

    seL4_SetMR(0, (seL4_Word)f->socket_id);
    seL4_Call(net_ep, seL4_MessageInfo_new(NET_ACCEPT_L, 0, 0, 1));

    int new_sock_id = (int)(long)seL4_GetMR(0);
    if (new_sock_id < 0) return -ECONNREFUSED;

    int idx = aios_fd_alloc();
    if (idx < 0) return -EMFILE;
    aios_fds[idx].active = 1;
    aios_fds[idx].is_socket = 1;
    aios_fds[idx].socket_id = new_sock_id;
    return AIOS_FD_BASE + idx;
}

/* ---- setsockopt (stub) ---- */
long aios_sys_setsockopt(va_list ap) {
    (void)ap;
    return 0;
}

/* ---- shutdown (stub) ---- */
long aios_sys_shutdown_sock(va_list ap) {
    (void)ap;
    return 0;
}

/* ---- connect(sockfd, addr, addrlen) -- stub for now ---- */
long aios_sys_connect(va_list ap) {
    (void)ap;
    return -ENOSYS;  /* TODO: TCP client SYN + block */
}

/* ---- getsockname(sockfd, addr, addrlen) ---- */
long aios_sys_getsockname(va_list ap) {
    int fd = va_arg(ap, long);
    uint8_t *sa = va_arg(ap, uint8_t *);
    int *addrlen = va_arg(ap, int *);

    if (fd < AIOS_FD_BASE) return -ENOTSOCK;
    aios_fd_t *f = &aios_fds[fd - AIOS_FD_BASE];
    if (!f->active || !f->is_socket) return -ENOTSOCK;
    if (sa) {
        sa[0] = 2; sa[1] = 0;  /* AF_INET */
        for (int i = 2; i < 16; i++) sa[i] = 0;
    }
    if (addrlen) *addrlen = 16;
    return 0;
}

/* ---- getpeername(sockfd, addr, addrlen) -- stub ---- */
long aios_sys_getpeername(va_list ap) {
    (void)ap;
    return -ENOTCONN;
}

/* ---- getsockopt(sockfd, level, optname, optval, optlen) -- stub ---- */
long aios_sys_getsockopt(va_list ap) {
    (void)ap;
    return 0;
}
