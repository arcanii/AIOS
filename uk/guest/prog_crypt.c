/* prog_crypt.c -- a gate for libaios crypt() (SHA-512 "$6$" password hashing, glibc-compatible).
 *
 * The strong proof: AIOS's crypt() must produce hashes BYTE-IDENTICAL to the host's glibc / `openssl
 * passwd -6`, so /etc/shadow hashes are portable and real. We hardcode reference vectors computed by
 * the host (openssl passwd -6) and assert crypt() reproduces them exactly, plus that a wrong password
 * does NOT, and that re-hashing the produced hash as a setting round-trips (the verify path login uses).
 *
 * Exit 0 iff every check passes -- the gate keys on it. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>

struct vec { const char *key, *setting, *expect; };

static const struct vec vecs[] = {
    /* openssl passwd -6 -salt aiossalt aios */
    { "aios", "$6$aiossalt",
      "$6$aiossalt$RniFLmVGV7vaoLmNnMHSgLxQ0nTG4iNL61yVU3c6W09Df0xuSmUaFvd1PVx9YAX8BDudi0ZCyxsFu.qDWkqD3/" },
    /* openssl passwd -6 -salt rootsalt root */
    { "root", "$6$rootsalt",
      "$6$rootsalt$DMaW/SWhAWN4kO5JlNx8ozBbTvstA7SEpK23VKJsNNwPfGKtVkA28Khht9Dlhody/oJANee.ewEebcJXbvL.T1" },
};

int
main(void)
{
    int fail = 0;
    unsigned int i;

    for (i = 0; i < sizeof vecs / sizeof vecs[0]; i++) {
        char *h = crypt(vecs[i].key, vecs[i].setting);
        if (!h || strcmp(h, vecs[i].expect) != 0) {
            printf("FAIL crypt(\"%s\", \"%s\")\n  got:  %s\n  want: %s\n",
                   vecs[i].key, vecs[i].setting, h ? h : "(null)", vecs[i].expect);
            fail = 1;
        } else {
            printf("crypt(\"%s\") matches host openssl passwd -6\n", vecs[i].key);
        }
    }

    /* a wrong password must NOT reproduce the stored hash. */
    char *bad = crypt("wrongpw", vecs[0].expect);   /* use the full hash as the setting (login's path) */
    if (bad && strcmp(bad, vecs[0].expect) == 0) { printf("FAIL wrong password matched\n"); fail = 1; }
    else printf("wrong password does not match the stored hash\n");

    /* the verify round-trip: crypt(correct_key, stored_hash) == stored_hash (this is how login checks). */
    char *ok = crypt(vecs[0].key, vecs[0].expect);
    if (!ok || strcmp(ok, vecs[0].expect) != 0) { printf("FAIL verify round-trip\n"); fail = 1; }
    else printf("verify round-trip: crypt(key, stored_hash) == stored_hash\n");

    /* an unsupported scheme returns NULL (not a crash / not a false match). */
    if (crypt("x", "$1$abc") != 0) { printf("FAIL $1$ should be unsupported (NULL)\n"); fail = 1; }
    else printf("unsupported scheme ($1$) -> NULL\n");

    printf(fail ? "prog_crypt: FAIL\n" : "prog_crypt: all crypt checks passed\n");
    return fail;
}
