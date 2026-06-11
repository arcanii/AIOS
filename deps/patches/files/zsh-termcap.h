/* AIOS termcap.h -- VT100 termcap for ZLE (v0.4.99) */
#ifndef AIOS_TERMCAP_H
#define AIOS_TERMCAP_H

int tgetent(char *bp, const char *name);
int tgetnum(const char *id);
int tgetflag(const char *id);
char *tgetstr(const char *id, char **area);
char *tgoto(const char *cap, int col, int row);
int tputs(const char *str, int affcnt, int (*putc_fn)(int));
char *tparm(const char *str, ...);

/* curterm stubs for zsh module system */
extern void *cur_term;
int setupterm(const char *term, int fd, int *err);
void *set_curterm(void *nterm);
int del_curterm(void *oterm);

#endif
