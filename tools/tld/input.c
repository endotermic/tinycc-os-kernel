/* SPDX-License-Identifier: BSD-2-Clause */
/* ELF reading (both classes into the generic form), archives, and the ELF writer. */
#include "tld.h"
#include <errno.h>

const char *gelf_shstr(GElf *e, uint32_t off)
{
    GSec *s;
    if (e->shstrndx <= 0 || e->shstrndx >= e->nsecs) return "";
    s = &e->secs[e->shstrndx];
    if (!s->data || off >= s->size) return "";
    return (const char *)s->data + off;
}

int gelf_is_rel_section(const GSec *s) { return s->type == SHT_REL || s->type == SHT_RELA; }

GSec *gelf_find_section(GElf *e, const char *name)
{
    for (int i = 1; i < e->nsecs; i++)
        if (!strcmp(e->secs[i].name, name)) return &e->secs[i];
    return NULL;
}

int gelf_symtab_index(GElf *e) { return e->symtab_idx; }

static void decode_rels(GElf *e, GSec *s)
{
    int rela = s->type == SHT_RELA;
    size_t ent = e->is64 ? (rela ? 24 : 16) : (rela ? 12 : 8);
    size_t n = s->size / ent, i;
    GSec *target = (s->info < (uint32_t)e->nsecs) ? &e->secs[s->info] : NULL;
    if (!s->data) return;
    s->rels = xcalloc(n, sizeof *s->rels);
    s->nrels = (int)n;
    for (i = 0; i < n; i++) {
        const uint8_t *p = s->data + i * ent;
        GRel *r = &s->rels[i];
        if (e->is64) {
            uint64_t info = rd64(p + 8);
            r->offset = rd64(p);
            r->sym = (uint32_t)(info >> 32);
            r->type = (uint32_t)info;
            r->addend = rela ? (int64_t)rd64(p + 16) : 0;
        } else {
            uint32_t info = rd32(p + 4);
            r->offset = rd32(p);
            r->sym = info >> 8;
            r->type = info & 0xff;
            r->addend = rela ? (int32_t)rd32(p + 8) : 0;
        }
        if (!rela && target && target->data && r->offset < target->size) {
            /* REL: the addend is in the relocated field; its size depends on the type */
            const uint8_t *f = target->data + r->offset;
            int sz = 4;
            if (e->e_machine == EM_386) {
                switch (r->type) {
                case R_386_8: case R_386_PC8: sz = 1; break;
                case R_386_16: case R_386_PC16: sz = 2; break;
                default: sz = 4;
                }
            }
            if (r->offset + sz <= target->size)
                r->addend = sz == 1 ? (int8_t)f[0] : sz == 2 ? (int16_t)rd16(f) : (int32_t)rd32(f);
        }
    }
}

