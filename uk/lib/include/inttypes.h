/* inttypes.h -- AIOS shadow header. dash's arithmetic uses intmax_t + PRIdMAX. intmax_t/uintmax_t
 * come from the compiler's <stdint.h>; on LP64 they are long, so the MAX format macros use "l". */
#ifndef _INTTYPES_H
#define _INTTYPES_H
#include <stdint.h>
#include <stdlib.h>     /* strtoll/strtoull for strtoimax/strtoumax */

#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define PRIXMAX "lX"
#define PRIoMAX "lo"

#define strtoimax(s, e, b) strtoll((s), (e), (b))
#define strtoumax(s, e, b) strtoull((s), (e), (b))

#endif /* _INTTYPES_H */
