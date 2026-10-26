#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * build-toolchain - rebuild the compiler and the linker from the sources shipped in this
 * image, with the compiler installed here: tcc + i386-tcc from /usr/src/tinycc.tar.gz
 * (tinycc with the project's patches, configured like the one in /usr/bin: static, musl)
 * and tld from /usr/src/tld.tar.gz, both installed into /usr/local, where build-kernel
 * finds them first.  GNU make (/usr/src/make-*.tar.gz) can be rebuilt as well (--make).
 * A C script: /usr/bin/tcc runs it (-run).
 * Usage: build-toolchain [--make] [--test]      Env: WORK (/build), JOBS, TCC (/usr/bin/tcc)
 *   --test runs tcc's test suite (make -k test) on the rebuilt compiler, log in $WORK/tcc-test.log;
 *   the exit status is 3 when tests failed (the toolchain is installed either way)
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

static const char *prog = "build-toolchain";

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
static long long file_size(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long long)st.st_size : -1; }

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

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)ftw;
    return (flag == FTW_DP ? rmdir(path) : unlink(path)) == 0 || errno == ENOENT ? 0 : -1;
}
/* rm -rf */
static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (nftw(path, rm_cb, 64, FTW_DEPTH | FTW_PHYS) != 0)
        die("cannot remove %s: %s", path, strerror(errno));
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

static int has_error(const char *line) { return strstr(line, "error") != NULL; }

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

/* run a command, its stdout and stderr into log (appended; truncated when trunc); the exit status */
static int run_log(char *const argv[], const char *log, int trunc)
{
    pid_t pid;
    int st;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        if (log) {
            int fd = open(log, O_WRONLY | O_CREAT | (trunc ? O_TRUNC : O_APPEND), 0644);
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
static int run(char *const argv[], const char *log) { return run_log(argv, log, 0); }

/* a command given as arguments, output appended to log */
static int runl(const char *log, const char *arg0, ...)
{
    struct argv a = { { 0 }, 0 };
    va_list ap;
    const char *s;
    arg(&a, arg0);
    va_start(ap, arg0);
    while ((s = va_arg(ap, const char *)))
        arg(&a, s);
    va_end(ap);
    return run(a.v, log);
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

static char *glob_first(const char *pattern)
{
    glob_t g;
    char *r = NULL;
    if (glob(pattern, 0, NULL, &g) == 0 && g.gl_pathc > 0)
        r = strdup(g.gl_pathv[0]);
    globfree(&g);
    return r;
}

static int is_mounted(const char *dir)
{
    char *s = read_file("/proc/mounts"), *key = fmt(" %s ", dir);
    int r = s && strstr(s, key) != NULL;
    free(s);
    free(key);
    return r;
}

static void copy_file(const char *from, const char *to, mode_t mode)
{
    static char buf[65536];
    ssize_t n;
    int in = open(from, O_RDONLY), out;
    if (in < 0)
        die("cannot read %s: %s", from, strerror(errno));
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0)
        die("cannot write %s: %s", to, strerror(errno));
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n)
            die("cannot write %s: %s", to, strerror(errno));
    close(in);
    close(out);
}

/* the entries of a directory, sorted, space-separated (ls | tr '\n' ' ') */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char *ls(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char *names[1024], *r = strdup(""), *t;
    int n = 0, i;
    if (!d)
        return r;
    while ((e = readdir(d)) && n < 1024)
        if (e->d_name[0] != '.')
            names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str);
    for (i = 0; i < n; i++) {
        t = fmt("%s%s ", r, names[i]);
        free(r);
        r = t;
    }
    return r;
}

/* the failed make targets of a test log: "[dir/target] Error N", each once with its count
 * (grep -oE '\[[^]]*\] Error [0-9]+' | sort | uniq -c) */
static void failed_targets(const char *path)
{
    char *s = read_file(path), *p, *keys[4096];
    int counts[4096], n = 0, i, j;
    for (p = s; s && (p = strchr(p, '[')); p++) {
        char *close = strchr(p, ']'), *e;
        if (!close || strchr(p, '\n') < close || strncmp(close, "] Error ", 8) != 0 || close[8] < '0' || close[8] > '9')
            continue;
        for (e = close + 8; *e >= '0' && *e <= '9'; e++)
            ;
        for (i = 0; i < n; i++)
            if (strlen(keys[i]) == (size_t)(e - p) && strncmp(keys[i], p, e - p) == 0)
                break;
        if (i == n && n < 4096) {
            keys[n] = strndup(p, e - p);
            counts[n++] = 0;
        }
        if (i < 4096)
            counts[i]++;
        p = e - 1;
    }
    free(s);
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (strcmp(keys[j], keys[i]) < 0) {
                char *tk = keys[i]; int tc = counts[i];
                keys[i] = keys[j]; counts[i] = counts[j];
                keys[j] = tk; counts[j] = tc;
            }
    for (i = 0; i < n; i++)
        printf("   %7d %s\n", counts[i], keys[i]);
}

