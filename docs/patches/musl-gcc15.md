# musllibc GCC 15 Patch

## Problem
GCC 15's linker rejects copy relocations against protected symbols.
musl uses `#pragma GCC visibility push(protected)` globally, which
causes link failure: "copy relocation against non-copyable protected symbol"

## Files Patched
- `deps/musllibc/src/internal/vis.h` — change `protected` to `default`
- `deps/musllibc/src/internal/stdio_impl.h` — change `protected` to `default`

## Re-apply after git pull
```bash
sed -i '' 's/visibility push(protected)/visibility push(default)/' deps/musllibc/src/internal/vis.h
sed -i '' 's/visibility("protected")/visibility("default")/' deps/musllibc/src/internal/stdio_impl.h
```
