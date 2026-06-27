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

CC = mpicc
CFLAGS = -O2 -Wall

all: mpichess mpichess-client

mpichess: mpichess.c mpichess.h
	$(CC) $(CFLAGS) -o mpichess mpichess.c

mpichess-client: mpichess-client.c mpichess.h
	$(CC) $(CFLAGS) -o mpichess-client mpichess-client.c

clean:
	rm -f mpichess mpichess-client *.log *.btr


