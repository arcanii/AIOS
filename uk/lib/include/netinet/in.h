/* netinet/in.h -- AIOS shadow header. IPv4 address types. struct sockaddr_in MUST match the layout in
 * aios_abi.h (struct aios_sockaddr_in) -- the kernel forwards these bytes to the host connect(2). */
#ifndef _NETINET_IN_H
#define _NETINET_IN_H
#include <sys/socket.h>
#include "aios_abi.h"

typedef unsigned short in_port_t;   /* network byte order */
typedef unsigned int   in_addr_t;   /* network byte order */

#define IPPROTO_TCP  AIOS_IPPROTO_TCP
#define IPPROTO_UDP  AIOS_IPPROTO_UDP
#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)   /* host order; wrap with htonl() */
#define INADDR_NONE      ((in_addr_t)0xffffffff)

struct in_addr { in_addr_t s_addr; };
struct sockaddr_in {
    sa_family_t    sin_family;   /* AF_INET */
    in_port_t      sin_port;     /* network byte order (htons) */
    struct in_addr sin_addr;     /* network byte order */
    unsigned char  sin_zero[8];
};

#endif /* _NETINET_IN_H */
