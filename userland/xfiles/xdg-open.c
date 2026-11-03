#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * xdg-open - the opener of the graphical session: open each FILE with the program that fits
 * its kind, found from the name's extension and the first bytes.  xfiles runs it (its
 * OPENER, this is the default name) on a double click, xfilesctl from its menu; any program
 * that calls xdg-open gets the same.  Nothing of the freedesktop.org xdg-utils is here: no
 * MIME database, no .desktop files, the image has neither.  A C script run by tcc, like
 * every script of this system.
 *
 * Usage: xdg-open FILE...
 *        xdg-open --with COMMAND FILE...   open the files with COMMAND (a program name, with
 *                                          arguments); a terminal program (vi, less, hexdump,
 *                                          tar, ...) runs in a terminal window, an X program
 *                                          directly
 *        xdg-open --run COMMAND [ARG...]   run COMMAND in this terminal, then wait for Enter
 *                                          before exiting (what "--with" and the opening of
 *                                          executables run inside the terminal window)
 *   directory              a new xfiles window on it
 *   PNG, JPEG, GIF         the links browser in graphics mode, on a page that holds the image
 *                          (links -g draws images inline; a bare image file makes it ask
 *                          whether to save or display it)
 *   HTML                   links -g
 *   executable             run in a terminal window, which stays open until Enter
 *                          (an ELF binary, or a #! script: tcc runs the C scripts of
 *                          this system through the kernel's #! line)
 *   archive (tar, zip)     its listing (tar tvf, unzip -l) in a terminal window
 *   text                   the editor in a terminal window ($EDITOR, default vi)
 *   anything else          hexdump -C in a terminal window
 * Env: EDITOR (vi), BROWSER (links -g), TERMINAL (mlterm; its -e runs a command), DISPLAY.
 * The programs are started detached (a new session, stdio on /dev/null), so that the caller
 * (xfiles waits for its children) is not held.  Exit status: 0 when every file was given to
 * a program, 1 when one could not be opened (named on stderr).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>

enum kind { K_DIR, K_IMAGE, K_HTML, K_EXEC, K_TAR, K_ZIP, K_TEXT, K_BINARY, K_MISSING };

static const char *editor, *browser, *terminal;

/* split "cmd arg arg" into argv[]; returns the count */
static int split(char *s, char **argv, int max)
{
    int n = 0;
    char *p;
    for (p = strtok(s, " \t"); p && n < max - 1; p = strtok(NULL, " \t"))
        argv[n++] = p;
    argv[n] = NULL;
    return n;
}

/* run argv detached: a grandchild in its own session, stdio on /dev/null; when TMPFILE
 * is given, the grandchild stays as the program's parent and removes the file when it ends */
static int spawn_cleanup(char **argv, const char *tmpfile)
{
    pid_t pid;
    int st;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (fork() != 0)
            _exit(0);
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2)
                close(fd);
        }
        if (tmpfile) {
            pid_t p = fork();
            if (p > 0) {
                waitpid(p, &st, 0);
                unlink(tmpfile);
                _exit(0);
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    waitpid(pid, &st, 0);
    return 0;
}

static int spawn(char **argv) { return spawn_cleanup(argv, NULL); }

/* an image: links -g shows it inline in a page, while a bare image URL makes it ask
 * whether to save or display; the page is a temporary HTML file with the image and the
 * file's name as its title, removed when the browser ends */
static int open_image(const char *path)
{
    char *page, *argv[64], *cmd, abs[PATH_MAX];
    const char *tmp = getenv("TMPDIR"), *name = strrchr(path, '/');
    FILE *f;
    int n;
    name = name ? name + 1 : path;
    if (path[0] != '/') {           /* the URL needs the absolute path */
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd) && (size_t)snprintf(abs, sizeof abs, "%s/%s", cwd, path) < sizeof abs)
            path = abs;
    }
    if (asprintf(&page, "%s/xdg-open.%d.html", tmp && *tmp ? tmp : "/tmp", (int)getpid()) < 0)
        return 1;
    f = fopen(page, "w");
    if (!f) {
        free(page);
        return 1;
    }
    fprintf(f, "<!DOCTYPE html><html><head><title>");
    for (; *name; name++)
        if (*name == '<' || *name == '&') fprintf(f, "&#%d;", (unsigned char)*name); else putc(*name, f);
    fprintf(f, "</title></head><body bgcolor=\"#1e1e1e\" text=\"#e0e0e0\"><img src=\"file://");
    for (; *path; path++)
        if (*path == '"' || *path == '<' || *path == '&' || *path == '%' || *path == '#' || *path == '?')
            fprintf(f, "%%%02X", (unsigned char)*path);
        else
            putc(*path, f);
    fprintf(f, "\"></body></html>\n");
    fclose(f);
    cmd = strdup(browser);
    n = split(cmd, argv, 60);
    argv[n++] = page; argv[n] = NULL;
    n = spawn_cleanup(argv, page) < 0;
    free(page);
    free(cmd);
    return n;
}

