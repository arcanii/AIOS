/* termios.h -- AIOS shadow header (see sys/types.h). Terminal line discipline: tcgetattr/tcsetattr
 * (real, via libaios -> the kernel -> the host tty), so a program can switch the terminal to RAW mode
 * (clear ICANON/ECHO/ISIG) for char-at-a-time, unechoed input. The flag-bit values + c_cc indices
 * match the host's (so the PAL translation is a field copy); struct termios MUST match struct
 * aios_termios in <aios_abi.h>. dash also includes this for JOBS=1 (it #undefs CEOF). */
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

/* c_cc indices */
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VTIME  5
#define VMIN   6
#define VSWTC  7
#define VSTART 8
#define VSTOP  9
#define VSUSP  10
#define VEOL   11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16
#define CEOF VEOF   /* dash reuses the name -- it #undefs this */

/* c_iflag */
#define IGNBRK 0000001
#define BRKINT 0000002
#define IGNPAR 0000004
#define PARMRK 0000010
#define INPCK  0000020
#define ISTRIP 0000040
#define INLCR  0000100
#define IGNCR  0000200
#define ICRNL  0000400
#define IXON   0002000
#define IXANY  0004000
#define IXOFF  0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

/* c_oflag */
#define OPOST  0000001
#define ONLCR  0000004
#define OCRNL  0000010
#define ONOCR  0000020
#define ONLRET 0000040

/* c_cflag */
#define CSIZE  0000060
#define CS5    0000000
#define CS6    0000020
#define CS7    0000040
#define CS8    0000060
#define CSTOPB 0000100
#define CREAD  0000200
#define PARENB 0000400
#define PARODD 0001000
#define HUPCL  0002000
#define CLOCAL 0004000

/* c_lflag */
#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400
#define ECHOCTL 0001000
#define ECHOKE  0004000
#define IEXTEN  0100000

/* baud rates (ignored on a pty, but programs set them) */
#define B0     0000000
#define B9600  0000015
#define B38400 0000017

/* tcsetattr `optional_actions` */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int   tcgetattr(int fd, struct termios *t);
int   tcsetattr(int fd, int actions, const struct termios *t);
pid_t tcgetpgrp(int fd);
int   tcsetpgrp(int fd, pid_t pgrp);

static inline speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
static inline speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }
static inline int cfsetispeed(struct termios *t, speed_t s) { t->c_ispeed = s; return 0; }
static inline int cfsetospeed(struct termios *t, speed_t s) { t->c_ospeed = s; return 0; }
static inline void cfmakeraw(struct termios *t) {
    t->c_iflag &= ~(unsigned)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(unsigned)OPOST;
    t->c_lflag &= ~(unsigned)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(unsigned)(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN]  = 1;
    t->c_cc[VTIME] = 0;
}

#endif /* _TERMIOS_H */
