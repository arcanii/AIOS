CC = aarch64-linux-gnu-gcc
OBJCOPY = aarch64-linux-gnu-objcopy
CFLAGS = -nostdlib -nostartfiles -ffreestanding -mcpu=cortex-a53 -mstrict-align -I. -I../include -O2 -ffunction-sections
LDFLAGS = -T link.ld -Wl,--build-id=none

SOURCES = $(wildcard *.c)
ELFS = $(SOURCES:.c=.elf)
BINS = $(shell echo $(SOURCES:.c=.BIN) | tr 'a-z' 'A-Z')

# Build all .c files into uppercase .BIN files
all: $(SOURCES:.c=.bin_target)

%.bin_target: %.c aios.h posix.h link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $*.elf $<
	$(OBJCOPY) -O binary $*.elf $(shell echo $* | tr 'a-z' 'A-Z').BIN
	@echo "  $(shell echo $* | tr 'a-z' 'A-Z').BIN: $$(ls -la $(shell echo $* | tr 'a-z' 'A-Z').BIN | awk '{print $$5}') bytes"

clean:
	rm -f *.elf *.bin_target

.PHONY: all clean
