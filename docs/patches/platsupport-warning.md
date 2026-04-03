# libsel4platsupport Warning Suppression

## Problem
libsel4platsupport prints "Warning: using printf before serial is set up" 
every time printf is called before platsupport_serial_setup_simple(). 
AIOS uses seL4_DebugPutChar for early boot output, so this warning is noise.

## File Patched
`deps/seL4_libs/libsel4platsupport/src/common.c` — comment out seL4_DebugPutString at line 275-276.

## Re-apply after git pull
```bash
sed -i '' 's/seL4_DebugPutString.*Warning.*printf.*/\/\/ &/' deps/seL4_libs/libsel4platsupport/src/common.c
```
