/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * libelf.c - a small libelf for the kernel's objtool (see libelf.h, README.md).
 *
 * Model: the whole file is read into memory; every section has a list of data blocks, the
 * first one pointing into that image (NULL for SHT_NOBITS and empty sections), the others
 * added by elf_newdata().  elf_update() lays the file out the way elfutils does without
 * ELF_F_LAYOUT: sections in index order after the ELF header, each at its alignment, each
 * data block at its d_align, the section size being the sum, and the section header table
 * after the last section.  Headers are kept in their ELF64 form and converted for ELF32.
 */
#include "gelf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct data_node {
    Elf_Data d;                 /* first: an Elf_Data pointer is a data_node pointer */
    Elf_Scn *scn;
    struct data_node *next;
};

struct Elf_Scn {
    Elf *elf;
    size_t index;
    Elf64_Shdr sh;
    struct data_node *data, *last;
};

struct Elf {
    int fd;
    Elf_Cmd cmd;
    int class;                  /* ELFCLASS32 or ELFCLASS64; 0 until gelf_newehdr() */
    int has_ehdr;
    unsigned int flags;
    Elf64_Ehdr ehdr;
    Elf32_Ehdr ehdr32;          /* elf32_getehdr()'s copy */
    unsigned char *image;
    size_t size;
    Elf_Scn **scn;
    size_t nscn, ascn;
};

static const char *err_msg;

static void seterr(const char *m)
{
    err_msg = m;
}

int elf_errno(void)
{
    int r = err_msg != NULL;
    err_msg = NULL;
    return r;
}

const char *elf_errmsg(int error)
{
    const char *m = err_msg;

    if (error == 0 || error == -1) {
        if (error == -1)
            err_msg = NULL;
        return m ? m : "no error";
    }
    return "unknown error";
}

unsigned int elf_version(unsigned int version)
{
    if (version == EV_NONE)
        return EV_CURRENT;
    if (version != EV_CURRENT) {
        seterr("unknown ELF version");
        return EV_NONE;
    }
    return EV_CURRENT;
}

static int native_data(void)
{
    union { uint16_t h; unsigned char c[2]; } u;

    u.h = 1;
    return u.c[0] ? ELFDATA2LSB : ELFDATA2MSB;
}

static size_t align_up(size_t v, size_t a)
{
    return a > 1 ? (v + a - 1) & ~(a - 1) : v;
}

static int is_pow2(size_t a)
{
    return a && !(a & (a - 1));
}

/* ---- conversions between the ELF32 file forms and the ELF64 forms kept in memory ---- */

static void ehdr_32_to_64(const Elf32_Ehdr *s, Elf64_Ehdr *d)
{
    memcpy(d->e_ident, s->e_ident, EI_NIDENT);
    d->e_type = s->e_type;
    d->e_machine = s->e_machine;
    d->e_version = s->e_version;
    d->e_entry = s->e_entry;
    d->e_phoff = s->e_phoff;
    d->e_shoff = s->e_shoff;
    d->e_flags = s->e_flags;
    d->e_ehsize = s->e_ehsize;
    d->e_phentsize = s->e_phentsize;
    d->e_phnum = s->e_phnum;
    d->e_shentsize = s->e_shentsize;
    d->e_shnum = s->e_shnum;
    d->e_shstrndx = s->e_shstrndx;
}

static void ehdr_64_to_32(const Elf64_Ehdr *s, Elf32_Ehdr *d)
{
    memcpy(d->e_ident, s->e_ident, EI_NIDENT);
    d->e_type = s->e_type;
    d->e_machine = s->e_machine;
    d->e_version = s->e_version;
    d->e_entry = (Elf32_Addr)s->e_entry;
    d->e_phoff = (Elf32_Off)s->e_phoff;
    d->e_shoff = (Elf32_Off)s->e_shoff;
    d->e_flags = s->e_flags;
    d->e_ehsize = s->e_ehsize;
    d->e_phentsize = s->e_phentsize;
    d->e_phnum = s->e_phnum;
    d->e_shentsize = s->e_shentsize;
    d->e_shnum = s->e_shnum;
    d->e_shstrndx = s->e_shstrndx;
}

