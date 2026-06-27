/* termios.h -- AIOS shadow header (see sys/types.h). dash with JOBS=1 includes <termios.h>; it does
 * NOT call the line-discipline functions (tcgetattr/tcsetattr) -- it only needs the header to exist,
 * the tty foreground-group ops (declared in <unistd.h>), and CEOF (which it promptly #undefs). The
 * struct + tc* declarations are here for completeness; a real line-discipline layer can grow later. */
#ifndef _TERMIOS_H
#define _TERMIOS_H
#include <sys/types.h>

#define NCCS 32
typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int  speed_t;

struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed, c_ospeed;
};

/* tcsetattr `optional_actions` */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* control-character index (dash #undefs this -- its lexer reuses the name) */
#define CEOF 4

int     tcgetattr(int fd, struct termios *t);
int     tcsetattr(int fd, int actions, const struct termios *t);
pid_t   tcgetpgrp(int fd);
int     tcsetpgrp(int fd, pid_t pgrp);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int     cfsetispeed(struct termios *t, speed_t s);
int     cfsetospeed(struct termios *t, speed_t s);

#endif /* _TERMIOS_H */
