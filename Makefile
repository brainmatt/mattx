# --- Kernel Module Build Config ---
obj-m += mattx.o
mattx-objs := mattx_kern.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# --- User-Space Build Config ---
CFLAGS_USER := -Wall -O2 $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0)
LDFLAGS_USER := $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0)

all: module daemon

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

daemon: mattx_discd.c
	gcc $(CFLAGS_USER) -o mattx-discd mattx_discd.c $(LDFLAGS_USER)

stub: mattx_stub.c
	gcc -Wall -O2 $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0) -o mattx-stub mattx_stub.c $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f mattx-discd
