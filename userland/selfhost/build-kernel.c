#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * build-kernel - rebuild the Linux kernel from the pruned source tree shipped in this image
 * (/usr/src/linux-*.tar.gz), with the toolchain installed here: tcc (every C and .S file, -S,
 * -E), i386-tcc (-m16, the real-mode code; the i386 guest's tcc is that compiler itself), tld
 * (ld, nm, objcopy, readelf, ar, strip), GNU make and busybox (sh, awk, sed, bc, tar, ...).
 * No gcc, no binutils, no flex/bison (the Kconfig lexer and parser come pre-generated, see
 * /usr/bin/flex and /usr/bin/bison).  A C script: /usr/bin/tcc runs it (-run).
 * It is the same make line as the kernel step of scripts/build.sh on the host.
 *
 * Usage: build-kernel [-j N] [make-target...]      (default target: bzImage + the modules dir)
 *   The source is unpacked into $WORK/linux (a tmpfs mounted on /build in the guest, so that
 *   df shows the usage and a full disk cannot take the whole RAM) and left there: a second run
 *   is incremental.  The result is $WORK/linux/arch/x86/boot/bzImage; "send-file" copies it to
 *   the host (run.sh --selfhost).
 * Env: WORK (/build), JOBS (-j; default nproc), TCC (the tcc to use: /usr/local/bin/tcc when
 *   build-toolchain made one, else /usr/bin/tcc), GEN (the generation number that becomes the
 *   kernel's "#N" in uname -v; default: the running kernel's + 1), SELFHOST_SRC (/usr/src),
 *   SELFHOST_SHARE (/usr/share/tinycc-os-kernel), SELFHOST_BIN (/usr/bin: the wrappers, tld,
 *   flex, bison), NO_TMPFS=1 (do not mount anything; scripts/selfhost-test.sh host does that),
 *   ARCH (x86_64 or i386: which kernel to build; default: the running machine's, uname -m).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <glob.h>
#include <ftw.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/utsname.h>

static const char *prog = "build-kernel";

static void die(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static char *fmt(const char *f, ...)
{
    char *s;
    va_list ap;
    va_start(ap, f);
    if (vasprintf(&s, f, ap) < 0)
        die("out of memory");
    va_end(ap);
    return s;
}

static const char *env_or(const char *name, const char *dflt)
{
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}

static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static int is_exec(const char *p) { return is_file(p) && access(p, X_OK) == 0; }
static int is_chr(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISCHR(st.st_mode); }
static long long file_size(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long long)st.st_size : -1; }
static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static void mkdir_p(const char *path)
{
    char *p = strdup(path), *s;
    for (s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
    free(p);
}

/* the whole file, NUL-terminated (NULL if it cannot be read) */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    size_t cap = 65536, len = 0, n;
    char *buf;
    if (!f)
        return NULL;
    buf = malloc(cap);
    while (buf && (n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len < 4096)
            buf = realloc(buf, cap *= 2);
    }
    fclose(f);
    if (buf)
        buf[len] = 0;
    return buf;
}

/* the last n lines of a file, to stdout */
static void tail(const char *path, int n)
{
    char *s = read_file(path), *p;
    int lines = 0;
    if (!s)
        return;
    p = s + strlen(s);
    if (p > s && p[-1] == '\n')
        p--;
    while (p > s) {
        if (p[-1] == '\n' && ++lines == n)
            break;
        p--;
    }
    fputs(p, stdout);
    if (!*p || p[strlen(p) - 1] != '\n')
        fputc('\n', stdout);
    free(s);
}

/* the lines of a file that match, at most max of them, to stdout; how many matched */
static int grep(const char *path, int (*match)(const char *), int max)
{
    char *s = read_file(path), *line, *nl;
    int n = 0;
    for (line = s; line && *line; line = nl ? nl + 1 : line + strlen(line)) {
        if ((nl = strchr(line, '\n')))
            *nl = 0;
        if (match(line) && ++n <= max)
            puts(line);
    }
    free(s);
    return n;
}

/* argument vectors for the commands */
struct argv { char *v[1024]; int n; };

static void arg(struct argv *a, const char *s)
{
    if (a->n >= 1023)
        die("too many arguments");
    a->v[a->n++] = (char *)s;
    a->v[a->n] = NULL;
}

static void argf(struct argv *a, const char *f, ...)
{
    char *s;
    va_list ap;
    va_start(ap, f);
    if (vasprintf(&s, f, ap) < 0)
        die("out of memory");
    va_end(ap);
    arg(a, s);
}

/* run a command, its stdout and stderr appended to log (when given); the exit status */
static int run(char *const argv[], const char *log)
{
    pid_t pid;
    int st;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        if (log) {
            int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0)
                _exit(126);
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "%s: cannot run %s: %s\n", prog, argv[0], strerror(errno));
        _exit(127);
    }
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            die("waitpid: %s", strerror(errno));
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* the first line of a command's output, without the newline ("" if it cannot be run) */
static char *capture_line(char *const argv[])
{
    int fds[2], st;
    pid_t pid;
    char buf[1024], *nl;
    ssize_t n, len = 0;
    if (pipe(fds) < 0)
        die("pipe: %s", strerror(errno));
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(fds[1], 1);
        if (null >= 0)
            dup2(null, 2);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    while (len < (ssize_t)sizeof buf - 1 && (n = read(fds[0], buf + len, sizeof buf - 1 - len)) > 0)
        len += n;
    close(fds[0]);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    buf[len] = 0;
    if ((nl = strchr(buf, '\n')))
        *nl = 0;
    return strdup(buf);
}

static char *capture_l(const char *arg0, ...)
{
    char *argv[16];
    int n = 0;
    va_list ap;
    argv[n++] = (char *)arg0;
    va_start(ap, arg0);
    while (n < 15 && (argv[n] = va_arg(ap, char *)))
        n++;
    va_end(ap);
    argv[n] = NULL;
    return capture_line(argv);
}

/* the first file matching a pattern (NULL if none) */
static char *glob_first(const char *pattern)
{
    glob_t g;
    char *r = NULL;
    if (glob(pattern, 0, NULL, &g) == 0 && g.gl_pathc > 0)
        r = strdup(g.gl_pathv[0]);
    globfree(&g);
    return r;
}

/* is a filesystem mounted on dir?  (a " dir " field in /proc/mounts) */
static int is_mounted(const char *dir)
{
    char *s = read_file("/proc/mounts"), *key = fmt(" %s ", dir);
    int r = s && strstr(s, key) != NULL;
    free(s);
    free(key);
    return r;
}

/* file count and total size of a tree (du -s) */
static long long du_files, du_bytes;
static int du_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)path; (void)ftw;
    if (flag == FTW_F) {
        du_files++;
        du_bytes += st->st_blocks * 512LL;
    }
    return 0;
}
static void du(const char *dir)
{
    du_files = du_bytes = 0;
    nftw(dir, du_cb, 64, FTW_PHYS);
}
static char *human(long long bytes)
{
    if (bytes >= 1LL << 30)
        return fmt("%.1fG", bytes / (double)(1LL << 30));
    if (bytes >= 1LL << 20)
        return fmt("%.1fM", bytes / (double)(1LL << 20));
    return fmt("%.1fK", bytes / 1024.0);
}

