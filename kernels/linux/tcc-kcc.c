#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * tcc-kcc: present tinycc to Kbuild as the kernel C compiler and assembler
 * (CC=tcc-kcc).  A C program: scripts/build.sh compiles it with the host tcc for the
 * host build (build/<arch>/kcc/tcc-kcc); inside the guest the same source runs as a
 * C script (#!/usr/bin/tcc -run, userland/selfhost/build-kernel).
 *
 *  * C sources, .S files, -S (asm-offsets), -E (linker scripts) go to tcc with
 *    -U__OPTIMIZE__ (see below).  __GNUC__/__GNUC_MINOR__/__GNUC_PATCHLEVEL__
 *    are defined so scripts/cc-version.sh and include/linux/compiler_types.h
 *    accept it; -Wunsupported is added when -Werror is present so Kbuild's
 *    cc-option probes fail honestly for flags tcc does not implement.
 *  * The compiler queries Kbuild makes are answered here: -Wa,--version
 *    (scripts/as-version.sh wants a "GNU assembler" line), -dumpmachine,
 *    -print-file-name.
 *  * -m16 (the real-mode boot code) goes to tcc as well: it execs the i386-tcc
 *    built next to it.  Nothing goes to gcc any more; KCC_GCC is kept as a hook
 *    for debugging (nothing sets use_gcc below).
 *
 *  * An in-tree tcc (src/tinycc/tcc, next to its include/ and libtcc1.a) gets -B
 *    for its own directory; an installed one (/usr/bin/tcc in the guest, whose
 *    tccdir is /usr/lib/tcc) is used as it is.
 *
 *  * KCC_TCC names the compiler of the target code when it is not TCC itself:
 *    the i386 kernel (ARCH=i386) is compiled by the i386-tcc cross compiler
 *    (scripts/build.sh), while TCC stays the host compiler of tcc-hostcc.
 *    -dumpmachine answers for that target (i686 when the compiler is the i386
 *    one, by its name or its -v line, or -m32 is given).
 *
 * Env: TCC (tcc binary), KCC_TCC (the target compiler, default TCC),
 *      KCC_GCC (fallback compiler, default gcc),
 *      KCC_LOG (append one line per invocation: tool and output file).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

static const char *env_or(const char *name, const char *dflt)
{
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}

/* the output of "tcc -v" (first line, "" if it cannot be run) */
static const char *tcc_version(const char *tcc)
{
    static char buf[256];
    int fds[2], st;
    ssize_t n, len = 0;
    pid_t pid;
    if (pipe(fds) < 0 || (pid = fork()) < 0)
        return "";
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(fds[1], 1);
        if (null >= 0)
            dup2(null, 2);
        close(fds[0]);
        close(fds[1]);
        execl(tcc, tcc, "-v", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    while (len < (ssize_t)sizeof buf - 1 && (n = read(fds[0], buf + len, sizeof buf - 1 - len)) > 0)
        len += n;
    close(fds[0]);
    waitpid(pid, &st, 0);
    buf[len] = 0;
    return buf;
}

int main(int argc, char **argv)
{
    const char *tcc = getenv("TCC"), *gcc = env_or("KCC_GCC", "gcc"), *log = getenv("KCC_LOG");
    const char *out = NULL, *machine;
    char *dir, *slash, *tccdefs, **av;
    int werror = 0, use_gcc = 0, i, n = 0;

    if (!tcc || !*tcc) {
        fprintf(stderr, "tcc-kcc: set TCC to the tinycc binary (scripts/build.sh does)\n");
        return 2;
    }
    tcc = env_or("KCC_TCC", tcc);
    dir = strdup(tcc);
    if ((slash = strrchr(dir, '/')))
        *slash = 0;
    else
        strcpy(dir, ".");
    tccdefs = malloc(strlen(dir) + sizeof "/include/tccdefs.h");
    sprintf(tccdefs, "%s/include/tccdefs.h", dir);
    machine = strstr(tcc, "i386") ? "i686-linux-gnu" : "x86_64-linux-gnu";

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (i > 1 && strcmp(argv[i - 1], "-o") == 0)
            out = a;
        if (strcmp(a, "-m32") == 0)
            machine = "i686-linux-gnu";
        else if (strcmp(a, "-m64") == 0)
            machine = "x86_64-linux-gnu";
        else if (strcmp(a, "-Werror") == 0)
            werror = 1;
        else if (strcmp(a, "-Wa,--version") == 0) {
            puts("GNU assembler (tcc-kcc) 2.47");
            return 0;
        } else if (strcmp(a, "-dumpmachine") == 0) {
            const char *v = tcc_version(tcc);
            puts(strstr(machine, "i686") || strstr(v, "i386") || strstr(v, "i686") ? "i686-linux-gnu" : "x86_64-linux-gnu");
            return 0;
        } else if (strncmp(a, "-print-file-name=", 17) == 0) {
            puts(a + 17);
            return 0;
        } else if (strcmp(a, "-print-libgcc-file-name") == 0) {
            puts("libgcc.a");
            return 0;
        }
    }

    if (log && *log) {
        FILE *f = fopen(log, "a");
        if (f) {
            fprintf(f, "%s %s ::", use_gcc ? "gcc" : "tcc", out ? out : "?");
            for (i = 1; i < argc; i++)
                fprintf(f, " %s", argv[i]);
            fputc('\n', f);
            fclose(f);
        }
    }

    if (use_gcc) {
        argv[0] = (char *)gcc;
        execvp(gcc, argv);
        fprintf(stderr, "tcc-kcc: cannot run %s: %s\n", gcc, strerror(errno));
        return 127;
    }

    /* -U__OPTIMIZE__: tcc does not fold what the kernel's compile-time assertions
     * (__compiletime_assert -> undefined __compiletime_assert_N) expect from an
     * optimizing compiler; include/linux/compiler_types.h documents -U__OPTIMIZE__
     * as the switch for that case. The gcc reference build still checks them. */
    av = malloc((argc + 10) * sizeof *av);
    av[n++] = (char *)tcc;
    if (access(tccdefs, F_OK) == 0) {
        char *b = malloc(strlen(dir) + 3), *inc = malloc(strlen(dir) + sizeof "-I/include");
        sprintf(b, "-B%s", dir);
        sprintf(inc, "-I%s/include", dir);
        av[n++] = b;
        av[n++] = inc;
    }
    av[n++] = "-D__GNUC__=15";
    av[n++] = "-D__GNUC_MINOR__=3";
    av[n++] = "-D__GNUC_PATCHLEVEL__=0";
    av[n++] = "-U__OPTIMIZE__";
    if (werror)
        av[n++] = "-Wunsupported";
    for (i = 1; i < argc; i++)
        av[n++] = argv[i];
    av[n] = NULL;
    execv(tcc, av);
    fprintf(stderr, "tcc-kcc: cannot run %s: %s\n", tcc, strerror(errno));
    return 127;
}
