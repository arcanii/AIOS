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
CONFIG       := debug
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
          -Iinclude

LDFLAGS := -L$(MICROKIT_DIR)/lib -lmicrokit -Tmicrokit.ld

# ── Directories ─────────────────────────────────────────
BUILD    := build
SRC      := src
DISK_IMG := disk.img

# ── Protection domains ──────────────────────────────────
PDS := serial_driver blk_driver fs_server orchestrator llm_server echo_server

OBJS := $(patsubst %,$(BUILD)/%.o,$(PDS))
ELFS := $(patsubst %,$(BUILD)/%.elf,$(PDS))

# ── Default target ──────────────────────────────────────
.PHONY: all clean run debug disk

all: $(BUILD)/loader.img

# ── Build directory ─────────────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

# ── Compile .c → .o ────────────────────────────────────
$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# ── Link .o → .elf ─────────────────────────────────────
$(BUILD)/%.elf: $(BUILD)/%.o
	$(LD) $< $(LDFLAGS) -o $@

# ── Pack into Microkit loader image ─────────────────────
$(BUILD)/loader.img: $(ELFS) hello.system
	$(MICROKIT_SDK)/bin/microkit hello.system \
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
		-device virtio-blk-device,drive=hd0

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
		-S -s

# ── Clean ────────────────────────────────────────────────
clean:
	rm -rf $(BUILD)

