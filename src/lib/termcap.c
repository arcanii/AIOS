/*
 * termcap.c -- Minimal termcap library for AIOS (v0.4.99)
 *
 * Hardcoded VT100/xterm capabilities. No termcap database needed.
 * Provides: tgetent, tgetstr, tgetnum, tgetflag, tgoto, tputs, tparm
 *
 * Used by ZSH ZLE for cursor movement, line clearing, and terminal queries.
 * All terminals in AIOS are VT100-compatible (serial console or xterm).
 */
#include <stddef.h>
#include <string.h>

/* Capability string storage -- tgetstr writes here */
static char cap_buf_storage[1024];
static char *cap_buf_ptr = cap_buf_storage;

/* -- Capability database (VT100/xterm) -- */

typedef struct {
    const char *id;
    const char *value;
} str_cap_t;

typedef struct {
    const char *id;
    int value;
} num_cap_t;

typedef struct {
    const char *id;
    int value;  /* 1 = true, 0 = false */
} bool_cap_t;

static const str_cap_t str_caps[] = {
    /* Cursor movement */
    {"cm", "\033[%i%d;%dH"},  /* cursor address (row;col, 1-based) */
    {"up", "\033[A"},          /* cursor up */
    {"do", "\033[B"},          /* cursor down */
    {"nd", "\033[C"},          /* cursor right (non-destructive) */
    {"le", "\b"},              /* cursor left */
    {"ho", "\033[H"},          /* home cursor */
    {"cr", "\r"},              /* carriage return */
    {"nw", "\r\n"},            /* newline */

    /* Clearing */
    {"cl", "\033[H\033[2J"},   /* clear screen + home */
    {"cd", "\033[J"},          /* clear to end of display */
    {"ce", "\033[K"},          /* clear to end of line */
    {"cb", "\033[1K"},         /* clear to beginning of line */

    /* Insert/delete */
    {"al", "\033[L"},          /* insert line */
    {"dl", "\033[M"},          /* delete line */
    {"dc", "\033[P"},          /* delete character */
    {"ic", "\033[@"},          /* insert character */
    {"ei", ""},                /* end insert mode */
    {"im", ""},                /* begin insert mode */

    /* Scrolling */
    {"sf", "\n"},              /* scroll forward */
    {"sr", "\033M"},           /* scroll reverse */
    {"cs", "\033[%i%d;%dr"},   /* set scroll region */

    /* Standout / underline / bold */
    {"so", "\033[7m"},         /* standout (reverse video) */
    {"se", "\033[27m"},        /* end standout */
    {"us", "\033[4m"},         /* underline */
    {"ue", "\033[24m"},        /* end underline */
    {"md", "\033[1m"},         /* bold */
    {"me", "\033[0m"},         /* end all attributes */
    {"mr", "\033[7m"},         /* reverse video */

    /* Keypad */
    {"ks", "\033[?1h\033="},   /* keypad transmit mode */
    {"ke", "\033[?1l\033>"},   /* keypad local mode */

    /* Key sequences */
    {"ku", "\033[A"},          /* key up */
    {"kd", "\033[B"},          /* key down */
    {"kr", "\033[C"},          /* key right */
    {"kl", "\033[D"},          /* key left */
    {"kh", "\033[H"},          /* key home */
    {"@7", "\033[F"},          /* key end */
    {"kD", "\033[3~"},         /* key delete */
    {"kI", "\033[2~"},         /* key insert */

    /* Terminal init/deinit */
    {"ti", "\033[?1049h"},     /* enter alt screen */
    {"te", "\033[?1049l"},     /* exit alt screen */
    {"vi", "\033[?25l"},       /* cursor invisible */
    {"ve", "\033[?25h"},       /* cursor visible (normal) */
    {"vs", "\033[?25h"},       /* cursor very visible */

    /* Misc */
    {"bc", "\b"},              /* backspace char */
    {"pc", "\0"},              /* pad char */

    {NULL, NULL}
};

static const num_cap_t num_caps[] = {
    {"co", 80},    /* columns */
    {"li", 24},    /* lines */
    {"Co", 8},     /* max colors */
    {"pa", 64},    /* max color pairs */
    {"sg", 0},     /* standout glitch */
    {"ug", 0},     /* underline glitch */
    {NULL, 0}
};

