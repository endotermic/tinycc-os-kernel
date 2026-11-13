/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * xdm-help - the helper text of the graphical login: a window along the top of the screen
 * next to xdm's greeter, with the proof-of-concept accounts, their passwords and how the
 * system is used.  Xlib + Xft (DejaVu Sans through fontconfig, as the greeter), static, built by
 * scripts/build.sh xdm with musl-tcc; /etc/X11/xdm/Xsetup_0 starts it before the greeter
 * appears, /etc/X11/xdm/Xstartup ends it when a session starts (its pid is in
 * /var/run/xdm/help.pid), and the server reset after a session brings it back with the
 * greeter.  Override-redirect: there is no window manager to decorate or move it.
 * Its background is the band of the frosted wallpaper that "wallpaper --greeter" publishes
 * in _WALLPAPER_FROSTED_PMAP_ID (blurred, tinted, brightness capped so that white text has
 * at least 7:1 contrast), with white text and a thin line along its lower edge, like the
 * greeter's box; without that pixmap, black text on white as before.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>

static const char *lines[] = {
    "tinycc-os-kernel: graphical login.  xdm's greeter on TinyX (Xfbdev), both compiled by tcc,",
    "like the kernel, musl, busybox and everything else on this screen.",
    "",
    "Proof-of-concept accounts (name / password):   ada / lovelace     grace / hopper     dennis / ritchie",
    "Type the name, Enter, the password, Enter.  The session is the JWM window manager with the links",
    "web browser in graphics mode (Escape opens its menu, g asks for an address; http://10.0.2.2:8000/",
    "is the host under QEMU).  JWM's menu is on the desktop, the tray's Menu button and Alt-F1: a terminal",
    "(mlterm, also Ctrl-Alt-T), a file manager (xfiles, also Ctrl-Alt-F: right click for its menu, drag",
    "and drop), the browser, Log out, which ends the session and brings this screen back, Reboot, Shut down.",
    "root has no password and cannot log in here:",
    "the text consoles (tty1, the serial port; Alt-F2..F4 when no X server runs) log in as ada,",
    "and su gives root there.  Nothing is saved across a reboot: the system lives in the initramfs.",
    NULL
};

static int bad;
static int x_error(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    bad = 1;
    return 0;
}

/* the band (0, 0, w, h) of the frosted wallpaper as a pixmap of its own, or None */
static Pixmap frosted_band(Display *dpy, int screen, int w, int h)
{
    Window root = RootWindow(dpy, screen), r;
    Atom prop = XInternAtom(dpy, "_WALLPAPER_FROSTED_PMAP_ID", True), type;
    int format, x, y;
    unsigned int gw, gh, bw, depth;
    unsigned long n, after;
    unsigned char *data = NULL;
    Pixmap src = None, pix = None;
    int (*old)(Display *, XErrorEvent *);
    GC gc;

    if (prop == None)
        return None;
    if (XGetWindowProperty(dpy, root, prop, 0, 1, False, XA_PIXMAP, &type, &format, &n, &after,
                           &data) == Success && type == XA_PIXMAP && format == 32 && n == 1)
        src = *(Pixmap *)data;
    if (data)
        XFree(data);
    if (src == None)
        return None;
    XSync(dpy, False);
    bad = 0;
    old = XSetErrorHandler(x_error);    /* the pixmap belongs to another client */
    if (!XGetGeometry(dpy, src, &r, &x, &y, &gw, &gh, &bw, &depth) ||
        depth != (unsigned int)DefaultDepth(dpy, screen))
        bad = 1;
    if (!bad) {
        pix = XCreatePixmap(dpy, root, w, h, depth);
        gc = XCreateGC(dpy, pix, 0, NULL);
        XCopyArea(dpy, src, pix, gc, 0, 0, w, h, 0, 0);
        XFreeGC(dpy, gc);
        XSync(dpy, False);
    }
    XSetErrorHandler(old);
    if (bad && pix != None) {
        XFreePixmap(dpy, pix);
        pix = None;
    }
    return pix;
}

static int xio_error(Display *d)
{
    (void)d;
    _exit(0);            /* the server went away (reset after a session): that is our end */
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    XftFont *font, *bold;
    XftDraw *draw;
    XftColor fg, linecolor;
    Visual *vis;
    Colormap cmap;
    Window w;
    GC gc;
    XSetWindowAttributes attr;
    int screen, width, height, n, lh, pad = 8;
    unsigned long black, white;
    Pixmap band;
    if (!dpy) {
        fprintf(stderr, "xdm-help: cannot open display %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    XSetIOErrorHandler(xio_error);
    screen = DefaultScreen(dpy);
    black = BlackPixel(dpy, screen);
    white = WhitePixel(dpy, screen);
    vis = DefaultVisual(dpy, screen);
    cmap = DefaultColormap(dpy, screen);
    font = XftFontOpenName(dpy, screen, "DejaVu Sans-9");
    bold = XftFontOpenName(dpy, screen, "DejaVu Sans-9:bold");
    if (!font || !bold) {
        fprintf(stderr, "xdm-help: no DejaVu Sans font (fontconfig: /etc/fonts, /usr/share/fonts)\n");
        return 1;
    }
    for (n = 0; lines[n]; n++)
        ;
    lh = font->ascent + font->descent + 3;
    width = DisplayWidth(dpy, screen);
    height = n * lh + 2 * pad;
    band = frosted_band(dpy, screen, width, height);
    attr.override_redirect = True;
    attr.background_pixel = white;
    attr.border_pixel = black;
    if (band != None) {     /* frosted glass, no border: the line below is drawn */
        attr.background_pixmap = band;
        w = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, width, height, 0, CopyFromParent,
                          InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixmap, &attr);
    } else {
        w = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, width - 2, height, 1, CopyFromParent,
                          InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel | CWBorderPixel, &attr);
    }
    XSelectInput(dpy, w, ExposureMask);
    gc = XCreateGC(dpy, w, 0, NULL);
    draw = XftDrawCreate(dpy, w, vis, cmap);
    XftColorAllocName(dpy, vis, cmap, band != None ? "#ffffff" : "#000000", &fg);
    XftColorAllocName(dpy, vis, cmap, "#8fa3b8", &linecolor);
    XMapRaised(dpy, w);
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose && ev.xexpose.count == 0) {
            int i;
            /* anti-aliased text is blended with what is under it: cleared first, so that
             * a second exposure does not draw the edges twice */
            XClearWindow(dpy, w);
            for (i = 0; i < n; i++) {
                XftFont *f = (i == 0 || i == 3) ? bold : font;  /* the title, the accounts */
                XftDrawStringUtf8(draw, &fg, f, pad, pad + i * lh + f->ascent,
                                  (const FcChar8 *)lines[i], (int)strlen(lines[i]));
            }
            if (band != None)
                XftDrawRect(draw, &linecolor, 0, height - 1, width, 1);
            XFlush(dpy);
        }
    }
    return 0;
}
