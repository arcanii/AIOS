/* prog_regex.c -- a direct correctness gate for the libaios BRE/ERE engine (regcomp/regexec).
 *
 * This exercises the regex engine the way vendored `grep` does -- compile REG_NOSUB, then a boolean
 * regexec(pat, text, 0, NULL, 0) -- across a battery of cases: literals, dot, the *, +, ?, {m,n}
 * quantifiers, ^ $ anchors (and where they are literal in BRE), bracket expressions (ranges, negation,
 * POSIX classes), alternation, grouping, \< \> word boundaries, REG_ICASE, escapes, the exact patterns
 * grep -w / -x build, compile-error reporting, and the catastrophic patterns that prove the matcher is
 * linear and never hangs. Exit 0 iff every case matches its expectation -- the run.sh gate keys on it.
 *
 * expect: 0 = must NOT match, 1 = must match, 2 = regcomp must report an error. */

#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct tc { const char *pat; int cflags; const char *text; int expect; const char *note; };

static struct tc tests[] = {
    /* literals + dot */
    { "abc",     0, "xabcy",   1, "BRE literal substring" },
    { "abc",     0, "abx",     0, "BRE literal no" },
    { "a.c",     0, "axc",     1, "dot" },
    { "a.c",     0, "ac",      0, "dot needs one char" },
    { "a.*z",    0, "a123z",   1, "dot-star span" },
    { "a.*z",    0, "a123",    0, "dot-star no z" },

    /* BRE star */
    { "ab*c",    0, "ac",      1, "BRE star zero" },
    { "ab*c",    0, "abbbc",   1, "BRE star many" },

    /* anchors */
    { "^abc",    0, "abcd",    1, "BRE anchor start" },
    { "^abc",    0, "xabc",    0, "BRE anchor start no" },
    { "abc$",    0, "xabc",    1, "BRE anchor end" },
    { "abc$",    0, "abcx",    0, "BRE anchor end no" },
    { "^abc$",   0, "abc",     1, "BRE full line" },
    { "^abc$",   0, "abcd",    0, "BRE full line no" },
    { "a^b",     0, "a^b",     1, "BRE caret literal mid" },
    { "a$b",     0, "a$b",     1, "BRE dollar literal mid" },

    /* brackets */
    { "[abc]",       0, "xbz",   1, "class member" },
    { "[abc]",       0, "xyz",   0, "class none" },
    { "[a-z]",       0, "9m9",   1, "range hit" },
    { "[^a-z]",      0, "abc9",  1, "neg range hits 9" },
    { "[^a-z]",      0, "abc",   0, "neg range all lower" },
    { "[[:digit:]]", 0, "abc5",  1, "posix digit" },
    { "[[:digit:]]", 0, "abc",   0, "posix digit none" },
    { "[^[:digit:]]",0, "12345", 0, "neg posix all digits" },
    { "[^[:digit:]]",0, "12a45", 1, "neg posix has alpha" },
    { "[]a]",        0, "]",     1, "literal ] first in class" },

    /* ERE quantifiers + alternation + groups */
    { "a+",      REG_EXTENDED, "baaa",   1, "ERE plus" },
    { "a+",      REG_EXTENDED, "b",      0, "ERE plus none" },
    { "ab?c",    REG_EXTENDED, "ac",     1, "ERE quest zero" },
    { "ab?c",    REG_EXTENDED, "abc",    1, "ERE quest one" },
    { "ab?c",    REG_EXTENDED, "abbc",   0, "ERE quest two no" },
    { "foo|bar", REG_EXTENDED, "xbarx",  1, "ERE alt" },
    { "foo|bar", REG_EXTENDED, "baz",    0, "ERE alt no" },
    { "(ab)+",   REG_EXTENDED, "ababab", 1, "ERE group plus" },
    { "(ab)+c",  REG_EXTENDED, "ababc",  1, "ERE group plus c" },
    { "(ab)+c",  REG_EXTENDED, "abac",   0, "ERE group plus c no" },

    /* intervals */
    { "a{2,3}",       REG_EXTENDED, "baaab", 1, "ERE interval hit" },
    { "a{2,3}",       REG_EXTENDED, "bab",   0, "ERE interval too few" },
    { "a{2}",         REG_EXTENDED, "baab",  1, "ERE exact count" },
    { "a{2}",         REG_EXTENDED, "bab",   0, "ERE exact count no" },
    { "ab\\{2,3\\}",  0, "abbb",  1, "BRE interval hit" },
    { "ab\\{2,3\\}",  0, "ab",    0, "BRE interval too few" },