GElf *gelf_read(const char *path, const uint8_t *image, size_t size, int image_owned)
{
    GElf *e;
    int is64, i;
    uint64_t shoff, phoff;
    uint16_t shentsize, shnum, phentsize, phnum, shstrndx;

    if (size < EI_NIDENT || memcmp(image, ELFMAG, SELFMAG))
        fatal("%s: not an ELF file", path);
    if (image[EI_DATA] != ELFDATA2LSB) fatal("%s: not little-endian", path);
    is64 = image[EI_CLASS] == ELFCLASS64;
    if (!is64 && image[EI_CLASS] != ELFCLASS32) fatal("%s: bad ELF class", path);
    if (size < (is64 ? sizeof(Elf64_Ehdr) : sizeof(Elf32_Ehdr))) fatal("%s: truncated", path);

    e = xcalloc(1, sizeof *e);
    e->path = xstrdup(path);
    e->is64 = is64;
    e->image = (uint8_t *)image; e->image_size = size; e->image_owned = image_owned;
    if (is64) {
        e->e_type = rd16(image + 16); e->e_machine = rd16(image + 18);
        e->e_entry = rd64(image + 24); phoff = rd64(image + 32); shoff = rd64(image + 40);
        e->e_flags = rd32(image + 48);
        phentsize = rd16(image + 54); phnum = rd16(image + 56);
        shentsize = rd16(image + 58); shnum = rd16(image + 60); shstrndx = rd16(image + 62);
    } else {
        e->e_type = rd16(image + 16); e->e_machine = rd16(image + 18);
        e->e_entry = rd32(image + 24); phoff = rd32(image + 28); shoff = rd32(image + 32);
        e->e_flags = rd32(image + 36);
        phentsize = rd16(image + 42); phnum = rd16(image + 44);
        shentsize = rd16(image + 46); shnum = rd16(image + 48); shstrndx = rd16(image + 50);
    }
    /* program headers */
    e->nphdrs = phnum;
    if (phnum) {
        if (phoff + (uint64_t)phnum * phentsize > size) fatal("%s: bad program header table", path);
        e->phdrs = xcalloc(phnum, sizeof *e->phdrs);
        for (i = 0; i < phnum; i++) {
            const uint8_t *p = image + phoff + i * phentsize;
            if (is64) {
                e->phdrs[i].type = rd32(p); e->phdrs[i].flags = rd32(p + 4);
                e->phdrs[i].offset = rd64(p + 8); e->phdrs[i].vaddr = rd64(p + 16); e->phdrs[i].paddr = rd64(p + 24);
                e->phdrs[i].filesz = rd64(p + 32); e->phdrs[i].memsz = rd64(p + 40); e->phdrs[i].align = rd64(p + 48);
            } else {
                e->phdrs[i].type = rd32(p); e->phdrs[i].offset = rd32(p + 4); e->phdrs[i].vaddr = rd32(p + 8);
                e->phdrs[i].paddr = rd32(p + 12); e->phdrs[i].filesz = rd32(p + 16); e->phdrs[i].memsz = rd32(p + 20);
                e->phdrs[i].flags = rd32(p + 24); e->phdrs[i].align = rd32(p + 28);
            }
        }
    }
    /* section headers */
    if (shoff && shnum == 0) {
        /* extended numbering: real count in sh_size of section 0 */
        if (shoff + shentsize > size) fatal("%s: bad section header table", path);
        shnum = (uint16_t)(is64 ? rd64(image + shoff + 32) : rd32(image + shoff + 20));
    }
    e->nsecs = shnum;
    e->shstrndx = shstrndx;
    if (shnum) {
        if (shoff + (uint64_t)shnum * shentsize > size) fatal("%s: bad section header table", path);
        e->secs = xcalloc(shnum, sizeof *e->secs);
        for (i = 0; i < shnum; i++) {
            const uint8_t *p = image + shoff + i * shentsize;
            GSec *s = &e->secs[i];
            uint32_t nameoff;
            if (is64) {
                nameoff = rd32(p); s->type = rd32(p + 4); s->flags = rd64(p + 8); s->addr = rd64(p + 16);
                s->offset = rd64(p + 24); s->size = rd64(p + 32); s->link = rd32(p + 40); s->info = rd32(p + 44);
                s->addralign = rd64(p + 48); s->entsize = rd64(p + 56);
            } else {
                nameoff = rd32(p); s->type = rd32(p + 4); s->flags = rd32(p + 8); s->addr = rd32(p + 12);
                s->offset = rd32(p + 16); s->size = rd32(p + 20); s->link = rd32(p + 24); s->info = rd32(p + 28);
                s->addralign = rd32(p + 32); s->entsize = rd32(p + 36);
            }
            s->name = (char *)(uintptr_t)nameoff; /* fixed up below */
            if (s->type != SHT_NOBITS && s->type != SHT_NULL) {
                if (s->offset + s->size > size) fatal("%s: section %d extends past the end of the file", path, i);
                s->data = e->image + s->offset;
            }
        }
        if (shstrndx == SHN_XINDEX) e->shstrndx = (int)e->secs[0].link;
        for (i = 0; i < shnum; i++)
            e->secs[i].name = xstrdup(gelf_shstr(e, (uint32_t)(uintptr_t)e->secs[i].name));
        /* symbols */
        e->symtab_idx = -1;
        for (i = 0; i < shnum; i++)
            if (e->secs[i].type == SHT_SYMTAB) { e->symtab_idx = i; break; }
        if (e->symtab_idx >= 0) {
            GSec *st = &e->secs[e->symtab_idx];
            size_t ent = is64 ? 24 : 16, n = st->size / ent, j;
            GSec *str = (st->link < (uint32_t)shnum) ? &e->secs[st->link] : NULL;
            GSec *shndx = NULL;
            for (i = 0; i < shnum; i++)
                if (e->secs[i].type == SHT_SYMTAB_SHNDX && e->secs[i].link == (uint32_t)e->symtab_idx) shndx = &e->secs[i];
            e->strtab_idx = str ? (int)st->link : -1;
            e->strtab = str && str->data ? (char *)str->data : "";
            e->syms = xcalloc(n, sizeof *e->syms);
            e->nsyms = (int)n;
            for (j = 0; j < n; j++) {
                const uint8_t *p = st->data + j * ent;
                GSym *s = &e->syms[j];
                uint32_t nameoff;
                if (is64) {
                    nameoff = rd32(p); s->info = p[4]; s->other = p[5]; s->shndx = rd16(p + 6);
                    s->value = rd64(p + 8); s->size = rd64(p + 16);
                } else {
                    nameoff = rd32(p); s->value = rd32(p + 4); s->size = rd32(p + 8);
                    s->info = p[12]; s->other = p[13]; s->shndx = rd16(p + 14);
                }
                if (s->shndx == SHN_XINDEX && shndx && shndx->data && (j + 1) * 4 <= shndx->size)
                    s->shndx = (uint16_t)rd32(shndx->data + j * 4); /* only small indices are supported */
                s->name = (str && nameoff < str->size) ? e->strtab + nameoff : "";
                if (ELF32_ST_TYPE(s->info) == STT_SECTION && !s->name[0] && s->shndx < shnum)
                    s->name = e->secs[s->shndx].name;
            }
        }
        for (i = 0; i < shnum; i++)
            if (gelf_is_rel_section(&e->secs[i])) decode_rels(e, &e->secs[i]);
    }
    return e;
}

