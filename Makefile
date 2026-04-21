# --- Kernel Module Build Config ---
obj-m += mattx.o
mattx-objs := mattx_main.o mattx_comm.o mattx_sched.o mattx_migr.o mattx_proc.o mattx_hooks.o mattx_import.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# --- User-Space Build Config ---
CFLAGS_USER := -Wall -O2 $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0)
LDFLAGS_USER := $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0)

all: module daemon stub migtest

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

daemon: mattx_discd.c
	gcc $(CFLAGS_USER) -o mattx-discd mattx_discd.c $(LDFLAGS_USER) -lpthread

stub: mattx_stub.c
	gcc $(CFLAGS_USER) -o mattx-stub mattx_stub.c $(LDFLAGS_USER)

migtest: migtest.c
	gcc -o migtest migtest.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f mattx-discd mattx-stub migtest

