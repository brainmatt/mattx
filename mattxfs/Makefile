obj-m += mattxfs.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# NEW: Tell the linker where to find mattx.ko's exported symbols!
KBUILD_EXTRA_SYMBOLS := $(PWD)/../Module.symvers

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
