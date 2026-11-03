#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * xfilesthumb - the thumbnailer of the xfiles file manager: xfiles runs it, in the
 * background, as "xfilesthumb FILE THUMB" for every file of the directory it shows, and
 * draws THUMB, a 64x64 PPM image, in place of the file's icon when it was written.  Images
 * are scaled to fit (the long side 64 pixels, the rest the window's background colour; a
 * small image is enlarged by a whole factor, so pixel art stays sharp); a text file gets a
 * page with its first lines in a 3x5 pixel font.  Everything else is skipped (exit status
 * 1: xfiles keeps the icon).  A C script run by tcc, like every script of this system:
 * tcc compiles it at each call and links libpng, libjpeg and zlib from /usr/lib (the
 * #pragma lines below; the libraries the image's programs are built with, installed with
 * their headers by scripts/build.sh for this script).
 *
 * Usage: xfilesthumb FILE THUMB.ppm
 *   PNG            libpng's simplified API: any depth and colour type, alpha composed over
 *                  the background
 *   JPEG           libjpeg, decoded at 1/2, 1/4 or 1/8 size when the picture is large
 *   PPM, PGM, PBM  P1-P6, ASCII or binary, maxval up to 65535 (own reader)
 *   text           the first 10 lines, 15 columns, in a 3x5 font on a light page; a file
 *                  is text when its first 4 KB have no NUL byte and few control characters
 * Env: XFILES_THUMB_BG (the background colour, "#rrggbb", default #1e1e1e: XFiles.background
 *   in /etc/X11/Xresources).
 */
#pragma comment(lib, "png16")
#pragma comment(lib, "jpeg")
#pragma comment(lib, "z")
#pragma comment(lib, "m")
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <png.h>
#include <jpeglib.h>

#define SIZE 64

static unsigned char bg[3] = { 0x1e, 0x1e, 0x1e };

/* ---- the image buffer ---------------------------------------------------------------- */

struct image {
    int w, h;
    unsigned char *rgb;         /* w*h*3 */
};

static struct image *image_new(int w, int h)
{
    struct image *im;
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768)
        return NULL;
    im = malloc(sizeof *im);
    im->w = w;
    im->h = h;
    im->rgb = malloc((size_t)w * h * 3);
    if (!im->rgb) {
        free(im);
        return NULL;
    }
    return im;
}

/* scale IM to fit SIZE x SIZE on the background: a box filter when shrinking (every
 * source pixel counts), a whole factor when enlarging */
static struct image *fit(struct image *im)
{
    struct image *out = image_new(SIZE, SIZE);
    int tw, th, ox, oy, x, y, c;
    if (!out)
        return NULL;
    for (y = 0; y < SIZE; y++)
        for (x = 0; x < SIZE; x++)
            memcpy(out->rgb + (y * SIZE + x) * 3, bg, 3);
    if (im->w <= SIZE && im->h <= SIZE) {
        int f = SIZE / (im->w > im->h ? im->w : im->h);
        if (f < 1)
            f = 1;
        tw = im->w * f;
        th = im->h * f;
        ox = (SIZE - tw) / 2;
        oy = (SIZE - th) / 2;
        for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++)
                memcpy(out->rgb + ((oy + y) * SIZE + ox + x) * 3, im->rgb + ((y / f) * im->w + x / f) * 3, 3);
        return out;
    }
    if (im->w >= im->h) {
        tw = SIZE;
        th = (im->h * SIZE + im->w / 2) / im->w;
    } else {
        th = SIZE;
        tw = (im->w * SIZE + im->h / 2) / im->h;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    ox = (SIZE - tw) / 2;
    oy = (SIZE - th) / 2;
    for (y = 0; y < th; y++) {
        int sy0 = y * im->h / th, sy1 = (y + 1) * im->h / th;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (x = 0; x < tw; x++) {
            int sx0 = x * im->w / tw, sx1 = (x + 1) * im->w / tw, sx, sy;
            unsigned long sum[3] = { 0, 0, 0 }, cnt = 0;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (sy = sy0; sy < sy1; sy++)
                for (sx = sx0; sx < sx1; sx++) {
                    const unsigned char *p = im->rgb + (sy * im->w + sx) * 3;
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                    cnt++;
                }
            for (c = 0; c < 3; c++)
                out->rgb[((oy + y) * SIZE + ox + x) * 3 + c] = (unsigned char)((sum[c] + cnt / 2) / cnt);
        }
    }
    return out;
}