static void shdr_32_to_64(const Elf32_Shdr *s, Elf64_Shdr *d)
{
    d->sh_name = s->sh_name;
    d->sh_type = s->sh_type;
    d->sh_flags = s->sh_flags;
    d->sh_addr = s->sh_addr;
    d->sh_offset = s->sh_offset;
    d->sh_size = s->sh_size;
    d->sh_link = s->sh_link;
    d->sh_info = s->sh_info;
    d->sh_addralign = s->sh_addralign;
    d->sh_entsize = s->sh_entsize;
}

static void shdr_64_to_32(const Elf64_Shdr *s, Elf32_Shdr *d)
{
    d->sh_name = s->sh_name;
    d->sh_type = s->sh_type;
    d->sh_flags = (Elf32_Word)s->sh_flags;
    d->sh_addr = (Elf32_Addr)s->sh_addr;
    d->sh_offset = (Elf32_Off)s->sh_offset;
    d->sh_size = (Elf32_Word)s->sh_size;
    d->sh_link = s->sh_link;
    d->sh_info = s->sh_info;
    d->sh_addralign = (Elf32_Word)s->sh_addralign;
    d->sh_entsize = (Elf32_Word)s->sh_entsize;
}

static void sym_32_to_64(const Elf32_Sym *s, Elf64_Sym *d)
{
    d->st_name = s->st_name;
    d->st_info = s->st_info;
    d->st_other = s->st_other;
    d->st_shndx = s->st_shndx;
    d->st_value = s->st_value;
    d->st_size = s->st_size;
}

static void sym_64_to_32(const Elf64_Sym *s, Elf32_Sym *d)
{
    d->st_name = s->st_name;
    d->st_value = (Elf32_Addr)s->st_value;
    d->st_size = (Elf32_Word)s->st_size;
    d->st_info = s->st_info;
    d->st_other = s->st_other;
    d->st_shndx = s->st_shndx;
}

/* ---- descriptors ---- */