int main(int argc, char **argv)
{
    const char *src = env_or("SELFHOST_SRC", "/usr/src"), *work = env_or("WORK", "/build"), *prefix = env_or("PREFIX", "/usr/local");
    const char *tcc = env_or("TCC", "/usr/bin/tcc"), *jobs = getenv("JOBS");
    char *log, *tag, *v, *ptcc, *j;
    int with_make = 0, test = 0, test_rc = 0, i;
    time_t t0;
    long el;
    struct argv cmd;
    glob_t g;

    if (!jobs || !*jobs)
        jobs = fmt("%ld", sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1);
    j = fmt("-j%s", jobs);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--make") == 0)
            with_make = 1;
        else if (strcmp(argv[i], "--test") == 0)
            test = 1;
        else {
            fprintf(stderr, "usage: build-toolchain [--make] [--test]\n");
            return 2;
        }
    }
    if (!is_file(fmt("%s/tinycc.tar.gz", src)))
        die("no %s/tinycc.tar.gz", src);
    if (!is_file(fmt("%s/tld.tar.gz", src)))
        die("no %s/tld.tar.gz", src);
    mkdir_p(work);
    if (!getenv("NO_TMPFS") && !is_mounted(work))
        mount("tmpfs", work, "tmpfs", 0, "size=85%");
    if (chdir(work) != 0)
        die("cannot enter %s: %s", work, strerror(errno));
    t0 = time(NULL);
    log = fmt("%s/build-toolchain.log", work);
    close(open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644));
    ptcc = fmt("%s/bin/tcc", prefix);

    printf("== tinycc: unpacking, configure --cc=%s (static, musl), make, make cross-i386\n", tcc);
    rm_rf("tinycc");
    if (runl(NULL, "tar", "xzf", fmt("%s/tinycc.tar.gz", src), NULL) != 0)
        die("unpacking tinycc.tar.gz failed");
    if (chdir("tinycc") != 0)
        die("the tarball has no tinycc/");
    /* the same configuration as the host gave the guest tcc (scripts/build.sh step_guest_tcc),
     * with /usr/local as the prefix; ar is tld's */
    cmd.n = 0;
    arg(&cmd, "./configure");
    argf(&cmd, "--cc=%s", tcc);
    arg(&cmd, "--ar=ar");
    argf(&cmd, "--prefix=%s", prefix);
    argf(&cmd, "--tccdir=%s/lib/tcc", prefix);
    arg(&cmd, "--config-musl");
    arg(&cmd, "--sysincludepaths={B}/include:/usr/include");
    arg(&cmd, "--libpaths={B}:/usr/lib");
    arg(&cmd, "--crtprefix=/usr/lib");
    arg(&cmd, "--tcc-switches=-static");
    arg(&cmd, "--extra-cflags=-O2");
    arg(&cmd, "--config-backtrace=no");
    arg(&cmd, "--config-bcheck=no");
    if (run(cmd.v, log) != 0) {
        tail(log, 20);
        die("tcc configure failed");
    }
    /* the version tag ("e23dabe+79p": base commit + patch count) comes from the installed tcc,
     * whose sources these are (scripts/build.sh archives the patched tree's HEAD): there is no
     * git in here, and without the override the Makefile would stamp the build "*" (modified) */
    v = capture_l(tcc, "-v", NULL);
    tag = strdup("");
    if (strncmp(v, "tcc version ", 12) == 0) {
        char *p = strchr(v + 12, ' '), *e;
        if (p && (e = strchr(++p, ' ')) && e[1] == '(')
            tag = strndup(p, e - p);
    }
    tag = fmt("GITHASH=%s", tag);
    if (runl(log, "make", j, tag, "GITMODF=", NULL) != 0) {
        grep(log, has_error, 10);
        die("tcc build failed (log: %s)", log);
    }
    if (runl(log, "make", j, "cross-i386", tag, "GITMODF=", NULL) != 0 && runl(log, "make", "i386-tcc", tag, "GITMODF=", NULL) != 0) {
        grep(log, has_error, 10);
        die("i386-tcc build failed (log: %s)", log);
    }
    if (runl(log, "make", "-s", "install", tag, "GITMODF=", NULL) != 0)
        die("tcc install failed");
    printf("   %s, %s\n", capture_l(ptcc, "-v", NULL), capture_l(fmt("%s/bin/i386-tcc", prefix), "-v", NULL));
    if (test) {
        /* the whole suite with -k (as scripts/build.sh tcc-test runs it on the host), its output
         * in a log of its own that the automatic run (tcc.selfhost=test) sends to the host.
         * CONFIG_dll=no: this image has no dynamic loader, a shared object cannot run here
         * (dlltest, tests2 113_btdll and 148_linker_symbols are skipped) */
        char *tlog = fmt("%s/tcc-test.log", work), *t;
        time_t t2 = time(NULL);
        int rc;
        printf("== tinycc test suite (make -k test; log: %s)\n", tlog);
        cmd.n = 0;
        arg(&cmd, "make");
        arg(&cmd, "-k");
        arg(&cmd, "-s");
        arg(&cmd, "test");
        arg(&cmd, "CONFIG_dll=no");
        rc = run_log(cmd.v, tlog, 1);
        t = read_file(tlog);
        if (rc == 0 && t && strstr(t, "ALL TESTS PASSED"))
            printf("   ALL TESTS PASSED in %ld s\n", (long)(time(NULL) - t2));
        else {
            test_rc = 3;
            printf("   some tests FAILED (%ld s); make targets with errors:\n", (long)(time(NULL) - t2));
            failed_targets(tlog);
        }
        free(t);
    }
    if (chdir(work) != 0)
        die("cannot enter %s", work);

    printf("== tld: unpacking, compiling with %s\n", ptcc);
    rm_rf("tld");
    if (runl(NULL, "tar", "xzf", fmt("%s/tld.tar.gz", src), NULL) != 0)
        die("unpacking tld.tar.gz failed");
    cmd.n = 0;
    arg(&cmd, ptcc);
    arg(&cmd, "-std=gnu99");
    arg(&cmd, "-O2");
    arg(&cmd, "-o");
    argf(&cmd, "%s/bin/tld", prefix);
    if (glob("tld/*.c", 0, NULL, &g) != 0)
        die("no tld/*.c in %s/tld.tar.gz", src);
    for (i = 0; i < (int)g.gl_pathc; i++)
        arg(&cmd, g.gl_pathv[i]);
    if (run(cmd.v, log) != 0)
        die("tld build failed (log: %s)", log);
    {
        static const char *tools[] = { "ld", "nm", "objcopy", "readelf", "ar", "strip" };
        for (i = 0; i < 6; i++) {
            char *l = fmt("%s/bin/%s", prefix, tools[i]);
            unlink(l);
            if (symlink("tld", l) != 0)
                die("cannot link %s: %s", l, strerror(errno));
        }
    }
    printf("   %s, %lld bytes\n", capture_l(fmt("%s/bin/ld", prefix), "-v", NULL), file_size(fmt("%s/bin/tld", prefix)));

    if (with_make) {
        char *mt = glob_first(fmt("%s/make-*.tar.gz", src));
        if (!mt)
            die("no %s/make-*.tar.gz", src);
        printf("== GNU make: unpacking, configure CC=%s, make\n", ptcc);
        rm_rf("make");
        mkdir_p("make");
        if (runl(NULL, "tar", "xzf", mt, "-C", "make", "--strip-components=1", NULL) != 0)
            die("unpacking %s failed", mt);
        mkdir_p("make/b");
        if (chdir("make/b") != 0)
            die("cannot enter make/b");
        if (runl(log, "../configure", fmt("CC=%s", ptcc), "--disable-nls", "--without-guile", "--disable-dependency-tracking", "--disable-load", NULL) != 0) {
            tail(log, 20);
            die("make configure failed");
        }
        if (runl(log, "make", j, NULL) != 0)
            die("make build failed (log: %s)", log);
        copy_file("make", fmt("%s/bin/make", prefix), 0755);
        printf("   %s\n", capture_l(fmt("%s/bin/make", prefix), "--version", NULL));
        if (chdir(work) != 0)
            die("cannot enter %s", work);
    }
    el = (long)(time(NULL) - t0);
    printf("== done in %ldm%lds; installed in %s/bin: %s\n", el / 60, el % 60, prefix, ls(fmt("%s/bin", prefix)));
    printf("   build-kernel now uses %s (TCC=/usr/bin/tcc for the shipped one)\n", ptcc);
    return test_rc;
}
