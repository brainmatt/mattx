obj-m += mattxfs.o

KDIR := /lib/modules/$(shell uname -r)/build
# Use absolute path for the current directory
MY_DIR := $(shell pwd)

all:
	# Pass the exact, absolute path to the parent Module.symvers
	$(MAKE) -C $(KDIR) M=$(MY_DIR) KBUILD_EXTRA_SYMBOLS="$(MY_DIR)/../Module.symvers" modules

clean:
	$(MAKE) -C $(KDIR) M=$(MY_DIR) clean