/* run argv in a terminal window: TERMINAL -e xdg-open --run argv... (the window stays
 * until Enter, so that a program's output can be read) */
static int in_terminal(char **argv, int hold)
{
    char *targv[64], *term = strdup(terminal);
    int n = split(term, targv, 32), i;
    targv[n++] = "-e";
    if (hold) {
        targv[n++] = "/usr/bin/xdg-open";
        targv[n++] = "--run";
    }
    for (i = 0; argv[i] && n < 63; i++)
        targv[n++] = argv[i];
    targv[n] = NULL;
    return spawn(targv);
}

static int has_ext(const char *name, const char *ext)
{
    size_t ln = strlen(name), le = strlen(ext);
    return ln > le && name[ln - le - 1] == '.' && strcasecmp(name + ln - le, ext) == 0;
}

static int is_text(const unsigned char *b, size_t n)
{
    size_t i, bad = 0;
    if (n == 0)
        return 1;
    for (i = 0; i < n; i++) {
        if (b[i] == 0)
            return 0;
        if (b[i] < 0x20 && b[i] != '\n' && b[i] != '\r' && b[i] != '\t' && b[i] != '\f' && b[i] != 0x1b)
            bad++;
    }
    return bad * 20 < n;        /* a few control characters are fine (UTF-8 is all > 0x7f) */
}

static enum kind classify(const char *path)
{
    struct stat st;
    unsigned char b[1024];
    ssize_t n = 0;
    int fd;
    if (stat(path, &st) != 0)
        return K_MISSING;
    if (S_ISDIR(st.st_mode))
        return K_DIR;
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        n = read(fd, b, sizeof b);
        close(fd);
    }
    if (n < 0)
        n = 0;
    if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)
        return K_IMAGE;
    if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff)
        return K_IMAGE;
    if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0))
        return K_IMAGE;
    if (has_ext(path, "png") || has_ext(path, "jpg") || has_ext(path, "jpeg") || has_ext(path, "gif"))
        return K_IMAGE;
    if (n >= 4 && memcmp(b, "PK\003\004", 4) == 0)
        return K_ZIP;
    if (has_ext(path, "zip"))
        return K_ZIP;
    if ((n >= 262 && memcmp(b + 257, "ustar", 5) == 0) || has_ext(path, "tar") || has_ext(path, "tgz")
        || has_ext(path, "tar.gz") || has_ext(path, "tar.xz") || has_ext(path, "tar.bz2"))
        return K_TAR;
    if ((st.st_mode & 0111) && n >= 4 && (memcmp(b, "\177ELF", 4) == 0 || memcmp(b, "#!", 2) == 0))
        return K_EXEC;
    if (has_ext(path, "html") || has_ext(path, "htm"))
        return K_HTML;
    if (n > 0) {
        char head[64];
        size_t m = n < 63 ? n : 63;
        memcpy(head, b, m);
        head[m] = 0;
        if (strncasecmp(head, "<!DOCTYPE html", 14) == 0 || strncasecmp(head, "<html", 5) == 0)
            return K_HTML;
    }
    if (is_text(b, n))
        return K_TEXT;
    return K_BINARY;
}

