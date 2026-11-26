/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * xdm-help - the helper text of the graphical login: a window along the bottom of the screen
 * (the greeter's box sits at a third of the height, above it)
 * next to xdm's greeter, with the proof-of-concept accounts, their passwords and how the
 * system is used.  Xlib + Xft (DejaVu Sans through fontconfig, as the greeter), static, built by
 * scripts/build.sh xdm with musl-tcc; /etc/X11/xdm/Xsetup_0 starts it before the greeter
 * appears, /etc/X11/xdm/Xstartup ends it when a session starts (its pid is in
 * /var/run/xdm/help.pid), and the server reset after a session brings it back with the
 * greeter.  Override-redirect: there is no window manager to decorate or move it.
 * Its background is the band of the frosted wallpaper that "wallpaper --greeter" publishes
 * in _WALLPAPER_FROSTED_PMAP_ID (blurred, tinted, brightness capped so that white text has
 * at least 7:1 contrast), with white text and a thin line along its upper edge, like the
 * greeter's box; without that pixmap, black text on white as before.
 * When the screen changes size (RandR: the X server follows a virtual machine's window) it
 * runs "wallpaper --greeter" again, for the new size, and moves its band to the new bottom
 * edge with a new piece of the frosted wallpaper; the greeter moves its box itself.
 * The text is wrapped to the screen's width (on a large screen it keeps the lines as written
 * below) at the largest size, 10.5 points down to 5.5, at which the band leaves room for the
 * greeter's box above it, so that a smaller screen keeps all of it; the band's top is published in the root window's _XDM_HELP_TOP
 * (CARDINAL), and the greeter keeps its box above it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>

static const char *lines[] = {
    "Autarkeia OS: graphical login.  xdm's greeter on TinyX (Xfbdev), both compiled by tcc,",
    "like the kernel, musl, busybox and everything else on this screen.",
    "",
    "Proof-of-concept accounts (name / password):   ada / lovelace     grace / hopper     dennis / ritchie",
    "Type the name, Enter, the password, Enter (or click into the fields and on Log in); Power restarts or",
    "shuts down the machine.  The session is the JWM window manager with the links",
    "web browser in graphics mode (Escape opens its menu, g asks for an address; http://10.0.2.2:8000/",
    "is the host under QEMU).  JWM's menu is on the desktop, the tray's Menu button and Alt-F1: a terminal",
    "(mlterm, also Ctrl-Alt-T), a file manager (xfiles, also Ctrl-Alt-F: right click for its menu, drag",
    "and drop), the browser, Log out, which ends the session and brings this screen back, Reboot, Shut down.",
    "root has no password and cannot log in, here or on a text console (/etc/securetty):",
    "the text consoles (tty1, the serial port; Alt-F2..F4 when no X server runs) log in as ada,",
    "and su in a user's terminal gives root.  Nothing is saved across a reboot: the system lives in the initramfs.",
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

/* the band (0, y0, w, h) of the frosted wallpaper as a pixmap of its own, or None */
static Pixmap frosted_band(Display *dpy, int screen, int y0, int w, int h)
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
        XCopyArea(dpy, src, pix, gc, 0, y0, w, h, 0, 0);
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
    return 0;
}

/* the room the greeter's box needs above the band (it is about 190 pixels high) */
#define MIN_BOX_SPACE 200

/* the text sizes tried, largest first: the largest at which every line fits the screen's
   width and the band leaves MIN_BOX_SPACE above it; the last one is taken when none does,
   so that the text is always there (the greeter's box then goes to the top) */
static const double sizes[] = { 10.5, 10, 9.5, 9, 8.5, 8, 7.5, 7, 6.5, 6, 5.5 };

static XftFont *font, *bold;
static int lh, pad = 8;

/* the text as it is drawn: the lines above taken as paragraphs (a paragraph ends at an
   empty line or where the weight changes) and wrapped to the band's width */
#define MAX_OUT 96
static char out[MAX_OUT][256];
static int out_bold[MAX_OUT], nout;

static int is_bold(int i)
{
    return i == 0 || i == 3;        /* the title, the accounts */
}

static int text_width(Display *dpy, XftFont *f, const char *t, int len)
{
    XGlyphInfo gi;

    XftTextExtentsUtf8(dpy, f, (const FcChar8 *)t, len, &gi);
    return gi.xOff;
}

static void put_line(const char *t, int len, int b)
{
    if (nout >= MAX_OUT)
        return;
    if (len > 255)
        len = 255;
    memcpy(out[nout], t, len);
    out[nout][len] = 0;
    out_bold[nout++] = b;
}

/* the paragraph para (words separated by spaces; two spaces after a full stop stay) wrapped
   to maxw pixels, greedily */
static void wrap(Display *dpy, XftFont *f, const char *para, int maxw, int b)
{
    const char *start = para, *p = para, *brk = NULL;

    while (*start) {
        p = start;
        brk = NULL;
        for (;;) {
            const char *e = p;
            while (*e && *e != ' ')
                e++;
            if (brk && text_width(dpy, f, start, (int)(e - start)) > maxw)
                break;          /* the word ending at e does not fit: break before it */
            brk = e;
            if (!*e)
                break;
            p = e + 1;
        }
        put_line(start, (int)(brk - start), b);
        start = brk;
        while (*start == ' ')
            start++;
    }
}

/* all of the text, wrapped to maxw with the fonts f and b */
static void wrap_all(Display *dpy, XftFont *f, XftFont *b, int maxw)
{
    char para[2048];
    int i, len = 0, pb = 0;

    nout = 0;
    for (i = 0; ; i++) {
        int end = !lines[i] || !lines[i][0] || (len && is_bold(i) != pb);
        if (end && len) {
            para[len] = 0;
            wrap(dpy, pb ? b : f, para, maxw, pb);
            len = 0;
        }
        if (!lines[i])
            break;
        if (!lines[i][0]) {
            put_line("", 0, 0);
            continue;
        }
        if (len && len < (int)sizeof para - 1)
            para[len++] = ' ';
        pb = is_bold(i);
        len += snprintf(para + len, sizeof para - len, "%s", lines[i]);
        if (len >= (int)sizeof para)
            len = sizeof para - 1;
    }
}

/* the fonts and the wrapped text for the screen's size; returns the band's height.  The
   wrap width is the screen's (less the margins), at most that of the longest line above at
   the largest size, so that a large screen shows the text as it is written */
static int choose_font(Display *dpy, int screen)
{
    int cap = 0;            /* the longest line at sizes[0], measured with this layout's fonts */
    int k, i, maxw, h = 0;
    char name[64];

    for (k = 0; k < (int)(sizeof sizes / sizeof sizes[0]); k++) {
        XftFont *f, *b;
        snprintf(name, sizeof name, "DejaVu Sans-%g", sizes[k]);
        f = XftFontOpenName(dpy, screen, name);
        snprintf(name, sizeof name, "DejaVu Sans-%g:bold", sizes[k]);
        b = XftFontOpenName(dpy, screen, name);
        if (!f || !b) {
            if (f)
                XftFontClose(dpy, f);
            if (b)
                XftFontClose(dpy, b);
            continue;
        }
        if (!cap)           /* the first size that opens: the largest */
            for (i = 0; lines[i]; i++) {
                int w = text_width(dpy, is_bold(i) ? b : f, lines[i], (int)strlen(lines[i]));
                if (w > cap)
                    cap = w;
            }
        maxw = DisplayWidth(dpy, screen) - 2 * pad;
        if (maxw > cap)
            maxw = cap;
        wrap_all(dpy, f, b, maxw);
        if (font)
            XftFontClose(dpy, font);
        if (bold)
            XftFontClose(dpy, bold);
        font = f;
        bold = b;
        lh = f->ascent + f->descent + 3;
        h = nout * lh + 2 * pad;
        if (DisplayHeight(dpy, screen) - h >= MIN_BOX_SPACE)
            break;
    }
    fprintf(stderr, "xdm-help: %dx%d: the text at %g points, %d lines, %d pixels high\n",
            DisplayWidth(dpy, screen), DisplayHeight(dpy, screen),
            sizes[k < (int)(sizeof sizes / sizeof sizes[0]) ? k : k - 1], nout, h);
    return h;
}

/* the top of the band in _XDM_HELP_TOP, for the greeter */
static void publish_top(Display *dpy, int screen, int y0)
{
    long top = y0 > 0 ? y0 : 0;

    XChangeProperty(dpy, RootWindow(dpy, screen), XInternAtom(dpy, "_XDM_HELP_TOP", False),
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&top, 1);
}

/* the wallpaper of the greeter for the screen's current size, waited for (it publishes the
   frosted copy the band and the greeter's box are cut from) */
static void redraw_wallpaper(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        execlp("wallpaper", "wallpaper", "--greeter", (char *)NULL);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    XftDraw *draw;
    XftColor fg, linecolor;
    Visual *vis;
    Colormap cmap;
    Window w;
    XSetWindowAttributes attr;
    int screen, width, height, y0;
    unsigned long black, white;
    Pixmap band;
    Atom frosted;
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
    height = choose_font(dpy, screen);
    if (!font || !bold) {
        fprintf(stderr, "xdm-help: no DejaVu Sans font (fontconfig: /etc/fonts, /usr/share/fonts)\n");
        return 1;
    }
    width = DisplayWidth(dpy, screen);
    y0 = DisplayHeight(dpy, screen) - height;
    band = frosted_band(dpy, screen, y0, width, height);
    attr.override_redirect = True;
    attr.background_pixel = white;
    attr.border_pixel = black;
    if (band != None) {     /* frosted glass, no border: the line below is drawn */
        attr.background_pixmap = band;
        w = XCreateWindow(dpy, RootWindow(dpy, screen), 0, y0, width, height, 0, CopyFromParent,
                          InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixmap, &attr);
    } else {
        w = XCreateWindow(dpy, RootWindow(dpy, screen), 0, y0 - 2, width - 2, height, 1, CopyFromParent,
                          InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel | CWBorderPixel, &attr);
    }
    XSelectInput(dpy, w, ExposureMask);
    /* the screen's size and the frosted wallpaper */
    XSelectInput(dpy, RootWindow(dpy, screen), StructureNotifyMask | PropertyChangeMask);
    frosted = XInternAtom(dpy, "_WALLPAPER_FROSTED_PMAP_ID", False);
    draw = XftDrawCreate(dpy, w, vis, cmap);
    XftColorAllocName(dpy, vis, cmap, band != None ? "#ffffff" : "#000000", &fg);
    XftColorAllocName(dpy, vis, cmap, "#8fa3b8", &linecolor);
    publish_top(dpy, screen, y0);
    XMapRaised(dpy, w);
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == ConfigureNotify && ev.xconfigure.window == RootWindow(dpy, screen) &&
            (ev.xconfigure.width != DisplayWidth(dpy, screen) ||
             ev.xconfigure.height != DisplayHeight(dpy, screen))) {
            /* a new screen size: Xlib's idea of it too (DisplayWidth/Height, which a
               connection otherwise keeps from its start), then the wallpaper for it; the
               band follows when the new frosted copy is published (PropertyNotify) */
            /* the size in millimetres in proportion (the server keeps the resolution;
               Xft takes its DPI from these, and the text would shrink otherwise) */
            Screen *sp = ScreenOfDisplay(dpy, screen);
            static int w0, h0, mw0, mh0;    /* from the start: no rounding adds up */
            if (!w0) {
                w0 = sp->width;
                h0 = sp->height;
                mw0 = sp->mwidth;
                mh0 = sp->mheight;
            }
            if (w0 > 0 && h0 > 0) {
                sp->mwidth = (int)((long)mw0 * ev.xconfigure.width / w0);
                sp->mheight = (int)((long)mh0 * ev.xconfigure.height / h0);
            }
            sp->width = ev.xconfigure.width;
            sp->height = ev.xconfigure.height;
            redraw_wallpaper();
            continue;
        }
        if (ev.type == PropertyNotify && ev.xproperty.atom == frosted &&
            ev.xproperty.state == PropertyNewValue) {
            /* the text size for the new screen, the band along its bottom edge, cut from
               the new frosted wallpaper */
            Pixmap nb;
            height = choose_font(dpy, screen);
            width = DisplayWidth(dpy, screen);
            y0 = DisplayHeight(dpy, screen) - height;
            nb = frosted_band(dpy, screen, y0, width, height);
            if (nb != None && band != None) {
                XFreePixmap(dpy, band);
                band = nb;
                XSetWindowBackgroundPixmap(dpy, w, band);
                XMoveResizeWindow(dpy, w, 0, y0, width, height);
            } else {
                if (nb != None)
                    XFreePixmap(dpy, nb);
                XMoveResizeWindow(dpy, w, 0, y0 - 2, width - 2, height);
            }
            publish_top(dpy, screen, y0);
            XRaiseWindow(dpy, w);
            XClearArea(dpy, w, 0, 0, 0, 0, True);   /* an Expose redraws the text */
            continue;
        }
        if (ev.type == Expose && ev.xexpose.count == 0) {
            int i;
            /* anti-aliased text is blended with what is under it: cleared first, so that
             * a second exposure does not draw the edges twice */
            XClearWindow(dpy, w);
            for (i = 0; i < nout; i++) {
                XftFont *f = out_bold[i] ? bold : font;
                XftDrawStringUtf8(draw, &fg, f, pad, pad + i * lh + f->ascent,
                                  (const FcChar8 *)out[i], (int)strlen(out[i]));
            }
            if (band != None)
                XftDrawRect(draw, &linecolor, 0, 0, width, 1);   /* the line along its upper edge */
            XFlush(dpy);
        }
    }
    return 0;
}
