/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * pkg-config.c - answers `pkg-config libelf --cflags|--libs` for this libelf, built into
 * LIBELF_DIR (include/, lib/libelf.a), and nothing else (exit 1, like pkg-config for an
 * unknown package).  tools/objtool/Makefile asks $(HOSTPKG_CONFIG) where libelf is;
 * scripts/build.sh gives it this program for the host build so that no host libelf is
 * needed.  In the guest the headers and the library are in /usr/include and /usr/lib and
 * the Makefile's fallback (-lelf) finds them without pkg-config.
 */
#include <stdio.h>
#include <string.h>

#ifndef LIBELF_DIR
#error "compile with -DLIBELF_DIR=\"<dir>\""
#endif

int main(int argc, char **argv)
{
    int i, cflags = 0, libs = 0, known = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cflags"))
            cflags = 1;
        else if (!strcmp(argv[i], "--libs"))
            libs = 1;
        else if (!strcmp(argv[i], "--exists") || !strncmp(argv[i], "--", 2))
            ;
        else if (!strcmp(argv[i], "libelf"))
            known = 1;
        else
            return 1;
    }
    if (!known)
        return 1;
    if (cflags)
        printf("-I%s/include%s", LIBELF_DIR, libs ? " " : "\n");
    if (libs)
        printf("-L%s/lib -lelf\n", LIBELF_DIR);
    return 0;
}
