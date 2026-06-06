#!/usr/bin/env python3
"""
AIOS Application Builder -- single command to rebuild everything

Usage: python3 scripts/build_apps.py [--no-tcc] [--no-sbase] [--no-dash]

Steps:
  1. ninja (incremental kernel + root task + libaios_posix)
  2. sbase (99 Unix tools)
  3. dash (login shell)
  4. tcc (compiler)
  5. libaios + SDK (augmented libc for tcc)
  6. disk image
"""
import os, sys, subprocess, time

AIOS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(AIOS, "build-04")
DASH_SRC = os.path.expanduser("~/Desktop/github_repos/dash/src")
TCC_SRC = os.path.expanduser("~/Desktop/github_repos/tcc")
MBEDTLS_SRC = os.path.expanduser("~/Desktop/github_repos/mbedtls")

def run(cmd, label, cwd=None, tail=3):
    print(f"\n--- {label} ---")
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True,
                       cwd=cwd, shell=isinstance(cmd, str))
    dt = time.time() - t0
    if r.returncode != 0:
        out = r.stdout + r.stderr
        lines = out.strip().splitlines()
        for l in lines[-10:]:
            print(f"  {l}")
        print(f"FAIL -- {label} ({dt:.1f}s)")
        return False
    lines = r.stdout.strip().splitlines()
    for l in lines[-tail:]:
        print(f"  {l}")
    print(f"  OK ({dt:.1f}s)")
    return True


