/* SPDX-License-Identifier: BSD-2-Clause */
/* tld - a small ELF linker (+ nm, objcopy, readelf -r, ar) for the tinycc-os-kernel
 * project.  C99, no libelf.  Internally everything is kept in 64-bit generic form;
 * the ELF class (32/64) only matters when reading and writing files. */
#ifndef TLD_H
#define TLD_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <elf.h>

#ifndef R_X86_64_REX_GOTPCRELX
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42
#endif
#ifndef SHF_COMPRESSED
#define SHF_COMPRESSED (1 << 11)
#endif

extern const char *progname;
void fatal(const char *fmt, ...);
void warning(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...);
uint8_t *read_file(const char *path, size_t *size);
int glob_match(const char *pat, const char *str);
uint32_t elf_hash(const char *name);
uint32_t gnu_hash(const char *name);
void sha1(const uint8_t *data, size_t len, uint8_t out[20]);

/* growable pointer array */
typedef struct { void **p; int n, cap; } PArr;
void parr_add(PArr *a, void *x);
#define PARR_FOREACH(T, v, a) for (int v##_i = 0; v##_i < (a).n && (((v) = (T)(a).p[v##_i]) || 1); v##_i++)

/* little-endian accessors */
static inline uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t rd64(const uint8_t *p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

/* ---------------------------------------------------------------- generic ELF image */
typedef struct GSec GSec;
typedef struct GSym GSym;
typedef struct GRel GRel;
typedef struct GElf GElf;

struct GSec {
    char *name;
    uint32_t type, link, info;
    uint64_t flags, addr, offset, size, addralign, entsize;
    uint8_t *data;          /* NULL for NOBITS; otherwise points into the file image (or owned) */
    int data_owned;
    /* decoded relocations (if this is a REL/RELA section) */
    GRel *rels; int nrels;
};

struct GSym {
    char *name;             /* interned pointer into strtab (or owned) */
    uint64_t value, size;
    unsigned char info, other;
    uint16_t shndx;
};

struct GRel {
    uint64_t offset;
    uint32_t type, sym;
    int64_t addend;         /* for REL: read from the field at load time */
};

struct GElf {
    char *path;             /* for messages */
    int is64;               /* ELF class */
    uint16_t e_type, e_machine;
    uint64_t e_entry;
    uint32_t e_flags;
    uint8_t *image; size_t image_size; int image_owned;
    /* section headers (index 0 = NULL section) */
    GSec *secs; int nsecs;
    int shstrndx;
    /* program headers */
    struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } *phdrs;
    int nphdrs;
    /* symbol table (from the SHT_SYMTAB section), symbols[0] is the null symbol */
    GSym *syms; int nsyms; int symtab_idx; int strtab_idx;
    char *strtab;
};

GElf *gelf_read(const char *path, const uint8_t *image, size_t size, int image_owned);
GElf *gelf_open(const char *path);
void gelf_free(GElf *e);
int gelf_is_rel_section(const GSec *s);
const char *gelf_shstr(GElf *e, uint32_t off);
GSec *gelf_find_section(GElf *e, const char *name);
int gelf_symtab_index(GElf *e);

/* ---------------------------------------------------------------- archives */
typedef struct {
    char *name;         /* member name (for thin archives, the path relative to the archive directory) */
    uint8_t *data;      /* member contents (loaded lazily for thin archives) */
    size_t size;
    size_t header_off;  /* offset of the member header in the archive (for the symbol index) */
    char *path;         /* for thin archives: the resolved path */
} ArMember;

typedef struct {
    char *path;
    int thin;
    ArMember *members; int nmembers;
} Archive;

int is_archive(const uint8_t *image, size_t size);
Archive *archive_read(const char *path, uint8_t *image, size_t size);
int archive_load_member(Archive *ar, ArMember *m);

/* ---------------------------------------------------------------- tools */
int ld_main(int argc, char **argv);
int nm_main(int argc, char **argv);
int objcopy_main(int argc, char **argv);
int readelf_main(int argc, char **argv);
int ar_main(int argc, char **argv);

/* ELF writer helper shared by ld and objcopy */
typedef struct {
    char *name; uint32_t type, link, info; uint64_t flags, addr, offset, size, addralign, entsize;
    uint8_t *data;  /* may be NULL for NOBITS */
} WSec;
typedef struct {
    uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align;
} WPhdr;
typedef struct {
    int is64; uint16_t e_type, e_machine; uint64_t e_entry; uint32_t e_flags;
    WSec *secs; int nsecs;     /* secs[0] is the null section; the writer appends .shstrtab itself */
    WPhdr *phdrs; int nphdrs;
    uint64_t shoff;            /* chosen by the writer */
    uint64_t phoff;            /* usually ehdr size */
} WElf;
void welf_write(WElf *w, const char *path);
size_t welf_ehdr_size(int is64);
size_t welf_phdr_size(int is64);
size_t welf_shdr_size(int is64);
size_t welf_sym_size(int is64);
void welf_put_sym(int is64, uint8_t *p, const GSym *s);
void welf_put_rel(int is64, int rela, uint8_t *p, const GRel *r);

/* string table builder */
typedef struct { char *buf; size_t len, cap; uint32_t *hash; size_t hcap, hn; } StrTab;
uint32_t strtab_add(StrTab *t, const char *s);
void strtab_init(StrTab *t);

#endif
