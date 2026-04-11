#!/bin/dash
# test_079.sh -- v0.4.79 feature verification
# Run: dash /tmp/test_079.sh

pass=0
fail=0
total=0

ok() {
    echo "  PASS: $1"
    pass=$((pass + 1))
    total=$((total + 1))
}

ng() {
    echo "  FAIL: $1"
    fail=$((fail + 1))
    total=$((total + 1))
}

echo "=== v0.4.79 Test Suite ==="
echo ""

# -- /proc/self/fd directory --
echo "[/proc/self/fd]"

ls /proc/self/fd > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "ls /proc/self/fd (exit 0)"; else ng "ls /proc/self/fd"; fi

# stat check (test -d uses fstatat internally)
test -d /proc/self/fd
if [ $? -eq 0 ]; then ok "test -d /proc/self/fd"; else ng "test -d /proc/self/fd"; fi

test -d /proc/self
if [ $? -eq 0 ]; then ok "test -d /proc/self"; else ng "test -d /proc/self"; fi

# -- readlinkat --
echo ""
echo "[readlinkat]"

readlink /proc/self/exe > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "readlink /proc/self/exe (exit 0)"; else ng "readlink /proc/self/exe"; fi

readlink /proc/self/fd/0 > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "readlink /proc/self/fd/0 (exit 0)"; else ng "readlink /proc/self/fd/0"; fi

readlink /proc/self/fd/1 > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "readlink /proc/self/fd/1 (exit 0)"; else ng "readlink /proc/self/fd/1"; fi

# -- procfs entries (exit code only) --
echo ""
echo "[procfs]"

cat /proc/cpuinfo > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "cat /proc/cpuinfo (exit 0)"; else ng "cat /proc/cpuinfo"; fi

cat /proc/loadavg > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "cat /proc/loadavg (exit 0)"; else ng "cat /proc/loadavg"; fi

cat /proc/stat > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "cat /proc/stat (exit 0)"; else ng "cat /proc/stat"; fi

# -- file create/read/unlink (builtins + exit codes) --
echo ""
echo "[ext2 unlink]"

echo "test079" > /tmp/t079.txt
test -f /tmp/t079.txt
if [ $? -eq 0 ]; then ok "echo > /tmp/t079.txt (created)"; else ng "echo > /tmp/t079.txt"; fi

rm /tmp/t079.txt 2>/dev/null
test -f /tmp/t079.txt
if [ $? -ne 0 ]; then ok "rm /tmp/t079.txt (gone)"; else ng "rm /tmp/t079.txt (still exists)"; fi

# -- pipe test (builtin echo on write side) --
echo ""
echo "[pipes]"

echo hello | cat > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "echo hello | cat (exit 0)"; else ng "echo hello | cat"; fi

# -- visual checks (print to terminal for manual verification) --
echo ""
echo "[visual checks]"
echo "  readlink /proc/self/exe:"
readlink /proc/self/exe
echo "  readlink /proc/self/fd/0:"
readlink /proc/self/fd/0
echo "  ls /proc/self/fd:"
ls /proc/self/fd

# -- summary --
echo ""
echo "=== Results: $pass/$total PASS, $fail FAIL ==="
