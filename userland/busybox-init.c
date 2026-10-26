#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * /init of the busybox image (userland/initramfs.list): a C script.  The kernel runs it
 * through the "#!" line: /usr/bin/tcc (the guest's static musl tcc) compiles it in memory
 * and runs main() (-run), so PID 1 is tcc running this program.  Every script of this
 * system is C run this way; there are no shell scripts in the image.
 *
 * It mounts the API filesystems (only /bin/busybox and /bin/sh exist in the cpio, so the
 * applets are called through busybox until --install has made the links), prints the
 * banner, then runs an interactive shell on PID 1's console.  The shell is a child, not
 * an exec: PID 1 exiting would panic the kernel.  When the shell ends ('exit' or Ctrl-D)
 * we reboot, and QEMU (-no-reboot) exits.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int is_exec(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, X_OK) == 0; }

/* the whole file, NUL-terminated (NULL if it cannot be read); *lenp gets its size */
static char *read_file(const char *path, size_t *lenp)
{
    FILE *f = fopen(path, "r");
    size_t cap = 4096, len = 0, n;
    char *buf;
    if (!f)
        return NULL;
    buf = malloc(cap);
    while (buf && (n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len < 1024)
            buf = realloc(buf, cap *= 2);
    }
    fclose(f);
    if (!buf)
        return NULL;
    buf[len] = 0;
    if (lenp)
        *lenp = len;
    return buf;
}

/* the first line of a file, without the newline ("" if unreadable) */
static char *first_line(const char *path)
{
    char *s = read_file(path, NULL), *nl;
    if (!s)
        return strdup("");
    if ((nl = strchr(s, '\n')))
        *nl = 0;
    return s;
}

/* run a command (PATH search), wait for it; its exit status, 127 if it cannot be run */
static int run(char *const argv[])
{
    pid_t pid;
    int st;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return 127;
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "init: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 127;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int runl(const char *arg0, ...)
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
    return run(argv);
}

