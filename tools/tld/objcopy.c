/* SPDX-License-Identifier: BSD-2-Clause */
/* objcopy: the subset Kbuild uses (-O binary, -j, -R/--remove-section, -S, --strip-debug, --prefix-symbols,
   --set-section-flags, -w --strip-unneeded-symbol, plain copy). */
#include "tld.h"
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

static PArr remove_pats, keep_only, flag_sets, strip_sym_pats, redefines;
static int out_binary, strip_all, strip_debug, strip_unneeded, wildcard;
static const char *prefix_symbols;

/* --prefix-symbols=PFX and --redefine-sym OLD=NEW: the new name of a symbol
   (GNU objcopy prefixes every named symbol, defined or undefined; the kernel's
   arch/x86/boot/startup confines its code with __pi_) */
static const char *renamed_symbol(const char *name)
{
    char *r;
    PARR_FOREACH(char *, r, redefines) {
        char *eq = strchr(r, '=');
        if (eq && (size_t)(eq - r) == strlen(name) && !strncmp(r, name, eq - r))
            return eq + 1;
    }
    if (prefix_symbols && name[0])
        return xasprintf("%s%s", prefix_symbols, name);
    return name;
}

static int pat_matches_list(PArr *pats, const char *name)
{
    /* GNU semantics: a name matches if it matches a positive pattern and no '!' pattern */
    int m = 0;
    char *p;
    PARR_FOREACH(char *, p, *pats) {
        if (p[0] == '!') { if (glob_match(p + 1, name)) return 0; }
        else if (wildcard ? glob_match(p, name) : !strcmp(p, name)) m = 1;
    }
    return m;
}

static int is_debug_sec(const char *n)
{
    return !strncmp(n, ".debug", 6) || !strncmp(n, ".zdebug", 7) || !strncmp(n, ".stab", 5) || !strcmp(n, ".line") || !strncmp(n, ".gnu.linkonce.wi.", 17);
}

static int section_removed(GElf *e, int i)
{
    GSec *s = &e->secs[i];
    if (i == 0) return 0;
    if (keep_only.n) return !pat_matches_list(&keep_only, s->name);
    if (pat_matches_list(&remove_pats, s->name)) return 1;
    if ((strip_all || strip_debug) && is_debug_sec(s->name)) return 1;
    if (strip_all && (s->type == SHT_SYMTAB || (i == e->strtab_idx && e->symtab_idx >= 0))) return 1;
    if (strip_all && gelf_is_rel_section(s) && e->e_type != ET_REL) return 1;
    return 0;
}

static uint64_t section_lma(GElf *e, GSec *s)
{
    for (int i = 0; i < e->nphdrs; i++) {
        if (e->phdrs[i].type != PT_LOAD) continue;
        if (s->addr >= e->phdrs[i].vaddr && s->addr + s->size <= e->phdrs[i].vaddr + e->phdrs[i].memsz)
            return e->phdrs[i].paddr + (s->addr - e->phdrs[i].vaddr);
    }
    return s->addr;
}

static void write_binary(GElf *e, const char *out)
{
    uint64_t lo = ~0ULL, hi = 0;
    uint8_t *buf;
    FILE *f;
    for (int i = 1; i < e->nsecs; i++) {
        GSec *s = &e->secs[i];
        uint64_t lma;
        if (section_removed(e, i)) continue;
        if (!(s->flags & SHF_ALLOC) && !keep_only.n) continue;
        if (s->type == SHT_NOBITS || !s->size) continue;
        lma = section_lma(e, s);
        if (lma < lo) lo = lma;
        if (lma + s->size > hi) hi = lma + s->size;
    }
    if (lo == ~0ULL) lo = hi = 0;
    buf = xcalloc(hi - lo ? hi - lo : 1, 1);
    for (int i = 1; i < e->nsecs; i++) {
        GSec *s = &e->secs[i];
        if (section_removed(e, i)) continue;
        if (!(s->flags & SHF_ALLOC) && !keep_only.n) continue;
        if (s->type == SHT_NOBITS || !s->size || !s->data) continue;
        memcpy(buf + (section_lma(e, s) - lo), s->data, s->size);
    }
    f = fopen(out, "wb");
    if (!f) fatal("cannot create %s: %s", out, strerror(errno));
    if (hi > lo) fwrite(buf, 1, hi - lo, f);
    if (fclose(f)) fatal("cannot write %s", out);
    free(buf);
}

