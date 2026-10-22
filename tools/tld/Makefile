# SPDX-License-Identifier: BSD-2-Clause
# tld - small binutils replacement (ld, nm, objcopy, readelf -r, ar) for tinycc-os-kernel
CC ?= gcc
CFLAGS ?= -std=gnu99 -O2 -Wall -Wno-unused-function
SRCS = main.c util.c input.c script.c ld.c nm.c objcopy.c readelf.c ar.c
all: tld links
tld: $(SRCS) tld.h script.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)
# tool names as symlinks (the tool is chosen from argv[0])
links: tld
	for t in ld nm objcopy readelf ar strip; do ln -sf tld $$t; done
test: all
	sh ./test.sh
clean:
	rm -f tld ld nm objcopy readelf ar strip
.PHONY: all links test clean
