#!/usr/bin/env python3
"""Write CMakeLists fragment for crypto_server to /tmp."""
import os, subprocess, sys

# ==============================
# crypto_server_cmake.txt  (fragment to add to CMakeLists.txt)
# ==============================

FNAME = "crypto_server_cmake.txt"
FPATH = f"/tmp/{FNAME}"

if os.path.exists(FPATH):
    print(f"[SKIP] {FPATH} already exists")
else:
    subprocess.run(["bash", "-c", f"""cat << 'ENDOFFILE' > {FPATH}
# ---- crypto_server build fragment ----
# Add this to your main CMakeLists.txt alongside the other server targets.
# Adjust source paths to match your directory layout.
#
# If the crypto sources live in src/crypto/:
#
#   set(CRYPTO_SOURCES
#       src/crypto/crypto_chacha20.c
#       src/crypto/entropy_collect.c
#       src/crypto/crypto_server.c
#   )
#
# Option A: Build as part of the root server (linked into aios_root)
#
#   target_sources(aios_root PRIVATE ${{CRYPTO_SOURCES}})
#   target_include_directories(aios_root PRIVATE src/crypto)
#
# Option B: Build as a standalone ELF (separate process)
#
#   add_executable(crypto_server ${{CRYPTO_SOURCES}})
#   target_include_directories(crypto_server PRIVATE src/crypto)
#   target_link_libraries(crypto_server sel4 muslc)
#   install(TARGETS crypto_server RUNTIME DESTINATION bin/aios)
ENDOFFILE"""], check=True)
    print(f"[DONE] wrote {FPATH}")

print("==============================")
print("  05_cmake_fragment complete")
print("==============================")
