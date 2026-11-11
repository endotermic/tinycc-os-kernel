# SPDX-License-Identifier: BSD-2-Clause
# libelf - the libelf subset the kernel's objtool needs, for tinycc-os-kernel
# make [CC=tcc] [PREFIX=dir]   -> $(PREFIX)/include/{libelf.h,gelf.h}, $(PREFIX)/lib/libelf.a,
#                                $(PREFIX)/pkg-config (answers for this libelf only)
CC ?= gcc
AR ?= ar
CFLAGS ?= -std=gnu99 -O2 -Wall -Wextra
PREFIX ?= out
all: $(PREFIX)/lib/libelf.a $(PREFIX)/include/libelf.h $(PREFIX)/include/gelf.h $(PREFIX)/pkg-config
$(PREFIX)/lib/libelf.a: libelf.c libelf.h gelf.h
	mkdir -p $(PREFIX)/lib && $(CC) $(CFLAGS) -I. -c -o $(PREFIX)/lib/libelf.o libelf.c && rm -f $@ && $(AR) rcs $@ $(PREFIX)/lib/libelf.o
$(PREFIX)/include/%.h: %.h
	mkdir -p $(PREFIX)/include && cp $< $@
$(PREFIX)/pkg-config: pkg-config.c
	$(CC) $(CFLAGS) -DLIBELF_DIR='"$(abspath $(PREFIX))"' -o $@ pkg-config.c
test: all
	sh ./test.sh
clean:
	rm -rf $(PREFIX)
.PHONY: all test clean
