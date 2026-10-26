#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * bison - stand-in for bison in the self-hosting image (a C script, run by tcc -run): Kbuild
 * runs "$(YACC) -o <out>.tab.c --defines=<out>.tab.h -t -l <in>.y" for
 * scripts/kconfig/parser.y; the generated parser was made on the host (bison) and shipped
 * in $KCONFIG_PREGEN (/usr/share/tinycc-os-kernel/kconfig), so copy it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static int copy_file(const char *from, const char *to)
{
    char buf[65536];
    ssize_t n;
    int in = open(from, O_RDONLY), out;
    if (in < 0)
        return -1;
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        close(in);
        return -1;
    }
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n) {
            n = -1;
            break;
        }
    close(in);
    return close(out) < 0 || n < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *d = getenv("KCONFIG_PREGEN"), *out = NULL, *defs = NULL, *in = NULL, *base, *dot;
    char src_c[4096], src_h[4096];
    int i, blen;
    if (!d)
        d = "/usr/share/tinycc-os-kernel/kconfig";
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--version") == 0) {
            printf("bison (GNU Bison) 3.8.2 (tinycc-os-kernel stand-in: pre-generated files from %s)\n", d);
            return 0;
        }
        if (strcmp(a, "-o") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (strncmp(a, "--defines=", 10) == 0)
            defs = a + 10;
        else if (a[0] != '-')
            in = a;
    }
    if (!out || !in) {
        fprintf(stderr, "bison (stand-in): expected -o <out>.tab.c --defines=<out>.tab.h <in>.y\n");
        return 2;
    }
    base = strrchr(in, '/') ? strrchr(in, '/') + 1 : in;
    dot = strrchr(base, '.');
    blen = dot && strcmp(dot, ".y") == 0 ? (int)(dot - base) : (int)strlen(base);
    snprintf(src_c, sizeof src_c, "%s/%.*s.tab.c", d, blen, base);
    snprintf(src_h, sizeof src_h, "%s/%.*s.tab.h", d, blen, base);
    if (access(src_c, R_OK) != 0 || access(src_h, R_OK) != 0) {
        fprintf(stderr, "bison (stand-in): no pre-generated %s/%.*s.tab.[ch]\n", d, blen, base);
        return 1;
    }
    if (copy_file(src_c, out) < 0) {
        fprintf(stderr, "bison (stand-in): cannot copy %s to %s: %s\n", src_c, out, strerror(errno));
        return 1;
    }
    if (defs && *defs && copy_file(src_h, defs) < 0) {
        fprintf(stderr, "bison (stand-in): cannot copy %s to %s: %s\n", src_h, defs, strerror(errno));
        return 1;
    }
    return 0;
}
