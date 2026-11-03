#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * startx - the graphical mode: start the X server (TinyX's Xfbdev on /dev/fb0, on a free
 * virtual terminal, which it switches the screen to), the JWM window manager and the links
 * web browser in its graphics mode on it; when the window manager ends (its menu's Log out,
 * Alt-F1), stop the server, and the screen returns to the text console it came from.  In an
 * image without /usr/bin/jwm (the jwm step not built) links alone runs, the size of the
 * screen, and the session ends with it.  A C script run by tcc, like every script of this
 * system.
 *
 * Usage: startx [links arguments...]        e.g.  startx https://linux.org/
 *   no arguments: links opens with no page (its menu is on the Escape key / the top line)
 * Env: DISPLAY (default :0, the server's display number), XSERVER_OPTS (extra options
 *   for Xfbdev, e.g. "vt7" or "-mouse /dev/input/mice"), XLOG (the server's output,
 *   default /tmp/Xfbdev.log); LINKS_XTERM is set to "mlterm -e" and TERMINAL to mlterm
 *   when the image has /usr/bin/mlterm (the terminal links, JWM's menu and the file manager
 *   open), EDITOR to vi and XDG_CACHE_HOME to $HOME/.cache (xfiles' thumbnails), unless
 *   already set; /etc/X11/Xresources and ~/.Xresources are loaded with xrdb (see session_env)
 * The server's log is printed when the server fails to start.  Status: that of links,
 * 1 when the server could not be started.
 *
 * How the start is synchronised: an X server sends SIGUSR1 to its parent once it accepts
 * connections, when the parent ignored SIGUSR1 at the time the server started (the xinit
 * convention: os/connection.c of the server); we ignore it, fork the server, and wait for
 * either that signal or the server's death.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const char *xlog = "/tmp/Xfbdev.log";

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void print_log(void)
{
    FILE *f = fopen(xlog, "r");
    int c;
    if (!f)
        return;
    fprintf(stderr, "---- %s ----\n", xlog);
    while ((c = getc(f)) != EOF)
        putc(c, stderr);
    fclose(f);
}

/* the session's environment and resources, once the server runs: the X resources of
 * /etc/X11/Xresources (userland/xfiles/Xresources: xfiles' font, colours and icon patterns,
 * xmenu's look) and the user's ~/.Xresources go into the server's RESOURCE_MANAGER property
 * with xrdb (-nocpp: the image has no C preprocessor); XDG_CACHE_HOME is where xfiles keeps
 * its thumbnails ($HOME/.cache; without it xfiles makes none), EDITOR what xdg-open opens
 * text with, TERMINAL the terminal it and xfilesctl run programs in */
static void session_env(void)
{
    const char *home = getenv("HOME");
    char path[512];
    if (access("/usr/bin/mlterm", X_OK) == 0) {
        setenv("LINKS_XTERM", "mlterm -e", 0);
        setenv("TERMINAL", "mlterm", 0);
    }
    setenv("EDITOR", "vi", 0);
    /* the character set of the session: musl's C locale is ASCII, its C.UTF-8 the one with
     * UTF-8 (xfiles' setlocale/XSupportsLocale, mlterm's encoding, busybox's ls) */
    setenv("LANG", "C.UTF-8", 0);
    /* libX11's locale data (XSupportsLocale, the input method dmenu needs) is staged under
     * /usr/share/X11/locale; the library was built to look in its build tree */
    setenv("XLOCALEDIR", "/usr/share/X11/locale", 0);
    if (home && *home && !getenv("XDG_CACHE_HOME")) {
        snprintf(path, sizeof path, "%s/.cache", home);
        setenv("XDG_CACHE_HOME", path, 1);
    }
    if (access("/usr/bin/xrdb", X_OK) == 0) {
        const char *files[2] = { "/etc/X11/Xresources", NULL };
        int i;
        if (home && *home) {
            snprintf(path, sizeof path, "%s/.Xresources", home);
            files[1] = path;
        }
        for (i = 0; i < 2; i++) {
            pid_t pid;
            int st;
            if (!files[i] || access(files[i], R_OK) != 0)
                continue;
            fflush(NULL);
            pid = fork();
            if (pid == 0) {
                execlp("xrdb", "xrdb", "-nocpp", "-merge", files[i], (char *)NULL);
                _exit(127);
            }
            if (pid > 0)
                waitpid(pid, &st, 0);
        }
    }
}

static pid_t start_server(const char *display)
{
    char *argv[32];
    int n = 0;
    char *opts = getenv("XSERVER_OPTS"), *p;
    pid_t pid;
    argv[n++] = "Xfbdev";
    argv[n++] = (char *)display;
    /* the fonts compiled into libXfont (fixed, cursor): the image carries no font files */
    argv[n++] = "-fp";
    argv[n++] = "built-ins";
    if (opts && *opts) {
        opts = strdup(opts);
        for (p = strtok(opts, " "); p && n < 30; p = strtok(NULL, " "))
            argv[n++] = p;
    }
    argv[n] = NULL;
    fflush(NULL);
    pid = fork();
    if (pid != 0)
        return pid;
    /* the server's messages go to the log; stdin from /dev/null */
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
        dup2(fd, 0);
        close(fd);
    }
    fd = open(xlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }
    signal(SIGUSR1, SIG_IGN);       /* the convention: inherited as ignored */
    execvp(argv[0], argv);
    _exit(127);
    return -1;
}