/* ---- PNG (libpng's simplified API) ----------------------------------------------------- */

static struct image *read_png(const char *path)
{
    png_image pi;
    png_color back = { bg[0], bg[1], bg[2] };
    struct image *im;
    memset(&pi, 0, sizeof pi);
    pi.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&pi, path))
        return NULL;
    pi.format = PNG_FORMAT_RGB;
    im = image_new(pi.width, pi.height);
    if (!im) {
        png_image_free(&pi);
        return NULL;
    }
    if (!png_image_finish_read(&pi, &back, im->rgb, 0, NULL)) {
        fprintf(stderr, "xfilesthumb: %s: %s\n", path, pi.message);
        png_image_free(&pi);
        free(im->rgb);
        free(im);
        return NULL;
    }
    return im;
}

/* ---- JPEG (libjpeg) ------------------------------------------------------------------- */

struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jmp;
};

static void jpeg_fail(j_common_ptr cinfo)
{
    struct jerr *e = (struct jerr *)cinfo->err;
    char msg[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, msg);
    fprintf(stderr, "xfilesthumb: jpeg: %s\n", msg);
    longjmp(e->jmp, 1);
}

static struct image *read_jpeg(const char *path)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr jerr;
    struct image *im = NULL;
    FILE *f = fopen(path, "rb");
    unsigned char *row;
    int denom;
    if (!f)
        return NULL;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_fail;
    if (setjmp(jerr.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        if (im) {
            free(im->rgb);
            free(im);
        }
        return NULL;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    /* decode small: the largest 1/2^n that keeps the short side at least 64 pixels */
    for (denom = 8; denom > 1; denom /= 2)
        if ((int)cinfo.image_width / denom >= SIZE && (int)cinfo.image_height / denom >= SIZE)
            break;
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.out_color_space = JCS_RGB;
    cinfo.dct_method = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&cinfo);
    im = image_new(cinfo.output_width, cinfo.output_height);
    if (!im)
        longjmp(jerr.jmp, 1);
    while (cinfo.output_scanline < cinfo.output_height) {
        row = im->rgb + (size_t)cinfo.output_scanline * im->w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    return im;
}

/* ---- PNM (P1-P6) ---------------------------------------------------------------------- */

static int pnm_int(FILE *f)
{
    int c, v = 0, any = 0;
    for (;;) {
        c = getc(f);
        if (c == '#') {
            while ((c = getc(f)) != EOF && c != '\n')
                ;
            continue;
        }
        if (c == EOF)
            return -1;
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            any = 1;
            while ((c = getc(f)) >= '0' && c <= '9')
                v = v * 10 + (c - '0');
            if (c != EOF)
                ungetc(c, f);
            return v;
        }
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            return any ? v : -1;
    }
}

static struct image *read_pnm(const char *path)
{
    FILE *f = fopen(path, "rb");
    struct image *im = NULL;
    int type, w, h, maxval = 1, x, y, c, n;
    if (!f)
        return NULL;
    if (getc(f) != 'P')
        goto fail;
    type = getc(f) - '0';
    if (type < 1 || type > 6)
        goto fail;
    w = pnm_int(f);
    h = pnm_int(f);
    if (w <= 0 || h <= 0)
        goto fail;
    if (type != 1 && type != 4) {
        maxval = pnm_int(f);
        if (maxval <= 0 || maxval > 65535)
            goto fail;
    }
    if (type >= 4)
        getc(f);                /* the single whitespace before the binary data */
    im = image_new(w, h);
    if (!im)
        goto fail;
    for (y = 0; y < h; y++) {
        int bits = 0, byte = 0;
        for (x = 0; x < w; x++) {
            unsigned char *p = im->rgb + (y * w + x) * 3;
            int v[3];
            switch (type) {
            case 1:             /* ASCII bitmap: 1 is black */
                do c = getc(f); while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
                if (c == EOF) goto fail;
                v[0] = v[1] = v[2] = c == '1' ? 0 : 255;
                break;
            case 4:
                if (bits == 0) {
                    byte = getc(f);
                    if (byte == EOF) goto fail;
                    bits = 8;
                }
                v[0] = v[1] = v[2] = (byte & 0x80) ? 0 : 255;
                byte <<= 1;
                bits--;
                break;
            case 2: case 3:
                n = type == 2 ? 1 : 3;
                for (c = 0; c < n; c++) {
                    v[c] = pnm_int(f);
                    if (v[c] < 0) goto fail;
                    v[c] = v[c] * 255 / maxval;
                }
                if (n == 1) v[1] = v[2] = v[0];
                break;
            default:            /* 5, 6 */
                n = type == 5 ? 1 : 3;
                for (c = 0; c < n; c++) {
                    int b1 = getc(f), b2 = maxval > 255 ? getc(f) : 0;
                    if (b1 == EOF || b2 == EOF) goto fail;
                    v[c] = maxval > 255 ? (b1 << 8 | b2) * 255 / maxval : b1 * 255 / maxval;
                }
                if (n == 1) v[1] = v[2] = v[0];
                break;
            }
            p[0] = v[0]; p[1] = v[1]; p[2] = v[2];
        }
    }
    fclose(f);
    return im;
fail:
    fclose(f);
    if (im) {
        free(im->rgb);
        free(im);
    }
    return NULL;
}

