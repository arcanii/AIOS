#!/bin/dash
# test_080.sh -- v0.4.80 feature verification
# Run: dash /bin/test/test_080.sh

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

echo "=== v0.4.80 Test Suite ==="
echo ""

# -- config files exist on disk --
echo "[config files]"

test -f /etc/environment
if [ $? -eq 0 ]; then ok "/etc/environment exists"; else ng "/etc/environment"; fi

test -f /etc/network.conf
if [ $? -eq 0 ]; then ok "/etc/network.conf exists"; else ng "/etc/network.conf"; fi

test -f /etc/hostname
if [ $? -eq 0 ]; then ok "/etc/hostname exists"; else ng "/etc/hostname"; fi

test -f /etc/passwd
if [ $? -eq 0 ]; then ok "/etc/passwd exists"; else ng "/etc/passwd"; fi

# -- environment from /etc/environment --
echo ""
echo "[environment]"

test -n "$HOME"
if [ $? -eq 0 ]; then ok "HOME=$HOME"; else ng "HOME not set"; fi

test -n "$PATH"
if [ $? -eq 0 ]; then ok "PATH=$PATH"; else ng "PATH not set"; fi

test "$SHELL" = "/bin/dash"
if [ $? -eq 0 ]; then ok "SHELL=$SHELL"; else ng "SHELL=$SHELL (expected /bin/dash)"; fi

test "$TERM" = "vt100"
if [ $? -eq 0 ]; then ok "TERM=$TERM"; else ng "TERM=$TERM (expected vt100)"; fi

test -n "$HOSTNAME"
if [ $? -eq 0 ]; then ok "HOSTNAME=$HOSTNAME"; else ng "HOSTNAME not set"; fi

# -- hostname --
echo ""
echo "[hostname]"

uname -n > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "uname -n (exit 0)"; else ng "uname -n"; fi

# -- /proc/mounts --
echo ""
echo "[/proc/mounts]"

cat /proc/mounts > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "cat /proc/mounts (exit 0)"; else ng "cat /proc/mounts"; fi

# -- pipes (v0.4.79 EOF fix) --
echo ""
echo "[pipes]"

echo hello | cat > /dev/null 2>&1
if [ $? -eq 0 ]; then ok "echo hello | cat (exit 0)"; else ng "echo hello | cat"; fi

# -- file ops --
echo ""
echo "[ext2]"

echo "cfg_test" > /tmp/cfg_test.txt
test -f /tmp/cfg_test.txt
if [ $? -eq 0 ]; then ok "write /tmp/cfg_test.txt"; else ng "write failed"; fi

rm /tmp/cfg_test.txt 2>/dev/null
test -f /tmp/cfg_test.txt
if [ $? -ne 0 ]; then ok "rm /tmp/cfg_test.txt"; else ng "rm failed"; fi

# -- visual checks --
echo ""
echo "[visual]"
echo "  uname -n:"
uname -n
echo "  /proc/mounts:"
cat /proc/mounts

# -- summary --
echo ""
echo "=== Results: $pass/$total PASS, $fail FAIL ==="