int main(int argc, char **argv)
{
    const char *display = getenv("DISPLAY");
    char **largv;
    pid_t server, client, wm, p;
    sigset_t set, old;
    int st, i, n, ready = 0, rc, have_wm;
    char mode[32];
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("usage: startx [links arguments...]   (e.g. startx https://linux.org/)\n"
               "starts the X server (Xfbdev) on a free virtual terminal, the JWM window manager\n"
               "and links -g on it (Log out in JWM's menu ends it; its Terminal entry and Ctrl-Alt-T open mlterm);\n"
               "env: DISPLAY (:0), XSERVER_OPTS (extra Xfbdev options), XLOG (%s)\n", xlog);
        return 0;
    }
    if (getenv("XLOG"))
        xlog = getenv("XLOG");
    if (!display || !*display)
        display = ":0";
    setenv("DISPLAY", display, 1);
    if (!exists("/dev/fb0")) {
        fprintf(stderr, "startx: no /dev/fb0: this machine has no framebuffer (QEMU: run.sh --vga opens the display, the VGA device is there anyway)\n");
        return 1;
    }
    if (access("/usr/bin/Xfbdev", X_OK) != 0) {
        fprintf(stderr, "startx: no /usr/bin/Xfbdev: the image was built without the X server (scripts/build.sh tinyx)\n");
        return 1;
    }
    /* SIGUSR1 ignored before the fork (see above), and blocked here with SIGCHLD so that
     * neither can slip by between the fork and the wait */
    signal(SIGUSR1, SIG_IGN);
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, &old);
    server = start_server(display);
    if (server < 0) {
        perror("startx: fork");
        return 1;
    }
    /* wait for the server: SIGUSR1 (ready), SIGCHLD (it died), or 15 s */
    for (i = 0; i < 150 && !ready; i++) {
        struct timespec ts = { 0, 100000000 };
        siginfo_t si;
        sigset_t wait = set;
        int sig = sigtimedwait(&wait, &si, &ts);
        if (sig == SIGUSR1)
            ready = 1;
        else if (sig == SIGCHLD || (sig < 0 && errno == EAGAIN && waitpid(server, &st, WNOHANG) == server)) {
            if (sig == SIGCHLD && waitpid(server, &st, WNOHANG) != server)
                continue;   /* some other child */
            fprintf(stderr, "startx: the X server exited with status %d before it was ready\n",
                    WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
            print_log();
            return 1;
        }
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    if (!ready) {
        fprintf(stderr, "startx: the X server did not come up within 15 s\n");
        print_log();
        kill(server, SIGTERM);
        waitpid(server, &st, 0);
        return 1;
    }
    /* the window manager when the image has it: JWM manages the windows, and its exit
     * (Log out in its menu) ends the session.  Without it links runs alone, its window the
     * size of the screen (-mode WxH from the framebuffer: "1024,768" in sysfs; links'
     * default size leaves a margin for a window manager's decorations). */
    /* the terminal emulator when the image has it: links runs its "run in terminal"
     * programs through $LINKS_XTERM (default "xterm -e"), and JWM's menu starts it;
     * /etc/mlterm/main holds its settings (the scrollbar on the right); the X resources
     * and the rest of the session's environment (session_env above) */
    session_env();
    /* the session runs in the home directory (the programs JWM's menu starts, the file
     * manager first of all, open there; /init starts startx from /root, which is not the
     * console user's) */
    if (getenv("HOME") && chdir(getenv("HOME")) != 0)
        chdir("/");
    have_wm = access("/usr/bin/jwm", X_OK) == 0;
    printf("startx: X server ready on %s (log: %s), running %slinks -g on it\n", display, xlog,
           have_wm ? "jwm and " : "");
    fflush(stdout);
    largv = calloc(argc + 6, sizeof *largv);
    n = 0;
    largv[n++] = "links";
    largv[n++] = "-g";
    if (!have_wm) {
        FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
        int w = 0, h = 0;
        if (f) {
            if (fscanf(f, "%d,%d", &w, &h) == 2 && w > 0 && h > 0) {
                snprintf(mode, sizeof mode, "%dx%d", w, h);
                largv[n++] = "-mode";
                largv[n++] = mode;
            }
            fclose(f);
        }
    }
    for (i = 1; i < argc; i++)
        largv[n++] = argv[i];
    wm = -1;
    if (have_wm) {
        wm = fork();
        if (wm == 0) {
            execl("/usr/bin/jwm", "jwm", (char *)NULL);
            perror("startx: jwm");
            _exit(127);
        }
    }
    client = fork();
    if (client == 0) {
        execvp(largv[0], largv);
        perror("startx: links");
        _exit(127);
    }
    /* the session: until the window manager ends (or, without one, links), or the server dies */
    rc = 1;
    while ((p = waitpid(-1, &st, 0)) > 0) {
        if (p == wm || (p == client && wm < 0)) {
            rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            break;
        }
        if (p == client) {
            client = -1;        /* links closed: the session goes on under the window manager */
            continue;
        }
        if (p == server) {
            fprintf(stderr, "startx: the X server died\n");
            print_log();
            if (client > 0) { kill(client, SIGTERM); waitpid(client, &st, 0); }
            if (wm > 0) { kill(wm, SIGTERM); waitpid(wm, &st, 0); }
            return 1;
        }
    }
    /* the session is over: stop the clients still running and the server, which switches
     * the screen back to the text console */
    if (p == wm && client > 0) { kill(client, SIGTERM); waitpid(client, &st, 0); }
    kill(server, SIGTERM);
    waitpid(server, &st, 0);
    return rc;
}