/* ---- text: a page with the first lines in a 3x5 font ----------------------------------- */

/* the glyphs, 3 columns by 5 rows, '#' a pixel; lower-case letters use the capitals */
static const struct { char c; const char *rows; } font[] = {
    { 'A', ".#.#.#####.##.#" }, { 'B', "##.#.###.#.###." }, { 'C', ".###..#..#...##" },
    { 'D', "##.#.##.##.###." }, { 'E', "####..##.#..###" }, { 'F', "####..##.#..#.." },
    { 'G', ".###..#.##.#.##" }, { 'H', "#.##.#####.##.#" }, { 'I', "###.#..#..#.###" },
    { 'J', "..#..#..##.#.#." }, { 'K', "#.##.###.#.##.#" }, { 'L', "#..#..#..#..###" },
    { 'M', "#.########.##.#" }, { 'N', "##.#.##.##.##.#" }, { 'O', ".#.#.##.##.#.#." },
    { 'P', "##.#.###.#..#.." }, { 'Q', ".#.#.##.#.#...#" }, { 'R', "##.#.###.#.##.#" },
    { 'S', ".###...#...###." }, { 'T', "###.#..#..#..#." }, { 'U', "#.##.##.##.####" },
    { 'V', "#.##.##.##.#.#." }, { 'W', "#.##.########.#" }, { 'X', "#.##.#.#.#.##.#" },
    { 'Y', "#.##.#.#..#..#." }, { 'Z', "###..#.#.#..###" }, { '0', "####.##.##.####" },
    { '1', ".#.##..#..#.###" }, { '2', "##...#.#.#..###" }, { '3', "###..#.##..####" },
    { '4', "#.##.####..#..#" }, { '5', "####..##...###." }, { '6', ".###..####.####" },
    { '7', "###..#.#..#..#." }, { '8', "####.#####.####" }, { '9', "####.####..###." },
    { '.', ".............#." }, { ',', "..........#.#.." }, { ':', "....#.....#...." },
    { ';', "....#.....#.#.." }, { '!', ".#..#..#.....#." }, { '?', "##...#.#.....#." },
    { '\'', ".#..#.........." }, { '"', "#.##.#........." }, { '(', ".#.#..#..#...#." },
    { ')', ".#...#..#..#.#." }, { '[', "##.#..#..#..##." }, { ']', ".##..#..#..#.##" },
    { '{', ".##.#.#...#..##" }, { '}', "##..#...#.#.##." }, { '<', "..#.#.#...#...#" },
    { '>', "#...#...#.#.#.." }, { '/', "..#..#.#.#..#.." }, { '\\', "#..#...#...#..#" },
    { '|', ".#..#..#..#..#." }, { '-', "......###......" }, { '_', "............###" },
    { '=', "...###...###..." }, { '+', "....#.###.#...." }, { '*', "#.#.#.###.#.#.#" },
    { '#', "#.#####.#####.#" }, { '%', "#.#..#.#.#..#.#" }, { '&', ".#.#.#.#.#.#.##" },
    { '@', ".#.#.#####...##" }, { '$', ".####..#..####." }, { '^', ".#.#.#........." },
    { '~', "....####......." }, { '`', "#...#.........." },
};

static const char *glyph(int c)
{
    size_t i;
    if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    for (i = 0; i < sizeof font / sizeof font[0]; i++)
        if (font[i].c == c)
            return font[i].rows;
    return c == ' ' ? NULL : "###############";     /* a block for what the font lacks */
}