/* the string of #define NAME "..." in a header ("" if absent) */
static char *define_string(const char *path, const char *name)
{
    char *s = read_file(path), *key = fmt("#define %s", name), *p, *q, *r = strdup("");
    if (s && (p = strstr(s, key)) && (p = strchr(p + strlen(key), '"')) && (q = strchr(++p, '"'))) {
        free(r);
        r = strndup(p, q - p);
    }
    free(s);
    free(key);
    return r;
}

static int count_prefix(const char *path, const char *prefix)
{
    char *s = read_file(path), *line;
    int n = 0;
    size_t len = strlen(prefix);
    for (line = s; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : line + strlen(line))
        if (strncmp(line, prefix, len) == 0)
            n++;
    free(s);
    return n;
}

static long meminfo_kb(const char *key)
{
    char *s = read_file("/proc/meminfo"), *p;
    long kb = 0;
    if (s && (p = strstr(s, key)))
        kb = atol(p + strlen(key));
    free(s);
    return kb;
}

static int match_error(const char *line)
{
    const char *p;
    if (strstr(line, " error: ") || strstr(line, "undefined reference"))
        return 1;
    for (p = line; (p = strstr(p, "Error ")); p++)
        if (p[6] >= '0' && p[6] <= '9')
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *src = env_or("SELFHOST_SRC", "/usr/src"), *share = env_or("SELFHOST_SHARE", "/usr/share/tinycc-os-kernel");
    const char *bin = env_or("SELFHOST_BIN", "/usr/bin"), *work = env_or("WORK", "/build"), *tcc = getenv("TCC");
    const char *make, *karch, *afrag, *fragments[3];
    char *jobs = (char *)getenv("JOBS"), *tarball, *log, *kcc_log, *tccv, *gen = (char *)getenv("GEN"), *img = "arch/x86/boot/bzImage";
    char *v1, *v2, *v3;
    struct utsname u;
    struct argv mk = { { 0 }, 0 }, cmd;
    time_t t0, t1;
    long el;
    int i, k;
    static const char *tools[] = { "ld", "nm", "objcopy", "readelf", "ar", "strip", "tcc-kcc", "tcc-hostcc", "objtool-stub", "flex", "bison" };

    if (!jobs || !*jobs)
        jobs = fmt("%ld", sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-j") == 0 && i + 1 < argc)
            jobs = argv[++i];
        else if (strncmp(argv[i], "-j", 2) == 0)
            jobs = argv[i] + 2;
        else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else
            break;
    }
    if (!tcc || !*tcc) {
        if (is_exec("/usr/local/bin/tcc"))
            tcc = "/usr/local/bin/tcc";
        else {
            /* command -v tcc */
            char *path = strdup(env_or("PATH", "/usr/local/bin:/usr/bin:/bin")), *d, *found = NULL;
            for (d = strtok(path, ":"); d && !found; d = strtok(NULL, ":")) {
                char *c = fmt("%s/tcc", d);
                if (is_exec(c))
                    found = c;
                else
                    free(c);
            }
            if (!found)
                die("no tcc");
            tcc = found;
        }
    }
    /* the real-mode code is compiled with -m16: the x86_64 tcc execs the i386-tcc next to it,
     * the i386 tcc (the i386 guest) is that compiler itself */
    tccv = capture_l(tcc, "-v", NULL);
    if (!strstr(tccv, "(i386 ")) {
        char *dir = strdup(tcc), *slash = strrchr(dir, '/'), *cross;
        if (slash)
            *slash = 0;
        else
            strcpy(dir, ".");
        cross = fmt("%s/i386-tcc", dir);
        if (!is_exec(cross))
            die("no i386-tcc next to %s (tcc -m16 execs it for the real-mode code)", tcc);
    }
    for (k = 0; k < (int)(sizeof tools / sizeof *tools); k++) {
        char *t = fmt("%s/%s", bin, tools[k]);
        if (!is_exec(t))
            die("no %s", t);
        free(t);
    }
    make = fmt("%s/make", bin);
    if (!is_exec(make))
        make = "make";
    tarball = glob_first(fmt("%s/linux-*.tar.gz", src));
    if (!tarball)
        die("no %s/linux-*.tar.gz", src);
    if (!is_file(fmt("%s/minimal.config", share)) || !is_file(fmt("%s/tcc.config", share)))
        die("no Kconfig fragments in %s", share);
    if (!is_file(fmt("%s/kconfig/lexer.lex.c", share)))
        die("no pre-generated Kconfig lexer in %s/kconfig", share);
    uname(&u);
    if (!gen || !*gen)
        gen = fmt("%ld", (u.version[0] == '#' ? strtol(u.version + 1, NULL, 10) : 0) + 1);

    /* the work directory: a tmpfs in the guest (85% of RAM at most; the tree needs ~70 MB, the
     * build ~160 MB), nothing on the host (NO_TMPFS) */
    mkdir_p(work);
    if (!getenv("NO_TMPFS") && !is_mounted(work)) {
        if (mount("tmpfs", work, "tmpfs", 0, "size=85%") != 0)
            fprintf(stderr, "%s: no tmpfs (CONFIG_TMPFS); building on the root filesystem (ramfs, no size limit)\n", prog);
    }
    if (chdir(work) != 0)
        die("cannot enter %s: %s", work, strerror(errno));
    t0 = time(NULL);
    if (!is_file("linux/Makefile")) {
        printf("== unpacking %s (%lld bytes) into %s/linux\n", base_name(tarball), file_size(tarball), work);
        fflush(stdout);
        cmd.n = 0;
        arg(&cmd, "tar");
        arg(&cmd, "xzf");
        arg(&cmd, tarball);
        if (run(cmd.v, NULL) != 0)
            die("unpacking %s failed", tarball);
        if (!is_file("linux/Makefile"))
            die("the tarball has no linux/Makefile");
        du("linux");
        printf("   %lld files, %s\n", du_files, human(du_bytes));
    }
    if (chdir("linux") != 0)
        die("cannot enter %s/linux", work);
    log = fmt("%s/build-kernel.log", work);
    kcc_log = fmt("%s/kcc.log", work);
    close(open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644));
    close(open(kcc_log, O_WRONLY | O_CREAT | O_TRUNC, 0644));
    setenv("TCC", tcc, 1);
    setenv("KCC_LOG", kcc_log, 1);
    setenv("KCONFIG_PREGEN", fmt("%s/kconfig", share), 1);
    /* the kernel's architecture: the machine this runs on (the guest), or ARCH from the host check */
    {
        const char *a = env_or("ARCH", u.machine);
        if (a[0] == 'i' && a[2] == '8' && a[3] == '6' && !a[4]) {
            karch = "i386";
            afrag = "i386.config";
        } else {
            karch = "x86";
            afrag = "x86_64.config";
        }
    }
    /* the same toolchain variables as scripts/build.sh step_kernel; LEX/YACC are the copies of
     * the pre-generated files; the version string names the generation and the builder */
    arg(&mk, make);
    argf(&mk, "ARCH=%s", karch);
    argf(&mk, "CC=%s/tcc-kcc", bin);
    argf(&mk, "HOSTCC=%s/tcc-hostcc", bin);
    argf(&mk, "OBJTOOL=%s/objtool-stub", bin);
    argf(&mk, "LD=%s/ld", bin);
    argf(&mk, "NM=%s/nm", bin);
    argf(&mk, "OBJCOPY=%s/objcopy", bin);
    argf(&mk, "READELF=%s/readelf", bin);
    argf(&mk, "AR=%s/ar", bin);
    argf(&mk, "STRIP=%s/strip", bin);
    argf(&mk, "LEX=%s/flex", bin);
    argf(&mk, "YACC=%s/bison", bin);
    argf(&mk, "KBUILD_BUILD_VERSION=%s", gen);
    argf(&mk, "KBUILD_BUILD_USER=%s", env_or("KBUILD_BUILD_USER", "tcc"));
    argf(&mk, "KBUILD_BUILD_HOST=%s", env_or("KBUILD_BUILD_HOST", "tinycc-os-guest"));
    if (!is_file(".config")) {
        printf("== configuring: tinyconfig + minimal.config + %s + tcc.config, olddefconfig\n", afrag);
        cmd = mk;
        arg(&cmd, "-s");
        arg(&cmd, "tinyconfig");
        if (run(cmd.v, log) != 0) {
            tail(log, 20);
            die("tinyconfig failed (log: %s)", log);
        }
    }
    fragments[0] = "minimal.config";
    fragments[1] = afrag;
    fragments[2] = "tcc.config";
    setenv("KCONFIG_CONFIG", ".config", 1);
    for (k = 0; k < 3; k++) {
        cmd.n = 0;
        arg(&cmd, "sh");
        arg(&cmd, "scripts/kconfig/merge_config.sh");
        arg(&cmd, "-m");
        arg(&cmd, ".config");
        argf(&cmd, "%s/%s", share, fragments[k]);
        run(cmd.v, log);
    }
    unsetenv("KCONFIG_CONFIG");
    cmd = mk;
    arg(&cmd, "-s");
    arg(&cmd, "olddefconfig");
    if (run(cmd.v, log) != 0) {
        tail(log, 20);
        die("olddefconfig failed (log: %s)", log);
    }
    cmd = mk;
    argf(&cmd, "-j%s", jobs);
    for (k = i; k < argc; k++)
        arg(&cmd, argv[k]);
    printf("== building generation %s with %s and %s, make -j%s", gen, tccv, capture_l(fmt("%s/ld", bin), "-v", NULL), jobs);
    for (k = i; k < argc; k++)
        printf(" %s", argv[k]);
    printf("\n   (log: %s; one line per compiler run: %s)\n", log, kcc_log);
    if (run(cmd.v, log) != 0) {
        grep(log, match_error, 20);
        tail(log, 5);
        die("kernel build failed (log: %s)", log);
    }
    t1 = time(NULL);
    el = (long)(t1 - t0);
    if (!is_file(img))
        die("no %s", img);
    printf("== done in %ldm%lds: %s/linux/%s (%lld bytes)\n", el / 60, el % 60, work, img, file_size(img));
    printf("   %d tcc and %d gcc compiler invocations, %d host-tool compiles\n",
           count_prefix(kcc_log, "tcc "), count_prefix(kcc_log, "gcc "), count_prefix(kcc_log, "host-tcc "));
    v1 = define_string("include/generated/utsversion.h", "UTS_VERSION");
    v2 = define_string("include/generated/compile.h", "LINUX_COMPILE_BY");
    v3 = define_string("include/generated/compile.h", "LINUX_COMPILE_HOST");
    printf("   version: %s (%s@%s)\n", v1, v2, v3);
    if (access("/proc/meminfo", R_OK) == 0) {
        du(work);
        printf("   memory: %ld MB total, %ld MB available; %s: %s used\n",
               meminfo_kb("MemTotal:") / 1024, meminfo_kb("MemAvailable:") / 1024, work, human(du_bytes));
    }
    if (is_exec(fmt("%s/send-file", bin)) && is_chr("/dev/ttyS1"))
        printf("   to copy it to the host (run.sh --selfhost):  send-file %s/linux/%s\n", work, img);
    return 0;
}