    /* dollar as anchor mid-pattern in ERE */
    { "a$b", REG_EXTENDED, "ab",  0, "ERE dollar mid never matches" },
    { "a$",  REG_EXTENDED, "ba",  1, "ERE dollar at end" },

    /* word boundaries */
    { "\\<cat\\>", 0, "the cat sat", 1, "word boundary both" },
    { "\\<cat\\>", 0, "category",    0, "word end fails on prefix" },
    { "\\<cat",    0, "scattered",   0, "word start fails inside" },
    { "\\<cat",    0, "a cat",       1, "word start hit" },
    { "\\<foo\\>", REG_EXTENDED, "a foo b", 1, "ERE word boundary" },

    /* icase */
    { "abc",   REG_ICASE, "xABCy", 1, "icase literal" },
    { "[a-z]", REG_ICASE, "ABC",   1, "icase class folds" },
    { "FOO",   REG_ICASE, "foo",   1, "icase both ways" },

    /* escapes + literal quantifier chars in BRE */
    { "a\\.c",     0, "a.c",  1, "escaped dot is literal" },
    { "a\\.c",     0, "axc",  0, "escaped dot no" },
    { "a\\*c",     0, "a*c",  1, "escaped star is literal" },
    { "a?b",       0, "a?b",  1, "BRE bare ? is literal" },
    { "a?b",       0, "ab",   0, "BRE bare ? literal no" },
    { "colou\\?r", 0, "color",  1, "BRE GNU optional absent" },
    { "colou\\?r", 0, "colour", 1, "BRE GNU optional present" },
    { "foo\\|bar", 0, "xbar",   1, "BRE GNU alternation" },
    { "\\(ab\\)*", 0, "abab",   1, "BRE group star" },

    /* the exact forms grep -w and grep -x build */
    { "\\<\\(foo\\)\\>", 0, "a foo b", 1, "grep -w BRE form" },
    { "\\<(foo)\\>",     REG_EXTENDED, "a foo b", 1, "grep -wE form" },
    { "^foo$",           0, "foo",     1, "grep -x form" },
    { "^foo$",           0, "foobar",  0, "grep -x no" },

    /* empty pattern matches everything */
    { "",  0, "anything", 1, "empty matches" },
    { "",  0, "",         1, "empty matches empty" },

    /* linear matcher: these must terminate quickly, no catastrophic backtracking */
    { "(a*)*b", REG_EXTENDED, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac", 0, "no catastrophic blowup" },
    { "(a*)*",  REG_EXTENDED, "aaaa", 1, "nullable star no infinite loop" },
    { "(a+)+",  REG_EXTENDED, "aaaa", 1, "nested plus" },

    /* compile errors are reported, not crashed-on */
    { "a\\",     0, "x", 2, "trailing backslash" },
    { "[abc",    0, "x", 2, "unterminated bracket" },
    { "a{2,1}",  REG_EXTENDED, "x", 2, "bad interval order" },
    { "\\(a",    0, "x", 2, "unmatched open group BRE" },
    { "a\\1",    0, "x", 2, "backreference unsupported" },
};

int
main(void)
{
    int n = (int)(sizeof(tests) / sizeof(tests[0]));
    int pass = 0, fail = 0;

    for (int i = 0; i < n; i++) {
        struct tc *t = &tests[i];
        regex_t re;
        int rc = regcomp(&re, t->pat, t->cflags | REG_NOSUB);

        if (rc != 0) {
            if (t->expect == 2) { pass++; continue; }
            char eb[128];
            regerror(rc, &re, eb, sizeof eb);
            printf("FAIL [%s] /%s/ unexpected compile error: %s\n", t->note, t->pat, eb);
            fail++;
            continue;
        }
        if (t->expect == 2) {
            printf("FAIL [%s] /%s/ expected a compile error but it compiled\n", t->note, t->pat);
            regfree(&re);
            fail++;
            continue;
        }

        int matched = (regexec(&re, t->text, 0, NULL, 0) == 0);
        if (matched == t->expect) {
            pass++;
        } else {
            printf("FAIL [%s] /%s/ on \"%s\": got %s, want %s\n", t->note, t->pat, t->text,
                   matched ? "match" : "no-match", t->expect ? "match" : "no-match");
            fail++;
        }
        regfree(&re);
    }

    printf("regex engine: %d/%d cases OK\n", pass, n);
    return fail == 0 ? 0 : 1;
}
