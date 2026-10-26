#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * send-file FILE... - copy files out of the virtual machine over the second serial port
 * (a C script, run by tcc -run).
 * run.sh --selfhost connects ttyS1 to a file on the host (QEMU -serial file:...); every file
 * goes out as a header line ">>> send-file NAME SIZE MD5" (on a line of its own: a newline
 * precedes it, since the previous file's last byte is arbitrary) followed by its bytes, and
 * scripts/recv-file.sh on the host cuts the files out of that capture and checks the sums.
 * The bytes go out in chunks, each through its own open and close of the port: closing a
 * serial port waits until its transmit buffer has drained, which ends the transmit
 * interrupt between chunks.  QEMU's 16550 raises the transmit-empty interrupt as soon as a
 * byte is written and traps every port write, so an uninterrupted stream keeps the CPU
 * that serves ttyS1 in one back-to-back interrupt (a new edge is pending at every return)
 * nested in a softirq that cannot finish; that CPU never gets back to idle, never passes
 * a quiescent state, and RCU reports a stall on it after 21 s (docs/PORT-NOTES.md,
 * "Self-hosting").  A drain every 32 KB costs ~0.5 s on a 2 MB file (~25 s).
 * Usage: send-file FILE...     Env: DEV (/dev/ttyS1), CHUNK (bytes per open, 32768)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <sys/stat.h>

/* MD5 (RFC 1321), enough to label the files the way md5sum does */
struct md5 { uint32_t a, b, c, d; uint64_t len; unsigned char buf[64]; unsigned n; };

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const unsigned char md5_r[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(struct md5 *m, const unsigned char *p)
{
    uint32_t w[16], a = m->a, b = m->b, c = m->c, d = m->d, f, t;
    unsigned i, g;
    for (i = 0; i < 16; i++)
        w[i] = p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    for (i = 0; i < 64; i++) {
        if (i < 16)
            f = (b & c) | (~b & d), g = i;
        else if (i < 32)
            f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
        else if (i < 48)
            f = b ^ c ^ d, g = (3 * i + 5) % 16;
        else
            f = c ^ (b | ~d), g = (7 * i) % 16;
        t = d;
        d = c;
        c = b;
        f += a + md5_k[i] + w[g];
        b += (f << md5_r[i]) | (f >> (32 - md5_r[i]));
        a = t;
    }
    m->a += a;
    m->b += b;
    m->c += c;
    m->d += d;
}

static void md5_init(struct md5 *m)
{
    m->a = 0x67452301;
    m->b = 0xefcdab89;
    m->c = 0x98badcfe;
    m->d = 0x10325476;
    m->len = 0;
    m->n = 0;
}

static void md5_update(struct md5 *m, const void *data, size_t len)
{
    const unsigned char *p = data;
    m->len += len;
    while (len > 0) {
        unsigned take = 64 - m->n;
        if (take > len)
            take = len;
        memcpy(m->buf + m->n, p, take);
        m->n += take;
        p += take;
        len -= take;
        if (m->n == 64) {
            md5_block(m, m->buf);
            m->n = 0;
        }
    }
}

static void md5_final(struct md5 *m, char hex[33])
{
    uint64_t bits = m->len * 8;
    unsigned char pad[72] = { 0x80 };
    unsigned padlen = (m->n < 56 ? 56 : 120) - m->n, i;
    uint32_t h[4];
    for (i = 0; i < 8; i++)
        pad[padlen + i] = bits >> (8 * i);
    md5_update(m, pad, padlen + 8);
    h[0] = m->a, h[1] = m->b, h[2] = m->c, h[3] = m->d;
    for (i = 0; i < 16; i++)
        sprintf(hex + 2 * i, "%02x", (h[i / 4] >> (8 * (i % 4))) & 0xff);
}

static int md5_file(const char *path, char hex[33])
{
    static unsigned char buf[65536];
    struct md5 m;
    ssize_t n;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    md5_init(&m);
    while ((n = read(fd, buf, sizeof buf)) > 0)
        md5_update(&m, buf, n);
    close(fd);
    if (n < 0)
        return -1;
    md5_final(&m, hex);
    return 0;
}

/* write all of buf to the port through an open and a close of its own (the drain) */
static int port_write(const char *dev, const void *buf, size_t len)
{
    const char *p = buf;
    int fd = open(dev, O_WRONLY | O_NOCTTY);
    if (fd < 0)
        return -1;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        p += n;
        len -= n;
    }
    return close(fd);
}

/* raw mode: no CR/LF translation, no echo, no flow control; the highest standard speed */
static void port_setup(const char *dev)
{
    struct termios t;
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS | CSIZE);
        t.c_cflag |= CS8 | CLOCAL | CREAD;
        t.c_lflag &= ~(ECHO | ISIG | ICANON | IEXTEN);
        t.c_oflag &= ~(OPOST | ONLCR | OCRNL);
        tcsetattr(fd, TCSANOW, &t);
    }
    close(fd);
}

int main(int argc, char **argv)
{
    const char *dev = getenv("DEV"), *chunk_env = getenv("CHUNK");
    static char buf[1 << 20];
    size_t chunk = chunk_env && atol(chunk_env) > 0 ? (size_t)atol(chunk_env) : 32768;
    struct stat st;
    int i;
    if (!dev)
        dev = "/dev/ttyS1";
    if (chunk > sizeof buf)
        chunk = sizeof buf;
    if (argc < 2) {
        fprintf(stderr, "usage: send-file FILE...\n");
        return 2;
    }
    if (stat(dev, &st) != 0 || !S_ISCHR(st.st_mode)) {
        fprintf(stderr, "send-file: no %s (CONFIG_SERIAL_8250_RUNTIME_UARTS >= 2, and QEMU -serial file:...)\n", dev);
        return 1;
    }
    port_setup(dev);
    for (i = 1; i < argc; i++) {
        const char *f = argv[i], *name = strrchr(f, '/') ? strrchr(f, '/') + 1 : f;
        char sum[33], header[4200];
        long long size, off;
        time_t t0;
        int fd;
        if (stat(f, &st) != 0 || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "send-file: %s: not a file\n", f);
            return 1;
        }
        size = st.st_size;
        if (md5_file(f, sum) < 0) {
            fprintf(stderr, "send-file: %s: %s\n", f, strerror(errno));
            return 1;
        }
        snprintf(header, sizeof header, "\n>>> send-file %s %lld %s\n", name, size, sum);
        if (port_write(dev, header, strlen(header)) < 0) {
            fprintf(stderr, "send-file: %s: write to %s failed\n", f, dev);
            return 1;
        }
        t0 = time(NULL);
        fd = open(f, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "send-file: %s: %s\n", f, strerror(errno));
            return 1;
        }
        for (off = 0; off < size; off += chunk) {
            ssize_t n = pread(fd, buf, chunk, off);
            if (n <= 0 || port_write(dev, buf, n) < 0) {
                fprintf(stderr, "send-file: %s: write to %s failed at %lld\n", f, dev, off);
                return 1;
            }
        }
        close(fd);
        printf("send-file: %s: %lld bytes sent in %ld s (md5 %s)\n", f, size, (long)(time(NULL) - t0), sum);
        fflush(stdout);
    }
    return 0;
}
