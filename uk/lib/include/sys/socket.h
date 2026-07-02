/* sys/socket.h -- AIOS shadow header. Networking is host-passthrough behind the AIOS boundary: a socket
 * is an AIOS fd (from socket()), so read()/write()/close() work on it; connect() is the client op. The
 * domain/type/protocol values + the sockaddr layout match the host's (see aios_abi.h) so the PAL forwards
 * the bytes straight through; a future seL4 PAL remaps. Server ops (bind/listen/accept) come next. */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <sys/types.h>
#include "aios_abi.h"

typedef unsigned int   socklen_t;
typedef unsigned short sa_family_t;

/* address families / socket types / protocols (AIOS-owned; match the host + aios_abi.h) */
#define AF_INET      AIOS_AF_INET
#define AF_UNSPEC    0
#define PF_INET      AF_INET
#define SOCK_STREAM  AIOS_SOCK_STREAM
#define SOCK_DGRAM   AIOS_SOCK_DGRAM

struct sockaddr { sa_family_t sa_family; char sa_data[14]; };

int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);

#endif /* _SYS_SOCKET_H */