static int open_one(const char *path)
{
    char *argv[64], *cmd;
    int n;
    switch (classify(path)) {
    case K_MISSING:
        fprintf(stderr, "xdg-open: %s: %s\n", path, strerror(errno));
        return 1;
    case K_DIR:
        argv[0] = "xfiles"; argv[1] = (char *)path; argv[2] = NULL;
        return spawn(argv) < 0;
    case K_IMAGE:
        return open_image(path);
    case K_HTML:
        cmd = strdup(browser);
        n = split(cmd, argv, 60);
        argv[n++] = (char *)path; argv[n] = NULL;
        return spawn(argv) < 0;
    case K_EXEC:
        argv[0] = (char *)path; argv[1] = NULL;
        return in_terminal(argv, 1) < 0;
    case K_TAR:
        argv[0] = "tar"; argv[1] = "tvf"; argv[2] = (char *)path; argv[3] = NULL;
        return in_terminal(argv, 1) < 0;
    case K_ZIP:
        argv[0] = "unzip"; argv[1] = "-l"; argv[2] = (char *)path; argv[3] = NULL;
        return in_terminal(argv, 1) < 0;
    case K_TEXT:
        cmd = strdup(editor);
        n = split(cmd, argv, 60);
        argv[n++] = (char *)path; argv[n] = NULL;
        return in_terminal(argv, 0) < 0;
    default:
        argv[0] = "hexdump"; argv[1] = "-C"; argv[2] = (char *)path; argv[3] = NULL;
        return in_terminal(argv, 1) < 0;
    }
}

/* --with: is the command one that needs a terminal? (an X program is run directly) */
static int wants_terminal(const char *cmd)
{
    static const char *xprogs[] = { "links", "xfiles", "mlterm", "xmenu", "dmenu", "jwm", "xdg-open", "xterm", NULL };
    const char *base = strrchr(cmd, '/');
    int i;
    base = base ? base + 1 : cmd;
    for (i = 0; xprogs[i]; i++)
        if (strcmp(base, xprogs[i]) == 0)
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    int i, rc = 0, n;
    char *cargv[64], *cmd;
    editor = getenv("EDITOR"); if (!editor || !*editor) editor = "vi";
    browser = getenv("BROWSER"); if (!browser || !*browser) browser = "links -g";
    terminal = getenv("TERMINAL"); if (!terminal || !*terminal) terminal = "mlterm";
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fprintf(argc < 2 ? stderr : stdout,
                "usage: xdg-open FILE...                open each file with the program for its kind\n"
                "       xdg-open --with COMMAND FILE...  open the files with COMMAND (terminal programs in a terminal)\n"
                "       xdg-open --run COMMAND [ARG...]  run COMMAND here, then wait for Enter\n"
                "directories: xfiles; images and HTML: $BROWSER (links -g); executables, archives: in\n"
                "$TERMINAL (mlterm); text: $EDITOR (vi) in the terminal; the rest: hexdump -C in it\n");
        return argc < 2 ? 2 : 0;
    }
    if (strcmp(argv[1], "--run") == 0) {
        pid_t pid;
        int st = 0, c;
        if (argc < 3)
            return 2;
        fflush(NULL);
        pid = fork();
        if (pid == 0) {
            execvp(argv[2], argv + 2);
            fprintf(stderr, "xdg-open: %s: %s\n", argv[2], strerror(errno));
            _exit(127);
        }
        if (pid > 0)
            waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
            printf("\n[%s exited with status %d]", argv[2], WEXITSTATUS(st));
        else if (WIFSIGNALED(st))
            printf("\n[%s ended by signal %d]", argv[2], WTERMSIG(st));
        printf("\n[Enter closes this window] ");
        fflush(stdout);
        while ((c = getchar()) != EOF && c != '\n')
            ;
        return 0;
    }
    if (strcmp(argv[1], "--with") == 0) {
        if (argc < 4)
            return 2;
        cmd = strdup(argv[2]);
        n = split(cmd, cargv, 40);
        if (n == 0)
            return 2;
        for (i = 3; i < argc && n < 62; i++)
            cargv[n++] = argv[i];
        cargv[n] = NULL;
        if (wants_terminal(cargv[0]))
            return in_terminal(cargv, 1) < 0;
        return spawn(cargv) < 0;
    }
    for (i = 1; i < argc; i++)
        rc |= open_one(argv[i]);
    return rc;
}
