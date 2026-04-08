/*
 * AIOS TTY Subsystem — IPC labels and constants
 */
#ifndef AIOS_TTY_H
#define AIOS_TTY_H

/* Legacy serial labels (backward compat with mini_shell) */
#define SER_PUTC        1
#define SER_GETC        2
#define SER_PUTS        3
#define SER_KEY_PUSH    4

/* TTY IPC labels */
#define TTY_WRITE       70   /* MR0=len, MR1..=data (packed 8/MR) */
#define TTY_READ        71   /* MR0=max_len → reply MR0=len, MR1..=data */
#define TTY_IOCTL       72   /* MR0=op, MR1..=args */
#define TTY_OPEN        73
#define TTY_CLOSE       74
#define TTY_INPUT       75   /* MR0=char (from root UART poll) */
#define TTY_SWITCH      76   /* MR0=vt_id */
#define TTY_GETATTR     77
#define TTY_SETFG       78   /* MR0=pid */

/* IOCTL operations */
#define TTY_IOCTL_SET_RAW       1
#define TTY_IOCTL_SET_COOKED    2
#define TTY_IOCTL_ECHO_ON       3
#define TTY_IOCTL_ECHO_OFF      4
#define TTY_IOCTL_GET_MODE      5
#define TTY_IOCTL_TCGETS        6
#define TTY_IOCTL_TCSETS        7
#define TTY_IOCTL_TCSETSW       8
#define TTY_IOCTL_TCSETSF       9

/* TTY modes */
#define TTY_MODE_COOKED  0
#define TTY_MODE_RAW     1

/* Buffer sizes */
#define TTY_IBUF_SZ     512
#define TTY_LBUF_SZ     256

#endif /* AIOS_TTY_H */
