/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dix-config.h for TinyX built by tcc against musl (userland/tinyx/Makefile): what
 * src/tinyx/configure.ac would have found on this system, written by hand because the
 * host build has no autotools.  The macro names are those of src/tinyx/include/
 * dix-config.h.in; a macro that stays undefined here is one the probe would have left
 * undefined (no XDMCP, no getpeereid/getpeerucred in musl, no alloca.c, no vm86).
 */
#ifndef _DIX_CONFIG_H_
#define _DIX_CONFIG_H_

#define BUILDERADDR "tinycc-os-kernel"
#define OSNAME "Linux (tinycc-os-kernel)"
/* libXfont's compiled-in fonts (fixed, cursor): no font files in the image */
#define COMPILEDDEFAULTFONTPATH "built-ins"
#define DPMSExtension 1
#define HAS_GETDTABLESIZE 1
#define HAS_GETIFADDRS 1
#define HAS_MMAP 1
#define HAVE_ALLOCA 1
#define HAVE_ALLOCA_H 1
#define HAVE_DIRENT_H 1
#define HAVE_FCNTL_H 1
#define HAVE_GETEUID 1
#define HAVE_GETOPT 1
#define HAVE_GETOPT_LONG 1
#define HAVE_GETUID 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIBM 1
#define HAVE_LINK 1
#define HAVE_LINUX_APM_BIOS_H 1
#define HAVE_LINUX_FB_H 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMORY_H 1
#define HAVE_MEMSET 1
#define HAVE_MKSTEMP 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRCHR 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_STRRCHR 1
#define HAVE_STRTOL 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_VPRINTF 1
/* xtrans: Unix sockets (/tmp/.X11-unix/X0) and TCP, IPv6 capable; TinyX itself does not
 * listen on TCP unless asked */
#define UNIXCONN 1
#define TCPCONN 1
#define IPv6 1
#define NDEBUG 1
#define PACKAGE_STRING "tinyx 1.3"
#define PROJECTROOT "/usr"
#define RES 1
#define SCREENSAVER 1
#define SMART_SCHEDULE 1
#define STDC_HEADERS 1
#define XF86BIGFONT 1
#define XORG_RELEASE "Release 1.0"
#define XORG_VERSION_CURRENT 10000000
#define XORG_DATE "22 Sep 2015"
#define XResExtension 1
#define DBE 1
#define XVENDORNAME "tinycc-os-kernel (TinyX)"
#define X_BYTE_ORDER X_LITTLE_ENDIAN
#define MONOTONIC_CLOCK 1
/* a kdrive server: dix/dixfonts.c registers libXfont's built-in fonts (the "built-ins"
 * font path element) only under this macro, and dix is compiled with dix-config.h alone,
 * so it belongs here as well as in kdrive-config.h */
#define KDRIVESERVER 1
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
/* unsigned long is 64 bits: the x86_64 build */
#if defined(__LP64__) || defined(__x86_64__)
#define _XSERVER64 1
#endif

#endif /* _DIX_CONFIG_H_ */