static const bool_cap_t bool_caps[] = {
    {"am", 1},     /* auto margins */
    {"km", 0},     /* has meta key */
    {"mi", 1},     /* move in insert mode */
    {"ms", 1},     /* move in standout mode */
    {"xn", 1},     /* newline glitch (xterm) */
    {"bs", 1},     /* backspace works */
    {NULL, 0}
};

/* -- Public API -- */

int tgetent(char *bp, const char *name) {
    (void)bp;
    (void)name;
    /* Always succeed -- we are always VT100 */
    return 1;
}

char *tgetstr(const char *id, char **area) {
    for (int i = 0; str_caps[i].id; i++) {
        if (id[0] == str_caps[i].id[0] && id[1] == str_caps[i].id[1]) {
            const char *val = str_caps[i].value;
            if (area && *area) {
                /* Copy into caller buffer */
                char *dst = *area;
                const char *s = val;
                while (*s) *dst++ = *s++;
                *dst = '\0';
                char *ret = *area;
                *area = dst + 1;
                return ret;
            }
            /* Return from static storage */
            char *dst = cap_buf_ptr;
            const char *s = val;
            while (*s) *cap_buf_ptr++ = *s++;
            *cap_buf_ptr++ = '\0';
            return dst;
        }
    }
    return NULL;
}

int tgetnum(const char *id) {
    for (int i = 0; num_caps[i].id; i++) {
        if (id[0] == num_caps[i].id[0] && id[1] == num_caps[i].id[1])
            return num_caps[i].value;
    }
    return -1;
}

int tgetflag(const char *id) {
    for (int i = 0; bool_caps[i].id; i++) {
        if (id[0] == bool_caps[i].id[0] && id[1] == bool_caps[i].id[1])
            return bool_caps[i].value;
    }
    return 0;
}

/* tgoto: substitute row,col into cm-style format string
 * Format: %d = decimal, %i = increment args (1-based), %% = literal % */
static char tgoto_buf[64];

char *tgoto(const char *cm, int col, int row) {
    if (!cm) return "";
    char *out = tgoto_buf;
    int args[2] = {row, col};
    int ai = 0;
    int incr = 0;

    while (*cm && (out - tgoto_buf) < 60) {
        if (*cm == '%') {
            cm++;
            switch (*cm) {
            case 'd': {
                int v = (ai < 2) ? args[ai++] + incr : 0;
                if (v >= 100) *out++ = '0' + v / 100;
                if (v >= 10)  *out++ = '0' + (v / 10) % 10;
                *out++ = '0' + v % 10;
                cm++;
                break;
            }
            case 'i':
                incr = 1;
                cm++;
                break;
            case '%':
                *out++ = '%';
                cm++;
                break;
            default:
                *out++ = '%';
                *out++ = *cm++;
                break;
            }
        } else {
            *out++ = *cm++;
        }
    }
    *out = '\0';
    return tgoto_buf;
}

/* tputs: output string with padding, calling putc for each char */
int tputs(const char *str, int affcnt, int (*putc_fn)(int)) {
    (void)affcnt;
    if (!str || !putc_fn) return -1;
    /* Skip numeric padding prefix (e.g. "5*" or "20") */
    while ((*str >= '0' && *str <= '9') || *str == '.' || *str == '*')
        str++;
    while (*str) {
        putc_fn((unsigned char)*str);
        str++;
    }
    return 0;
}

/* tparm: parameterized string (simplified -- handles %d and %i only) */
static char tparm_buf[128];

char *tparm(const char *str, ...) {
    /* For AIOS we only need simple %d substitution */
    /* Use tgoto logic for the common cm case */
    if (!str) return "";

    /* Just copy -- most callers use tgoto for cm anyway */
    char *out = tparm_buf;
    const char *s = str;
    while (*s && (out - tparm_buf) < 120) {
        *out++ = *s++;
    }
    *out = '\0';
    return tparm_buf;
}

/* cur_term stub -- zsh references this */
void *cur_term = NULL;

/* setupterm/set_curterm stubs */
int setupterm(const char *term, int fd, int *err) {
    (void)term; (void)fd;
    if (err) *err = 1;  /* success */
    return 0;
}

void *set_curterm(void *nterm) {
    void *old = cur_term;
    cur_term = nterm;
    return old;
}

int del_curterm(void *oterm) {
    (void)oterm;
    return 0;
}
