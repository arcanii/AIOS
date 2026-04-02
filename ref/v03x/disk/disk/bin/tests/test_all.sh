#!/bin/sh
echo "========================================"
echo "  AIOS Full Test Suite"
echo "========================================"
echo ""
./test_basic
echo ""
./test_fileio
echo ""
./test_threads
echo ""
./test_signals
echo ""
echo "========================================"
echo "  All tests complete"
echo "========================================"