static int is_text(const unsigned char *b, size_t n)
{
    size_t i, bad = 0;
    for (i = 0; i < n; i++) {
        if (b[i] == 0)
            return 0;
        if (b[i] < 0x20 && b[i] != '\n' && b[i] != '\r' && b[i] != '\t' && b[i] != '\f' && b[i] != 0x1b)
            bad++;
    }
    return n > 0 && bad * 20 < n;
}

static struct image *render_text(const unsigned char *b, size_t n)
{
    struct image *im = image_new(SIZE, SIZE);
    static const unsigned char page[3] = { 0xf8, 0xf8, 0xf8 }, edge[3] = { 0xa0, 0xa0, 0xa0 }, ink[3] = { 0x30, 0x30, 0x30 };
    int x, y, col = 0, row = 0;
    size_t i;
    const int cols = 15, rows = 10, left = 2, top = 2;
    if (!im)
        return NULL;
    for (y = 0; y < SIZE; y++)
        for (x = 0; x < SIZE; x++)
            memcpy(im->rgb + (y * SIZE + x) * 3,
                   (x == 0 || y == 0 || x == SIZE - 1 || y == SIZE - 1) ? edge : page, 3);
    for (i = 0; i < n && row < rows; i++) {
        int c = b[i], k;
        const char *g;
        if (c == '\r' || c == '\f')
            continue;
        if (c == '\n') {
            row++;
            col = 0;
            continue;
        }
        if (c == '\t') {
            col = (col / 4 + 1) * 4;
            continue;
        }
        if (c >= 0x80) {        /* one glyph per UTF-8 sequence */
            if ((c & 0xc0) == 0x80)
                continue;
            c = 0x80;
        }
        if (col >= cols) {      /* the rest of the line is cut, like a page */
            col++;
            continue;
        }
        g = glyph(c);
        if (g)
            for (k = 0; k < 15; k++)
                if (g[k] == '#') {
                    x = left + col * 4 + k % 3;
                    y = top + row * 6 + k / 3;
                    memcpy(im->rgb + (y * SIZE + x) * 3, ink, 3);
                }
        col++;
    }
    return im;
}

/* ---- main ------------------------------------------------------------------------------ */

static int write_ppm(const char *path, struct image *im)
{
    char *tmp, *slash;
    FILE *f;
    if (asprintf(&tmp, "%s.%d.tmp", path, (int)getpid()) < 0)
        return -1;
    slash = strrchr(tmp, '/');
    if (slash) {            /* the thumbnail directory may be missing */
        *slash = 0;
        mkdir(tmp, 0700);
        *slash = '/';
    }
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "xfilesthumb: %s: %s\n", tmp, strerror(errno));
        free(tmp);
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    fwrite(im->rgb, 3, (size_t)im->w * im->h, f);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char head[4096];
    ssize_t n;
    FILE *f;
    struct image *im = NULL, *out;
    const char *e;
    if (argc != 3 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fprintf(argc != 3 ? stderr : stdout, "usage: xfilesthumb FILE THUMB.ppm\n"
                "writes a 64x64 PPM thumbnail of a PNG, JPEG, PNM or text file (status 1: not one of those)\n");
        return argc != 3 ? 2 : 0;
    }
    e = getenv("XFILES_THUMB_BG");
    if (e && e[0] == '#' && strlen(e) == 7) {
        unsigned r, g, b;
        if (sscanf(e + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
            bg[0] = r; bg[1] = g; bg[2] = b;
        }
    }
    f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    n = fread(head, 1, sizeof head, f);
    fclose(f);
    if (n <= 0)
        return 1;
    if (n >= 8 && memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0)
        im = read_png(argv[1]);
    else if (n >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff)
        im = read_jpeg(argv[1]);
    else if (n >= 3 && head[0] == 'P' && head[1] >= '1' && head[1] <= '6'
             && (head[2] == ' ' || head[2] == '\n' || head[2] == '\r' || head[2] == '\t' || head[2] == '#'))
        im = read_pnm(argv[1]);
    else if (is_text(head, n)) {
        out = render_text(head, n);
        return out && write_ppm(argv[2], out) == 0 ? 0 : 1;
    }
    if (!im)
        return 1;
    out = fit(im);
    if (!out)
        return 1;
    return write_ppm(argv[2], out) == 0 ? 0 : 1;
}
