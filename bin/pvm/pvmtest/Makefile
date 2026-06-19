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
