# SPDX-License-Identifier: BSD-2-Clause
# Makefile - builds TinyX's Xfbdev (src/tinyx: Tiny Core's kdrive fork of X server 1.2)
# without autotools and libtool: one static binary from the source lists of the tree's
# Makefile.am files, with the configuration that autoconf would have written in
# dix-config.h and kdrive-config.h here beside this file.  Everything is compiled with the
# same flags; the server links against the tcc-built libXfont, libfontenc and zlib.
#   make -f userland/tinyx/Makefile SRC=src/tinyx XP=build/<arch>/x/usr CC=build/<arch>/musl-tcc O=build/<arch>/tinyx
# scripts/build.sh tinyx runs this.
SRC ?= ../../src/tinyx
XP ?= /usr
O ?= .
CC ?= tcc
CFLAGS ?= -O2
HERE := $(dir $(lastword $(MAKEFILE_LIST)))
VENDOR ?= tinycc-os-kernel

# every directory a server source includes from (the Makefile.am AM_CFLAGS/INCS of the tree)
INCS = -I$(HERE) -I$(SRC)/include -I$(SRC)/Xext -I$(SRC)/damageext -I$(SRC)/xfixes -I$(SRC)/mi \
       -I$(SRC)/miext/shadow -I$(SRC)/miext/damage -I$(SRC)/render -I$(SRC)/randr -I$(SRC)/fb \
       -I$(SRC)/kdrive/src -I$(SRC)/kdrive/linux -I$(SRC)/kdrive/fbdev -I$(XP)/include
DEFS = -DHAVE_DIX_CONFIG_H -DHAVE_KDRIVE_CONFIG_H -D_GNU_SOURCE \
       -DVENDOR_STRING='"$(VENDOR)"' -DVENDOR_RELEASE=10000000
ALL_CFLAGS = $(CFLAGS) $(DEFS) $(INCS)
LIBS = -L$(XP)/lib -lXfont -lfontenc -lz -lm

# the source lists of the Makefile.am files, without Xvesa (vm86: i386 only, needs
# /dev/mem) and without XDMCP (os/xdmcp.c); strlcat/strlcpy because musl has neither
DIX = atom colormap cursor devices dispatch dixfonts dixutils events extension ffs gc globals \
      glyphcurs grabs initatoms main pixmap privates property resource swaprep swapreq tables \
      window strcasecmp
FB = fb24_32 fballpriv fbarc fbbits fbblt fbbltone fbcompose fbcopy fbfill fbfillrect fbfillsp \
     fbgc fbgetsp fbglyph fbimage fbline fboverlay fbpict fbpixmap fbpoint fbpush fbscreen fbseg \
     fbsetsp fbsolid fbstipple fbtile fbtrap fbutil fbwindow fbedge fbcmap
MI = miarc mibank mibitblt micmap micursor midash midispcur mieq miexpose mifillarc mifillrct \
     mifpolycon migc miglblt mipointer mipoly mipolycon mipolygen mipolypnt mipolyrect mipolyseg \
     mipolytext mipolyutil mipushpxl miregion miscrinit mispans misprite mivaltree miwideline \
     miwindow mizerarc mizerclip mizerline miinitext
XEXT = shape sleepuntil xtest shm bigreq sync xcmisc xres saver xf86bigfont dpms
SHADOW = shadow shalloc shpacked shplanar8 shplanar shrot16pack_180 shrot16pack_270 \
         shrot16pack_270YX shrot16pack_90 shrot16pack_90YX shrot16pack shrot32pack_180 \
         shrot32pack_270 shrot32pack_90 shrot32pack shrot8pack_180 shrot8pack_270 shrot8pack_90 \
         shrot8pack shrotate
OS = WaitFor access auth connection io mitauth oscolor osinit utils xdmauth xstrans xauembed log \
     strlcat strlcpy
RANDR = mirandr randr rrcrtc rrdispatch rrinfo rrmode rroutput rrpointer rrproperty rrscreen \
        rrsdispatch rrxinerama
RENDER = animcur filter glyph miglyph miindex mipict mirect mitrap mitri picture render renderedge
DBE = dbe midbe
XFIXES = cursor region saveset select xfixes
KDRIVE = kcmap kdrive kinfo kinput kmap kmode kshadow
LINUX = keyboard linux mouse
FBDEV = fbdev fbinit

SRCS = $(DIX:%=dix/%.c) $(FB:%=fb/%.c) $(MI:%=mi/%.c) $(XEXT:%=Xext/%.c) \
       miext/damage/damage.c $(SHADOW:%=miext/shadow/%.c) $(OS:%=os/%.c) $(RANDR:%=randr/%.c) \
       $(RENDER:%=render/%.c) $(DBE:%=dbe/%.c) $(XFIXES:%=xfixes/%.c) damageext/damageext.c \
       $(KDRIVE:%=kdrive/src/%.c) $(LINUX:%=kdrive/linux/%.c) $(FBDEV:%=kdrive/fbdev/%.c)
OBJS = $(SRCS:%.c=$(O)/obj/%.o)

all: $(O)/Xfbdev

$(O)/Xfbdev: $(OBJS)
	@echo "  LD      Xfbdev"
	@$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

# quiet, Kbuild style ("  CC  dix/main.c"): scripts/build.sh counts these lines for its bar
$(O)/obj/%.o: $(SRC)/%.c $(HERE)dix-config.h $(HERE)kdrive-config.h
	@mkdir -p $(dir $@)
	@echo "  CC      $*.c"
	@$(CC) $(ALL_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(O)/obj $(O)/Xfbdev

.PHONY: all clean
