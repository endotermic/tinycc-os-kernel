/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * wallpaper - the desktop background of the graphical mode: draws a JPEG or PNG photograph
 * on the root window so that it fills the whole screen without being stretched or skewed:
 * scaled by one factor in both directions until it covers the screen ("cover"), centred,
 * and what overhangs on two opposite sides is cut off.  The pixmap is the root window's
 * background and is published in the _XROOTPMAP_ID and ESETROOT_PMAP_ID properties (the
 * Esetroot convention), which the desktop (xfiles -d, patches/xfiles) draws its icons over.
 *
 *   wallpaper                   the user's choice (~/.config/wallpaper holds its path) or,
 *                               without one, the system's default
 *   wallpaper --default         the system's default, /usr/share/backgrounds/default.jpg (a
 *                               link to elementary/Pablo Garcia Saldana.jpg)
 *   wallpaper --greeter         the default, and a frosted copy of the screen (blurred,
 *                               tinted, brightness capped: frost()) in the root window's
 *                               _WALLPAPER_FROSTED_PMAP_ID, the background of the login
 *                               screen's panels (/etc/X11/xdm/Xsetup_0)
 *   wallpaper FILE              FILE, for now
 *   wallpaper -s FILE|default   FILE from now on: draws it and records it in ~/.config/wallpaper
 *                               ("default" removes the record)
 *   wallpaper -m                prints a JWM menu of the pictures in /usr/share/backgrounds
 *                               (JWM's "Wallpaper" menu: <Dynamic>exec:wallpaper -m</Dynamic>)
 *   wallpaper --cover WxH IN OUT
 *                               writes IN, scaled down until it just covers WxH, as the JPEG
 *                               OUT (baseline, quality 90); scripts/build.sh shrinks the
 *                               photographs with it for the image (they are 3000 to 6000
 *                               pixels wide)
 *
 * JWM runs it as its desktop background command (<Background type="command">wallpaper,
 * /etc/system.jwmrc).  Static, musl, compiled by tcc with libjpeg, libpng, zlib and Xlib
 * (scripts/build.sh, step wallpapers).
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <jpeglib.h>
#include <png.h>

#ifndef BACKGROUNDS
#define BACKGROUNDS     "/usr/share/backgrounds"
#endif
#define DEFAULT_PICTURE BACKGROUNDS "/default.jpg"
#define CHOICE_FILE     ".config/wallpaper"     /* under $HOME */

static const char *prog = "wallpaper";

/* an RGB picture, 3 bytes per pixel, rows without padding */
struct picture {
    int w, h;
    unsigned char *rgb;
};

static void die(const char *fmt, const char *arg)
{
    fprintf(stderr, "%s: ", prog);
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory (%s)", "malloc");
    return p;
}

/* ---- decoding ------------------------------------------------------------------------ */

struct jerr {
    struct jpeg_error_mgr mgr;
    jmp_buf jump;
    char msg[JMSG_LENGTH_MAX];
};

static void jerr_exit(j_common_ptr cinfo)
{
    struct jerr *e = (struct jerr *)cinfo->err;
    (*cinfo->err->format_message)(cinfo, e->msg);
    longjmp(e->jump, 1);
}

/*
 * The JPEG decoder scales by M/8 (M = 1..8) while it decodes: the smallest M whose result,
 * scaled once more by one factor, still covers cw x ch (0 x 0: no scaling), so a large
 * photograph is never decoded at full size when a quarter of it covers the screen.
 */
static int load_jpeg(const char *path, struct picture *pic, int cw, int ch)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr err;
    FILE *f;
    unsigned char *row;
    int m;

    if (!(f = fopen(path, "rb")))
        return -1;
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jerr_exit;
    pic->rgb = NULL;
    if (setjmp(err.jump)) {
        fprintf(stderr, "%s: %s: %s\n", prog, path, err.msg);
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        free(pic->rgb);
        pic->rgb = NULL;
        return -2;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    cinfo.scale_denom = 8;
    cinfo.scale_num = 8;
    if (cw > 0 && ch > 0) {
        /* the cover factor; the decoder's result is at least M/8 of the picture */
        double sx = (double)cw / cinfo.image_width, sy = (double)ch / cinfo.image_height;
        double s = sx > sy ? sx : sy;
        for (m = 1; m < 8 && m < 8 * s; m++)
            ;
        cinfo.scale_num = m;        /* 8: smaller than the screen, decoded whole, enlarged later */
    }
    jpeg_start_decompress(&cinfo);
    pic->w = cinfo.output_width;
    pic->h = cinfo.output_height;
    pic->rgb = xmalloc((size_t)pic->w * pic->h * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        row = pic->rgb + (size_t)cinfo.output_scanline * pic->w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    return 0;
}

static int load_png(const char *path, struct picture *pic)
{
    png_image img;

    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path))
        return -1;
    img.format = PNG_FORMAT_RGB;    /* alpha composed over black */
    pic->w = img.width;
    pic->h = img.height;
    pic->rgb = xmalloc(PNG_IMAGE_SIZE(img));
    if (!png_image_finish_read(&img, NULL, pic->rgb, 0, NULL)) {
        fprintf(stderr, "%s: %s: %s\n", prog, path, img.message);
        free(pic->rgb);
        pic->rgb = NULL;
        return -2;
    }
    return 0;
}