static void copy_elf(GElf *e, const char *out)
{
    WElf w;
    int *newidx = xcalloc(e->nsecs, sizeof *newidx);
    int n = 1, i, symtab_new = 0, strtab_new = 0;
    WSec *ws;
    uint8_t *symdata = NULL; uint64_t symsize = 0; int first_global = 0;
    StrTab strtab = { 0 };
    int *symmap = NULL;

    memset(&w, 0, sizeof w);
    w.is64 = e->is64; w.e_type = e->e_type; w.e_machine = e->e_machine; w.e_entry = e->e_entry; w.e_flags = e->e_flags;
    for (i = 1; i < e->nsecs; i++) {
        if (i == e->shstrndx) { newidx[i] = -1; continue; }
        newidx[i] = section_removed(e, i) ? -1 : n++;
    }
    ws = xcalloc(n + 1, sizeof *ws);
    /* symbol table rewrite */
    if (e->symtab_idx >= 0 && newidx[e->symtab_idx] > 0) {
        int keep_count = 0;
        symmap = xmalloc((e->nsyms ? e->nsyms : 1) * sizeof *symmap);
        strtab_init(&strtab);
        symdata = xmalloc((e->nsyms ? e->nsyms : 1) * welf_sym_size(e->is64));
        for (i = 0; i < e->nsyms; i++) {
            GSym s = e->syms[i];
            int drop = 0;
            symmap[i] = -1;
            if (i > 0) {
                if (s.shndx > 0 && s.shndx < e->nsecs && newidx[s.shndx] < 0) drop = 1;
                if (strip_sym_pats.n && pat_matches_list(&strip_sym_pats, s.name)) drop = 1;
                if (strip_debug && ELF32_ST_TYPE(s.info) != STT_FILE && s.shndx > 0 && s.shndx < e->nsecs && is_debug_sec(e->secs[s.shndx].name)) drop = 1;
            }
            if (drop) continue;
            if (s.shndx > 0 && s.shndx < e->nsecs) s.shndx = (uint16_t)newidx[s.shndx];
            if (ELF32_ST_TYPE(s.info) == STT_SECTION && s.shndx > 0 && s.shndx < e->nsecs) s.name = "";
            else if (ELF32_ST_TYPE(s.info) != STT_FILE) s.name = (char *)renamed_symbol(s.name);
            s.name = (char *)(uintptr_t)strtab_add(&strtab, s.name);
            if (ELF32_ST_BIND(e->syms[i].info) == STB_LOCAL) first_global = keep_count + 1;
            welf_put_sym(e->is64, symdata + (size_t)keep_count * welf_sym_size(e->is64), &s);
            symmap[i] = keep_count++;
        }
        symsize = (uint64_t)keep_count * welf_sym_size(e->is64);
        if (first_global > keep_count) first_global = keep_count;
    }
    for (i = 1; i < e->nsecs; i++) {
        GSec *s = &e->secs[i];
        WSec *x;
        if (newidx[i] < 0) continue;
        x = &ws[newidx[i]];
        x->name = s->name; x->type = s->type; x->flags = s->flags; x->addr = s->addr; x->offset = s->offset;
        x->size = s->size; x->addralign = s->addralign; x->entsize = s->entsize; x->data = s->data;
        x->link = s->link < (uint32_t)e->nsecs && newidx[s->link] > 0 ? (uint32_t)newidx[s->link] : 0;
        x->info = (s->flags & SHF_INFO_LINK) || gelf_is_rel_section(s) ? (s->info < (uint32_t)e->nsecs && newidx[s->info] > 0 ? (uint32_t)newidx[s->info] : 0) : s->info;
        {
            char *fs;
            PARR_FOREACH(char *, fs, flag_sets) {
                char *eq = strchr(fs, '=');
                if (eq && (size_t)(eq - fs) == strlen(s->name) && !strncmp(fs, s->name, eq - fs)) {
                    /* flags list: alloc,load,noload,readonly,code,data,rom,share,contents,merge,strings */
                    uint64_t nf = 0; int noload = 0; int readonly = 0;
                    char *list = xstrdup(eq + 1), *tok = strtok(list, ",");
                    while (tok) {
                        if (!strcmp(tok, "alloc")) nf |= SHF_ALLOC;
                        else if (!strcmp(tok, "load")) nf |= SHF_ALLOC;
                        else if (!strcmp(tok, "noload")) noload = 1;
                        else if (!strcmp(tok, "readonly")) readonly = 1;
                        else if (!strcmp(tok, "code")) nf |= SHF_EXECINSTR | SHF_ALLOC;
                        else if (!strcmp(tok, "data")) nf |= SHF_WRITE | SHF_ALLOC;
                        else if (!strcmp(tok, "merge")) nf |= SHF_MERGE;
                        else if (!strcmp(tok, "strings")) nf |= SHF_STRINGS;
                        tok = strtok(NULL, ",");
                    }
                    free(list);
                    if (!readonly && !(nf & SHF_EXECINSTR)) nf |= SHF_WRITE;
                    x->flags = (s->flags & (SHF_TLS | SHF_GROUP | SHF_LINK_ORDER | SHF_INFO_LINK)) | nf;
                    if (noload) { x->type = SHT_NOBITS; x->data = NULL; }
                }
            }
        }
        if (s->type == SHT_SYMTAB && symdata) {
            x->data = symdata; x->size = symsize; x->info = (uint32_t)first_global; x->offset = 0;
            symtab_new = newidx[i];
        }
        if (i == e->strtab_idx && symdata) { x->data = (uint8_t *)strtab.buf; x->size = strtab.len; x->offset = 0; strtab_new = newidx[i]; }
        if (gelf_is_rel_section(s) && symmap) {
            /* remap symbol indices in relocations */
            int rela = s->type == SHT_RELA;
            size_t ent = e->is64 ? (rela ? 24 : 16) : (rela ? 12 : 8);
            uint8_t *d = xmalloc(s->size ? s->size : 1);
            memcpy(d, s->data, s->size);
            for (int k = 0; k < s->nrels; k++) {
                GRel r = s->rels[k];
                r.sym = r.sym < (uint32_t)e->nsyms && symmap[r.sym] >= 0 ? (uint32_t)symmap[r.sym] : 0;
                welf_put_rel(e->is64, rela, d + k * ent, &r);
            }
            x->data = d;
        }
    }
    (void)symtab_new; (void)strtab_new;
    /* file layout: keep the original offsets of sections with contents (program headers rely on them);
       rebuilt sections (.symtab/.strtab) go after the last original one */
    {
        uint64_t end = welf_ehdr_size(e->is64) + (uint64_t)e->nphdrs * welf_phdr_size(e->is64);
        for (i = 1; i < n; i++) if (ws[i].type != SHT_NOBITS && ws[i].offset && ws[i].offset + ws[i].size > end) end = ws[i].offset + ws[i].size;
        for (i = 1; i < n; i++) if (ws[i].type != SHT_NOBITS && ws[i].offset == 0 && ws[i].size) { end = (end + 7) & ~7ULL; ws[i].offset = end; end += ws[i].size; }
    }
    w.secs = ws; w.nsecs = n;
    w.nphdrs = e->nphdrs;
    w.phdrs = xcalloc(e->nphdrs ? e->nphdrs : 1, sizeof *w.phdrs);
    for (i = 0; i < e->nphdrs; i++) {
        w.phdrs[i].type = e->phdrs[i].type; w.phdrs[i].flags = e->phdrs[i].flags; w.phdrs[i].offset = e->phdrs[i].offset;
        w.phdrs[i].vaddr = e->phdrs[i].vaddr; w.phdrs[i].paddr = e->phdrs[i].paddr; w.phdrs[i].filesz = e->phdrs[i].filesz;
        w.phdrs[i].memsz = e->phdrs[i].memsz; w.phdrs[i].align = e->phdrs[i].align;
    }
    /* like GNU objcopy, a PT_LOAD's sizes follow the sections it still holds: p_filesz ends
       at the last kept section with contents, p_memsz at the last kept section.  The kernel
       strips .modinfo, an alloc section the linker script places after .bss/.brk, out of
       vmlinux; while it is there the data segment's file size spans the NOBITS sections, and
       the 32-bit decompressor would copy that "file" over itself (parse_elf copies p_filesz) */
    for (i = 0; i < e->nphdrs; i++) {
        uint64_t lo = e->phdrs[i].vaddr, hi = lo + e->phdrs[i].memsz, fend = 0, mend = 0;
        int j, any = 0;
        if (w.phdrs[i].type != PT_LOAD || hi == lo) continue;
        for (j = 1; j < n; j++) {
            WSec *s = &ws[j];
            if (!(s->flags & SHF_ALLOC) || s->addr < lo || s->addr >= hi) continue;
            any = 1;
            if (s->addr + s->size > mend) mend = s->addr + s->size;
            if (s->type != SHT_NOBITS && s->addr + s->size > fend) fend = s->addr + s->size;
        }
        if (!any) continue;
        w.phdrs[i].filesz = fend > lo ? fend - lo : 0;
        w.phdrs[i].memsz = mend > lo ? mend - lo : 0;
        if (w.phdrs[i].memsz < w.phdrs[i].filesz) w.phdrs[i].memsz = w.phdrs[i].filesz;
    }
    welf_write(&w, out);
    if (e->e_type != ET_REL) chmod(out, 0755);
}

