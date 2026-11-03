#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * /etc/X11/xdm/Xsession - the session that xdm runs after a login, as the user, with
 * DISPLAY, HOME, USER, PATH and XAUTHORITY set by xdm: the JWM window manager with the
 * links web browser in graphics mode started under it (as userland/startx.c runs them;
 * the mlterm terminal from JWM's menu, Ctrl-Alt-T, or links' "run in terminal"; the
 * xfiles file manager from the menu or Ctrl-Alt-F), after the X resources of
 * /etc/X11/Xresources and ~/.Xresources are loaded with xrdb.
 * When the window manager ends (Log out in its menu) the session ends, xdm resets the
 * server (which closes the clients left) and shows the greeter again.  In an image
 * without /usr/bin/jwm links alone is the session, its window the size of the screen, and
 * the session ends with it.  A C script, like every script of this system; xdm execs it
 * directly (the kernel's #! line).  xdm passes "failsafe" when F1 was held: the same
 * session here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

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

int main(void)
{
    char mode[32];
    FILE *f;
    int w = 0, h = 0;
    if (getenv("HOME"))
        chdir(getenv("HOME"));
    /* the terminal emulator (JWM's menu, links' "run in terminal" through LINKS_XTERM),
     * the X resources and the rest of the environment */
    session_env();
    if (access("/usr/bin/jwm", X_OK) == 0) {
        fflush(NULL);
        if (fork() == 0) {
            execlp("links", "links", "-g", (char *)NULL);
            perror("Xsession: links");
            _exit(127);
        }
        execl("/usr/bin/jwm", "jwm", (char *)NULL);
        perror("Xsession: jwm");
        return 1;
    }
    f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    if (f) {
        if (fscanf(f, "%d,%d", &w, &h) == 2 && w > 0 && h > 0)
            snprintf(mode, sizeof mode, "%dx%d", w, h);
        else
            w = 0;
        fclose(f);
    }
    if (w > 0)
        execlp("links", "links", "-g", "-mode", mode, (char *)NULL);
    execlp("links", "links", "-g", (char *)NULL);
    perror("Xsession: links");
    return 1;
}
