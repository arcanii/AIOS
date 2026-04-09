# tcc Patches for AIOS (v0.4.70-0.4.72)

This documents all changes made to the tcc source tree
(~/Desktop/github_repos/tcc) for AIOS compatibility.

## Patch 1: TLS LE Relocation Support (v0.4.71)

**File:** arm64-link.c

seL4 runtime stores the IPC buffer pointer in a __thread variable.
GCC emits R_AARCH64_TLSLE_ADD_TPREL_HI12 (549) and
R_AARCH64_TLSLE_ADD_TPREL_LO12_NC (551) relocations for thread-
local access. tcc arm64-link.c did not handle these types.

Added case handlers in relocate() that compute AArch64 variant-1
TLS offset: TP + TCB(16) + (symbol_addr - first_TLS_section_addr).
Uses FIRST TLS section address as base so .tbss offsets correctly
include .tdata size.

Also added R_AARCH64_PREL64 (260) for PC-relative 64-bit data
references used by sel4runtime.

## Patch 2: Compiled-in Predefs (v0.4.71)

**File:** config.h

Set CONFIG_TCC_PREDEFS=1 so tcc uses compiled-in preprocessor
definitions (tccdefs_.h) instead of searching for tccdefs.h at
runtime. Runtime file lookup fails on AIOS because the predefs
file is not on the ext2 disk.

**File:** tccdefs_.h

Generated from tcc source via the tccdefs generator. Contains
built-in type definitions, macros, and declarations.

## Patch 3: Direct ADRP for Static Binaries (v0.4.72)

**File:** arm64-gen.c, function arm64_sym()

**Problem:** arm64_sym() unconditionally used GOT-based addressing
(R_AARCH64_ADR_GOT_PAGE + R_AARCH64_LD64_GOT_LO12_NC) for all
symbol references. This requires a Global Offset Table section
filled with symbol addresses. For static executables on seL4,
the ELF loader maps PT_LOAD segments but does not set up a GOT.
Result: every access to a global variable or string literal in
tcc-compiled code faulted with a data abort.

**Diagnosis path:**
1. Empty main (return 42) worked -- no global access
2. Intra-module function call (return foo()) worked -- uses BL relocation
3. Global variable access (return x) faulted -- uses ADRP relocation
4. String literal (return s[0]) faulted -- string is global data

**Fix:** When output_type does not include TCC_OUTPUT_DYN, emit
direct PC-relative references instead of GOT indirection:

  Before (GOT):  ADRP Xr, :got_page:sym + LDR Xr, [Xr, :got_lo12:sym]
  After (direct): ADRP Xr, :pg_hi21:sym + ADD Xr, Xr, :lo12:sym

Both produce the symbol address in register Xr. Direct mode avoids
the GOT indirection and does not require a GOT section. The GOT
path is preserved for shared library / dynamic output modes.

**Marker:** AIOS_STATIC_ADRP_FIX

## Commit Message Format

```
aios: tcc arm64 patches for seL4 static binaries

- arm64-link.c: TLS LE relocation (types 549, 551) + PREL64 (260)
- arm64-gen.c: direct ADRP+ADD for static binaries (no GOT)
- config.h: CONFIG_TCC_PREDEFS=1 for compiled-in definitions
```
