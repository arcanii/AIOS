#!/bin/bash
# Quick sbase smoke test — run each tool with minimal args
# Boot AIOS, then paste these commands one at a time

echo "=== AIOS sbase smoke test ==="

# Group 1: No-arg tools (should just work)
echo "--- no-arg tools ---"
true && echo "true: OK"
false || echo "false: OK"
pwd
whoami
hostname
uname
uname -a
date
tty
logname
printenv

# Group 2: Text processing (stdin/file)
echo "--- text tools ---"
echo "hello world" | wc
echo "hello world" | wc -l
echo "hello" | rev
echo "3 2 1" | tr ' ' '\n' | sort
echo "aaa" | md5sum
echo "hello" | cut -c1-3
echo "hello" | fold -w 3
echo "hello world" | grep hello
echo "1 2 3" | tr ' ' '\n'
seq 1 5
cal
expr 2 + 3

# Group 3: File operations
echo "--- file ops ---"
ls /
ls -l /etc
cat /etc/hostname
head -1 /etc/passwd
wc /etc/hostname
strings /bin/true | head -3

# Group 4: Path tools
echo "--- path tools ---"
basename /etc/hostname
dirname /etc/hostname
readlink /bin/ls
which ls
env

# Group 5: Filesystem mutations
echo "--- fs mutations ---"
mkdir /tmp/testdir
touch /tmp/testfile
ls /tmp
cp /etc/hostname /tmp/hostname_copy
cat /tmp/hostname_copy
rm /tmp/hostname_copy
rmdir /tmp/testdir

echo "=== done ==="
