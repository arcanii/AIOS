# ══════════════════════════════════════════════════════════
# AIOS Makefile
#
# Build system for the AI Operating System on seL4/Microkit.
# Targets: all, disk, run, debug, clean
#
# Copyright (c) 2025 AIOS Project
# SPDX-License-Identifier: MIT
# ══════════════════════════════════════════════════════════

# ── SDK & Board ──────────────────────────────────────────
MICROKIT_SDK ?= $(HOME)/microkit/microkit-sdk-2.1.0
BOARD        := qemu_virt_aarch64
CONFIG       := smp-debug
MICROKIT_DIR := $(MICROKIT_SDK)/board/$(BOARD)/$(CONFIG)

# ── Toolchain (auto-detect available cross-compiler) ─────
ifneq ($(shell which aarch64-linux-gnu-gcc 2>/dev/null),)
    CC := aarch64-linux-gnu-gcc
    LD := aarch64-linux-gnu-ld
else ifneq ($(shell which aarch64-elf-gcc 2>/dev/null),)
    CC := aarch64-elf-gcc
    LD := aarch64-elf-ld
else
    $(error No AArch64 cross-compiler found. Install aarch64-linux-gnu-gcc or aarch64-elf-gcc)
endif

# ── Compiler / Linker flags ─────────────────────────────
CFLAGS := -c -mcpu=cortex-a53 -mstrict-align \
          -nostdlib -ffreestanding \
          -g -Wall -Werror \
          -I$(MICROKIT_DIR)/include \
          -Iinclude -Ilibc/include

LDFLAGS := -L$(MICROKIT_DIR)/lib -lmicrokit -Tmicrokit.ld

# ── Directories ─────────────────────────────────────────
BUILD    := build
SRC      := src
DISK_IMG := disk_ext2.img

# ── Protection domains ──────────────────────────────────
PDS := serial_driver blk_driver fs_server vfs_server orchestrator sandbox net_driver net_server auth_server

OBJS := $(patsubst %,$(BUILD)/%.o,$(PDS))
ELFS := $(patsubst %,$(BUILD)/%.elf,$(PDS))

# ── Default target ──────────────────────────────────────
.PHONY: all clean run debug disk


all: $(BUILD)/loader.img

# ── Build number (auto-increments every compile) ─────────────
.PHONY: build_number
build_number:
	@sh tools/bump_build.sh

# ── Build directory ─────────────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

# ── Compile .c → .o ────────────────────────────────────
$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# ── Link .o → .elf ─────────────────────────────────────
# ── Compile fs_fat16 ────────────────────────────────────
$(BUILD)/fs_server.o: src/fs/vfs.c
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/fs_fat16.o: src/fs/fat16.c
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/fs_fat32.o: src/fs/fat32.c
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/fs_ext2.o: src/fs/ext2.c
	$(CC) -c $(CFLAGS) $< -o $@

# ── Link fs_server with FAT backends ───────────────────
$(BUILD)/fs_server.elf: $(BUILD)/fs_server.o $(BUILD)/fs_fat16.o $(BUILD)/fs_fat32.o $(BUILD)/fs_ext2.o $(BUILD)/util.o
	$(LD) $^ $(LDFLAGS) -o $@


$(BUILD)/vfs_server.elf: $(BUILD)/vfs_server.o $(BUILD)/util.o
	$(LD) $^ $(LDFLAGS) -o $@
$(BUILD)/orchestrator.elf: $(BUILD)/orchestrator.o $(BUILD)/memset.o $(BUILD)/util.o
	$(LD) $^ $(LDFLAGS) -o $@

$(BUILD)/memset.o: src/memset.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/util.o: src/util.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/net_server.elf: $(BUILD)/net_server.o $(BUILD)/util.o
	$(LD) $^ $(LDFLAGS) -o $@

$(BUILD)/net_driver.elf: $(BUILD)/net_driver.o $(BUILD)/util.o
	$(LD) $^ $(LDFLAGS) -o $@

# ── setjmp.o (AArch64 asm) ───────────────────────────────
$(BUILD)/setjmp.o: src/arch/aarch64/setjmp.S | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/sandbox.elf: $(BUILD)/sandbox.o $(BUILD)/setjmp.o
	$(LD) $^ $(LDFLAGS) -o $@

$(BUILD)/%.elf: $(BUILD)/%.o
	$(LD) $< $(LDFLAGS) -o $@

# ── Pack into Microkit loader image ─────────────────────
$(BUILD)/loader.img: build_number $(ELFS) aios.system
	$(MICROKIT_SDK)/bin/microkit aios.system \
		--search-path $(BUILD) \
		--board $(BOARD) \
		--config $(CONFIG) \
		-o $@ \
		-r $(BUILD)/report.txt

# ── Disk image ──────────────────────────────────────────
# Creates a 64 MiB FAT16 image with test file and optional
# model/tokenizer binaries.
# Requires: mtools (brew install mtools / apt install mtools)

disk: $(DISK_IMG)

