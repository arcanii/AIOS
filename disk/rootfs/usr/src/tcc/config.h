/* AIOS tcc cross-compile configuration
 *
 * tcc runs on AIOS (AArch64) and generates AArch64 ELF binaries.
 * Cross-compiled from macOS via aios-cc.
 *
 * Disk layout:
 *   /bin/tcc                        tcc binary
 *   /usr/lib/tcc/include/           tcc built-in headers (stdarg.h etc)
 *   /usr/include/                   musl libc headers
 *   /usr/lib/                       libc.a, crt objects, libtcc1.a
 */

#define TCC_VERSION "0.9.28rc"
#define TCC_TARGET_ARM64 1
#define CONFIG_TCC_STATIC 1
#define GCC_MAJOR 15
#define GCC_MINOR 0

/* Paths on the AIOS ext2 filesystem */
#define CONFIG_TCCDIR "/usr/lib/tcc"
#define CONFIG_TCC_SYSINCLUDEPATHS \
    "{B}/include" \
    ":" "/usr/include"
#define CONFIG_TCC_LIBPATHS \
    "{B}" \
    ":" "/usr/lib"
#define CONFIG_TCC_CRTPREFIX "/usr/lib"
#define CONFIG_TCC_ELFINTERP ""

/* Disable features not available on AIOS */
/* #undef CONFIG_TCC_SEMLOCK */
/* #undef CONFIG_TCC_BCHECK */
/* #undef CONFIG_TCC_BACKTRACE */
#define CONFIG_TCC_PREDEFS 1

/* musl libc target */
#define CONFIG_TCC_MUSL 1

/* tcc uses these internally */
#define TCC_LIBTCC1 "libtcc1.a"
