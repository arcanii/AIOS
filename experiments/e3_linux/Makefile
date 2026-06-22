# Build the E3 kernel module natively on the Raspberry Pi (Raspberry Pi OS).
#   sudo apt update && sudo apt install -y raspberrypi-kernel-headers
#   make
obj-m += e3_dvm_test.o
obj-m += e3_wfi.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
