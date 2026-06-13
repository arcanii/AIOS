#ifndef AIOS_NET_PROTO_H
#define AIOS_NET_PROTO_H

/*
 * net_proto.h -- canonical network IPC labels (client <-> net server).
 *
 * v0.4.229 (netd Stage 0): single source of truth for the NET_* request
 * labels, factored out of root_shared.h so the future MMU-isolated netd
 * process and the in-root net server share ONE definition (DESIGN_NETD s5).
 *
 * These values are the on-the-wire protocol. They MUST NOT change without
 * rebuilding every client: dash/zsh/sshd link the posix library, which keeps
 * its own literal mirror (NET_*_L in posix_internal.h) so it stays independent
 * of root_shared.h. The _Static_asserts next to that mirror pin it to these
 * values, so a skew is a LOUD build failure instead of a silent ABI break.
 *
 * Label space: 90-109 is reserved for net (110+ is display -- root_shared.h).
 * 99-102 (the SHM bulk-data path) are defined but dormant; 103 (NET_DIAG) is
 * reserved for the Stage 3/4 userland /bin/netdiag tool. See DESIGN_NETD s11.
 */

#define NET_SOCKET       90
#define NET_BIND         91
#define NET_LISTEN       92
#define NET_ACCEPT       93
#define NET_CONNECT      94
#define NET_SENDTO       95
#define NET_RECVFROM     96
#define NET_CLOSE_SOCK   97
#define NET_CLEANUP_PID  98   /* free every socket owned by an exiting pid */
#define NET_SETSOCKOPT   99
#define NET_SENDTO_SHM   100  /* dormant: SHM bulk send */
#define NET_RECVFROM_SHM 101  /* dormant: SHM bulk recv */
#define NET_MAP_SHM      102  /* dormant: map the SHM bulk frame */
#define NET_DIAG         103  /* reserved: Stage 3/4 /bin/netdiag -> net server */

#endif /* AIOS_NET_PROTO_H */
