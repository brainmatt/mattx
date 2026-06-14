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

obj-m += mattxfs.o

KDIR := /lib/modules/$(shell uname -r)/build
# Use absolute path for the current directory
MY_DIR := $(shell pwd)

all:
	# Pass the exact, absolute path to the parent Module.symvers
	$(MAKE) -C $(KDIR) M=$(MY_DIR) KBUILD_EXTRA_SYMBOLS="$(MY_DIR)/../Module.symvers" modules

clean:
	$(MAKE) -C $(KDIR) M=$(MY_DIR) clean
	rm -f $(MY_DIR)/../Module.symvers