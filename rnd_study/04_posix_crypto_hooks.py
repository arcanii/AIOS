#!/usr/bin/env python3
"""Write POSIX integration snippets for crypto_server to /tmp.

These are reference fragments, not complete files.  They show
the code to add to posix_file.c (for /dev/urandom reads) and
posix_misc.c (for getrandom syscall).  Adjust to match your
current source layout.
"""
import os, subprocess, sys

# ==============================
# posix_crypto_hooks.c  (reference snippets)
# ==============================

FNAME = "posix_crypto_hooks.c"
FPATH = f"/tmp/{FNAME}"

if os.path.exists(FPATH):
    print(f"[SKIP] {FPATH} already exists")
else:
    subprocess.run(["bash", "-c", f"""cat << 'ENDOFFILE' > {FPATH}
/* posix_crypto_hooks.c -- Reference integration snippets
 *
 * These are NOT a standalone compilation unit.  They show the
 * code fragments to add into posix_file.c and posix_misc.c
 * to route /dev/urandom and getrandom() through crypto_server.
 *
 * Prerequisites:
 *   - crypto_server running and its endpoint stored in crypto_ep
 *   - crypto_server.h included for opcode definitions
 */


/* ============================================================
 * SNIPPET 1:  Add to posix_internal.h (or equivalent)
 * ============================================================ */

/*
extern seL4_CPtr crypto_ep;
*/


/* ============================================================
 * SNIPPET 2:  Add to posix_file.c -- inside handle_read()
 *             where you dispatch on the fd type / path
 * ============================================================ */

/*
static ssize_t crypto_read_random(void *user_buf, size_t count)
{{
    size_t total = 0;
    uint8_t *dst = (uint8_t *)user_buf;

    while (total < count) {{
        size_t want = count - total;
        if (want > CRYPTO_MAX_RANDOM_BYTES)
            want = CRYPTO_MAX_RANDOM_BYTES;

        seL4_SetMR(0, CRYPTO_OP_RANDOM);
        seL4_SetMR(1, (seL4_Word)want);
        seL4_MessageInfo_t msg = seL4_MessageInfo_new(0, 0, 0, 2);
        msg = seL4_Call(crypto_ep, msg);

        size_t nwords = seL4_MessageInfo_get_length(msg);
        size_t got = nwords * sizeof(seL4_Word);
        if (got > want) got = want;

        for (size_t i = 0; i < nwords; i++) {{
            seL4_Word w = seL4_GetMR(i);
            size_t remain = want - (i * sizeof(seL4_Word));
            size_t chunk = sizeof(seL4_Word);
            if (chunk > remain) chunk = remain;
            __builtin_memcpy(dst + total, &w, chunk);
            total += chunk;
        }}
    }}

    return (ssize_t)total;
}}
*/

/* Then in your read dispatch:
 *
 *   if (fd_is_dev_urandom(fd) || fd_is_dev_random(fd)) {{
 *       return crypto_read_random(user_buf, count);
 *   }}
 */


/* ============================================================
 * SNIPPET 3:  Add to posix_misc.c -- getrandom() handler
 *             SYS_getrandom = 278 on AArch64
 * ============================================================ */

/*
static long sys_getrandom(void *buf, size_t buflen, unsigned int flags)
{{
    (void)flags;  // GRND_RANDOM and GRND_NONBLOCK not yet meaningful

    if (buf == NULL || buflen == 0)
        return -EINVAL;

    // Cap at a reasonable maximum per call
    if (buflen > 256)
        buflen = 256;

    return (long)crypto_read_random(buf, buflen);
}}
*/

/* In your syscall dispatch table, wire SYS_getrandom (278):
 *
 *   case 278:
 *       result = sys_getrandom((void *)x0, (size_t)x1, (unsigned int)x2);
 *       break;
 */


/* ============================================================
 * SNIPPET 4:  /dev/urandom and /dev/random node setup
 *             Add to your devfs or ext2 disk image builder
 * ============================================================ */

/* If using mkdisk.py / Ext2Builder, register these paths:
 *
 *   /dev/urandom  -- reads serviced by crypto_server
 *   /dev/random   -- same backing (modern practice: no blocking)
 *
 * In the fd table, mark these fds with a DEV_RANDOM type flag
 * so handle_read() can dispatch to crypto_read_random().
 */
ENDOFFILE"""], check=True)
    print(f"[DONE] wrote {FPATH}")

print("==============================")
print("  04_posix_crypto_hooks complete")
print("==============================")
