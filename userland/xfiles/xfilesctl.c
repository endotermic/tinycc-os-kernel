#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * xfilesctl - the control script of the xfiles file manager: xfiles itself only draws the
 * icon grid and runs this program for everything that acts on files, waiting for it to
 * return (so nothing here blocks for long; programs that stay are started detached).  A C
 * script run by tcc, like every script of this system.  Called by xfiles as:
 *
 *   xfilesctl menu [FILE...]        right click: the selected files, or none on the background
 *                                   (on the desktop, xfiles -d with XFILES_DESKTOP=1: the
 *                                   session's entries too: terminal, file manager, browser,
 *                                   Restart JWM, Log out; the background has no JWM menu)
 *   xfilesctl drop-ask FILE...      files dropped here with no modifier (the current directory
 *                                   is the target): ask copy / move / link
 *   xfilesctl drop-copy FILE...     dropped with Control: copy them here
 *   xfilesctl drop-move FILE...     dropped with Shift: move them here
 *   xfilesctl drop-link FILE...     dropped with Control+Shift: link them here
 *   xfilesctl ^KEY [FILE...]        a key with Control, a function key, Delete or BackSpace
 *                                   (the keysym's name), with the selected files
 *
 * What the keys do (the menu lists them too):
 *   Ctrl-L / Ctrl-G  address bar: a prompt (dmenu, embedded in the window) listing the
 *                    directories here; a directory or path goes there ("-" / "+" are the
 *                    history, "~" the home), a file is opened
 *   Ctrl-C, Ctrl-X   remember the selected files for pasting (copy / move)
 *   Ctrl-V           paste them into this directory
 *   Ctrl-N           new folder (a prompt for its name)
 *   Ctrl-T           a terminal (mlterm) in this directory
 *   Ctrl-O           open the selected files with a command typed at a prompt
 *   Ctrl-I           properties of the selected files (or of this directory)
 *   Ctrl-W           a new xfiles window on this directory
 *   F2               rename the selected files, one prompt each
 *   F5, Ctrl-R       reload the directory
 *   Delete           delete the selected files, after a confirmation
 *   BackSpace        the parent directory
 *   Ctrl-A, Ctrl-H   select all, show or hide the dot files: xfiles does these itself
 *                    (patches/xfiles); the menu reaches them through _CONTROL_GOTO
 *
 * Helpers: xmenu (the pop-up menu and the confirmations), dmenu (the prompts; -w WINDOWID
 * embeds it in xfiles' window through XEmbed), xprop (_CONTROL_GOTO on the window changes
 * its directory), xdg-open (opens a file by its kind), cp/mv/ln/rm/mkdir (busybox).  Names
 * that already exist in the target get a _1, _2, ... suffix before the extension.  The
 * paste buffer is the file $XDG_CACHE_HOME/xfiles/clipboard ($HOME/.cache/xfiles/clipboard):
 * the first line "copy" or "move", then the paths.
 * Env: WINDOWID (set by xfiles), XFILES_DESKTOP (set by xfiles -d), HOME, XDG_CACHE_HOME,
 * TERMINAL (mlterm), DISPLAY.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <locale.h>

#define FONT    "DejaVu Sans:size=11"
#define BG      "#1e1e1e"
#define FG      "#e0e0e0"
#define SELBG   "#2a6fb0"
#define SELFG   "#ffffff"

static const char *windowid;
static char cwd[PATH_MAX];

/* ---- running programs ------------------------------------------------------------- */

/* run argv with INPUT on its stdin and return its stdout (without the final newline;
 * NULL when nothing was printed or the program could not run) */
static char *capture(char **argv, const char *input)
{
    int in[2], out[2], st;
    pid_t pid;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    ssize_t n;
    if (pipe(in) < 0 || pipe(out) < 0)
        return NULL;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return NULL;
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (input) {
        const char *p = input;
        size_t left = strlen(input);
        signal(SIGPIPE, SIG_IGN);
        while (left > 0 && (n = write(in[1], p, left)) > 0) {
            p += n;
            left -= n;
        }
    }
    close(in[1]);
    for (;;) {
        if (len + 4096 > cap) {
            cap = cap ? cap * 2 : 8192;
            buf = realloc(buf, cap);
        }
        n = read(out[0], buf + len, cap - len - 1);
        if (n <= 0)
            break;
        len += n;
    }
    close(out[0]);
    waitpid(pid, &st, 0);
    if (len == 0) {
        free(buf);
        return NULL;
    }
    buf[len] = 0;
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = 0;
    return buf;
}

/* run argv and wait; its exit status, 127 when it could not run; stderr is kept */
static int run(char **argv)
{
    pid_t pid;
    int st;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return 127;
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "xfilesctl: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

/* run argv detached (its own session, stdio on /dev/null): xfiles waits for us, not for it */
static void detach(char **argv)
{
    pid_t pid;
    int st;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        if (fork() != 0)
            _exit(0);
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
            if (fd > 2)
                close(fd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    waitpid(pid, &st, 0);
}

/* ---- the three dialogs: menu, prompt, message ------------------------------------- */

/* xmenu on ITEMS (its input format: "label\toutput" lines, "" a separator, "label\t:" a
 * line that cannot be chosen); the output of the chosen item, NULL when dismissed */
static char *menu(const char *items)
{
    char *argv[8];
    int n = 0;
    argv[n++] = "xmenu";
    if (windowid) {
        argv[n++] = "-t";
        argv[n++] = (char *)windowid;
    }
    argv[n] = NULL;
    return capture(argv, items);
}

/* dmenu embedded in the xfiles window with the prompt LABEL and the newline-separated
 * ITEMS to choose from (none: whatever is typed is returned); NULL when cancelled */
static char *prompt(const char *label, const char *items)
{
    char *argv[24];
    int n = 0;
    argv[n++] = "dmenu";
    argv[n++] = "-p"; argv[n++] = (char *)label;
    argv[n++] = "-fn"; argv[n++] = FONT;
    argv[n++] = "-nb"; argv[n++] = BG;
    argv[n++] = "-nf"; argv[n++] = FG;
    argv[n++] = "-sb"; argv[n++] = SELBG;
    argv[n++] = "-sf"; argv[n++] = SELFG;
    if (windowid) {
        argv[n++] = "-w";
        argv[n++] = (char *)windowid;
    }
    argv[n] = NULL;
    return capture(argv, items ? items : "");
}

/* a question with two answers; 1 when YES was chosen */
static int confirm(const char *question, const char *yes)
{
    char *items, *r;
    int ok;
    if (asprintf(&items, "%s\t:\n\n%s\tyes\nCancel\tno\n", question, yes) < 0)
        return 0;
    r = menu(items);
    free(items);
    ok = r && strcmp(r, "yes") == 0;
    free(r);
    return ok;
}

/* show LINES (newline-separated) in a menu that only closes */
static void message(const char *title, const char *lines)
{
    char *items = NULL, *r, *copy = strdup(lines), *p, *nl;
    size_t cap = strlen(lines) * 2 + strlen(title) + 64, len;
    items = malloc(cap);
    snprintf(items, cap, "%s\t:\n\n", title);
    len = strlen(items);
    for (p = copy; p && *p; p = nl) {
        nl = strchr(p, '\n');
        if (nl)
            *nl++ = 0;
        len += snprintf(items + len, cap - len, "%s\t:\n", *p ? p : " ");
    }
    snprintf(items + len, cap - len, "\nClose\tok\n");
    r = menu(items);
    free(r);
    free(items);
    free(copy);
}

/* ---- paths and the paste buffer ----------------------------------------------------- */

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* DIR/NAME, or DIR/NAME_1.ext, _2, ... when it exists (the suffix before the extension) */
static char *unique_dest(const char *dir, const char *name)
{
    char *dest, *body, *ext;
    struct stat st;
    int i;
    if (asprintf(&dest, "%s/%s", dir, name) < 0)
        return NULL;
    if (lstat(dest, &st) != 0)
        return dest;
    body = strdup(name);
    ext = strrchr(body, '.');
    if (ext == body || ext == NULL)      /* ".hidden" or no extension: no split */
        ext = "";
    else
        *ext++ = 0;
    for (i = 1; ; i++) {
        free(dest);
        if (asprintf(&dest, "%s/%s_%d%s%s", dir, body, i, *ext ? "." : "", ext) < 0)
            return NULL;
        if (lstat(dest, &st) != 0)
            break;
    }
    free(body);
    return dest;
}

static char *clipboard_path(void)
{
    static char path[PATH_MAX];
    const char *c = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (c && *c)
        snprintf(path, sizeof path, "%s/xfiles", c);
    else
        snprintf(path, sizeof path, "%s/.cache/xfiles", home ? home : "/tmp");
    mkdir(path, 0700);      /* $HOME/.cache may be missing: two levels */
    {
        char *slash = strrchr(path, '/');
        *slash = 0;
        mkdir(path, 0755);
        *slash = '/';
    }
    mkdir(path, 0700);
    strncat(path, "/clipboard", sizeof path - strlen(path) - 1);
    return path;
}

static void clipboard_write(const char *op, int n, char **files)
{
    FILE *f = fopen(clipboard_path(), "w");
    int i;
    if (!f)
        return;
    fprintf(f, "%s\n", op);
    for (i = 0; i < n; i++)
        fprintf(f, "%s\n", files[i]);
    fclose(f);
}

/* the paste buffer: OP ("copy"/"move") and the paths; the count, 0 when empty */
static int clipboard_read(char *op, size_t oplen, char ***files)
{
    FILE *f = fopen(clipboard_path(), "r");
    char line[PATH_MAX + 2];
    int n = 0, cap = 0;
    *files = NULL;
    if (!f)
        return 0;
    if (!fgets(op, oplen, f)) {
        fclose(f);
        return 0;
    }
    op[strcspn(op, "\n")] = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!*line)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            *files = realloc(*files, cap * sizeof **files);
        }
        (*files)[n++] = strdup(line);
    }
    fclose(f);
    return n;
}

