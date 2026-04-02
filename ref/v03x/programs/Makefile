CC = aarch64-linux-gnu-gcc
OBJCOPY = aarch64-linux-gnu-objcopy
CFLAGS = -nostdlib -nostartfiles -ffreestanding -mcpu=cortex-a53 -mstrict-align -I. -I../include -O2 -ffunction-sections
LDFLAGS = -T link.ld -Wl,--build-id=none

# Architecture support objects
ARCH_DIR = ../src/arch/aarch64
ARCH_OBJS = setjmp.o

SOURCES = $(wildcard *.c)
ELFS = $(SOURCES:.c=.elf)
BINS = $(shell echo $(SOURCES:.c=.BIN) | tr 'a-z' 'A-Z')

# Build all .c files into uppercase .BIN files
all: $(ARCH_OBJS) $(SOURCES:.c=.bin_target)

# Build arch support objects
setjmp.o: $(ARCH_DIR)/setjmp.S
	$(CC) $(CFLAGS) -c $< -o $@

%.bin_target: %.c aios.h link.ld $(ARCH_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $*.elf $< $(ARCH_OBJS)
	$(OBJCOPY) -O binary $*.elf $(shell echo $* | tr 'a-z' 'A-Z').BIN
	@echo "  $(shell echo $* | tr 'a-z' 'A-Z').BIN: $$(ls -la $(shell echo $* | tr 'a-z' 'A-Z').BIN | awk '{print $$5}') bytes"

clean:
	rm -f *.elf *.bin_target $(ARCH_OBJS)

.PHONY: all clean