static int read_file(Elf *elf)
{
    struct stat st;
    size_t got = 0;

    if (fstat(elf->fd, &st) < 0) {
        seterr(strerror(errno));
        return -1;
    }
    elf->size = (size_t)st.st_size;
    elf->image = malloc(elf->size ? elf->size : 1);
    if (!elf->image) {
        seterr("out of memory");
        return -1;
    }
    while (got < elf->size) {
        ssize_t n = pread(elf->fd, elf->image + got, elf->size - got, (off_t)got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            seterr(n < 0 ? strerror(errno) : "short read");
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

static Elf_Scn *add_scn(Elf *elf)
{
    Elf_Scn *s;

    if (elf->nscn == elf->ascn) {
        size_t n = elf->ascn ? elf->ascn * 2 : 16;
        Elf_Scn **p = realloc(elf->scn, n * sizeof *p);
        if (!p) {
            seterr("out of memory");
            return NULL;
        }
        elf->scn = p;
        elf->ascn = n;
    }
    s = calloc(1, sizeof *s);
    if (!s) {
        seterr("out of memory");
        return NULL;
    }
    s->elf = elf;
    s->index = elf->nscn;
    elf->scn[elf->nscn++] = s;
    return s;
}

static Elf_Type type_of_section(Elf64_Word sh_type)
{
    switch (sh_type) {
    case SHT_SYMTAB:
    case SHT_DYNSYM:
        return ELF_T_SYM;
    case SHT_RELA:
        return ELF_T_RELA;
    case SHT_REL:
        return ELF_T_REL;
    case SHT_SYMTAB_SHNDX:
        return ELF_T_WORD;
    default:
        return ELF_T_BYTE;
    }
}

static struct data_node *add_data(Elf_Scn *s)
{
    struct data_node *n = calloc(1, sizeof *n);

    if (!n) {
        seterr("out of memory");
        return NULL;
    }
    n->scn = s;
    n->d.d_type = ELF_T_BYTE;
    n->d.d_version = EV_CURRENT;
    n->d.d_align = 1;
    if (s->last)
        s->last->next = n;
    else
        s->data = n;
    s->last = n;
    return n;
}

static int parse(Elf *elf)
{
    unsigned char *p = elf->image;
    size_t shentsize, shnum, i;
    Elf64_Shdr sh0;

    if (elf->size < EI_NIDENT || memcmp(p, ELFMAG, SELFMAG) != 0) {
        seterr("not an ELF file");
        return -1;
    }
    if (p[EI_CLASS] != ELFCLASS32 && p[EI_CLASS] != ELFCLASS64) {
        seterr("unknown ELF class");
        return -1;
    }
    if (p[EI_DATA] != native_data()) {
        seterr("byte order not supported");
        return -1;
    }
    elf->class = p[EI_CLASS];
    if (elf->class == ELFCLASS64) {
        if (elf->size < sizeof(Elf64_Ehdr)) {
            seterr("truncated ELF header");
            return -1;
        }
        memcpy(&elf->ehdr, p, sizeof(Elf64_Ehdr));
        shentsize = sizeof(Elf64_Shdr);
    } else {
        if (elf->size < sizeof(Elf32_Ehdr)) {
            seterr("truncated ELF header");
            return -1;
        }
        ehdr_32_to_64((const Elf32_Ehdr *)p, &elf->ehdr);
        shentsize = sizeof(Elf32_Shdr);
    }
    elf->has_ehdr = 1;
    if (elf->ehdr.e_shoff == 0)
        return 0;
    if (elf->ehdr.e_shentsize != shentsize || elf->ehdr.e_shoff + shentsize > elf->size) {
        seterr("invalid section header table");
        return -1;
    }
    if (elf->class == ELFCLASS64)
        memcpy(&sh0, p + elf->ehdr.e_shoff, sizeof sh0);
    else
        shdr_32_to_64((const Elf32_Shdr *)(p + elf->ehdr.e_shoff), &sh0);
    shnum = elf->ehdr.e_shnum ? elf->ehdr.e_shnum : (size_t)sh0.sh_size;
    if (shnum * shentsize > elf->size - elf->ehdr.e_shoff) {
        seterr("invalid section header table");
        return -1;
    }
    for (i = 0; i < shnum; i++) {
        Elf_Scn *s = add_scn(elf);
        struct data_node *n;
        unsigned char *h = p + elf->ehdr.e_shoff + i * shentsize;

        if (!s)
            return -1;
        if (elf->class == ELFCLASS64)
            memcpy(&s->sh, h, sizeof s->sh);
        else
            shdr_32_to_64((const Elf32_Shdr *)h, &s->sh);
        n = add_data(s);
        if (!n)
            return -1;
        n->d.d_type = type_of_section(s->sh.sh_type);
        n->d.d_size = (size_t)s->sh.sh_size;
        n->d.d_align = s->sh.sh_addralign ? (size_t)s->sh.sh_addralign : 1;
        if (s->sh.sh_type != SHT_NOBITS && s->sh.sh_size) {
            if (s->sh.sh_offset > elf->size || s->sh.sh_size > elf->size - s->sh.sh_offset) {
                seterr("section data outside the file");
                return -1;
            }
            n->d.d_buf = p + s->sh.sh_offset;
        }
    }
    return 0;
}

Elf *elf_begin(int fd, Elf_Cmd cmd, Elf *ref)
{
    Elf *elf;

    (void)ref;
    elf = calloc(1, sizeof *elf);
    if (!elf) {
        seterr("out of memory");
        return NULL;
    }
    elf->fd = fd;
    elf->cmd = cmd;
    switch (cmd) {
    case ELF_C_READ:
    case ELF_C_READ_MMAP:
    case ELF_C_READ_MMAP_PRIVATE:
    case ELF_C_RDWR:
    case ELF_C_RDWR_MMAP:
        if (read_file(elf) < 0 || parse(elf) < 0) {
            elf_end(elf);
            return NULL;
        }
        return elf;
    case ELF_C_WRITE:
    case ELF_C_WRITE_MMAP:
        return elf;
    default:
        seterr("unsupported command");
        free(elf);
        return NULL;
    }
}

int elf_end(Elf *elf)
{
    size_t i;

    if (!elf)
        return 0;
    for (i = 0; i < elf->nscn; i++) {
        struct data_node *n = elf->scn[i]->data, *next;
        for (; n; n = next) {
            next = n->next;
            free(n);
        }
        free(elf->scn[i]);
    }
    free(elf->scn);
    free(elf->image);
    free(elf);
    return 0;
}

Elf_Kind elf_kind(Elf *elf)
{
    return elf ? ELF_K_ELF : ELF_K_NONE;
}

unsigned int elf_flagelf(Elf *elf, Elf_Cmd cmd, unsigned int flags)
{
    if (!elf)
        return 0;
    if (cmd == ELF_C_SET)
        elf->flags |= flags;
    else if (cmd == ELF_C_CLR)
        elf->flags &= ~flags;
    else
        seterr("unsupported command");
    return elf->flags;
}

/* ---- sections ---- */

Elf_Scn *elf_getscn(Elf *elf, size_t index)
{
    if (!elf || index >= elf->nscn) {
        seterr("invalid section index");
        return NULL;
    }
    return elf->scn[index];
}

Elf_Scn *elf_newscn(Elf *elf)
{
    if (!elf || !elf->has_ehdr) {
        seterr("no ELF header yet");
        return NULL;
    }
    if (elf->nscn == 0 && !add_scn(elf))      /* the null section comes first */
        return NULL;
    return add_scn(elf);
}

Elf_Scn *elf_nextscn(Elf *elf, Elf_Scn *scn)
{
    size_t i;

    if (!elf)
        return NULL;
    i = scn ? scn->index + 1 : 1;
    return i < elf->nscn ? elf->scn[i] : NULL;
}

size_t elf_ndxscn(Elf_Scn *scn)
{
    return scn ? scn->index : SHN_UNDEF;
}

int elf_getshdrnum(Elf *elf, size_t *dst)
{
    if (!elf || !elf->has_ehdr) {
        seterr("no ELF header");
        return -1;
    }
    *dst = elf->nscn;
    return 0;
}

int elf_getshdrstrndx(Elf *elf, size_t *dst)
{
    if (!elf || !elf->has_ehdr) {
        seterr("no ELF header");
        return -1;
    }
    if (elf->ehdr.e_shstrndx == SHN_XINDEX)
        *dst = elf->nscn ? elf->scn[0]->sh.sh_link : 0;
    else
        *dst = elf->ehdr.e_shstrndx;
    return 0;
}

char *elf_strptr(Elf *elf, size_t index, size_t offset)
{
    Elf_Scn *s = elf_getscn(elf, index);
    struct data_node *n;
    size_t cur = 0;

    if (!s)
        return NULL;
    if (s->sh.sh_type != SHT_STRTAB) {
        seterr("not a string table section");
        return NULL;
    }
    for (n = s->data; n; n = n->next) {
        cur = align_up(cur, n->d.d_align);
        if (offset >= cur && offset - cur < n->d.d_size) {
            char *str = (char *)n->d.d_buf + (offset - cur);
            if (n->d.d_buf && memchr(str, 0, n->d.d_size - (offset - cur)))
                return str;
            break;
        }
        cur += n->d.d_size;
    }
    seterr("invalid string table offset");
    return NULL;
}

/* ---- section data ---- */

Elf_Data *elf_getdata(Elf_Scn *scn, Elf_Data *data)
{
    struct data_node *n;

    if (!scn) {
        seterr("invalid section");
        return NULL;
    }
    n = data ? ((struct data_node *)data)->next : scn->data;
    return n ? &n->d : NULL;
}

Elf_Data *elf_newdata(Elf_Scn *scn)
{
    struct data_node *n;

    if (!scn) {
        seterr("invalid section");
        return NULL;
    }
    if (scn->index == 0) {
        seterr("section 0 cannot have data");
        return NULL;
    }
    n = add_data(scn);
    return n ? &n->d : NULL;
}

/* ---- the file header ---- */

int gelf_getclass(Elf *elf)
{
    return elf ? elf->class : ELFCLASSNONE;
}

GElf_Ehdr *gelf_getehdr(Elf *elf, GElf_Ehdr *dst)
{
    if (!elf || !elf->has_ehdr) {
        seterr("no ELF header");
        return NULL;
    }
    *dst = elf->ehdr;
    return dst;
}

Elf64_Ehdr *elf64_getehdr(Elf *elf)
{
    if (!elf || !elf->has_ehdr || elf->class != ELFCLASS64) {
        seterr("no ELF64 header");
        return NULL;
    }
    return &elf->ehdr;
}

Elf32_Ehdr *elf32_getehdr(Elf *elf)
{
    if (!elf || !elf->has_ehdr || elf->class != ELFCLASS32) {
        seterr("no ELF32 header");
        return NULL;
    }
    ehdr_64_to_32(&elf->ehdr, &elf->ehdr32);
    return &elf->ehdr32;
}

unsigned long gelf_newehdr(Elf *elf, int class)
{
    if (!elf) {
        seterr("invalid descriptor");
        return 0;
    }
    if (class != ELFCLASS32 && class != ELFCLASS64) {
        seterr("unknown ELF class");
        return 0;
    }
    if (elf->has_ehdr && elf->class != class) {
        seterr("ELF class cannot change");
        return 0;
    }
    elf->class = class;
    if (!elf->has_ehdr) {
        memset(&elf->ehdr, 0, sizeof elf->ehdr);
        elf->has_ehdr = 1;
        if (elf->nscn == 0 && !add_scn(elf))  /* the null section */
            return 0;
    }
    return (unsigned long)&elf->ehdr;
}

int gelf_update_ehdr(Elf *elf, GElf_Ehdr *src)
{
    if (!elf || !elf->has_ehdr) {
        seterr("no ELF header");
        return 0;
    }
    if (elf->class == ELFCLASS32 &&
        (src->e_entry > 0xffffffffULL || src->e_phoff > 0xffffffffULL || src->e_shoff > 0xffffffffULL)) {
        seterr("value does not fit ELF32");
        return 0;
    }
    elf->ehdr = *src;
    return 1;
}

/* ---- section headers ---- */

GElf_Shdr *gelf_getshdr(Elf_Scn *scn, GElf_Shdr *dst)
{
    if (!scn) {
        seterr("invalid section");
        return NULL;
    }
    *dst = scn->sh;
    return dst;
}

int gelf_update_shdr(Elf_Scn *scn, GElf_Shdr *src)
{
    if (!scn) {
        seterr("invalid section");
        return 0;
    }
    scn->sh = *src;
    return 1;
}

/* ---- table entries (symbols, relocations) ---- */

static size_t fsize(int class, Elf_Type type)
{
    int c64 = class == ELFCLASS64;

    switch (type) {
    case ELF_T_BYTE: return 1;
    case ELF_T_HALF: return 2;
    case ELF_T_WORD:
    case ELF_T_SWORD: return 4;
    case ELF_T_XWORD:
    case ELF_T_SXWORD: return 8;
    case ELF_T_ADDR:
    case ELF_T_OFF: return c64 ? 8 : 4;
    case ELF_T_EHDR: return c64 ? sizeof(Elf64_Ehdr) : sizeof(Elf32_Ehdr);
    case ELF_T_SHDR: return c64 ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr);
    case ELF_T_PHDR: return c64 ? sizeof(Elf64_Phdr) : sizeof(Elf32_Phdr);
    case ELF_T_SYM: return c64 ? sizeof(Elf64_Sym) : sizeof(Elf32_Sym);
    case ELF_T_REL: return c64 ? sizeof(Elf64_Rel) : sizeof(Elf32_Rel);
    case ELF_T_RELA: return c64 ? sizeof(Elf64_Rela) : sizeof(Elf32_Rela);
    case ELF_T_DYN: return c64 ? sizeof(Elf64_Dyn) : sizeof(Elf32_Dyn);
    default: return 0;
    }
}

size_t gelf_fsize(Elf *elf, Elf_Type type, size_t count, unsigned int version)
{
    size_t s;

    if (!elf || version != EV_CURRENT) {
        seterr("invalid descriptor or version");
        return 0;
    }
    s = fsize(elf->class, type);
    if (!s) {
        seterr("unknown data type");
        return 0;
    }
    return s * count;
}

/* the entry ndx of a data block of the given type: its address and the descriptor's class */
static void *entry(Elf_Data *data, Elf_Type type, int ndx, int *class)
{
    struct data_node *n = (struct data_node *)data;
    size_t es;

    if (!data) {
        seterr("invalid data descriptor");
        return NULL;
    }
    if (data->d_type != type) {
        seterr("data block has another type");
        return NULL;
    }
    *class = n->scn->elf->class;
    es = fsize(*class, type);
    if (ndx < 0 || ((size_t)ndx + 1) * es > data->d_size) {
        seterr("index out of range");
        return NULL;
    }
    if (!data->d_buf) {
        seterr("data block has no buffer");
        return NULL;
    }
    return (unsigned char *)data->d_buf + (size_t)ndx * es;
}

GElf_Sym *gelf_getsym(Elf_Data *data, int ndx, GElf_Sym *dst)
{
    int class;
    void *e = entry(data, ELF_T_SYM, ndx, &class);

    if (!e)
        return NULL;
    if (class == ELFCLASS64)
        memcpy(dst, e, sizeof *dst);
    else
        sym_32_to_64((const Elf32_Sym *)e, dst);
    return dst;
}

int gelf_update_sym(Elf_Data *data, int ndx, GElf_Sym *src)
{
    int class;
    void *e = entry(data, ELF_T_SYM, ndx, &class);

    if (!e)
        return 0;
    if (class == ELFCLASS64) {
        memcpy(e, src, sizeof *src);
    } else {
        if (src->st_value > 0xffffffffULL || src->st_size > 0xffffffffULL) {
            seterr("value does not fit ELF32");
            return 0;
        }
        sym_64_to_32(src, (Elf32_Sym *)e);
    }
    return 1;
}

GElf_Sym *gelf_getsymshndx(Elf_Data *symdata, Elf_Data *shndxdata, int ndx, GElf_Sym *dst, Elf32_Word *xshndx)
{
    if (!gelf_getsym(symdata, ndx, dst))
        return NULL;
    if (shndxdata) {
        int class;
        Elf32_Word *w = entry(shndxdata, ELF_T_WORD, ndx, &class);
        if (!w)
            return NULL;
        *xshndx = *w;
    }
    return dst;
}

int gelf_update_symshndx(Elf_Data *symdata, Elf_Data *shndxdata, int ndx, GElf_Sym *src, Elf32_Word xshndx)
{
    if (!gelf_update_sym(symdata, ndx, src))
        return 0;
    if (shndxdata) {
        int class;
        Elf32_Word *w = entry(shndxdata, ELF_T_WORD, ndx, &class);
        if (!w)
            return 0;
        *w = xshndx;
    }
    return 1;
}

GElf_Rel *gelf_getrel(Elf_Data *data, int ndx, GElf_Rel *dst)
{
    int class;
    void *e = entry(data, ELF_T_REL, ndx, &class);

    if (!e)
        return NULL;
    if (class == ELFCLASS64) {
        memcpy(dst, e, sizeof *dst);
    } else {
        const Elf32_Rel *r = e;
        dst->r_offset = r->r_offset;
        dst->r_info = ELF64_R_INFO(ELF32_R_SYM(r->r_info), ELF32_R_TYPE(r->r_info));
    }
    return dst;
}

GElf_Rela *gelf_getrela(Elf_Data *data, int ndx, GElf_Rela *dst)
{
    int class;
    void *e = entry(data, ELF_T_RELA, ndx, &class);

    if (!e)
        return NULL;
    if (class == ELFCLASS64) {
        memcpy(dst, e, sizeof *dst);
    } else {
        const Elf32_Rela *r = e;
        dst->r_offset = r->r_offset;
        dst->r_info = ELF64_R_INFO(ELF32_R_SYM(r->r_info), ELF32_R_TYPE(r->r_info));
        dst->r_addend = r->r_addend;
    }
    return dst;
}

int gelf_update_rel(Elf_Data *data, int ndx, GElf_Rel *src)
{
    int class;
    void *e = entry(data, ELF_T_REL, ndx, &class);

    if (!e)
        return 0;
    if (class == ELFCLASS64) {
        memcpy(e, src, sizeof *src);
    } else {
        Elf32_Rel *r = e;
        r->r_offset = (Elf32_Addr)src->r_offset;
        r->r_info = ELF32_R_INFO(ELF64_R_SYM(src->r_info), ELF64_R_TYPE(src->r_info));
    }
    return 1;
}

int gelf_update_rela(Elf_Data *data, int ndx, GElf_Rela *src)
{
    int class;
    void *e = entry(data, ELF_T_RELA, ndx, &class);

    if (!e)
        return 0;
    if (class == ELFCLASS64) {
        memcpy(e, src, sizeof *src);
    } else {
        Elf32_Rela *r = e;
        r->r_offset = (Elf32_Addr)src->r_offset;
        r->r_info = ELF32_R_INFO(ELF64_R_SYM(src->r_info), ELF64_R_TYPE(src->r_info));
        r->r_addend = (Elf32_Sword)src->r_addend;
    }
    return 1;
}

/* ---- layout and output ---- */

static int write_all(int fd, const unsigned char *p, size_t n)
{
    size_t done = 0;

    if (lseek(fd, 0, SEEK_SET) < 0) {
        seterr(strerror(errno));
        return -1;
    }
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            seterr(w < 0 ? strerror(errno) : "short write");
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

int64_t elf_update(Elf *elf, Elf_Cmd cmd)
{
    size_t ehsize, shentsize, talign, offset, shoff, total, i;
    unsigned char *buf;
    int64_t r;

    if (!elf || !elf->has_ehdr) {
        seterr("no ELF header");
        return -1;
    }
    if (cmd != ELF_C_NULL && cmd != ELF_C_WRITE) {
        seterr("unsupported command");
        return -1;
    }
    if (elf->ehdr.e_phnum) {
        seterr("program headers are not supported");
        return -1;
    }
    if (elf->class == ELFCLASS64) {
        ehsize = sizeof(Elf64_Ehdr);
        shentsize = sizeof(Elf64_Shdr);
        talign = 8;
    } else {
        ehsize = sizeof(Elf32_Ehdr);
        shentsize = sizeof(Elf32_Shdr);
        talign = 4;
    }

    /* 1. the sections: every section header gets its offset, a section with data blocks its
       size and alignment from them (the null section keeps its header) */
    offset = ehsize;
    for (i = 1; i < elf->nscn; i++) {
        Elf_Scn *s = elf->scn[i];
        size_t align = s->sh.sh_addralign ? (size_t)s->sh.sh_addralign : 1;
        struct data_node *n;

        if (!is_pow2(align)) {
            seterr("section alignment is not a power of two");
            return -1;
        }
        if (s->data) {
            size_t size = 0;
            for (n = s->data; n; n = n->next) {
                size_t da = n->d.d_align ? n->d.d_align : 1;
                if (!is_pow2(da)) {
                    seterr("data alignment is not a power of two");
                    return -1;
                }
                if (n->d.d_version != EV_CURRENT) {
                    seterr("unknown data version");
                    return -1;
                }
                if (da > align)
                    align = da;
                size = align_up(size, da);
                n->d.d_off = (int64_t)size;
                size += n->d.d_size;
            }
            s->sh.sh_size = size;
            s->sh.sh_addralign = align;
        }
        offset = align_up(offset, align);
        s->sh.sh_offset = offset;
        if (s->sh.sh_type != SHT_NOBITS)
            offset += (size_t)s->sh.sh_size;
    }

    /* 2. the section header table and the file header */
    shoff = align_up(offset, talign);
    total = elf->nscn ? shoff + elf->nscn * shentsize : offset;
    memcpy(elf->ehdr.e_ident, ELFMAG, SELFMAG);
    elf->ehdr.e_ident[EI_CLASS] = (unsigned char)elf->class;
    elf->ehdr.e_ident[EI_DATA] = (unsigned char)native_data();
    elf->ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    elf->ehdr.e_version = EV_CURRENT;
    elf->ehdr.e_ehsize = (Elf64_Half)ehsize;
    elf->ehdr.e_phentsize = 0;
    elf->ehdr.e_phoff = 0;
    elf->ehdr.e_shentsize = elf->nscn ? (Elf64_Half)shentsize : 0;
    elf->ehdr.e_shoff = elf->nscn ? shoff : 0;
    if (elf->nscn >= SHN_LORESERVE) {
        elf->ehdr.e_shnum = 0;
        elf->scn[0]->sh.sh_size = elf->nscn;
    } else {
        elf->ehdr.e_shnum = (Elf64_Half)elf->nscn;
    }
    if (elf->ehdr.e_shstrndx >= SHN_LORESERVE && elf->ehdr.e_shstrndx != SHN_XINDEX) {
        elf->scn[0]->sh.sh_link = elf->ehdr.e_shstrndx;
        elf->ehdr.e_shstrndx = SHN_XINDEX;
    }
    if (cmd == ELF_C_NULL)
        return (int64_t)total;

    /* 3. the bytes */
    buf = calloc(1, total ? total : 1);
    if (!buf) {
        seterr("out of memory");
        return -1;
    }
    if (elf->class == ELFCLASS64)
        memcpy(buf, &elf->ehdr, sizeof(Elf64_Ehdr));
    else
        ehdr_64_to_32(&elf->ehdr, (Elf32_Ehdr *)buf);
    for (i = 0; i < elf->nscn; i++) {
        Elf_Scn *s = elf->scn[i];
        struct data_node *n;

        if (i && s->sh.sh_type != SHT_NOBITS)
            for (n = s->data; n; n = n->next)
                if (n->d.d_buf && n->d.d_size)
                    memcpy(buf + s->sh.sh_offset + n->d.d_off, n->d.d_buf, n->d.d_size);
        if (elf->class == ELFCLASS64)
            memcpy(buf + shoff + i * shentsize, &s->sh, sizeof(Elf64_Shdr));
        else
            shdr_64_to_32(&s->sh, (Elf32_Shdr *)(buf + shoff + i * shentsize));
    }
    r = write_all(elf->fd, buf, total);
    free(buf);
    if (r < 0)
        return -1;
    if (ftruncate(elf->fd, (off_t)total) < 0) {
        seterr(strerror(errno));
        return -1;
    }
    return (int64_t)total;
}