/* by content, not by name: JPEG starts with FF D8, PNG with its 8-byte signature */
static void load(const char *path, struct picture *pic, int cw, int ch)
{
    unsigned char magic[8] = { 0 };
    FILE *f = fopen(path, "rb");
    int r = -1;

    if (!f)
        die("cannot open %s", path);
    if (fread(magic, 1, sizeof magic, f) < 2)
        magic[0] = 0;
    fclose(f);
    if (magic[0] == 0xFF && magic[1] == 0xD8)
        r = load_jpeg(path, pic, cw, ch);
    else if (!memcmp(magic, "\x89PNG\r\n\x1a\n", 8))
        r = load_png(path, pic);
    else
        die("%s: neither a JPEG nor a PNG picture", path);
    if (r)
        die("%s: cannot decode the picture", path);
}

/* ---- scaling ------------------------------------------------------------------------- */

/*
 * One axis of the resampling: for each of the n output positions, the first source pixel
 * and the weights (sum 1 << 14) of the source pixels that contribute.  Output position i
 * covers the source interval [(i + off) / scale, (i + off + 1) / scale): when the picture
 * shrinks (scale < 1) every source pixel in it counts by the length it overlaps (an area
 * average, no aliasing); when it grows, the two nearest source pixels are interpolated
 * linearly.
 */
struct axis {
    int *first, *count;
    int *weight;        /* count[i] entries per position, maxcount apart */
    int maxcount;
};

#define WBITS 14

static void make_axis(struct axis *a, int n, int srcn, double scale, int off)
{
    int i, j;
    double span = scale < 1.0 ? 1.0 / scale : 1.0;

    a->maxcount = (int)span + 3;
    a->first = xmalloc(sizeof(int) * n);
    a->count = xmalloc(sizeof(int) * n);
    a->weight = xmalloc(sizeof(int) * n * a->maxcount);
    for (i = 0; i < n; i++) {
        double w[64], sum = 0;
        int lo, hi, c, acc;
        int *wt = a->weight + i * a->maxcount;
        if (scale < 1.0) {
            double s0 = (i + off) / scale, s1 = (i + off + 1) / scale;
            lo = (int)s0;
            hi = (int)s1;
            if (hi >= srcn) hi = srcn - 1;
            if (lo > hi) lo = hi;
            for (j = lo; j <= hi && j - lo < 64; j++) {
                double a0 = j < s0 ? s0 : j, a1 = j + 1 > s1 ? s1 : j + 1;
                w[j - lo] = a1 > a0 ? a1 - a0 : 0;
                sum += w[j - lo];
            }
            hi = j - 1;
        } else {
            double c0 = (i + off + 0.5) / scale - 0.5;
            if (c0 < 0) c0 = 0;
            if (c0 > srcn - 1) c0 = srcn - 1;
            lo = (int)c0;
            hi = lo + 1 < srcn ? lo + 1 : lo;
            w[0] = 1.0 - (c0 - lo);
            w[1] = c0 - lo;
            if (hi == lo) w[0] = 1.0;
            sum = 1.0;
        }
        c = hi - lo + 1;
        if (c > a->maxcount) c = a->maxcount;
        if (sum <= 0) { w[0] = 1; sum = 1; c = 1; }
        acc = 0;
        for (j = 0; j < c; j++) {
            wt[j] = (int)(w[j] / sum * (1 << WBITS) + 0.5);
            acc += wt[j];
        }
        wt[0] += (1 << WBITS) - acc;    /* exactly 1 in total */
        a->first[i] = lo;
        a->count[i] = c;
    }
}

