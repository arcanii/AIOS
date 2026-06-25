/* ctype.h -- AIOS shadow header (see sys/types.h). Implemented in libaios. */
#ifndef _CTYPE_H
#define _CTYPE_H

int isspace(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int isprint(int c);
int ispunct(int c);
int iscntrl(int c);
int isblank(int c);
int isgraph(int c);
int toupper(int c);
int tolower(int c);

#endif /* _CTYPE_H */