def main():
    skip_tcc = "--no-tcc" in sys.argv
    skip_sbase = "--no-sbase" in sys.argv
    skip_dash = "--no-dash" in sys.argv
    t_start = time.time()

    # 1. ninja
    if not run(["ninja"], "ninja (incremental)", cwd=BUILD):
        sys.exit(1)

    # 2. sbase
    if not skip_sbase:
        run([sys.executable, os.path.join(AIOS, "scripts", "build_sbase.py")],
            "sbase (99 tools)")

    # 3. dash
    if not skip_dash and os.path.isdir(DASH_SRC):
        dash_srcs = [
            "main.c", "eval.c", "parser.c", "expand.c", "exec.c",
            "jobs.c", "trap.c", "redir.c", "input.c", "output.c",
            "var.c", "cd.c", "error.c", "options.c", "memalloc.c",
            "mystring.c", "syntax.c", "nodes.c", "builtins.c",
            "init.c", "show.c", "arith_yacc.c", "arith_yylex.c",
            "miscbltin.c", "system.c", "alias.c", "histedit.c",
            "mail.c", "signames.c",
            "bltin/test.c", "bltin/printf.c", "bltin/times.c",
        ]
        cmd = [os.path.join(AIOS, "scripts", "aios-cc")]
        cmd += [os.path.join(DASH_SRC, s) for s in dash_srcs]
        cmd += ["-I", DASH_SRC, "-include",
                os.path.join(DASH_SRC, "config.h"),
                "-DSHELL", "-DSMALL", "-DGLOB_BROKEN",
                "-o", os.path.join(BUILD, "sbase", "dash")]
        run(cmd, "dash (login shell)")

    # 3b. Standalone aios-cc apps. These are NOT in projects/aios/CMakeLists.txt,
    # so ninja never builds them -- before this step a clean rm -rf build-04 would
    # silently drop them from the disk. psutil ships under 3 argv[0] command names
    # (pidof, pkill, killall) as byte-identical copies.
    apps = [("netconsole.c", "netconsole", []),
            ("netconsole2.c", "netconsole2", []),
            ("psutil.c", "pidof", ["pkill", "killall"]),
            ("nslookup.c", "nslookup", [])]
    for src, out, aliases in apps:
        outp = os.path.join(BUILD, "sbase", out)
        cmd = [os.path.join(AIOS, "scripts", "aios-cc"),
               os.path.join(AIOS, "src", "apps", src), "-o", outp]
        if run(cmd, "app: " + out) and aliases:
            with open(outp, "rb") as f:
                data = f.read()
            for a in aliases:
                ap = os.path.join(BUILD, "sbase", a)
                with open(ap, "wb") as f:
                    f.write(data)
                os.chmod(ap, 0o755)
            print("  aliases: " + ", ".join(aliases))

    # 3c. sshd -- the SSH server (src/ssh/*.c) links against libmbedcrypto.a,
    # which is gitignored and NOT in projects/aios/CMakeLists.txt, so a clean
    # rm -rf build-04 drops it (same gap netconsole/psutil/nslookup had). We
    # rebuild the crypto archive (build_mbedtls.py, ~1.3s) then link sshd with
    # the mbedTLS include path + the arm_neon.h -isystem fix. Skipped cleanly if
    # the mbedTLS source is not checked out.
    if os.path.isdir(MBEDTLS_SRC):
        if run([sys.executable, os.path.join(AIOS, "scripts", "build_mbedtls.py")],
               "libmbedcrypto.a (mbedTLS crypto)", tail=4):
            gcc_inc = subprocess.check_output(
                ["aarch64-linux-gnu-gcc", "-print-file-name=include"]).decode().strip()
            mbed_flags = ["-I", os.path.join(MBEDTLS_SRC, "include"),
                          "-isystem", gcc_inc, "-DMBEDTLS_ALLOW_PRIVATE_ACCESS",
                          os.path.join(BUILD, "libmbedcrypto.a")]
            ssh_srcs = ["sshd_main.c", "ssh_transport.c", "ssh_kex.c",
                        "ssh_crypto.c", "ssh_encrypt.c", "ssh_auth.c",
                        "ssh_channel.c", "ssh_sftp.c"]
            cmd = [os.path.join(AIOS, "scripts", "aios-cc")]
            cmd += [os.path.join(AIOS, "src", "ssh", s) for s in ssh_srcs]
            cmd += ["-I", os.path.join(AIOS, "src", "ssh")] + mbed_flags
            cmd += ["-o", os.path.join(BUILD, "sbase", "sshd")]
            run(cmd, "app: sshd (SSH server)")
            # runtime crypto smoke (scripts/ssh_qemu_test.py runs it pre-SSH)
            cmd = [os.path.join(AIOS, "scripts", "aios-cc"),
                   os.path.join(AIOS, "src", "apps", "test_mbedtls.c")] + mbed_flags
            cmd += ["-o", os.path.join(BUILD, "sbase", "test_mbedtls")]
            run(cmd, "app: test_mbedtls (crypto smoke)")
    else:
        print("\n--- app: sshd ---\n  SKIP (no mbedTLS source at %s)" % MBEDTLS_SRC)

    # 4. tcc
    if not skip_tcc and os.path.isdir(TCC_SRC):
        cmd = [os.path.join(AIOS, "scripts", "aios-cc"),
               os.path.join(TCC_SRC, "tcc.c"),
               "-I", TCC_SRC, "-I", os.path.join(TCC_SRC, "include"),
               "-include", os.path.join(TCC_SRC, "config.h"),
               "-o", os.path.join(BUILD, "sbase", "tcc")]
        run(cmd, "tcc (compiler)")

    # 5. libaios + SDK
    run([sys.executable, os.path.join(AIOS, "scripts", "build_libaios.py")],
        "libaios + CRT", tail=5)
    run([sys.executable, os.path.join(AIOS, "scripts", "build_tcc_libc_blob.py")],
        "tcc libc blob (libaios_tcc.o)", tail=4)
    run([sys.executable, os.path.join(AIOS, "scripts", "build_tcc_sdk.py")],
        "tcc SDK", tail=4)

    # 6. disk image
    cmd = [sys.executable, os.path.join(AIOS, "scripts", "mkdisk.py"),
           os.path.join(AIOS, "disk", "disk_ext2.img"),
           "--rootfs", os.path.join(AIOS, "disk", "rootfs"),
           "--install-elfs", os.path.join(BUILD, "sbase"),
           "--aios-elfs", os.path.join(BUILD, "projects", "aios"),
           "--install-sdk", os.path.join(BUILD, "tcc-sdk")]
    run(cmd, "disk image", tail=3)

    dt = time.time() - t_start
    print(f"\n=== Build complete ({dt:.0f}s) ===")


if __name__ == "__main__":
    main()