static void free_axis(struct axis *a)
{
    free(a->first);
    free(a->count);
    free(a->weight);
}

/*
 * The part [offx, offx + dw) x [offy, offy + dh) of the source scaled by `scale` (the same
 * factor in both directions, so nothing is stretched): horizontally first, into rows of dw
 * pixels (only the source rows the vertical pass reads), then vertically.
 */
static void resample(const struct picture *src, struct picture *dst, int dw, int dh,
                     double scale, int offx, int offy)
{
    struct axis ax, ay;
    int x, y, k, r0, r1;
    unsigned short *tmp;

    make_axis(&ax, dw, src->w, scale, offx);
    make_axis(&ay, dh, src->h, scale, offy);
    r0 = ay.first[0];
    r1 = ay.first[dh - 1] + ay.count[dh - 1];
    tmp = xmalloc(sizeof(unsigned short) * 3 * (size_t)dw * (r1 - r0));
    for (y = r0; y < r1; y++) {
        const unsigned char *s = src->rgb + (size_t)y * src->w * 3;
        unsigned short *t = tmp + (size_t)(y - r0) * dw * 3;
        for (x = 0; x < dw; x++) {
            const int *wt = ax.weight + x * ax.maxcount;
            const unsigned char *p = s + ax.first[x] * 3;
            int r = 0, g = 0, b = 0;
            for (k = 0; k < ax.count[x]; k++, p += 3) {
                r += p[0] * wt[k];
                g += p[1] * wt[k];
                b += p[2] * wt[k];
            }
            /* 8 bits of fraction kept for the second pass */
            t[0] = (r + (1 << (WBITS - 9))) >> (WBITS - 8);
            t[1] = (g + (1 << (WBITS - 9))) >> (WBITS - 8);
            t[2] = (b + (1 << (WBITS - 9))) >> (WBITS - 8);
            t += 3;
        }
    }
    dst->w = dw;
    dst->h = dh;
    dst->rgb = xmalloc((size_t)dw * dh * 3);
    for (y = 0; y < dh; y++) {
        const int *wt = ay.weight + y * ay.maxcount;
        unsigned char *d = dst->rgb + (size_t)y * dw * 3;
        for (x = 0; x < dw * 3; x++) {
            const unsigned short *t = tmp + (size_t)(ay.first[y] - r0) * dw * 3 + x;
            long v = 0;
            for (k = 0; k < ay.count[y]; k++, t += (size_t)dw * 3)
                v += (long)*t * wt[k];
            v = (v + (1L << (WBITS + 7))) >> (WBITS + 8);
            d[x] = v > 255 ? 255 : v;
        }
    }
    free(tmp);
    free_axis(&ax);
    free_axis(&ay);
}

/*
 * Cover: the factor that makes the picture at least w x h in both directions (the larger of
 * the two ratios), and the centred w x h window into the scaled picture.
 */
static void cover(const struct picture *src, int w, int h, struct picture *dst)
{
    double sx = (double)w / src->w, sy = (double)h / src->h;
    double s = sx > sy ? sx : sy;
    int sw = (int)(src->w * s + 0.5), sh = (int)(src->h * s + 0.5);

    if (sw < w) sw = w;
    if (sh < h) sh = h;
    resample(src, dst, w, h, s, (sw - w) / 2, (sh - h) / 2);
}

/* ---- --cover: the build's shrinking ------------------------------------------------- */

static int write_jpeg(const char *path, const struct picture *pic)
{
    struct jpeg_compress_struct cinfo;
    struct jerr err;
    FILE *f;
    unsigned char *row;

    if (!(f = fopen(path, "wb")))
        die("cannot create %s", path);
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jerr_exit;
    if (setjmp(err.jump)) {
        fprintf(stderr, "%s: %s: %s\n", prog, path, err.msg);
        jpeg_destroy_compress(&cinfo);
        fclose(f);
        return -1;
    }
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = pic->w;
    cinfo.image_height = pic->h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    cinfo.optimize_coding = TRUE;
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        row = pic->rgb + (size_t)cinfo.next_scanline * pic->w * 3;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    if (fclose(f))
        die("cannot write %s", path);
    return 0;
}

