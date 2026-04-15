#include <stdio.h>

struct FA { int x; };
typedef struct { int t; void *ref; } CType;

typedef struct TS {
    int v;
    unsigned short r;
    union {
        struct {
            int c;
            union { int sym_scope; int jnext; struct FA f; int auxtype; };
        };
        long long enum_val;
        int *d;
        struct TS *cleanup_func;
    };
    CType type;
    union {
        struct TS *next;
        int *e;
        int asm_label;
        struct TS *cleanupstate;
        int *vla_array_str;
    };
    struct TS *prev;
    union {
        struct TS *prev_tok;
        struct TS *cleanup_sym;
        struct TS *cleanup_label;
    };
} TS;

int main(void)
{
    TS a, b;
    int pass = 0, fail = 0;

    a.v = 42; a.r = 7;
    if (a.v == 42 && a.r == 7) { puts("T1 basic: OK"); pass++; }
    else { puts("T1 basic: FAIL"); fail++; }

    a.c = 100;
    if (a.c == 100) { puts("T2 nested c: OK"); pass++; }
    else { puts("T2 nested c: FAIL"); fail++; }

    b.v = 99;
    a.cleanupstate = &b;
    if (a.cleanupstate->v == 99) { puts("T3 cleanupstate: OK"); pass++; }
    else { puts("T3 cleanupstate: FAIL"); fail++; }

    a.cleanup_sym = &b;
    if (a.cleanup_sym->v == 99) { puts("T4 cleanup_sym: OK"); pass++; }
    else { puts("T4 cleanup_sym: FAIL"); fail++; }

    a.cleanup_label = &b;
    if (a.cleanup_label->v == 99) { puts("T5 cleanup_label: OK"); pass++; }
    else { puts("T5 cleanup_label: FAIL"); fail++; }

    b.cleanup_label = &a;
    a.v = 77;
    if (b.cleanup_label->v == 77) { puts("T6 chain: OK"); pass++; }
    else { puts("T6 chain: FAIL"); fail++; }

    TS *g = &a;
    g->cleanup_label = &b;
    b.r = 0x20;
    if (g->cleanup_label->r == 0x20) { puts("T7 ptr deref: OK"); pass++; }
    else { puts("T7 ptr deref: FAIL"); fail++; }

    printf("Result: %d/%d PASS\n", pass, pass + fail);
    return fail;
}
