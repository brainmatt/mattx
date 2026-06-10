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

# Makefile for MattX MPI Debugging Suite

# Use the standard MPI C compiler wrapper
CC = mpicc
CFLAGS = -g -Wall

all: mpitest mpitest-client

mpitest: mpitest.c
	$(CC) $(CFLAGS) -o mpitest mpitest.c

mpitest-client: mpitest-client.c
	$(CC) $(CFLAGS) -o mpitest-client mpitest-client.c

clean:
	rm -f mpitest mpitest-client
	rm -f /tmp/mpitest.log

.PHONY: all clean