/* scaled down (never up) until it just covers w x h; the whole picture, nothing cut off */
static int cmd_cover(const char *size, const char *in, const char *out)
{
    struct picture src, dst;
    int w, h, dw, dh;
    double s, sx, sy;

    if (sscanf(size, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
        die("--cover: %s is not a size WIDTHxHEIGHT", size);
    load(in, &src, w, h);
    sx = (double)w / src.w;
    sy = (double)h / src.h;
    s = sx > sy ? sx : sy;
    if (s >= 1.0) {
        dst = src;      /* already small enough: re-encoded as it is */
    } else {
        dw = (int)(src.w * s + 0.5);
        dh = (int)(src.h * s + 0.5);
        if (dw < w) dw = w;
        if (dh < h) dh = h;
        resample(&src, &dst, dw, dh, s, 0, 0);
        free(src.rgb);
    }
    if (write_jpeg(out, &dst))
        return 1;
    printf("%dx%d\n", dst.w, dst.h);
    free(dst.rgb);
    return 0;
}

/* ---- the root window ----------------------------------------------------------------- */

static int shift_of(unsigned long mask)
{
    int s = 0;
    if (!mask)
        return 0;
    while (!(mask & 1)) { mask >>= 1; s++; }
    return s;
}

static int bits_of(unsigned long mask)
{
    int n = 0;
    while (mask) { n += mask & 1; mask >>= 1; }
    return n;
}

/* the screen-sized RGB picture as an XImage of the root window's TrueColor visual */
static XImage *to_ximage(Display *dpy, int scr, const struct picture *pic)
{
    Visual *vis = DefaultVisual(dpy, scr);
    int depth = DefaultDepth(dpy, scr);
    XImage *img;
    int x, y;
    int rs = shift_of(vis->red_mask), gs = shift_of(vis->green_mask), bs = shift_of(vis->blue_mask);
    int rb = bits_of(vis->red_mask), gb = bits_of(vis->green_mask), bb = bits_of(vis->blue_mask);

    if (vis->class != TrueColor && vis->class != DirectColor)
        die("the screen's visual is not TrueColor (%s)", "no colour map support");
    img = XCreateImage(dpy, vis, depth, ZPixmap, 0, NULL, pic->w, pic->h, 32, 0);
    if (!img)
        die("cannot create an XImage (%s)", "XCreateImage");
    img->data = xmalloc((size_t)img->bytes_per_line * pic->h);
    for (y = 0; y < pic->h; y++) {
        const unsigned char *p = pic->rgb + (size_t)y * pic->w * 3;
        if (img->bits_per_pixel == 32 && rb == 8 && gb == 8 && bb == 8 &&
            img->byte_order == LSBFirst) {
            uint32_t *d = (uint32_t *)(img->data + (size_t)y * img->bytes_per_line);
            for (x = 0; x < pic->w; x++, p += 3)
                d[x] = (uint32_t)p[0] << rs | (uint32_t)p[1] << gs | (uint32_t)p[2] << bs;
        } else {
            for (x = 0; x < pic->w; x++, p += 3)
                XPutPixel(img, x, y,
                          (unsigned long)(p[0] >> (8 - rb)) << rs |
                          (unsigned long)(p[1] >> (8 - gb)) << gs |
                          (unsigned long)(p[2] >> (8 - bb)) << bs);
        }
    }
    return img;
}

/* ---- --greeter: the frosted copy ----------------------------------------------------- */

/*
 * The panels of the login screen (xdm's greeter, xdm-help) are "frosted glass": their
 * background is the wallpaper under them, blurred (the photograph's detail would compete
 * with the text), darkened by a tint, and with its brightness capped so that the text keeps
 * a guaranteed contrast whatever the photograph: relative luminance at most FROST_LMAX,
 * which gives white text 7:1 (WCAG AAA: (1 + 0.05) / (0.10 + 0.05)) and the greeter's
 * light red error text over 4.5:1 (AA).  Blur: three box blurs in each direction (close
 * to a Gaussian), radius 1/48 of the screen's smaller side.
 */
#define FROST_LMAX      0.10
#define FROST_TINT_A    0.35                    /* the tint's share */
static const unsigned char frost_tint[3] = { 0x10, 0x18, 0x22 };

static void box_blur_line(unsigned char *p, int n, int stride, int r, unsigned char *tmp)
{
    int i, c, sum[3] = { 0, 0, 0 }, win = 2 * r + 1;

    for (i = 0; i < n; i++)
        memcpy(tmp + 3 * i, p + (size_t)i * stride, 3);
    /* edges extended: the window starts as r + 1 copies of the first pixel */
    for (c = 0; c < 3; c++) {
        sum[c] = tmp[c] * (r + 1);
        for (i = 1; i <= r; i++)
            sum[c] += tmp[3 * (i < n ? i : n - 1) + c];
    }
    for (i = 0; i < n; i++) {
        int in = i + r + 1 < n ? i + r + 1 : n - 1, out = i - r > 0 ? i - r : 0;
        for (c = 0; c < 3; c++) {
            p[(size_t)i * stride + c] = (sum[c] + win / 2) / win;
            sum[c] += tmp[3 * in + c] - tmp[3 * out + c];
        }
    }
}

static double srgb_to_lin(int v)
{
    double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static void frost(struct picture *pic)
{
    int r = (pic->w < pic->h ? pic->w : pic->h) / 48, pass, x, y, i;
    unsigned char *tmp = xmalloc(3 * (size_t)(pic->w > pic->h ? pic->w : pic->h));
    double lin[256];
    unsigned char back[4096];       /* linear (12 bits) -> sRGB */

    if (r < 2)
        r = 2;
    for (pass = 0; pass < 3; pass++) {
        for (y = 0; y < pic->h; y++)
            box_blur_line(pic->rgb + (size_t)y * pic->w * 3, pic->w, 3, r, tmp);
        for (x = 0; x < pic->w; x++)
            box_blur_line(pic->rgb + (size_t)x * 3, pic->h, pic->w * 3, r, tmp);
    }
    free(tmp);
    for (i = 0; i < 256; i++)
        lin[i] = srgb_to_lin(i);
    for (i = 0; i < 4096; i++) {
        double l = i / 4095.0, c = l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1 / 2.4) - 0.055;
        back[i] = (unsigned char)(c * 255 + 0.5);
    }
    for (i = 0; i < pic->w * pic->h; i++) {
        unsigned char *p = pic->rgb + 3 * (size_t)i;
        double l[3], Y;
        int c;
        for (c = 0; c < 3; c++)     /* the tint, mixed in linear light */
            l[c] = lin[p[c]] * (1 - FROST_TINT_A) + lin[frost_tint[c]] * FROST_TINT_A;
        Y = 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2];
        if (Y > FROST_LMAX)
            for (c = 0; c < 3; c++)
                l[c] *= FROST_LMAX / Y;
        for (c = 0; c < 3; c++)
            p[c] = back[(int)(l[c] * 4095 + 0.5)];
    }
}

static Pixmap get_pixmap_prop(Display *dpy, Window root, Atom prop)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    Pixmap pix = None;

    if (XGetWindowProperty(dpy, root, prop, 0, 1, False, XA_PIXMAP, &type, &format, &n, &after,
                           &data) == Success && type == XA_PIXMAP && format == 32 && n == 1)
        pix = *(Pixmap *)data;
    if (data)
        XFree(data);
    return pix;
}