/* ---- the file operations ------------------------------------------------------------ */

/* report what failed, if anything */
static char *errors;
static void failed(const char *what, const char *path)
{
    char *s;
    if (asprintf(&s, "%s%s: %s", errors ? errors : "", what, path) >= 0) {
        free(errors);
        errors = s;
        strcat(errors, "\n");
    }
}

/* copy, move or link FILES into DIR */
static void transfer(const char *op, int n, char **files, const char *dir)
{
    int i;
    char *argv[8], *dest;
    for (i = 0; i < n; i++) {
        const char *name = basename_of(files[i]);
        if (!*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (strcmp(op, "move") == 0) {
            /* a move within the same directory is nothing to do */
            char *fdir = strdup(files[i]), *s = strrchr(fdir, '/');
            int same;
            if (s == fdir) s[1] = 0; else if (s) *s = 0;
            same = strcmp(fdir, dir) == 0;
            free(fdir);
            if (same)
                continue;
        }
        dest = unique_dest(dir, name);
        if (!dest)
            continue;
        if (strcmp(op, "copy") == 0) {
            argv[0] = "cp"; argv[1] = "-a"; argv[2] = "--"; argv[3] = files[i]; argv[4] = dest; argv[5] = NULL;
        } else if (strcmp(op, "move") == 0) {
            argv[0] = "mv"; argv[1] = "--"; argv[2] = files[i]; argv[3] = dest; argv[4] = NULL;
        } else {
            argv[0] = "ln"; argv[1] = "-s"; argv[2] = "--"; argv[3] = files[i]; argv[4] = dest; argv[5] = NULL;
        }
        if (run(argv) != 0)
            failed(argv[0], files[i]);
        free(dest);
    }
    if (errors) {
        message("Something went wrong", errors);
        free(errors);
        errors = NULL;
    }
}

static void paste(void)
{
    char op[32], **files;
    int n = clipboard_read(op, sizeof op, &files);
    if (n == 0) {
        message("Paste", "Nothing to paste: select files and press Ctrl-C (copy) or Ctrl-X (move) first.");
        return;
    }
    transfer(op, n, files, cwd);
    if (strcmp(op, "move") == 0)
        unlink(clipboard_path());      /* moved files cannot be pasted twice */
}

static void delete_files(int n, char **files)
{
    char *q, *argv[6];
    int i;
    if (n == 0)
        return;
    if (n == 1)
        asprintf(&q, "Delete \"%s\"?", basename_of(files[0]));
    else
        asprintf(&q, "Delete %d items?", n);
    if (!confirm(q, n == 1 ? "Delete it (no undo)" : "Delete them (no undo)")) {
        free(q);
        return;
    }
    free(q);
    for (i = 0; i < n; i++) {
        argv[0] = "rm"; argv[1] = "-rf"; argv[2] = "--"; argv[3] = files[i]; argv[4] = NULL;
        if (run(argv) != 0)
            failed("rm", files[i]);
    }
    if (errors) {
        message("Something went wrong", errors);
        free(errors);
        errors = NULL;
    }
}

static void rename_files(int n, char **files)
{
    int i;
    char *label, *name, *dest, *argv[6];
    for (i = 0; i < n; i++) {
        const char *old = basename_of(files[i]);
        if (asprintf(&label, "Rename \"%s\" to:", old) < 0)
            return;
        name = prompt(label, "");
        free(label);
        if (!name || !*name || strcmp(name, old) == 0 || strchr(name, '/')) {
            free(name);
            continue;
        }
        if (asprintf(&dest, "%s/%s", cwd, name) < 0)
            return;
        if (access(dest, F_OK) == 0) {
            char *m;
            asprintf(&m, "\"%s\" exists already; \"%s\" was not renamed.", name, old);
            message("Rename", m);
            free(m);
        } else {
            argv[0] = "mv"; argv[1] = "--"; argv[2] = files[i]; argv[3] = dest; argv[4] = NULL;
            if (run(argv) != 0)
                failed("mv", files[i]);
        }
        free(dest);
        free(name);
    }
    if (errors) {
        message("Something went wrong", errors);
        free(errors);
        errors = NULL;
    }
}

static void new_folder(void)
{
    char *name = prompt("New folder:", ""), *argv[5];
    if (!name || !*name) {
        free(name);
        return;
    }
    argv[0] = "mkdir"; argv[1] = "-p"; argv[2] = "--"; argv[3] = name; argv[4] = NULL;
    if (run(argv) != 0) {
        failed("mkdir", name);
        message("Something went wrong", errors);
        free(errors);
        errors = NULL;
    }
    free(name);
}

/* ---- navigation ---------------------------------------------------------------------- */

/* tell the xfiles window to go to PATH ("-"/"+": its history; "^a", "^h": its actions) */
static void goto_dir(const char *path)
{
    char *argv[12];
    if (!windowid) {
        fprintf(stderr, "xfilesctl: no WINDOWID: cannot change the window's directory\n");
        return;
    }
    argv[0] = "xprop"; argv[1] = "-id"; argv[2] = (char *)windowid;
    argv[3] = "-format"; argv[4] = "_CONTROL_GOTO"; argv[5] = "8u";
    argv[6] = "-set"; argv[7] = "_CONTROL_GOTO"; argv[8] = (char *)path; argv[9] = NULL;
    run(argv);
}

/* the address bar: a prompt listing the directories here; a directory or path goes
 * there, a file is opened */
static void address_bar(void)
{
    DIR *d = opendir(".");
    struct dirent *e;
    char *items = NULL, *answer, *path, **names = NULL;
    size_t len = 0, cap = 0, nn = 0, ncap = 0, i;
    struct stat st;
    if (d) {
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            if (stat(e->d_name, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            if (nn == ncap) {
                ncap = ncap ? ncap * 2 : 32;
                names = realloc(names, ncap * sizeof *names);
            }
            names[nn++] = strdup(e->d_name);
        }
        closedir(d);
    }
    /* sorted, after the two fixed entries */
    for (i = 1; i < nn; i++) {
        char *k = names[i];
        size_t j = i;
        while (j > 0 && strcoll(names[j - 1], k) > 0) {
            names[j] = names[j - 1];
            j--;
        }
        names[j] = k;
    }
    cap = 64;
    items = malloc(cap);
    len = snprintf(items, cap, "..\n~\n");
    for (i = 0; i < nn; i++) {
        size_t l = strlen(names[i]) + 2;
        if (len + l > cap) {
            cap = (len + l) * 2;
            items = realloc(items, cap);
        }
        len += snprintf(items + len, cap - len, "%s/\n", names[i]);
        free(names[i]);
    }
    free(names);
    answer = prompt(cwd, items);
    free(items);
    if (!answer || !*answer) {
        free(answer);
        return;
    }
    if (strcmp(answer, "-") == 0 || strcmp(answer, "+") == 0) {
        goto_dir(answer);
        free(answer);
        return;
    }
    if (answer[0] == '~' && (answer[1] == '/' || answer[1] == 0)) {
        const char *home = getenv("HOME");
        asprintf(&path, "%s%s", home ? home : "", answer + 1);
    } else if (answer[0] == '/') {
        path = strdup(answer);
    } else {
        asprintf(&path, "%s/%s", cwd, answer);
    }
    free(answer);
    len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = 0;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        goto_dir(path);
    } else if (stat(path, &st) == 0) {
        char *argv[3] = { "xdg-open", path, NULL };
        detach(argv);
    } else {
        char *m;
        asprintf(&m, "%s: %s", path, strerror(errno));
        message("Go to", m);
        free(m);
    }
    free(path);
}

/* ---- properties, open with, terminal -------------------------------------------------- */

static void human(off_t n, char *out, size_t len)
{
    const char *u[] = { "bytes", "KB", "MB", "GB", "TB" };
    double v = n;
    int i = 0;
    while (v >= 1024 && i < 4) {
        v /= 1024;
        i++;
    }
    if (i == 0)
        snprintf(out, len, "%lld bytes", (long long)n);
    else
        snprintf(out, len, "%.1f %s (%lld bytes)", v, u[i], (long long)n);
}

static void properties(int n, char **files)
{
    char *lines = NULL, *title, buf[128], mode[11], tbuf[64];
    struct stat st;
    struct passwd *pw;
    struct group *gr;
    const char *kind;
    char *one[1];
    if (n == 0) {
        one[0] = cwd;
        files = one;
        n = 1;
    }
    if (n > 1) {
        off_t total = 0;
        int i, dirs = 0;
        for (i = 0; i < n; i++)
            if (lstat(files[i], &st) == 0) {
                total += st.st_size;
                if (S_ISDIR(st.st_mode))
                    dirs++;
            }
        human(total, buf, sizeof buf);
        asprintf(&lines, "%d items (%d folders)\nTotal size of the files: %s\nIn: %s", n, dirs, buf, cwd);
        message("Properties", lines);
        free(lines);
        return;
    }
    if (lstat(files[0], &st) != 0) {
        asprintf(&lines, "%s: %s", files[0], strerror(errno));
        message("Properties", lines);
        free(lines);
        return;
    }
    kind = S_ISDIR(st.st_mode) ? "folder" : S_ISLNK(st.st_mode) ? "symbolic link" : S_ISREG(st.st_mode) ? "file"
         : S_ISCHR(st.st_mode) ? "character device" : S_ISBLK(st.st_mode) ? "block device"
         : S_ISFIFO(st.st_mode) ? "named pipe" : S_ISSOCK(st.st_mode) ? "socket" : "other";
    snprintf(mode, sizeof mode, "%c%c%c%c%c%c%c%c%c%c",
             S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : '-',
             st.st_mode & S_IRUSR ? 'r' : '-', st.st_mode & S_IWUSR ? 'w' : '-', st.st_mode & S_IXUSR ? 'x' : '-',
             st.st_mode & S_IRGRP ? 'r' : '-', st.st_mode & S_IWGRP ? 'w' : '-', st.st_mode & S_IXGRP ? 'x' : '-',
             st.st_mode & S_IROTH ? 'r' : '-', st.st_mode & S_IWOTH ? 'w' : '-', st.st_mode & S_IXOTH ? 'x' : '-');
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", localtime(&st.st_mtime));
    pw = getpwuid(st.st_uid);
    gr = getgrgid(st.st_gid);
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(files[0]);
        struct dirent *e;
        int count = 0, hidden = 0;
        if (d) {
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                count++;
                if (e->d_name[0] == '.')
                    hidden++;
            }
            closedir(d);
        }
        snprintf(buf, sizeof buf, "%d entries (%d hidden)", count, hidden);
    } else {
        human(st.st_size, buf, sizeof buf);
    }
    asprintf(&lines, "Name: %s\nKind: %s\nSize: %s\nOwner: %s:%s\nMode: %s (%04o)\nModified: %s\nPath: %s%s%s",
             basename_of(files[0]), kind, buf,
             pw ? pw->pw_name : "?", gr ? gr->gr_name : "?", mode, (unsigned)(st.st_mode & 07777), tbuf, files[0],
             S_ISLNK(st.st_mode) ? "\nLink to: " : "", "");
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t l = readlink(files[0], target, sizeof target - 1);
        if (l > 0) {
            char *s;
            target[l] = 0;
            asprintf(&s, "%s%s", lines, target);
            free(lines);
            lines = s;
        }
    }
    asprintf(&title, "Properties of %s", basename_of(files[0]));
    message(title, lines);
    free(title);
    free(lines);
}

