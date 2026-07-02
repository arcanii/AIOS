/* sys/socket.h -- AIOS shadow header. Networking is host-passthrough behind the AIOS boundary: a socket
 * is an AIOS fd (from socket()), so read()/write()/close() work on it; connect() is the client op,
 * bind()/listen()/accept() are the server ops (accept() returns a NEW AIOS fd). The domain/type/protocol
 * values + the sockaddr layout match the host's (see aios_abi.h) so the PAL forwards the bytes straight
 * through; a future seL4 PAL remaps. */
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

/* setsockopt levels + option names (values match the host; see aios_abi.h) */
#define SOL_SOCKET    AIOS_SOL_SOCKET
#define SO_REUSEADDR  AIOS_SO_REUSEADDR
#define SO_REUSEPORT  AIOS_SO_REUSEPORT

struct sockaddr { sa_family_t sa_family; char sa_data[14]; };

int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);

#endif /* _SYS_SOCKET_H */