/*
 * frosted: also publish a frosted copy of the screen (frost()) in the root window's
 * _WALLPAPER_FROSTED_PMAP_ID, which xdm's greeter (patches/xdm, xlogin*backgroundProperty)
 * and xdm-help cut their backgrounds from.
 */
static int set_root(const char *path, int frosted)
{
    Display *dpy;
    int scr, sw, sh;
    Window root;
    Pixmap pix, old, fpix = None;
    Atom xroot, esetroot, frost_atom;
    struct picture src, scaled;
    XImage *img;
    GC gc;

    if (!(dpy = XOpenDisplay(NULL)))
        die("cannot open the display %s", getenv("DISPLAY") ? getenv("DISPLAY") : "(DISPLAY is not set)");
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    sw = DisplayWidth(dpy, scr);
    sh = DisplayHeight(dpy, scr);

    load(path, &src, sw, sh);       /* a JPEG: at the smallest size that still covers */
    cover(&src, sw, sh, &scaled);
    free(src.rgb);
    img = to_ximage(dpy, scr, &scaled);

    pix = XCreatePixmap(dpy, root, sw, sh, DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, pix, 0, NULL);
    XPutImage(dpy, pix, gc, img, 0, 0, 0, 0, sw, sh);
    XDestroyImage(img);
    if (frosted) {
        frost(&scaled);
        img = to_ximage(dpy, scr, &scaled);
        fpix = XCreatePixmap(dpy, root, sw, sh, DefaultDepth(dpy, scr));
        XPutImage(dpy, fpix, gc, img, 0, 0, 0, 0, sw, sh);
        XDestroyImage(img);
    }
    XFreeGC(dpy, gc);
    free(scaled.rgb);

    /* the previous wallpaper's pixmap belongs to the client that set it (kept alive with
     * RetainPermanent): when it is ours (the two properties agree), that client goes */
    xroot = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    esetroot = XInternAtom(dpy, "ESETROOT_PMAP_ID", False);
    old = get_pixmap_prop(dpy, root, esetroot);
    if (old != None && old == get_pixmap_prop(dpy, root, xroot))
        XKillClient(dpy, old);

    XSetWindowBackgroundPixmap(dpy, root, pix);
    XClearWindow(dpy, root);
    XChangeProperty(dpy, root, xroot, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pix, 1);
    XChangeProperty(dpy, root, esetroot, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&pix, 1);
    frost_atom = XInternAtom(dpy, "_WALLPAPER_FROSTED_PMAP_ID", False);
    if (fpix != None)
        XChangeProperty(dpy, root, frost_atom, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&fpix, 1);
    else    /* it was cut from the old wallpaper, whose client may just have been killed */
        XDeleteProperty(dpy, root, frost_atom);
    XSetCloseDownMode(dpy, RetainPermanent);
    XCloseDisplay(dpy);
    return 0;
}

