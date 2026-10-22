/* SPDX-License-Identifier: BSD-2-Clause */
#include "tld.h"
#include <errno.h>

const char *progname = "tld";

void fatal(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void warning(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: warning: ", progname);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) fatal("out of memory"); return p; }
void *xcalloc(size_t n, size_t m) { void *p = calloc(n ? n : 1, m ? m : 1); if (!p) fatal("out of memory"); return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) fatal("out of memory"); return p; }
char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *p = xmalloc(n); memcpy(p, s, n); return p; }
char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }

char *xasprintf(const char *fmt, ...)
{
    va_list ap; int n; char *p;
    va_start(ap, fmt); n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    p = xmalloc(n + 1);
    va_start(ap, fmt); vsnprintf(p, n + 1, fmt, ap); va_end(ap);
    return p;
}

uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    size_t cap = 65536, len = 0, n;
    uint8_t *buf;
    if (!f) fatal("cannot open %s: %s", path, strerror(errno));
    buf = xmalloc(cap);
    while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) { cap *= 2; buf = xrealloc(buf, cap); }
    }
    if (ferror(f)) fatal("cannot read %s: %s", path, strerror(errno));
    fclose(f);
    *size = len;
    return buf;
}

void parr_add(PArr *a, void *x)
{
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->p = xrealloc(a->p, a->cap * sizeof *a->p); }
    a->p[a->n++] = x;
}

/* fnmatch-like glob: *, ?, [...] with ranges and ! / ^ negation. */
int glob_match(const char *pat, const char *str)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            for (; *str; str++)
                if (glob_match(pat, str)) return 1;
            return glob_match(pat, str);
        } else if (*pat == '?') {
            if (!*str) return 0;
            pat++; str++;
        } else if (*pat == '[') {
            const char *p = pat + 1;
            int neg = 0, ok = 0, first = 1;
            if (!*str) return 0;
            if (*p == '!' || *p == '^') { neg = 1; p++; }
            for (; *p && (*p != ']' || first); p++, first = 0) {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    if ((unsigned char)*str >= (unsigned char)p[0] && (unsigned char)*str <= (unsigned char)p[2]) ok = 1;
                    p += 2;
                } else if (*p == *str) ok = 1;
            }
            if (*p != ']') return 0; /* malformed */
            if (ok == neg) return 0;
            pat = p + 1; str++;
        } else {
            if (*pat != *str) return 0;
            pat++; str++;
        }
    }
    return *str == 0;
}

uint32_t elf_hash(const char *name)
{
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

uint32_t gnu_hash(const char *name)
{
    uint32_t h = 5381;
    while (*name) h = h * 33 + (unsigned char)*name++;
    return h;
}

/* SHA-1 (for --build-id=sha1) */
static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t total = ((len + 8) / 64 + 1) * 64, i;
    uint8_t *m = xcalloc(total, 1);
    memcpy(m, data, len);
    m[len] = 0x80;
    for (i = 0; i < 8; i++) m[total - 1 - i] = (uint8_t)(((uint64_t)len * 8) >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80], a, b, c, d, e, f, k, t;
        int j;
        for (j = 0; j < 16; j++) w[j] = ((uint32_t)m[off + 4*j] << 24) | (m[off + 4*j + 1] << 16) | (m[off + 4*j + 2] << 8) | m[off + 4*j + 3];
        for (; j < 80; j++) w[j] = rol(w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16], 1);
        a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
        for (j = 0; j < 80; j++) {
            if (j < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (j < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            t = rol(a, 5) + f + e + k + w[j];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(m);
    for (i = 0; i < 5; i++) { out[4*i] = h[i] >> 24; out[4*i+1] = h[i] >> 16; out[4*i+2] = h[i] >> 8; out[4*i+3] = h[i]; }
}

void strtab_init(StrTab *t) { t->cap = 4096; t->buf = xmalloc(t->cap); t->buf[0] = 0; t->len = 1; t->hash = NULL; t->hcap = 0; t->hn = 0; }
uint32_t strtab_add(StrTab *t, const char *s)
{
    size_t n = strlen(s) + 1, off;
    unsigned h, i;
    if (!t->buf) strtab_init(t);
    if (n == 1) return 0;
    /* dedupe identical strings (GNU ld also tail-merges; we only remove exact duplicates) */
    if (t->hn * 2 >= t->hcap) {
        uint32_t *old = t->hash; size_t oldcap = t->hcap;
        t->hcap = t->hcap ? t->hcap * 2 : 1024;
        t->hash = xcalloc(t->hcap, sizeof *t->hash);
        for (size_t k = 0; k < oldcap; k++) if (old && old[k]) {
            const char *os = t->buf + old[k]; unsigned hh = 5381; for (const char *p = os; *p; p++) hh = hh * 33 + (unsigned char)*p;
            for (i = hh & (t->hcap - 1); t->hash[i]; i = (i + 1) & (t->hcap - 1)) ;
            t->hash[i] = old[k];
        }
        free(old);
    }
    h = 5381; for (const char *p = s; *p; p++) h = h * 33 + (unsigned char)*p;
    for (i = h & (t->hcap - 1); t->hash[i]; i = (i + 1) & (t->hcap - 1))
        if (!strcmp(t->buf + t->hash[i], s)) return t->hash[i];
    while (t->len + n > t->cap) { t->cap *= 2; t->buf = xrealloc(t->buf, t->cap); }
    off = t->len;
    memcpy(t->buf + off, s, n);
    t->len += n;
    t->hash[i] = (uint32_t)off; t->hn++;
    return (uint32_t)off;
}
