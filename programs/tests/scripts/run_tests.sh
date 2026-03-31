echo "=== AIOS POSIX Test Suite ==="
echo ""
echo "--- fnmatch ---"
/bin/tests/test_fnmatch
echo ""
echo "--- regex ---"
/bin/tests/test_regex
echo ""
echo "--- glob ---"
/bin/tests/test_glob
echo ""
echo "--- wordexp ---"
/bin/tests/test_wordexp
echo ""
echo "--- ioctl ---"
/bin/tests/test_ioctl
echo ""
echo "--- statvfs ---"
/bin/tests/test_statvfs
echo ""
echo "--- posix (sscanf/syslog/strtod) ---"
/bin/tests/test_posix
echo ""
echo "--- setjmp ---"
/bin/tests/test_setjmp
echo ""
echo "=== Test Suite Complete ==="