static void open_with(int n, char **files)
{
    char *cmd, **argv;
    int i, k = 0;
    if (n == 0)
        return;
    cmd = prompt("Open with:", "vi\nless\nhexdump -C\nlinks -g\nxfiles\ntar tvf\nunzip -l\nsh\n");
    if (!cmd || !*cmd) {
        free(cmd);
        return;
    }
    argv = calloc(n + 4, sizeof *argv);
    argv[k++] = "xdg-open";
    argv[k++] = "--with";
    argv[k++] = cmd;
    for (i = 0; i < n; i++)
        argv[k++] = files[i];
    argv[k] = NULL;
    detach(argv);
    free(argv);
    free(cmd);
}

static void open_files(int n, char **files)
{
    char **argv = calloc(n + 2, sizeof *argv);
    int i;
    argv[0] = "xdg-open";
    for (i = 0; i < n; i++)
        argv[i + 1] = files[i];
    argv[n + 1] = NULL;
    detach(argv);
    free(argv);
}

static void terminal_here(void)
{
    const char *t = getenv("TERMINAL");
    char *copy = strdup(t && *t ? t : "mlterm"), *argv[16], *p;
    int n = 0;
    for (p = strtok(copy, " "); p && n < 15; p = strtok(NULL, " "))
        argv[n++] = p;
    argv[n] = NULL;
    detach(argv);
    free(copy);
}