/* the first line of a command's output ("" if it cannot be run) */
static char *capture_line(char *const argv[])
{
    int fds[2], st;
    pid_t pid;
    char buf[512], *nl;
    ssize_t n, len = 0;
    if (pipe(fds) < 0)
        return strdup("");
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return strdup("");
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

/* the value of "key:" in /proc/meminfo, in MB */
static long meminfo_mb(const char *key)
{
    char *s = read_file("/proc/meminfo", NULL), *p;
    long kb = 0;
    if (s && (p = strstr(s, key)) && (p == s || p[-1] == '\n'))
        kb = atol(p + strlen(key));
    free(s);
    return kb / 1024;
}

static void banner(FILE *out)
{
    struct utsname u;
    char *s, *p, *bb_build;
    size_t len;
    uname(&u);
    /* Which busybox is this?  tcc names its read-only data section .data.ro (the name stays in
     * the stripped binary); a static glibc carries its tunables' names. */
    if ((s = read_file("/bin/busybox", &len)) && memmem(s, len, ".data.ro", 8))
        bb_build = "static, musl -- compiled and linked by tinycc as well";
    else if (s && memmem(s, len, "GLIBC_TUNABLES", 14))
        bb_build = "static, glibc -- the gcc reference build, not tinycc";
    else
        bb_build = "static, unknown toolchain";
    free(s);
    fprintf(out, "\n*** tinycc-os-kernel: %s %s %s, %s\n", u.sysname, u.release, u.machine, u.version);
    fprintf(out, "*** every file of this kernel was compiled, assembled and linked by tinycc (tcc, tld)\n");
    /* "BusyBox v1.38.0 (date) multi-call binary." -> the first two words */
    s = capture_line((char *[]){ "/bin/busybox", NULL });
    if ((p = strchr(s, ' ')) && (p = strchr(p + 1, ' ')))
        *p = 0;
    fprintf(out, "*** %s (%s)\n", s, bb_build);
    free(s);
    if (is_exec("/usr/bin/tcc")) {
        s = capture_line((char *[]){ "tcc", "-v", NULL });
        fprintf(out, "*** %s is installed, static, musl libc:  try  tcc -run hello.c\n", s);
        free(s);
    }
    /* the build number counts the generations: #1 was built on the host (scripts/build.sh),
     * #2 inside a guest running #1, and so on (userland/selfhost/build-kernel sets
     * KBUILD_BUILD_VERSION and KBUILD_BUILD_HOST=tinycc-os-guest) */
    s = first_line("/proc/version");
    if ((p = strchr(s, '('))) {
        char *e = strchr(++p, ')');
        if (e)
            *e = 0;
        if ((e = strrchr(p, '@')) && strcmp(e, "@tinycc-os-guest") == 0) {
            char gen[16] = "?";
            if (u.version[0] == '#')
                snprintf(gen, sizeof gen, "%ld", strtol(u.version + 1, NULL, 10));
            fprintf(out, "*** generation %s: this kernel was built inside a tinycc-os-kernel guest (%s), with the guest's own tcc, tld and make\n", gen, p);
        }
    }
    free(s);
    if (is_exec("/usr/bin/build-kernel")) {
        fprintf(out, "*** self-hosting: build-kernel rebuilds this kernel from /usr/src, build-toolchain rebuilds tcc and tld,\n");
        fprintf(out, "***   send-file copies results to the host; %ld CPUs, %ld MB RAM; see /usr/share/tinycc-os-kernel/README\n",
                sysconf(_SC_NPROCESSORS_ONLN), meminfo_mb("MemTotal:"));
    }
    if (exists("/sys/class/graphics/fb0")) {
        char *size = first_line("/sys/class/graphics/fb0/virtual_size"), *name = first_line("/sys/class/graphics/fb0/name");
        if ((p = strchr(size, ',')))
            *p = 'x';
        fprintf(out, "*** screen: framebuffer %s (%s), tty1-tty6\n", size, name);
        free(size);
        free(name);
    } else if (exists("/dev/tty1")) {
        fprintf(out, "*** screen: VGA text console, tty1-tty6\n");
    }
    fprintf(out, "*** 'exit' (or Ctrl-D) in the console's shell shuts the machine down\n\n");
    fflush(out);
}

/* a session of its own with the given tty as its controlling terminal, running the command */
static pid_t spawn_on_tty(const char *tty, char *const argv[], int with_banner)
{
    pid_t pid;
    fflush(NULL);
    pid = fork();
    if (pid != 0)
        return pid;
    setsid();
    if (tty) {
        /* the session leader opens the tty itself, which makes it its controlling terminal */
        int fd = open(tty, O_RDWR);
        if (fd < 0)
            _exit(1);
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }
    if (with_banner)
        banner(stdout);
    execvp(argv[0], argv);
    _exit(127);
    return -1;
}

static void wait_for(pid_t pid)
{
    int st;
    pid_t p;
    /* PID 1 reaps every orphan that ends meanwhile as well */
    while ((p = waitpid(-1, &st, 0)) != pid)
        if (p < 0 && errno != EINTR)
            break;
}

static void shutdown(void)
{
    fflush(NULL);
    /* let the console UART send what was printed: reboot(2) would cut the last line */
    tcdrain(1);
    usleep(200000);
    sync();
    reboot(RB_AUTOBOOT);
    for (;;)
        pause();
}

/* send FILE to the host if it exists; its status (0 when it does not exist) */
static int send_if_exists(const char *file)
{
    return exists(file) ? runl("send-file", file, NULL) : 0;
}

int main(void)
{
    char *s, *p, *primary = NULL;
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    runl("/bin/busybox", "--install", "-s", NULL);
    /* the kernel gives PID 1 no PATH; busybox's default, so that the shells see the same */
    if (!getenv("PATH"))
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    chdir("/root");
    /* A shell on the screen (tty1, keyboard) too, when there is one and /dev/console is
     * another device (the last "console=" of the command line, listed last in
     * console/active).  Not via cttyhack, which always picks that last console device. */
    s = first_line("/sys/class/tty/console/active");
    if ((p = strrchr(s, ' ')))
        primary = p + 1;
    else if (*s)
        primary = s;
    if (exists("/dev/tty1") && (!primary || strcmp(primary, "tty0") != 0))
        spawn_on_tty("/dev/tty1", (char *[]){ "sh", NULL }, 1);
    free(s);
    /* the shell on /dev/console's device (cttyhack finds it: ttyS0, or tty0 = the active VT) */
    banner(stdout);
    /* tcc.selfhost=auto (run.sh --selfhost --auto): rebuild the kernel (auto=all: the toolchain
     * first, then the kernel with it; auto=test: the toolchain and tcc's test suite, no
     * kernel), send the results to the host and shut down, no shell */
    s = first_line("/proc/cmdline");
    if ((p = strstr(s, "tcc.selfhost=")) && (p == s || p[-1] == ' ')) {
        char *mode = p + strlen("tcc.selfhost=");
        int rc = 0;
        if ((p = strchr(mode, ' ')))
            *p = 0;
        printf("*** tcc.selfhost=%s: automatic self-hosting run\n", mode);
        if (strcmp(mode, "all") == 0)
            rc = runl("build-toolchain", NULL);
        else if (strcmp(mode, "test") == 0)
            rc = runl("build-toolchain", "--test", NULL);
        if (strcmp(mode, "test") == 0) {
            int r;
            if ((r = send_if_exists("/build/build-toolchain.log")))
                rc = r;
            if ((r = send_if_exists("/build/tcc-test.log")))
                rc = r;
        } else {
            if (rc == 0)
                rc = runl("build-kernel", NULL);
            if (rc == 0)
                rc = runl("send-file", "/build/linux/arch/x86/boot/bzImage", "/build/build-kernel.log", NULL);
            else {
                send_if_exists("/build/build-kernel.log");
                send_if_exists("/build/build-toolchain.log");
            }
        }
        printf("*** self-hosting run finished with status %d, shutting down\n", rc);
        shutdown();
    }
    free(s);
    wait_for(spawn_on_tty(NULL, (char *[]){ "cttyhack", "sh", NULL }, 0));
    printf("*** shell exited, rebooting\n");
    shutdown();
    return 0;
}
