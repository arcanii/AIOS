/* pwd.h -- AIOS shadow header (see sys/types.h). getpwuid/getpwnam read /etc/passwd (implemented in
 * libaios), returning a pointer to static storage, so ls -l shows real user names; a missing or
 * unreadable file yields NULL -> the numeric-uid fallback. struct passwd MUST match the copy in libaios. */
#ifndef _PWD_H
#define _PWD_H
#include <sys/types.h>

struct passwd {
    char        *pw_name;
    char        *pw_passwd;
    uid_t        pw_uid;
    gid_t        pw_gid;
    char        *pw_gecos;
    char        *pw_dir;
    char        *pw_shell;
};

struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);

#endif /* _PWD_H */
