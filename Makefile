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
DISK_IMG := disk.img

# ── Protection domains ──────────────────────────────────
PDS := serial_driver blk_driver fs_server orchestrator llm_server echo_server sandbox net_driver net_server auth_server

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
$(BUILD)/fs_server.elf: $(BUILD)/fs_server.o $(BUILD)/fs_fat16.o $(BUILD)/fs_fat32.o $(BUILD)/fs_ext2.o
	$(LD) $^ $(LDFLAGS) -o $@

$(BUILD)/orchestrator.elf: $(BUILD)/orchestrator.o $(BUILD)/memset.o
	$(LD) $^ $(LDFLAGS) -o $@

$(BUILD)/memset.o: src/memset.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

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
		-cpu cortex-a53 \
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
		-cpu cortex-a53 \
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
PROGS_SRC := $(wildcard programs/*.c)
PROGS_BIN := $(patsubst programs/%.c,programs/%.bin,$(PROGS_SRC))

programs/%.bin: programs/%.c
	$(CC) -c -mcpu=cortex-a53 -ffreestanding -nostdlib -O2 -ffunction-sections -Iprograms $< -o /tmp/$*.o
	$(LD) -T programs/link.ld /tmp/$*.o -o /tmp/$*.elf
	aarch64-linux-gnu-objcopy -O binary /tmp/$*.elf $@
	@echo "  $@: $$(wc -c < $@) bytes"

programs: $(PROGS_BIN)
	@echo "Built $(words $(PROGS_BIN)) sandbox programs"

# Inject all .bin programs onto disk image
inject: $(PROGS_BIN) $(DISK_IMG)
	@for f in programs/*.bin; do \
		name=$$(basename $$f .bin | tr 'a-z' 'A-Z'); \
		echo "  $$f -> $${name}.BIN"; \
		mcopy -i $(DISK_IMG) -o $$f "::$${name}.BIN"; \
	done
	@echo "Disk contents:"
	@mdir -i $(DISK_IMG) :: | grep BIN

# ── Version management ──────────────────────────────────
# Usage:
#   make              — builds and increments build number
#   make bump-patch   — increment Z in X.Y.Z (edit version.h)
#   make bump-minor   — increment Y, reset Z to 0
#   make version      — print current version and build number
.PHONY: bump-patch bump-minor version

version:
	@echo "Version: $$(grep AIOS_VERSION_MAJOR include/aios/version.h | head -1 | awk '{print $$3}').$$(grep AIOS_VERSION_MINOR include/aios/version.h | head -1 | awk '{print $$3}').$$(grep AIOS_VERSION_PATCH include/aios/version.h | head -1 | awk '{print $$3}')"
	@echo "Build:   $$(cat .build_number)"

bump-patch:
	@PATCH=$$(grep 'AIOS_VERSION_PATCH' include/aios/version.h | head -1 | awk '{print $$3}'); \
	NEW=$$((PATCH + 1)); \
	sed -i '' "s/AIOS_VERSION_PATCH  *$$PATCH/AIOS_VERSION_PATCH  $$NEW/" include/aios/version.h; \
	echo "Version bumped to 0.1.$$NEW"

bump-minor:
	@MINOR=$$(grep 'AIOS_VERSION_MINOR' include/aios/version.h | head -1 | awk '{print $$3}'); \
	NEW=$$((MINOR + 1)); \
	sed -i '' "s/AIOS_VERSION_MINOR  *$$MINOR/AIOS_VERSION_MINOR  $$NEW/" include/aios/version.h; \
	sed -i '' "s/AIOS_VERSION_PATCH  *[0-9]*/AIOS_VERSION_PATCH  0/" include/aios/version.h; \
	echo "Version bumped to 0.$$NEW.0"
