/* sys/utsname.h -- AIOS shadow header. uname() reports AIOS's OWN identity (sysname "AIOS", the AIOS
 * version as release), not the host's -- a guest sees the AIOS kernel, never Linux. struct utsname MUST
 * match the one in libaios.c (the kernel-agnostic libc fills these fields). */
#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int uname(struct utsname *u);

#endif /* _SYS_UTSNAME_H */
