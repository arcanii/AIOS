#!/bin/bash
# AIOS automated test runner
# Usage: sh tools/test.sh [test_name]
# Runs QEMU with scripted input, captures output, checks for expected results

QEMU="qemu-system-aarch64"
KERNEL="build/loader.img"
DISK="disk.img"
TIMEOUT=10
PASS=0
FAIL=0
RESULTS=""

run_test() {
    local name="$1"
    local input="$2"
    local expect="$3"
    
    # Write input commands to a temp file
    local infile=$(mktemp)
    local outfile=$(mktemp)
    
    # Add commands: exec SHELL.BIN, then test commands, then exit + shutdown
    printf "exec SHELL.BIN\r\n" > "$infile"
    while IFS= read -r cmd; do
        printf "%s\r\n" "$cmd" >> "$infile"
    done <<< "$input"
    printf "exit\r\n" >> "$infile"
    printf "shutdown\r\n" >> "$infile"
    
    # Run QEMU with piped input
    timeout $TIMEOUT $QEMU \
        -machine virt,virtualization=on \
        -cpu cortex-a53 \
        -m 2G \
        -display none \
        -serial file:"$outfile" \
        -monitor none \
        -kernel "$KERNEL" \
        -drive file="$DISK",format=raw,if=none,id=hd0 \
        -device virtio-blk-device,drive=hd0 \
        -device virtio-net-device,netdev=net0 \
        -netdev user,id=net0,hostfwd=tcp::8888-:80 \
        -chardev file,id=chin,path="$infile" \
        2>/dev/null &
    
    local pid=$!
    sleep $TIMEOUT
    kill $pid 2>/dev/null
    wait $pid 2>/dev/null
    
    # Check output for expected strings
    local pass=1
    local missing=""
    while IFS= read -r pattern; do
        [ -z "$pattern" ] && continue
        if ! grep -q "$pattern" "$outfile" 2>/dev/null; then
            pass=0
            missing="$missing  MISSING: $pattern\n"
        fi
    done <<< "$expect"
    
    if [ $pass -eq 1 ]; then
        printf "  ✅ %s\n" "$name"
        PASS=$((PASS + 1))
    else
        printf "  ❌ %s\n" "$name"
        printf "$missing"
        FAIL=$((FAIL + 1))
    fi
    RESULTS="$RESULTS$name:$pass\n"
    
    rm -f "$infile" "$outfile"
}

# Recreate clean disk before tests
echo "=== AIOS Test Suite ==="
echo "Preparing clean disk..."
rm -f disk.img
make disk > /dev/null 2>&1
make inject > /dev/null 2>&1
echo ""

# ── Boot test ──
echo "── Boot & Version ──"
run_test "Boot banner" \
    "" \
    "AIOS v0.1
seL4 14.0.0
FAT16"

# ── File operations ──
echo ""
echo "── File Operations ──"

run_test "echo > redirect" \
    "echo hello world > test.txt
cat test.txt" \
    "hello world"

# Need fresh disk for each test that modifies files
rm -f disk.img; make disk > /dev/null 2>&1; make inject > /dev/null 2>&1

run_test "echo >> append" \
    "echo first > test.txt
echo second >> test.txt
cat test.txt" \
    "first
second"

rm -f disk.img; make disk > /dev/null 2>&1; make inject > /dev/null 2>&1

run_test "cp and cat" \
    "cp hello.txt copy.txt
cat copy.txt" \
    "Hello from AIOS disk"

rm -f disk.img; make disk > /dev/null 2>&1; make inject > /dev/null 2>&1

run_test "stat file" \
    "stat hello.txt" \
    "Size: 50 bytes"

run_test "wc file" \
    "wc hello.txt" \
    "1 9 50 hello.txt"

# ── Pipes ──
echo ""
echo "── Pipes ──"

run_test "cat | wc" \
    "cat hello.txt | wc" \
    "9 5"

run_test "echo | cat" \
    "echo piped text | cat" \
    "piped text"

run_test "ls | wc" \
    "ls | wc" \
    ""

# ── Input redirection ──
echo ""
echo "── Input Redirection ──"

run_test "wc < file" \
    "wc < hello.txt" \
    "1 9 50"

# ── Directory operations ──
echo ""
echo "── Directory Operations ──"

rm -f disk.img; make disk > /dev/null 2>&1; make inject > /dev/null 2>&1

run_test "mkdir and rmdir" \
    "mkdir mydir
ls
rmdir mydir
ls" \
    "MYDIR"

rm -f disk.img; make disk > /dev/null 2>&1; make inject > /dev/null 2>&1

run_test "nested mkdir" \
    "mkdir mydir
mkdir mydir/sub
cp hello.txt mydir/sub/test.txt
cat mydir/sub/test.txt" \
    "Hello from AIOS disk"

# ── External programs ──
echo ""
echo "── External Programs ──"

rm -f disk.img; make disk > /dev/null 2>&1; make inject > /dev/null 2>&1

run_test "hello program" \
    "hello" \
    "Hello"

run_test "fib program" \
    "fib" \
    "fib"

run_test "sieve program" \
    "sieve" \
    ""

# ── Rename ──
echo ""
echo "── Rename ──"

rm -f disk.img; make disk > /dev/null 2>&1; make inject > /dev/null 2>&1

run_test "rename file" \
    "cp hello.txt test.txt
rename test.txt renamed.txt
cat renamed.txt" \
    "Hello from AIOS disk"

# ── Summary ──
echo ""
echo "================================"
echo "  Results: $PASS passed, $FAIL failed"
echo "================================"

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
