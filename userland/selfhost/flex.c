#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * flex - stand-in for flex in the self-hosting image (a C script, run by tcc -run): Kbuild
 * runs "$(LEX) -o<out> -L <in>.l" for scripts/kconfig/lexer.l; the generated lexer was made
 * on the host (flex) and shipped in $KCONFIG_PREGEN (/usr/share/tinycc-os-kernel/kconfig),
 * so copy it.
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
    const char *d = getenv("KCONFIG_PREGEN"), *out = NULL, *in = NULL, *base, *dot;
    char src[4096];
    int i;
    if (!d)
        d = "/usr/share/tinycc-os-kernel/kconfig";
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--version") == 0) {
            printf("flex 2.6.4 (tinycc-os-kernel stand-in: pre-generated files from %s)\n", d);
            return 0;
        }
        if (strncmp(a, "-o", 2) == 0)
            out = a + 2;
        else if (a[0] != '-')
            in = a;
    }
    if (!out || !*out || !in) {
        fprintf(stderr, "flex (stand-in): expected -o<out> <in>.l\n");
        return 2;
    }
    base = strrchr(in, '/') ? strrchr(in, '/') + 1 : in;
    dot = strrchr(base, '.');
    snprintf(src, sizeof src, "%s/%.*s.lex.c", d, (int)(dot && strcmp(dot, ".l") == 0 ? dot - base : (long)strlen(base)), base);
    if (access(src, R_OK) != 0) {
        fprintf(stderr, "flex (stand-in): no pre-generated %s\n", src);
        return 1;
    }
    if (copy_file(src, out) < 0) {
        fprintf(stderr, "flex (stand-in): cannot copy %s to %s: %s\n", src, out, strerror(errno));
        return 1;
    }
    return 0;
}