GElf *gelf_open(const char *path)
{
    size_t size;
    uint8_t *image = read_file(path, &size);
    return gelf_read(path, image, size, 1);
}

void gelf_free(GElf *e)
{
    int i;
    if (!e) return;
    for (i = 0; i < e->nsecs; i++) {
        free(e->secs[i].name);
        free(e->secs[i].rels);
        if (e->secs[i].data_owned) free(e->secs[i].data);
    }
    free(e->secs); free(e->syms); free(e->phdrs);
    if (e->image_owned) free(e->image);
    free(e->path);
    free(e);
}

/* ---------------------------------------------------------------- archives */
int is_archive(const uint8_t *image, size_t size)
{
    return size >= 8 && (!memcmp(image, "!<arch>\n", 8) || !memcmp(image, "!<thin>\n", 8));
}

static size_t ar_num(const char *p, int n)
{
    size_t v = 0;
    while (n-- > 0 && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

Archive *archive_read(const char *path, uint8_t *image, size_t size)
{
    Archive *ar = xcalloc(1, sizeof *ar);
    size_t off = 8;
    char *longnames = NULL; size_t longnames_size = 0;
    PArr members = { 0 };
    const char *slash = strrchr(path, '/');
    char *dir = slash ? xstrndup(path, slash - path + 1) : xstrdup("");

    ar->path = xstrdup(path);
    ar->thin = !memcmp(image, "!<thin>\n", 8);
    while (off + 60 <= size) {
        const char *h = (const char *)image + off;
        size_t msize = ar_num(h + 48, 10);
        char name[17];
        ArMember *m;
        if (memcmp(h + 58, "`\n", 2)) fatal("%s: bad archive member header at %zu", path, off);
        memcpy(name, h, 16); name[16] = 0;
        if (!strncmp(name, "// ", 3) || !strcmp(name, "//              ")) {
            longnames = (char *)image + off + 60; longnames_size = msize;
            off += 60 + msize + (msize & 1);
            continue;
        }
        if (name[0] == '/' && (name[1] == ' ' || name[1] == 'S')) { /* symbol index "/" or "/SYM64/" */
            off += 60 + msize + (msize & 1);
            continue;
        }
        m = xcalloc(1, sizeof *m);
        m->header_off = off;
        if (name[0] == '/' && name[1] >= '0' && name[1] <= '9') {
            size_t lo = ar_num(name + 1, 15);
            const char *s, *end;
            if (!longnames || lo >= longnames_size) fatal("%s: bad long name reference", path);
            s = longnames + lo;
            end = memchr(s, '\n', longnames_size - lo);
            if (!end) end = longnames + longnames_size;
            if (end > s && end[-1] == '/') end--;
            m->name = xstrndup(s, end - s);
        } else {
            char *e = strchr(name, '/');
            if (e) *e = 0;
            else { e = name + 15; while (e > name && e[-1] == ' ') e--; *e = 0; }
            m->name = xstrdup(name);
        }
        if (ar->thin) {
            m->size = 0;
            m->path = (m->name[0] == '/') ? xstrdup(m->name) : xasprintf("%s%s", dir, m->name);
            off += 60; /* no data */
        } else {
            m->data = image + off + 60;
            m->size = msize;
            off += 60 + msize + (msize & 1);
        }
        parr_add(&members, m);
    }
    ar->nmembers = members.n;
    ar->members = xcalloc(members.n ? members.n : 1, sizeof *ar->members);
    for (int i = 0; i < members.n; i++) { ar->members[i] = *(ArMember *)members.p[i]; free(members.p[i]); }
    free(members.p);
    free(dir);
    return ar;
}

int archive_load_member(Archive *ar, ArMember *m)
{
    if (m->data) return 1;
    if (!ar->thin) return 0;
    m->data = read_file(m->path, &m->size);
    return 1;
}

/* ---------------------------------------------------------------- writer */
size_t welf_ehdr_size(int is64) { return is64 ? 64 : 52; }
size_t welf_phdr_size(int is64) { return is64 ? 56 : 32; }
size_t welf_shdr_size(int is64) { return is64 ? 64 : 40; }
size_t welf_sym_size(int is64) { return is64 ? 24 : 16; }

void welf_put_sym(int is64, uint8_t *p, const GSym *s)
{
    uint32_t nameoff = (uint32_t)(uintptr_t)s->name; /* caller stores the strtab offset in name */
    if (is64) {
        wr32(p, nameoff); p[4] = s->info; p[5] = s->other; wr16(p + 6, s->shndx);
        wr64(p + 8, s->value); wr64(p + 16, s->size);
    } else {
        wr32(p, nameoff); wr32(p + 4, (uint32_t)s->value); wr32(p + 8, (uint32_t)s->size);
        p[12] = s->info; p[13] = s->other; wr16(p + 14, s->shndx);
    }
}

void welf_put_rel(int is64, int rela, uint8_t *p, const GRel *r)
{
    if (is64) {
        wr64(p, r->offset); wr64(p + 8, ((uint64_t)r->sym << 32) | r->type);
        if (rela) wr64(p + 16, (uint64_t)r->addend);
    } else {
        wr32(p, (uint32_t)r->offset); wr32(p + 4, (r->sym << 8) | (r->type & 0xff));
        if (rela) wr32(p + 8, (uint32_t)r->addend);
    }
}

/* Writes the file.  Section offsets must have been assigned by the caller (secs[i].offset),
   except for .shstrtab which the writer appends after the last section data; the section
   header table follows.  Sections with offset 0 and data are placed after everything else. */
void welf_write(WElf *w, const char *path)
{
    int is64 = w->is64, i;
    size_t ehsz = welf_ehdr_size(is64), phsz = welf_phdr_size(is64), shsz = welf_shdr_size(is64);
    uint64_t end = 0, shstr_off, shoff, filesize;
    StrTab shstr = { 0 };
    uint32_t *nameoffs;
    uint8_t *buf;
    FILE *f;
    int nsecs = w->nsecs + 1; /* + .shstrtab */

    strtab_init(&shstr);
    nameoffs = xcalloc(nsecs, sizeof *nameoffs);
    for (i = 1; i < w->nsecs; i++) nameoffs[i] = strtab_add(&shstr, w->secs[i].name);
    nameoffs[w->nsecs] = strtab_add(&shstr, ".shstrtab");

    end = ehsz + (uint64_t)w->nphdrs * phsz;
    for (i = 1; i < w->nsecs; i++) {
        WSec *s = &w->secs[i];
        if (s->type == SHT_NOBITS) { if (s->offset > end) end = s->offset; continue; }
        if (s->offset == 0 && s->size) { s->offset = (end + 7) & ~7ULL; }
        if (s->offset + s->size > end) end = s->offset + s->size;
    }
    shstr_off = end;
    end += shstr.len;
    shoff = (end + 7) & ~7ULL;
    filesize = shoff + (uint64_t)nsecs * shsz;

    buf = xcalloc(filesize, 1);
    /* ELF header */
    memcpy(buf, ELFMAG, 4);
    buf[EI_CLASS] = is64 ? ELFCLASS64 : ELFCLASS32;
    buf[EI_DATA] = ELFDATA2LSB; buf[EI_VERSION] = EV_CURRENT;
    if (is64) {
        wr16(buf + 16, w->e_type); wr16(buf + 18, w->e_machine); wr32(buf + 20, 1);
        wr64(buf + 24, w->e_entry); wr64(buf + 32, w->nphdrs ? ehsz : 0); wr64(buf + 40, shoff);
        wr32(buf + 48, w->e_flags); wr16(buf + 52, (uint16_t)ehsz); wr16(buf + 54, (uint16_t)phsz);
        wr16(buf + 56, (uint16_t)w->nphdrs); wr16(buf + 58, (uint16_t)shsz); wr16(buf + 60, (uint16_t)nsecs);
        wr16(buf + 62, (uint16_t)w->nsecs);
    } else {
        wr16(buf + 16, w->e_type); wr16(buf + 18, w->e_machine); wr32(buf + 20, 1);
        wr32(buf + 24, (uint32_t)w->e_entry); wr32(buf + 28, w->nphdrs ? (uint32_t)ehsz : 0); wr32(buf + 32, (uint32_t)shoff);
        wr32(buf + 36, w->e_flags); wr16(buf + 40, (uint16_t)ehsz); wr16(buf + 42, (uint16_t)phsz);
        wr16(buf + 44, (uint16_t)w->nphdrs); wr16(buf + 46, (uint16_t)shsz); wr16(buf + 48, (uint16_t)nsecs);
        wr16(buf + 50, (uint16_t)w->nsecs);
    }
    for (i = 0; i < w->nphdrs; i++) {
        uint8_t *p = buf + ehsz + i * phsz;
        WPhdr *ph = &w->phdrs[i];
        if (is64) {
            wr32(p, ph->type); wr32(p + 4, ph->flags); wr64(p + 8, ph->offset); wr64(p + 16, ph->vaddr);
            wr64(p + 24, ph->paddr); wr64(p + 32, ph->filesz); wr64(p + 40, ph->memsz); wr64(p + 48, ph->align);
        } else {
            wr32(p, ph->type); wr32(p + 4, (uint32_t)ph->offset); wr32(p + 8, (uint32_t)ph->vaddr); wr32(p + 12, (uint32_t)ph->paddr);
            wr32(p + 16, (uint32_t)ph->filesz); wr32(p + 20, (uint32_t)ph->memsz); wr32(p + 24, ph->flags); wr32(p + 28, (uint32_t)ph->align);
        }
    }
    for (i = 1; i < w->nsecs; i++) {
        WSec *s = &w->secs[i];
        if (s->type != SHT_NOBITS && s->size && s->data) memcpy(buf + s->offset, s->data, s->size);
    }
    memcpy(buf + shstr_off, shstr.buf, shstr.len);
    for (i = 0; i < nsecs; i++) {
        uint8_t *p = buf + shoff + i * shsz;
        WSec tmp, *s;
        if (i == 0) { continue; }
        if (i == w->nsecs) {
            memset(&tmp, 0, sizeof tmp);
            tmp.type = SHT_STRTAB; tmp.offset = shstr_off; tmp.size = shstr.len; tmp.addralign = 1;
            s = &tmp;
        } else s = &w->secs[i];
        if (is64) {
            wr32(p, nameoffs[i]); wr32(p + 4, s->type); wr64(p + 8, s->flags); wr64(p + 16, s->addr);
            wr64(p + 24, s->offset); wr64(p + 32, s->size); wr32(p + 40, s->link); wr32(p + 44, s->info);
            wr64(p + 48, s->addralign); wr64(p + 56, s->entsize);
        } else {
            wr32(p, nameoffs[i]); wr32(p + 4, s->type); wr32(p + 8, (uint32_t)s->flags); wr32(p + 12, (uint32_t)s->addr);
            wr32(p + 16, (uint32_t)s->offset); wr32(p + 20, (uint32_t)s->size); wr32(p + 24, s->link); wr32(p + 28, s->info);
            wr32(p + 32, (uint32_t)s->addralign); wr32(p + 36, (uint32_t)s->entsize);
        }
    }
    f = fopen(path, "wb");
    if (!f) fatal("cannot create %s: %s", path, strerror(errno));
    if (fwrite(buf, 1, filesize, f) != filesize) fatal("cannot write %s: %s", path, strerror(errno));
    if (fclose(f)) fatal("cannot write %s: %s", path, strerror(errno));
    free(buf); free(nameoffs); free(shstr.buf);
}