/* ---- the user's choice --------------------------------------------------------------- */

static char *choice_path(void)
{
    const char *home = getenv("HOME");
    char *p;

    if (!home || !*home)
        return NULL;
    p = xmalloc(strlen(home) + sizeof "/" CHOICE_FILE);
    sprintf(p, "%s/%s", home, CHOICE_FILE);
    return p;
}

static char *read_choice(void)
{
    char *p = choice_path(), buf[PATH_MAX], *nl;
    FILE *f;

    if (!p || !(f = fopen(p, "r"))) {
        free(p);
        return NULL;
    }
    free(p);
    if (!fgets(buf, sizeof buf, f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    if ((nl = strchr(buf, '\n')))
        *nl = 0;
    if (!*buf || access(buf, R_OK))
        return NULL;
    return strdup(buf);
}

static void save_choice(const char *file)
{
    char *p = choice_path(), *slash;
    FILE *f;

    if (!p)
        die("HOME is not set: cannot record %s", file);
    if (!file) {
        if (unlink(p) && errno != ENOENT)
            die("cannot remove %s", p);
        free(p);
        return;
    }
    slash = strrchr(p, '/');
    *slash = 0;
    mkdir(p, 0755);
    *slash = '/';
    if (!(f = fopen(p, "w")) || fprintf(f, "%s\n", file) < 0 || fclose(f))
        die("cannot write %s", p);
    free(p);
}

/* ---- -m: JWM's menu ------------------------------------------------------------------ */

/* XML text; in_quotes: an attribute (a label, where '_' stands for ' ': file names in the
 * image have no spaces), else the text of a <Program> (inside the shell's '...') */
static void xml(const char *s, int in_quotes)
{
    for (; *s; s++) {
        switch (*s) {
        case '_': putchar(in_quotes ? ' ' : '_'); break;
        case '&': fputs("&amp;", stdout); break;
        case '<': fputs("&lt;", stdout); break;
        case '>': fputs("&gt;", stdout); break;
        case '"': fputs(in_quotes ? "&quot;" : "\"", stdout); break;
        case '\'': fputs("'\\''", stdout); break;  /* inside the shell's '...' */
        default: putchar(*s);
        }
    }
}

static int is_picture(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png"));
}

static int cmp(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* one submenu per directory of /usr/share/backgrounds (its name as the label), each
 * picture an entry labelled with its name without the extension; the current one marked */
static int cmd_menu(void)
{
    DIR *top, *d;
    struct dirent *e, *f;
    char *cur = read_choice(), dir[PATH_MAX], path[PATH_MAX];
    char *dirs[64], *files[256];
    int nd = 0, nf, i, j;
    char resolved_default[PATH_MAX];

    if (!realpath(DEFAULT_PICTURE, resolved_default))
        resolved_default[0] = 0;
    puts("<?xml version=\"1.0\"?>\n<JWM>");
    fputs("  <Program label=\"Default", stdout);
    if (*resolved_default) {
        char *b = strrchr(resolved_default, '/') + 1, *dot = strrchr(b, '.');
        if (dot)
            *dot = 0;
        fputs(" (", stdout);
        xml(b, 1);
        fputs(")", stdout);
    }
    printf("%s\">wallpaper -s default</Program>\n  <Separator/>\n", cur ? "" : " *");
    if ((top = opendir(BACKGROUNDS))) {
        while ((e = readdir(top)) && nd < 64) {
            struct stat st;
            if (e->d_name[0] == '.')
                continue;
            snprintf(dir, sizeof dir, "%s/%s", BACKGROUNDS, e->d_name);
            if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
                dirs[nd++] = strdup(e->d_name);
        }
        closedir(top);
    }
    qsort(dirs, nd, sizeof *dirs, cmp);
    for (i = 0; i < nd; i++) {
        snprintf(dir, sizeof dir, "%s/%s", BACKGROUNDS, dirs[i]);
        if (!(d = opendir(dir)))
            continue;
        nf = 0;
        while ((f = readdir(d)) && nf < 256)
            if (f->d_name[0] != '.' && is_picture(f->d_name))
                files[nf++] = strdup(f->d_name);
        closedir(d);
        qsort(files, nf, sizeof *files, cmp);
        if (nd > 1) {
            fputs("  <Menu label=\"", stdout);
            xml(dirs[i], 1);
            puts("\">");
        }
        for (j = 0; j < nf; j++) {
            char *dot = strrchr(files[j], '.');
            snprintf(path, sizeof path, "%s/%s", dir, files[j]);
            fputs("    <Program label=\"", stdout);
            *dot = 0;
            xml(files[j], 1);
            *dot = '.';
            if (cur && !strcmp(cur, path))
                fputs(" *", stdout);
            fputs("\">wallpaper -s '", stdout);
            xml(path, 0);
            puts("'</Program>");
            free(files[j]);
        }
        if (nd > 1)
            puts("  </Menu>");
        free(dirs[i]);
    }
    puts("</JWM>");
    free(cur);
    return 0;
}

/* ---- main ---------------------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
            "usage: %s                    the recorded choice (~/" CHOICE_FILE ") or the default\n"
            "       %s --default          the default picture (" DEFAULT_PICTURE ")\n"
            "       %s --greeter          the same, and its frosted copy for the login screen\n"
            "       %s FILE               FILE (JPEG or PNG), filling the screen, centred\n"
            "       %s -s FILE|default    the same, and record it as the choice\n"
            "       %s -m                 a JWM menu of the pictures in " BACKGROUNDS "\n"
            "       %s --cover WxH IN OUT shrink IN until it just covers WxH, as the JPEG OUT\n",
            prog, prog, prog, prog, prog, prog, prog);
    exit(2);
}

int main(int argc, char **argv)
{
    char *file;

    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")))
        usage();
    if (argc == 5 && !strcmp(argv[1], "--cover"))
        return cmd_cover(argv[2], argv[3], argv[4]);
    if (argc == 2 && !strcmp(argv[1], "-m"))
        return cmd_menu();
    if (argc == 2 && !strcmp(argv[1], "--default"))
        return set_root(DEFAULT_PICTURE, 0);
    if (argc == 2 && !strcmp(argv[1], "--greeter"))
        return set_root(DEFAULT_PICTURE, 1);
    if (argc == 3 && !strcmp(argv[1], "-s")) {
        if (!strcmp(argv[2], "default")) {
            save_choice(NULL);
            return set_root(DEFAULT_PICTURE, 0);
        }
        if (access(argv[2], R_OK))
            die("cannot read %s", argv[2]);
        file = realpath(argv[2], NULL);
        set_root(file ? file : argv[2], 0);
        save_choice(file ? file : argv[2]);
        return 0;
    }
    if (argc == 2 && argv[1][0] != '-')
        return set_root(argv[1], 0);
    if (argc != 1)
        usage();
    file = read_choice();
    return set_root(file ? file : DEFAULT_PICTURE, 0);
}
