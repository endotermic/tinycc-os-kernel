/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * xdm-help - the helper text of the graphical login: a window along the top of the screen
 * next to xdm's greeter, with the proof-of-concept accounts, their passwords and how the
 * system is used.  Xlib only (the server's built-in 'fixed' font), static, built by
 * scripts/build.sh xdm with musl-tcc; /etc/X11/xdm/Xsetup_0 starts it before the greeter
 * appears, /etc/X11/xdm/Xstartup ends it when a session starts (its pid is in
 * /var/run/xdm/help.pid), and the server reset after a session brings it back with the
 * greeter.  Override-redirect: there is no window manager to decorate or move it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static const char *lines[] = {
    "tinycc-os-kernel: graphical login.  xdm's greeter on TinyX (Xfbdev), both compiled by tcc,",
    "like the kernel, musl, busybox and everything else on this screen.",
    "",
    "Proof-of-concept accounts (name / password):   ada / lovelace     grace / hopper     dennis / ritchie",
    "Type the name, Enter, the password, Enter.  The session is the JWM window manager with the links",
    "web browser in graphics mode (Escape opens its menu, g asks for an address; http://10.0.2.2:8000/",
    "is the host under QEMU).  JWM's menu is on the desktop, the tray's Menu button and Alt-F1;",
    "its Log out ends the session and this screen returns.  root has no password and cannot log in here:",
    "the text consoles (tty1, the serial port; Alt-F2..F4 when no X server runs) log in as ada,",
    "and su gives root there.  Nothing is saved across a reboot: the system lives in the initramfs.",
    NULL
};

static int xio_error(Display *d)
{
    (void)d;
    _exit(0);            /* the server went away (reset after a session): that is our end */
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    XFontStruct *font;
    Window w;
    GC gc;
    XSetWindowAttributes attr;
    int screen, width, height, n, lh, pad = 8;
    unsigned long black, white;
    if (!dpy) {
        fprintf(stderr, "xdm-help: cannot open display %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    XSetIOErrorHandler(xio_error);
    screen = DefaultScreen(dpy);
    black = BlackPixel(dpy, screen);
    white = WhitePixel(dpy, screen);
    font = XLoadQueryFont(dpy, "fixed");
    if (!font) {
        fprintf(stderr, "xdm-help: no 'fixed' font\n");
        return 1;
    }
    for (n = 0; lines[n]; n++)
        ;
    lh = font->ascent + font->descent + 2;
    width = DisplayWidth(dpy, screen);
    height = n * lh + 2 * pad;
    attr.override_redirect = True;
    attr.background_pixel = white;
    attr.border_pixel = black;
    w = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, width - 2, height, 1, CopyFromParent,
                      InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel | CWBorderPixel, &attr);
    XSelectInput(dpy, w, ExposureMask);
    gc = XCreateGC(dpy, w, 0, NULL);
    XSetFont(dpy, gc, font->fid);
    XSetForeground(dpy, gc, black);
    XMapRaised(dpy, w);
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose && ev.xexpose.count == 0) {
            int i;
            for (i = 0; i < n; i++)
                XDrawString(dpy, w, gc, pad, pad + i * lh + font->ascent, lines[i], (int)strlen(lines[i]));
            /* the first line in bold: drawn twice, one pixel apart */
            XDrawString(dpy, w, gc, pad + 1, pad + font->ascent, lines[0], (int)strlen(lines[0]));
            XDrawString(dpy, w, gc, pad + 1, pad + 3 * lh + font->ascent, lines[3], (int)strlen(lines[3]));
            XFlush(dpy);
        }
    }
    return 0;
}
