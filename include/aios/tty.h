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
#define TTY_POLL        76   /* v0.4.99: MR0=avail_count reply */
#define TTY_SWITCH      79   /* MR0=vt_id */
#define TTY_GETATTR     77
#define TTY_SETFG       78   /* MR0=pid */
/* v0.4.295 PTY step 2 -- master-side ops (instance id in MR0). The serial console is
 * instance 0; PTYs are 1..MAX_TTY-1 (docs/DESIGN_PTY_SSH.md). */
#define TTY_PTY_ALLOC       80   /* (no args) -> reply MR0 = instance id (1..), or <0 if full */
#define TTY_PTY_INPUT       81   /* MR0=inst, MR1=len, MR2..=keystroke bytes -> line discipline */
#define TTY_PTY_MASTER_READ 82   /* MR0=inst, MR1=max -> reply MR0=len, MR1..=shell output + echo */
#define TTY_PTY_WINSZ       83   /* MR0=inst, MR1=rows, MR2=cols */
#define TTY_PTY_FREE        84   /* MR0=inst -> release the instance */

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