static void new_window(void)
{
    char *argv[3] = { "xfiles", cwd, NULL };
    detach(argv);
}

/* the desktop's session entries: a program detached, or the running JWM told to restart
 * or to end the session (jwm -restart / -exit send it a client message) */
static void run2(char *a, char *b, char *c)
{
    char *argv[4] = { a, b, c, NULL };
    detach(argv);
}

/* ---- the menus ------------------------------------------------------------------------ */

static void context_menu(int n, char **files)
{
    char op[32], **clip, *items, *choice;
    int has_clip = clipboard_read(op, sizeof op, &clip) > 0;
    if (n > 0) {
        asprintf(&items,
                 "Open\topen\n"
                 "Open with...\tCtrl-O\topenwith\n"
                 "\n"
                 "Copy\tCtrl-C\tcopy\n"
                 "Cut\tCtrl-X\tcut\n"
                 "%s"
                 "\n"
                 "Rename...\tF2\trename\n"
                 "Delete...\tDelete\tdelete\n"
                 "\n"
                 "New folder...\tCtrl-N\tnewdir\n"
                 "Terminal here\tCtrl-T\tterm\n"
                 "Properties...\tCtrl-I\tprops\n",
                 has_clip ? "Paste here\tCtrl-V\tpaste\n" : "");
    } else if (getenv("XFILES_DESKTOP")) {
        asprintf(&items,
                 "Terminal\tCtrl-T\tterm\n"
                 "File manager\tfilemgr\n"
                 "Web browser\tweb\n"
                 "\n"
                 "%s"
                 "New folder...\tCtrl-N\tnewdir\n"
                 "Select all\tCtrl-A\tselall\n"
                 "Show / hide dot files\tCtrl-H\thidden\n"
                 "Reload\tF5\treload\n"
                 "Properties of the desktop folder...\tCtrl-I\tprops\n"
                 "\n"
                 "Restart JWM\tjwmrestart\n"
                 "Log out...\tlogout\n",
                 has_clip ? "Paste\tCtrl-V\tpaste\n" : "");
    } else {
        asprintf(&items,
                 "Terminal here\tCtrl-T\tterm\n"
                 "New window\tCtrl-W\tnewwin\n"
                 "\n"
                 "%s"
                 "New folder...\tCtrl-N\tnewdir\n"
                 "Select all\tCtrl-A\tselall\n"
                 "Show / hide dot files\tCtrl-H\thidden\n"
                 "\n"
                 "Go to...\tCtrl-L\tgoto\n"
                 "Up\tBackSpace\tup\n"
                 "Home\thome\n"
                 "Reload\tF5\treload\n"
                 "\n"
                 "Properties of this folder...\tCtrl-I\tprops\n",
                 has_clip ? "Paste\tCtrl-V\tpaste\n" : "");
    }
    /* xmenu takes "label<TAB>output": fold the key hint into the label */
    {
        char *p, *q, *out = malloc(strlen(items) * 2 + 1);
        for (p = items, q = out; *p; p++) {
            if (*p == '\t') {
                char *t2 = strchr(p + 1, '\t'), *nl = strchr(p + 1, '\n');
                if (t2 && nl && t2 < nl) {      /* label \t key \t output: "label  (key)" */
                    *q++ = ' '; *q++ = ' '; *q++ = '(';
                    for (p++; p < t2; p++)
                        *q++ = *p;
                    *q++ = ')';
                    *q++ = '\t';
                    continue;
                }
            }
            *q++ = *p;
        }
        *q = 0;
        free(items);
        items = out;
    }
    choice = menu(items);
    free(items);
    if (!choice)
        return;
    if (strcmp(choice, "open") == 0) open_files(n, files);
    else if (strcmp(choice, "openwith") == 0) open_with(n, files);
    else if (strcmp(choice, "copy") == 0) clipboard_write("copy", n, files);
    else if (strcmp(choice, "cut") == 0) clipboard_write("move", n, files);
    else if (strcmp(choice, "paste") == 0) paste();
    else if (strcmp(choice, "rename") == 0) rename_files(n, files);
    else if (strcmp(choice, "delete") == 0) delete_files(n, files);
    else if (strcmp(choice, "newdir") == 0) new_folder();
    else if (strcmp(choice, "term") == 0) terminal_here();
    else if (strcmp(choice, "newwin") == 0) new_window();
    else if (strcmp(choice, "props") == 0) properties(n, files);
    else if (strcmp(choice, "selall") == 0) goto_dir("^a");
    else if (strcmp(choice, "hidden") == 0) goto_dir("^h");
    else if (strcmp(choice, "goto") == 0) address_bar();
    else if (strcmp(choice, "up") == 0) goto_dir("..");
    else if (strcmp(choice, "home") == 0) goto_dir(getenv("HOME") ? getenv("HOME") : "/");
    else if (strcmp(choice, "reload") == 0) goto_dir(".");
    else if (strcmp(choice, "filemgr") == 0) run2("xfiles", getenv("HOME") ? getenv("HOME") : "/", NULL);
    else if (strcmp(choice, "web") == 0) run2("links", "-g", NULL);
    else if (strcmp(choice, "jwmrestart") == 0) run2("jwm", "-restart", NULL);
    else if (strcmp(choice, "logout") == 0 && confirm("End the session?", "Log out")) run2("jwm", "-exit", NULL);
    free(choice);
}

