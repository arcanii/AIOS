/* arpa/inet.h -- AIOS shadow header. Byte-order + address conversion (implemented in libaios; pure). */
#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>

unsigned short htons(unsigned short x);
unsigned short ntohs(unsigned short x);
unsigned int   htonl(unsigned int x);
unsigned int   ntohl(unsigned int x);
in_addr_t      inet_addr(const char *cp);   /* dotted-quad -> network-order u32, or INADDR_NONE */

#endif /* _ARPA_INET_H */