int objcopy_main(int argc, char **argv)
{
    PArr files = { 0 };
    int i;
    const char *in, *out;
    GElf *e;
    for (i = 1; i < argc; i++) {
        char *a = argv[i], *val = NULL;
        if (a[0] != '-') { parr_add(&files, a); continue; }
        if ((val = strchr(a, '='))) *val++ = 0;
#define ARG() (val ? val : (i + 1 < argc ? argv[++i] : (fatal("option %s needs an argument", a), (char *)0)))
        if (!strcmp(a, "-O") || !strcmp(a, "--output-target")) { char *t = ARG(); if (!strcmp(t, "binary")) out_binary = 1; else if (strncmp(t, "elf", 3)) fatal("unsupported output target %s", t); }
        else if (!strcmp(a, "-I") || !strcmp(a, "--input-target") || !strcmp(a, "-F") || !strcmp(a, "--target") || !strcmp(a, "-B")) { ARG(); }
        else if (!strcmp(a, "-R") || !strcmp(a, "--remove-section")) parr_add(&remove_pats, ARG());
        else if (!strcmp(a, "-j") || !strcmp(a, "--only-section")) parr_add(&keep_only, ARG());
        else if (!strcmp(a, "-S") || !strcmp(a, "--strip-all")) strip_all = 1;
        else if (!strcmp(a, "-g") || !strcmp(a, "--strip-debug")) strip_debug = 1;
        else if (!strcmp(a, "--strip-unneeded")) strip_unneeded = 1;
        else if (!strcmp(a, "-w") || !strcmp(a, "--wildcard")) wildcard = 1;
        else if (!strcmp(a, "--strip-unneeded-symbol") || !strcmp(a, "--strip-symbol") || !strcmp(a, "-N")) parr_add(&strip_sym_pats, ARG());
        else if (!strcmp(a, "--set-section-flags")) parr_add(&flag_sets, ARG());
        else if (!strcmp(a, "--prefix-symbols")) prefix_symbols = ARG();
        else if (!strcmp(a, "--redefine-sym")) parr_add(&redefines, ARG());
        else if (!strcmp(a, "--version") || !strcmp(a, "-V")) { printf("GNU objcopy (tld) 2.47\n"); return 0; }
        else if (!strcmp(a, "-p") || !strcmp(a, "--preserve-dates") || !strcmp(a, "-v") || !strcmp(a, "--verbose")) {}
        else fatal("unsupported objcopy option %s", a);
#undef ARG
    }
    if (files.n < 1) fatal("usage: objcopy [options] in [out]");
    in = files.p[0]; out = files.n > 1 ? files.p[1] : NULL;
    (void)strip_unneeded;
    e = gelf_open(in);
    if (out_binary) {
        write_binary(e, out ? out : in);
    } else {
        if (!out) {
            char *tmp = xasprintf("%s.tld-tmp", in);
            copy_elf(e, tmp);
            if (rename(tmp, in)) fatal("cannot rename %s: %s", tmp, strerror(errno));
        } else copy_elf(e, out);
    }
    return 0;
}
