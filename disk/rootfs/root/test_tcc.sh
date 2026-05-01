#!/bin/dash
# Test on-AIOS tcc + pre-linked libaios_tcc.o blob.
echo "=== test_tcc.sh start ==="
echo "Compiling /tmp/hi.c via on-AIOS tcc..."
/bin/tcc -o /tmp/hi /tmp/hi.c
echo "tcc exit=$?"
echo "Running /tmp/hi:"
/tmp/hi
echo "/tmp/hi exit=$?"
echo "=== test_tcc.sh DONE ==="
