/* netdb.h -- AIOS shadow header. The name resolver: gethostbyname / getaddrinfo, implemented in libaios
 * as a from-scratch UDP DNS client over the AIOS socket ABI (SOCK_DGRAM + connect + read/write) with
 * SO_RCVTIMEO for timeout/retry -- no new PAL/ABI. The nameserver comes from $AIOS_DNS_SERVER ("ip[:port]")
 * else /etc/resolv.conf. Scope: A records (IPv4) only, no search domains, first answer wins. struct
 * hostent/addrinfo live in aios_abi.h so libaios.c sees the identical layout under every build flag. */
#ifndef _NETDB_H
#define _NETDB_H
#include "aios_abi.h"

#define h_addr h_addr_list[0]

#define EAI_NONAME AIOS_EAI_NONAME
#define EAI_FAIL   AIOS_EAI_FAIL
#define EAI_MEMORY AIOS_EAI_MEMORY

struct hostent *gethostbyname(const char *name);
int  getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);

#endif /* _NETDB_H */