static void drop_ask(int n, char **files)
{
    char *items, *choice;
    if (n == 0)
        return;
    asprintf(&items, "%d item%s dropped here\t:\n\nCopy here\tcopy\nMove here\tmove\nLink here\tlink\n\nCancel\tcancel\n",
             n, n == 1 ? "" : "s");
    choice = menu(items);
    free(items);
    if (choice && (strcmp(choice, "copy") == 0 || strcmp(choice, "move") == 0 || strcmp(choice, "link") == 0))
        transfer(choice, n, files, cwd);
    free(choice);
}

int main(int argc, char **argv)
{
    const char *cmd;
    char **files;
    int n, i;
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fprintf(argc < 2 ? stderr : stdout,
                "usage: xfilesctl menu|drop-ask|drop-copy|drop-move|drop-link|^KEY [FILE...]\n"
                "the control script xfiles calls (its right-click menu, drops and keys); see its head\n");
        return argc < 2 ? 2 : 0;
    }
    setlocale(LC_ALL, "");
    windowid = getenv("WINDOWID");
    if (windowid && !*windowid)
        windowid = NULL;
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "/");
    cmd = argv[1];
    /* the files, as absolute paths (xfiles passes them so; a drop from elsewhere may not) */
    files = calloc(argc, sizeof *files);
    n = 0;
    for (i = 2; i < argc; i++) {
        if (!argv[i][0])
            continue;
        if (argv[i][0] == '/')
            files[n++] = argv[i];
        else
            asprintf(&files[n++], "%s/%s", cwd, argv[i]);
    }
    if (strcmp(cmd, "menu") == 0) context_menu(n, files);
    else if (strcmp(cmd, "drop-ask") == 0) drop_ask(n, files);
    else if (strcmp(cmd, "drop-copy") == 0) transfer("copy", n, files, cwd);
    else if (strcmp(cmd, "drop-move") == 0) transfer("move", n, files, cwd);
    else if (strcmp(cmd, "drop-link") == 0) transfer("link", n, files, cwd);
    else if (cmd[0] == '^') {
        const char *k = cmd + 1;
        if (strcasecmp(k, "l") == 0 || strcasecmp(k, "g") == 0) address_bar();
        else if (strcasecmp(k, "c") == 0) { if (n) clipboard_write("copy", n, files); }
        else if (strcasecmp(k, "x") == 0) { if (n) clipboard_write("move", n, files); }
        else if (strcasecmp(k, "v") == 0) paste();
        else if (strcasecmp(k, "n") == 0) new_folder();
        else if (strcasecmp(k, "t") == 0) terminal_here();
        else if (strcasecmp(k, "o") == 0) { if (n) open_with(n, files); }
        else if (strcasecmp(k, "i") == 0) properties(n, files);
        else if (strcasecmp(k, "w") == 0) new_window();
        else if (strcmp(k, "F2") == 0) rename_files(n, files);
        else if (strcmp(k, "F5") == 0 || strcasecmp(k, "r") == 0) goto_dir(".");
        else if (strcmp(k, "Delete") == 0) delete_files(n, files);
        else if (strcmp(k, "BackSpace") == 0) goto_dir("..");
        /* Ctrl-A, Ctrl-H and the rest: xfiles handles or ignores them */
    } else {
        fprintf(stderr, "xfilesctl: unknown command '%s'\n", cmd);
        return 2;
    }
    return 0;
}
