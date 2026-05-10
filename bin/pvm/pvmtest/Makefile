# Makefile for MattX PVM3 Debugging Suite

# Ensure PVM_ROOT and PVM_ARCH are set
ifndef PVM_ROOT
$(error PVM_ROOT is not set. Please set it (e.g., export PVM_ROOT=/usr/lib/pvm3))
endif

ifndef PVM_ARCH
$(error PVM_ARCH is not set. Please set it (e.g., export PVM_ARCH=LINUX64))
endif

CC = gcc
CFLAGS = -g -Wall -I$(PVM_ROOT)/include
LDFLAGS = -L$(PVM_ROOT)/lib/$(PVM_ARCH) -lpvm3

# The PVM bin directory where pvm_spawn looks for executables
PVM_BIN_DIR = $(PVM_ROOT)/bin/$(PVM_ARCH)

all: pvmtest pvmtest-client install

pvmtest: pvmtest.c
	$(CC) $(CFLAGS) -o pvmtest pvmtest.c $(LDFLAGS)

pvmtest-client: pvmtest-client.c
	$(CC) $(CFLAGS) -o pvmtest-client pvmtest-client.c $(LDFLAGS)

install: pvmtest-client
	@echo "Copying pvmtest-client to $(PVM_BIN_DIR) so pvm_spawn can find it..."
	cp pvmtest-client $(PVM_BIN_DIR)/

clean:
	rm -f pvmtest pvmtest-client
	rm -f $(PVM_BIN_DIR)/pvmtest-client
	rm -f /tmp/pvmtest.log

.PHONY: all install clean
