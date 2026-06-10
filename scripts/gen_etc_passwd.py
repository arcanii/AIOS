#!/usr/bin/env python3
"""Generate disk/rootfs/etc/passwd with salted-KDF password hashes (v0.4.189).

Mirrors the auth-server KDF in src/apps/auth_server.c EXACTLY:
    h = SHA3-512(salt || password)
    repeat AIOS_KDF_ITERS:  h = SHA3-512(h || salt || password)
    stored = "$a1$" + salt_hex + "$" + h_hex

The KDF constants are read from include/aios/aios_auth.h so the host and device
never drift. Re-run this whenever AIOS_KDF_ITERS / AIOS_KDF_SALT_BYTES change,
then rebuild the disk image (scripts/build_apps.py / mkdisk.py).

Only the password-hash field of the known default users is rewritten; uid/gid/
gecos/home/shell and any other users are preserved. Fresh random salts are
generated each run.
"""
import hashlib
import os
import re
import secrets
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(REPO, "include/aios/aios_auth.h")
PASSWD = os.path.join(REPO, "disk/rootfs/etc/passwd")

# The documented AIOS default credentials (username -> password).
DEFAULT_PW = {"root": "root", "user": "user"}


def read_define(name):
    pat = re.compile(r"^\s*#define\s+%s\s+(\d+)" % re.escape(name))
    with open(HDR) as f:
        for line in f:
            m = pat.match(line)
            if m:
                return int(m.group(1))
    sys.exit("error: #define %s not found in %s" % (name, HDR))


ITERS = read_define("AIOS_KDF_ITERS")
SALT_BYTES = read_define("AIOS_KDF_SALT_BYTES")


def kdf(salt: bytes, password: bytes) -> bytes:
    h = hashlib.sha3_512(salt + password).digest()
    for _ in range(ITERS):
        h = hashlib.sha3_512(h + salt + password).digest()
    return h


def make_passhash(password: str) -> str:
    salt = secrets.token_bytes(SALT_BYTES)
    return "$a1$" + salt.hex() + "$" + kdf(salt, password.encode()).hex()


def main():
    if not os.path.exists(PASSWD):
        sys.exit("error: %s does not exist" % PASSWD)
    out = []
    rewrote = []
    for line in open(PASSWD).read().splitlines():
        if not line.strip() or line.startswith("#"):
            out.append(line)
            continue
        f = line.split(":")
        if len(f) < 2 or f[0] not in DEFAULT_PW:
            out.append(line)
            continue
        f[1] = make_passhash(DEFAULT_PW[f[0]])
        rewrote.append(f[0])
        out.append(":".join(f))
    with open(PASSWD, "w") as fh:
        fh.write("\n".join(out) + "\n")
    print("Wrote %s (KDF iters=%d, salt=%d bytes)" % (PASSWD, ITERS, SALT_BYTES))
    for u in rewrote:
        print("  %-8s -> password %r" % (u, DEFAULT_PW[u]))
    missing = set(DEFAULT_PW) - set(rewrote)
    if missing:
        print("  NOTE: default users not present in passwd, skipped: %s"
              % ", ".join(sorted(missing)))


if __name__ == "__main__":
    main()