$(DISK_IMG):
	@echo "Creating FAT16 disk image..."
	dd if=/dev/zero of=$@ bs=1M count=128
	mformat -i $@ -t 256 -h 16 -s 63 -M 512 -v AIOS ::
	printf "Hello from AIOS disk! This is our FAT filesystem.\n" \
		| mcopy -i $@ - ::hello.txt
	@if [ -f code25M.bin ]; then \
		echo "  Adding code25M.bin -> CODE25M.BIN"; \
		mcopy -i $@ code25M.bin ::CODE25M.BIN; \
	else \
		echo "  code25M.bin not found (skipping model)"; \
	fi
	@if [ -f tokenizer.bin ]; then \
		echo "  Adding tokenizer.bin -> TOK.BIN"; \
		mcopy -i $@ tokenizer.bin ::TOK.BIN; \
	else \
		echo "  tokenizer.bin not found (skipping tokenizer)"; \
	fi
	@echo "  Adding reference files..."
	
	for f in ref/*; do \
		echo "    $$f -> R_$$(basename $$f)"; \
		mcopy -i $@ $$f "::R_$$(basename $$f)"; \
	done
	@echo "Disk contents:"
	mdir -i $@ ::

# ── Run in QEMU ─────────────────────────────────────────
run: $(BUILD)/loader.img $(DISK_IMG)
	qemu-system-aarch64 \
		-machine virt,virtualization=on \
		-cpu cortex-a53 -smp 4 \
		-m 2G \
		-nographic \
		-serial mon:stdio \
		-kernel $< \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
		-device virtio-blk-device,drive=hd0 \
		-device virtio-net-device,netdev=net0 \
		-netdev user,id=net0,hostfwd=tcp::8888-:80

# ── Run with GDB server ─────────────────────────────────
debug: $(BUILD)/loader.img $(DISK_IMG)
	qemu-system-aarch64 \
		-machine virt,virtualization=on \
		-cpu cortex-a53 -smp 4 \
		-m 2G \
		-nographic \
		-serial mon:stdio \
		-kernel $< \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
		-device virtio-blk-device,drive=hd0 \
		-device virtio-net-device,netdev=net0 \
		-netdev user,id=net0,hostfwd=tcp::8888-:80 \
		-S -s

# ── Clean ────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)


# ── Sandbox programs ────────────────────────────────────
# All intermediates go in $(BUILD)/prg/ to keep source tree clean.

PRG_CFLAGS := -c -mcpu=cortex-a53 -ffreestanding -nostdlib -O2 -ffunction-sections -Iprograms -Iinclude

PROGS_SRC := $(wildcard programs/*.c)
PROGS_BIN := $(patsubst programs/%.c,programs/%.bin,$(PROGS_SRC))

SBIN_SRC  := $(wildcard programs/sbin/*.c)
SBIN_BIN  := $(patsubst programs/sbin/%.c,programs/sbin/%.bin,$(SBIN_SRC))

TESTS_SRC := $(wildcard programs/tests/*.c)
TESTS_BIN := $(patsubst programs/tests/%.c,programs/tests/%.bin,$(TESTS_SRC))

.PHONY: programs
programs: $(PROGS_BIN) $(SBIN_BIN) $(TESTS_BIN)
	@echo "Built $(words $(PROGS_BIN)) programs + $(words $(SBIN_BIN)) sbin + $(words $(TESTS_BIN)) tests"

$(BUILD)/prg:
	mkdir -p $(BUILD)/prg

programs/tests/%.bin: programs/tests/%.c | $(BUILD)/prg
	@mkdir -p $(BUILD)/prg/tests
	$(CC) $(PRG_CFLAGS) $< -o $(BUILD)/prg/tests/$*.o
	$(LD) -T programs/link.ld $(BUILD)/prg/tests/$*.o -o $(BUILD)/prg/tests/$*.elf
	aarch64-linux-gnu-objcopy -O binary $(BUILD)/prg/tests/$*.elf $@
	@echo "  $@: $$(wc -c < $@) bytes"

programs/sbin/%.bin: programs/sbin/%.c | $(BUILD)/prg
	@mkdir -p $(BUILD)/prg/sbin
	$(CC) $(PRG_CFLAGS) $< -o $(BUILD)/prg/sbin/$*.o
	$(LD) -T programs/link.ld $(BUILD)/prg/sbin/$*.o -o $(BUILD)/prg/sbin/$*.elf
	aarch64-linux-gnu-objcopy -O binary $(BUILD)/prg/sbin/$*.elf $@
	@echo "  $@: $$(wc -c < $@) bytes"

programs/%.bin: programs/%.c | $(BUILD)/prg
	$(CC) $(PRG_CFLAGS) $< -o $(BUILD)/prg/$*.o
	$(LD) -T programs/link.ld $(BUILD)/prg/$*.o -o $(BUILD)/prg/$*.elf
	aarch64-linux-gnu-objcopy -O binary $(BUILD)/prg/$*.elf $@
	@echo "  $@: $$(wc -c < $@) bytes"


# Inject all .bin programs onto disk image
inject: $(PROGS_BIN) $(DISK_IMG)
	@for f in programs/*.bin; do \
		name=$$(basename $$f .bin | tr 'a-z' 'A-Z'); \
		echo "  $$f -> $${name}.BIN"; \
		mcopy -i $(DISK_IMG) -o $$f "::$${name}.BIN"; \
	done
	@echo "Disk contents:"
	@mdir -i $(DISK_IMG) :: | grep BIN

# ── ext2 disk build ─────────────────────────────────────
EXT2_SIZE_MB := 128

.PHONY: ext2-create ext2-inject ext2-disk

# Create a fresh ext2 image
ext2-create:
	@echo "Creating ext2 disk image ($(EXT2_SIZE_MB) MB)..."
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=$(EXT2_SIZE_MB) 2>/dev/null
	python3 tools/mkext2.py $(DISK_IMG) $(EXT2_SIZE_MB)
	@echo "Created $(DISK_IMG)"

# Inject programs + config files into ext2 image
ext2-inject: $(DISK_IMG)
	@echo "Injecting programs into ext2 image..."
	python3 tools/ext2_inject.py $(DISK_IMG) programs/*.bin
	@if ls programs/sbin/*.bin 1>/dev/null 2>&1; then \
		echo "Injecting sbin programs..."; \
		python3 tools/ext2_inject.py $(DISK_IMG) programs/sbin/*.bin; \
	fi
	@if ls programs/tests/*.bin 1>/dev/null 2>&1; then \
		echo "Injecting test programs..."; \
		python3 tools/ext2_inject.py $(DISK_IMG) programs/tests/*.bin; \
	fi
	@echo "Injecting config files..."
	@if [ -d disk/etc ]; then \
		for f in disk/etc/*; do \
			echo "  $$f"; \
			python3 tools/ext2_inject.py $(DISK_IMG) "$$f"; \
		done; \
	fi
	@if [ -f disk/hello.txt ]; then \
		python3 tools/ext2_inject.py $(DISK_IMG) disk/hello.txt; \
	fi

# Full disk rebuild: create + inject
ext2-disk: ext2-create ext2-inject
	@echo "ext2 disk ready: $(DISK_IMG)"

# ── Version management ──────────────────────────────────
# Usage:
#   make              — builds and increments build number
#   make bump-patch   — increment Z in X.Y.Z (edit version.h)
#   make bump-minor   — increment Y, reset Z to 0
#   make version      — print current version and build number
.PHONY: system-check
system-check:
	python3 tools/system_validate.py

.PHONY: bump-patch bump-minor version

version:
	@echo "Version: $$(grep AIOS_VERSION_MAJOR include/aios/version.h | head -1 | awk '{print $$3}').$$(grep AIOS_VERSION_MINOR include/aios/version.h | head -1 | awk '{print $$3}').$$(grep AIOS_VERSION_PATCH include/aios/version.h | head -1 | awk '{print $$3}')"
	@echo "Build:   $$(cat .build_number)"

bump-patch:
	@PATCH=$$(grep 'AIOS_VERSION_PATCH' include/aios/version.h | head -1 | awk '{print $$3}'); \
	NEW=$$((PATCH + 1)); \
	sed -i '' "s/AIOS_VERSION_PATCH  *$$PATCH/AIOS_VERSION_PATCH  $$NEW/" include/aios/version.h; \
	MAJ=$$(grep AIOS_VERSION_MAJOR include/aios/version.h | head -1 | awk '{print $$3}'); \
	MIN=$$(grep AIOS_VERSION_MINOR include/aios/version.h | head -1 | awk '{print $$3}'); \
	echo "Version bumped to $$MAJ.$$MIN.$$NEW"

bump-minor:
	@MINOR=$$(grep 'AIOS_VERSION_MINOR' include/aios/version.h | head -1 | awk '{print $$3}'); \
	NEW=$$((MINOR + 1)); \
	sed -i '' "s/AIOS_VERSION_MINOR  *$$MINOR/AIOS_VERSION_MINOR  $$NEW/" include/aios/version.h; \
	sed -i '' "s/AIOS_VERSION_PATCH  *[0-9]*/AIOS_VERSION_PATCH  0/" include/aios/version.h; \
	MAJ=$$(grep AIOS_VERSION_MAJOR include/aios/version.h | head -1 | awk '{print $$3}'); \
	echo "Version bumped to $$MAJ.$$NEW.0"


# Run POSIX compliance audit and update docs/POSIX_COMPLIANCE.md
.PHONY: posix-audit
posix-audit:
	python3 tools/posix_audit.py

# -- Regenerate aios_system.json from aios.system (single source of truth) --
.PHONY: gen-json
gen-json:
	@python3 tools/gen_system_json.py aios.system aios_system.json
	@echo "aios_system.json regenerated from aios.system"

# -- Run all validation and generation steps --
.PHONY: all-checks
all-checks: gen-json check
	@echo "All checks and generation complete"

