/* sys/ioctl.h -- AIOS shadow header. No terminal-geometry source yet: ioctl always fails (ENOTTY),
 * so dash falls back to its default terminal width. struct winsize for TIOCGWINSZ callers. */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

int ioctl(int fd, unsigned long request, ...);

#endif /* _SYS_IOCTL_H */
