#
# MattX - The Modern Single System Image (SSI) Cluster
# 
# Copyright (c) 2026 by Matthias Rechenburg
# All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# Commercial licensing options are available upon request.
#

# --- Kernel Module Build Config ---
ccflags-y += -Wno-error=date-time
obj-m += mattx.o
mattx-objs := mattx_main.o mattx_comm.o mattx_sched.o mattx_migr.o mattx_proc.o mattx_hooks.o mattx_import.o mattx_guest.o mattx_fileio.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# --- User-Space Build Config ---
CFLAGS_USER := -Wall -O2 $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0)
LDFLAGS_USER := $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0)

all: module daemon stub migtest migtest2 servertestpoll servertestselect dfsatest epolltest

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

migtest2: bin/migtest2.c
	gcc -o bin/migtest2 bin/migtest2.c

servertestpoll: bin/servertestpoll.c
	gcc -o bin/servertestpoll bin/servertestpoll.c

servertestselect: bin/servertestselect.c
	gcc -o bin/servertestselect bin/servertestselect.c

dfsatest: bin/dfsatest.c
	gcc -o bin/dfsatest bin/dfsatest.c

epolltest: bin/epolltest.c
	gcc -o bin/epolltest bin/epolltest.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f bin/migtest bin/migtest2 bin/servertestpoll bin/servertestselect bin/mattx-stub sbin/mattx-discd bin/dfsatest bin/epolltest
	rm -f mattxfs/Module.symvers

install:
	sudo rm -f /usr/local/bin/migtest
	sudo rm -f /usr/local/bin/migtest2
	sudo rm -f /usr/local/bin/servertestpoll
	sudo rm -f /usr/local/bin/servertestselect
	sudo rm -f /usr/local/bin/mattx-stub
	sudo rm -f /usr/local/sbin/mattx-discd
	sudo rm -f /usr/local/bin/mattx-admin
	sudo rm -f /usr/local/bin/dfsatest
	sudo rm -f /etc/mattx.conf
	sudo rm -f /etc/systemd/system/mattx.service
	sudo cp -f bin/migtest /usr/local/bin/migtest
	sudo cp -f bin/migtest2 /usr/local/bin/migtest2
	sudo cp -f bin/servertestpoll /usr/local/bin/servertestpoll
	sudo cp -f bin/servertestselect /usr/local/bin/servertestselect
	sudo cp -f bin/dfsatest /usr/local/bin/dfsatest
	sudo cp -f bin/epolltest /usr/local/bin/epolltest
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
	sudo rm -f /usr/local/bin/migtest2
	sudo rm -f /usr/local/bin/servertestpoll
	sudo rm -f /usr/local/bin/servertestselect
	sudo rm -f /usr/local/bin/dfsatest
	sudo rm -f /usr/local/bin/epolltest
	sudo rm -f /usr/local/bin/mattx-stub
	sudo rm -f /usr/local/sbin/mattx-discd
	sudo rm -f /usr/local/bin/mattx-admin
	sudo rm -f /etc/mattx.conf
