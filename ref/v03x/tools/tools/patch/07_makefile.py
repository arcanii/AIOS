"""Patch Makefile: add ext2-create, ext2-inject, ext2-disk targets."""

from .utils import set_module, log, read_file, write_file, insert_before

PATH = "Makefile"

EXT2_TARGETS = (
    '# ── ext2 disk build ─────────────────────────────────────\n'
    'EXT2_SIZE_MB := 128\n'
    '\n'
    '.PHONY: ext2-create ext2-inject ext2-disk\n'
    '\n'
    '# Create a fresh ext2 image\n'
    'ext2-create:\n'
    '\t@echo "Creating ext2 disk image ($(EXT2_SIZE_MB) MB)..."\n'
    '\tdd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(EXT2_SIZE_MB) 2>/dev/null\n'
    '\tmkfs.ext2 -q -b 4096 $(DISK_IMG)\n'
    '\t@echo "Created $(DISK_IMG)"\n'
    '\n'
    '# Inject programs + config files into ext2 image\n'
    'ext2-inject: $(DISK_IMG)\n'
    '\t@echo "Injecting programs into ext2 image..."\n'
    '\tpython3 tools/ext2_inject.py $(DISK_IMG) programs/*.BIN\n'
    '\t@echo "Injecting config files..."\n'
    '\t@if [ -d disk/etc ]; then \\\n'
    '\t\tfor f in disk/etc/*; do \\\n'
    '\t\t\techo "  $$f"; \\\n'
    '\t\t\tpython3 tools/ext2_inject.py $(DISK_IMG) "$$f"; \\\n'
    '\t\tdone; \\\n'
    '\tfi\n'
    '\t@if [ -f disk/hello.txt ]; then \\\n'
    '\t\tpython3 tools/ext2_inject.py $(DISK_IMG) disk/hello.txt; \\\n'
    '\tfi\n'
    '\n'
    '# Full disk rebuild: create + inject\n'
    'ext2-disk: ext2-create ext2-inject\n'
    '\t@echo "ext2 disk ready: $(DISK_IMG)"\n'
    '\n'
)

def run():
    set_module("MAKE")
    log("=== Patching Makefile ===")
    src = read_file(PATH)
    ok = True

    if 'ext2-inject' not in src:
        src, s = insert_before(src,
            '# ── Version management',
            EXT2_TARGETS,
            "ext2 targets")
        ok = ok and s
    else:
        log("ext2 targets already present, skipping")

    write_file(PATH, src)
    return ok
