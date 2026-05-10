CC = gcc
CFLAGS = -O2 -Wall

all: pvmtest pvmtest-client

pvmtest: pvmtest.c
	$(CC) $(CFLAGS) -o pvmtest pvmtest.c -lpvm3

pvmtest-client: pvmtest-client.c
	$(CC) $(CFLAGS) -o pvmtest-client pvmtest-client.c -lpvm3

install: all
	mv pvmtest $(PVM_ROOT)/bin/$(PVM_ARCH)/
	mv pvmtest-client $(PVM_ROOT)/bin/$(PVM_ARCH)/

