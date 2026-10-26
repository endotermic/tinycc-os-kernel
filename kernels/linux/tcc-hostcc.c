#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * tcc-hostcc: tinycc as Kbuild's HOSTCC (fixdep, kconfig, modpost, sorttable, relocs,
 * vdso2c, mkpiggy, tools/build ...), against the host libc.  tools/include needs a
 * __GNUC__ (for __must_be_array and friends): claim a modest GCC 4.2 like the busybox
 * build does, so that glibc's headers do not take the paths of newer gcc extensions.
 * The -U of the __REDIRECT macros lets glibc define its own without "redefined" noise;
 * __NO_INLINE__ keeps glibc's gnu89 "extern inline" helpers (duplicate definitions) out.
 * Inside the guest (userland/selfhost/build-kernel) the libc is musl, where these are no-ops.
 * -B only for an in-tree tcc (src/tinycc/tcc: its include/ and libtcc1.a sit next to it); an
 * installed tcc (/usr/bin/tcc in the guest) knows its tccdir.
 * A C program: compiled by scripts/build.sh for the host build (build/<arch>/kcc/tcc-hostcc),
 * run as a C script (#!/usr/bin/tcc -run) in the guest.
 * Env: TCC (tcc binary), KCC_LOG (append one line per invocation).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv)
{
    const char *tcc = getenv("TCC"), *log = getenv("KCC_LOG");
    char *dir, *slash, *tccdefs, **av;
    int i, n = 0;

    if (!tcc || !*tcc) {
        fprintf(stderr, "tcc-hostcc: set TCC to the tinycc binary (scripts/build.sh does)\n");
        return 2;
    }
    dir = strdup(tcc);
    if ((slash = strrchr(dir, '/')))
        *slash = 0;
    else
        strcpy(dir, ".");
    tccdefs = malloc(strlen(dir) + sizeof "/include/tccdefs.h");
    sprintf(tccdefs, "%s/include/tccdefs.h", dir);
    if (log && *log) {
        FILE *f = fopen(log, "a");
        if (f) {
            fputs("host-tcc ? ::", f);
            for (i = 1; i < argc; i++)
                fprintf(f, " %s", argv[i]);
            fputc('\n', f);
            fclose(f);
        }
    }
    av = malloc((argc + 12) * sizeof *av);
    av[n++] = (char *)tcc;
    if (access(tccdefs, F_OK) == 0) {
        char *b = malloc(strlen(dir) + 3), *inc = malloc(strlen(dir) + sizeof "-I/include");
        sprintf(b, "-B%s", dir);
        sprintf(inc, "-I%s/include", dir);
        av[n++] = b;
        av[n++] = inc;
    }
    av[n++] = "-D__GNUC__=4";
    av[n++] = "-D__GNUC_MINOR__=2";
    av[n++] = "-D__GNUC_PATCHLEVEL__=0";
    av[n++] = "-U__REDIRECT";
    av[n++] = "-U__REDIRECT_NTH";
    av[n++] = "-U__REDIRECT_NTHNL";
    av[n++] = "-D__USER_LABEL_PREFIX__=";
    av[n++] = "-D__NO_INLINE__";
    for (i = 1; i < argc; i++)
        av[n++] = argv[i];
    av[n] = NULL;
    execv(tcc, av);
    fprintf(stderr, "tcc-hostcc: cannot run %s: %s\n", tcc, strerror(errno));
    return 127;
}
