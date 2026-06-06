#!/usr/bin/env python3
"""QEMU SSH reconnect regression test (v0.4.178).

Boots AIOS under QEMU (-smp 4, the config that reproduced the bug), logs in
on the serial console, then makes 6 sequential SSH connections (3 iterations
x {pty, no-pty}) running `echo CONN<n>; exit`. Each must echo its marker back.

History: before v0.4.178 only the FIRST connection per boot worked; the rest
failed ("key exchange failed" / "Permission denied"). The cause was NOT the
shell fork (that was a red herring) -- it was two pre-existing leaks, both
fixed in v0.4.178:
  1. O_NONBLOCK leaked across connections via fd-slot reuse (channel_relay set
     it and never restored it; aios_fd_alloc did not zero reused slots) ->
     sock_read_exact got EAGAIN and treated it as a fatal read error.
  2. sshd never released its auth_server session (AUTH_LOGIN without
     AUTH_LOGOUT) -> the 4-slot session table filled after a few logins.
Expected result now: conn 0..2 all PASS for both -tt and -T.
"""
import importlib.util, os, subprocess, time
REPO=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK="/tmp/aios-ssh-rc.sock"; ASKPASS="/tmp/aios-ssh-askpass.sh"
spec=importlib.util.spec_from_file_location("ac",REPO+"/scripts/aios_console.py")
ac=importlib.util.module_from_spec(spec); spec.loader.exec_module(ac)

def ssh_run(commands, pty, timeout=40):
    open(ASKPASS,"w").write("#!/bin/sh\necho root\n"); os.chmod(ASKPASS,0o755)
    env=dict(os.environ); env.update(SSH_ASKPASS=ASKPASS,SSH_ASKPASS_REQUIRE="force",
        DISPLAY=os.environ.get("DISPLAY",":0"))
    cmd=["ssh","-tt" if pty else "-T","-p","2222","-o","StrictHostKeyChecking=no",
        "-o","UserKnownHostsFile=/dev/null","-o","PreferredAuthentications=password",
        "-o","PubkeyAuthentication=no","-o","KbdInteractiveAuthentication=no",
        "-o","NumberOfPasswordPrompts=1","-o","ConnectTimeout=10","-o","LogLevel=ERROR",
        "root@127.0.0.1"]
    try:
        r=subprocess.run(cmd,input=commands.encode(),capture_output=True,env=env,
            timeout=timeout,start_new_session=True)
        return r.returncode,r.stdout.decode("utf-8","replace"),r.stderr.decode("utf-8","replace")
    except subprocess.TimeoutExpired:
        return None,"","TIMEOUT"

def qemu_cmd():
    c=["qemu-system-aarch64","-machine","virt,virtualization=on","-cpu","cortex-a53",
       "-smp","4","-m","2G","-display","none","-monitor","none","-no-reboot",
       "-serial","unix:%s,server"%SOCK,
       "-kernel",REPO+"/build-04/images/aios_root-image-arm-qemu-arm-virt"]
    for i,p in enumerate(["disk/disk_ext2.img","disk/log_ext2.img"]):
        pp=os.path.join(REPO,p)
        if os.path.exists(pp):
            c+=["-drive","file=%s,format=raw,if=none,id=hd%d"%(pp,i),"-device","virtio-blk-device,drive=hd%d"%i]
    c+=["-netdev","user,id=n0,hostfwd=tcp:127.0.0.1:2222-:2222","-device","virtio-net-device,netdev=n0"]
    return c

def main():
    if os.path.exists(SOCK): os.unlink(SOCK)
    proc=subprocess.Popen(qemu_cmd())
    npass=0; ntotal=0
    try:
        s=ac.connect_qemu_socket(SOCK); con=ac.Console(s.fileno(),echo=False)
        con.read_until(["AIOS login:"],120); time.sleep(6); con.read_until(["__q__"],3)
        con.ensure_shell("root","root",60,nudge=True,settle=1.0)
        time.sleep(1)
        for i in range(3):
            for pty in (True, False):
                lab="-tt" if pty else "-T "
                rc,sout,serr=ssh_run("echo CONN%d_%s\nexit\n"%(i,lab.strip()),pty)
                mark="CONN%d_%s"%(i,lab.strip())
                ok=mark in sout; ntotal+=1; npass+=1 if ok else 0
                print("conn %d %s: %s rc=%s %s"%(i,lab,"PASS" if ok else "FAIL",rc,
                    "" if ok else repr((sout[:80],serr[:80]))),flush=True)
                time.sleep(3)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except: proc.kill()
        if os.path.exists(SOCK): os.unlink(SOCK)
    print("\n%d/%d PASS"%(npass,ntotal))
    raise SystemExit(0 if npass==ntotal and ntotal>0 else 1)

if __name__=="__main__":
    main()
