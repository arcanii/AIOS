echo "=== AIOS POSIX Test Suite ==="
echo ""
echo "--- fnmatch ---"
test_fnmatch
echo ""
echo "--- regex ---"
test_regex
echo ""
echo "--- glob ---"
test_glob
echo ""
echo "--- wordexp ---"
test_wordexp
echo ""
echo "--- ioctl ---"
test_ioctl
echo ""
echo "--- statvfs ---"
test_statvfs
echo ""
echo "--- posix (sscanf/syslog/strtod) ---"
test_posix
echo ""
echo "--- setjmp ---"
test_setjmp
echo ""
echo "=== Test Suite Complete ==="
