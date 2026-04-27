# --- Kernel Module Build Config ---
obj-m += mattx.o
mattx-objs := mattx_main.o mattx_comm.o mattx_sched.o mattx_migr.o mattx_proc.o mattx_hooks.o mattx_import.o mattx_guest.o mattx_fileio.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# --- User-Space Build Config ---
CFLAGS_USER := -Wall -O2 $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0)
LDFLAGS_USER := $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0)

all: module daemon stub migtest servertest

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

daemon: sbin/mattx_discd.c
	gcc $(CFLAGS_USER) -o sbin/mattx-discd sbin/mattx_discd.c $(LDFLAGS_USER) -lpthread

stub: bin/mattx_stub.c
	gcc $(CFLAGS_USER) -o bin/mattx-stub bin/mattx_stub.c $(LDFLAGS_USER)

migtest: bin/migtest.c
	gcc -o bin/migtest bin/migtest.c

servertest: bin/servertest.c
	gcc -o bin/servertest bin/servertest.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f bin/migtest bin/servertest bin/mattx-stub sbin/mattx-discd

install:
	sudo rm -f /usr/local/bin/migtest /usr/local/bin/servertest /usr/local/bin/mattx-stub /usr/local/sbin/mattx-discd /usr/local/bin/mattx-admin
	sudo cp -f bin/migtest /usr/local/bin/migtest
	sudo cp -f bin/servertest /usr/local/bin/servertest
	sudo cp -f bin/mattx-stub /usr/local/bin/mattx-stub
	sudo cp -f sbin/mattx-discd /usr/local/sbin/mattx-discd
	sudo cp -f bin/mattx-admin /usr/local/bin/mattx-admin
	sudo chmod +x /usr/local/bin/mattx-admin
	sudo cp -f etc/mattx.conf /etc/mattx.conf
	sudo chmod 644 /etc/mattx.conf

	# install the kernel module

	echo "NOTICE: MattX installation complete."
	echo "Please configure /etc/mattx.conf before running the daemon."




uninstall:
	sudo rm -f /usr/local/bin/migtest
	sudo rm -f /usr/local/bin/servertest
	sudo rm -f /usr/local/bin/mattx-stub
	sudo rm -f /usr/local/sbin/mattx-discd
	sudo rm -f /usr/local/bin/mattx-admin
	sudo rm -f /etc/mattx.conf
