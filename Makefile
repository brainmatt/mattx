# --- Kernel Module Build Config ---
obj-m += mattx.o
mattx-objs := mattx_main.o mattx_comm.o mattx_sched.o mattx_migr.o mattx_proc.o mattx_hooks.o mattx_import.o mattx_guest.o mattx_fileio.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# --- User-Space Build Config ---
CFLAGS_USER := -Wall -O2 $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0)
LDFLAGS_USER := $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0)

all: module daemon stub migtest servertestpoll servertestselect

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	# mattxfs
	cd mattxfs && $(MAKE) && cd -

daemon: sbin/mattx_discd.c
	gcc $(CFLAGS_USER) -o sbin/mattx-discd sbin/mattx_discd.c $(LDFLAGS_USER) -lpthread

stub: bin/mattx_stub.c
	gcc $(CFLAGS_USER) -o bin/mattx-stub bin/mattx_stub.c $(LDFLAGS_USER)

migtest: bin/migtest.c
	gcc -o bin/migtest bin/migtest.c

servertestpoll: bin/servertestpoll.c
	gcc -o bin/servertestpoll bin/servertestpoll.c

servertestselect: bin/servertestselect.c
	gcc -o bin/servertestselect bin/servertestselect.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f bin/migtest bin/servertestpoll bin/servertestselect bin/mattx-stub sbin/mattx-discd

install:
	sudo rm -f /usr/local/bin/migtest
	sudo rm -f /usr/local/bin/servertestpoll
	sudo rm -f /usr/local/bin/servertestselect
	sudo rm -f /usr/local/bin/mattx-stub
	sudo rm -f /usr/local/sbin/mattx-discd
	sudo rm -f /usr/local/bin/mattx-admin
	sudo rm -f /etc/mattx.conf
	sudo rm -f /etc/systemd/system/mattx.service
	sudo cp -f bin/migtest /usr/local/bin/migtest
	sudo cp -f bin/servertestpoll /usr/local/bin/servertestpoll
	sudo cp -f bin/servertestselect /usr/local/bin/servertestselect
	sudo cp -f bin/mattx-stub /usr/local/bin/mattx-stub
	sudo cp -f sbin/mattx-discd /usr/local/sbin/mattx-discd
	sudo cp -f bin/mattx-admin /usr/local/bin/mattx-admin
	sudo chmod +x /usr/local/bin/mattx-admin
	sudo cp -f etc/mattx.conf /etc/mattx.conf
	sudo chmod 644 /etc/mattx.conf
	sudo cp -f init/mattx-discd.service /etc/systemd/system/mattx-discd.service


	# install the kernel module

	echo "NOTICE: MattX installation complete."
	echo "Please configure /etc/mattx.conf before running the daemon."




uninstall:
	sudo rm -f /usr/local/bin/migtest
	sudo rm -f /usr/local/bin/servertestpoll
	sudo rm -f /usr/local/bin/servertestselect
	sudo rm -f /usr/local/bin/mattx-stub
	sudo rm -f /usr/local/sbin/mattx-discd
	sudo rm -f /usr/local/bin/mattx-admin
	sudo rm -f /etc/mattx.conf
