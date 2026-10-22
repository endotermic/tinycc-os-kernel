/* SPDX-License-Identifier: BSD-2-Clause */
/* tld: the linker.  See README.md for the model. */
#include "script.h"
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

typedef struct Obj Obj;
typedef struct InSec InSec;
typedef struct Symbol Symbol;
typedef struct OSec OSec;
typedef struct MergeEnt MergeEnt;
typedef struct MergeGroup MergeGroup;

struct InSec {
    Obj *obj;
    GSec *g;
    int idx;                /* section index in obj */
    OSec *out;              /* assigned output section or NULL */
    InputSpec *spec;        /* which spec placed it */
    int spec_order;         /* index of the S_INPUT statement (for grouping) */
    uint64_t outoff;        /* offset within output section */
    uint64_t size;          /* size in output (merged sections may shrink) */
    int discarded, assigned, keep;
    GSec *relsec;           /* relocations applying to this section */
    MergeGroup *mg;         /* string-merge group, or NULL */
    int mg_first;           /* this input section carries the merged data */
    int linker_created;     /* synthetic (.dynsym etc.) */
    uint8_t *data;          /* data override for synthetic sections */
    int out_shndx;          /* for -r: not used */
};

struct Obj {
    char *name;             /* display name (archive(member)) */
    GElf *e;
    InSec *secs;            /* indexed like the ELF sections */
    Symbol **symmap;        /* for each ELF symbol: the global Symbol, or NULL for locals */
    int *outsym;            /* output symtab index of each ELF symbol (-1 if dropped) */
    int order;
};

struct Symbol {
    char *name;
    Obj *obj; int symidx;   /* defining object & symbol index (if defined in an object) */
    InSec *sec;             /* defining input section (NULL if abs/undef/common/script) */
    uint64_t value, size;   /* for object symbols: st_value; for script symbols: address */
    int bind, type, vis;
    int defined, weak, common, script, dynamic_export;
    uint64_t common_align;
    OSec *osec;             /* script symbol: relative section (may be removed), or NULL for abs */
    int referenced;
    int outidx, dynidx;
    int referenced_strong, linker_local, startstop, reloc_referenced, plt_referenced;
    uint16_t versym;
    uint32_t dynstr_off;
};

struct MergeEnt { const char *s; int len; int suffix_of; uint64_t outoff; int first_pos; };
struct MergeGroup {
    uint64_t flags, entsize, align;
    PArr secs;              /* InSec* members in input order */
    /* string table */
    MergeEnt *ents; int nents, cap;
    uint64_t size;
    /* per input section: map of string start offsets -> ent index */
    uint8_t *data;
};

struct OSec {
    char *name;
    OutSecDef *def;         /* NULL for orphans and for -r auto sections */
    PArr insecs;            /* InSec* in placement order */
    uint64_t addr, lma, size, align, entsize, flags;
    uint32_t type;
    int has_at;
    int removed, keep;
    int has_input_spec, has_dot_assign, first_is_dot_assign;
    int alloc;              /* contributes to dot */
    int sectype;            /* OST_* */
    int idx;                /* output section header index (0 if not emitted) */
    int order;              /* creation order */
    uint64_t offset;        /* file offset */
    uint8_t *data;          /* assembled contents */
    int fill_set; uint64_t fill;
    int nphdr; int phdrs[16];
    int is_debug;
    /* data commands inside (BYTE/LONG...): list of (offset,size,expr) resolved at final pass */
    PArr datacmds;
    int secsym_idx;         /* -r / emit-relocs: index of the section symbol in the output symtab */
    PArr relout;            /* emitted relocation entries (GRel) for -r / --emit-relocs */
    OSec *relsec_out;       /* its .rela.X output section */
    int is_reloc_sec;       /* an output relocation section (-r) */
    OSec *rel_target;
    int in_segment;         /* default phdr builder: segment number */
};

typedef struct { uint64_t off; int size; Expr *e; } DataCmd;
typedef struct { uint64_t v; OSec *sec; int valid; } Val;

/* ---------------------------------------------------------------- state */
static struct {
    int is64, machine;
    int relocatable, shared, pie, emit_relocs, strip_debug, strip_all, gc_sections;
    int bsymbolic, no_undefined, build_id, verbose, eh_frame_hdr;
    int orphan_warn;
    uint64_t maxpage;
    char *output, *soname, *entry_opt;
    PArr script_files;
    Script *sc;
    PArr objs;              /* Obj* in link order */
    PArr osecs;             /* OSec* in output order */
    PArr undefs_cmdline;    /* -u symbols */
    PArr lib_dirs, libs;
    int noexecstack;
    int has_version_script;
    /* symbol table */
    Symbol **symtab; int nsyms, symcap;
    PArr symlist;           /* Symbol* in creation order */
    /* layout */
    int pass, final;
    int nphdrs_est;
    /* dynamic */
    InSec *dynsym_sec, *dynstr_sec, *hash_sec, *gnuhash_sec, *dynamic_sec, *versym_sec, *verdef_sec, *buildid_sec;
    PArr dynsyms;           /* Symbol* exported, in dynsym order (index = i+1) */
    StrTab dynstr;
    uint32_t dynstr_soname_off;
    Obj *synthetic_obj;
    char *entry_sym;
    int gnuhash_nbuckets_tmp, gnuhash_maskwords, gnuhash_shift, first_global;
} L;

static void opt_error(const char *o) { fatal("unrecognized option '%s'", o); }

/* ---------------------------------------------------------------- symbol table */
static unsigned hash_str(const char *s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }

static Symbol *sym_lookup(const char *name, int create)
{
    unsigned h, i;
    if (!L.symtab) { L.symcap = 1 << 16; L.symtab = xcalloc(L.symcap, sizeof *L.symtab); }
    if (L.nsyms * 2 >= L.symcap) {
        Symbol **old = L.symtab; int oldcap = L.symcap;
        L.symcap *= 2; L.symtab = xcalloc(L.symcap, sizeof *L.symtab);
        for (int j = 0; j < oldcap; j++) if (old[j]) {
            unsigned k = hash_str(old[j]->name) & (L.symcap - 1);
            while (L.symtab[k]) k = (k + 1) & (L.symcap - 1);
            L.symtab[k] = old[j];
        }
        free(old);
    }
    h = hash_str(name);
    for (i = h & (L.symcap - 1); L.symtab[i]; i = (i + 1) & (L.symcap - 1))
        if (!strcmp(L.symtab[i]->name, name)) return L.symtab[i];
    if (!create) return NULL;
    L.symtab[i] = xcalloc(1, sizeof(Symbol));
    L.symtab[i]->name = xstrdup(name);
    L.symtab[i]->outidx = -1; L.symtab[i]->dynidx = -1;
    L.nsyms++;
    parr_add(&L.symlist, L.symtab[i]);
    return L.symtab[i];
}

/* ---------------------------------------------------------------- input loading */
static InSec *sec_of_sym(Obj *o, const GSym *s)
{
    if (s->shndx == SHN_UNDEF || s->shndx >= SHN_LORESERVE) return NULL;
    if (s->shndx >= o->e->nsecs) return NULL;
    return &o->secs[s->shndx];
}

static void add_object(const char *name, GElf *e)
{
    Obj *o = xcalloc(1, sizeof *o);
    int i;
    if (e->e_type != ET_REL) fatal("%s: not a relocatable object", name);
    if (L.machine == 0) {
        L.machine = e->e_machine; L.is64 = e->is64;
    } else if (e->e_machine != L.machine) {
        fatal("%s: incompatible machine type (%d) for this link", name, e->e_machine);
    }
    o->name = xstrdup(name); o->e = e; o->order = L.objs.n;
    o->secs = xcalloc(e->nsecs, sizeof *o->secs);
    for (i = 0; i < e->nsecs; i++) {
        InSec *s = &o->secs[i];
        s->obj = o; s->g = &e->secs[i]; s->idx = i; s->size = e->secs[i].size;
    }
    for (i = 0; i < e->nsecs; i++) {
        GSec *g = &e->secs[i];
        if (gelf_is_rel_section(g) && g->info < (uint32_t)e->nsecs) o->secs[g->info].relsec = g;
    }
    o->symmap = xcalloc(e->nsyms ? e->nsyms : 1, sizeof *o->symmap);
    o->outsym = xmalloc((e->nsyms ? e->nsyms : 1) * sizeof *o->outsym);
    for (i = 0; i < e->nsyms; i++) o->outsym[i] = -1;
    parr_add(&L.objs, o);
    /* global symbols */
    for (i = 1; i < e->nsyms; i++) {
        GSym *gs = &e->syms[i];
        int bind = ELF32_ST_BIND(gs->info), type = ELF32_ST_TYPE(gs->info);
        Symbol *sym;
        if (bind == STB_LOCAL) continue;
        if (type == STT_SECTION || type == STT_FILE) continue;
        if (!gs->name[0]) continue;
        sym = sym_lookup(gs->name, 1);
        o->symmap[i] = sym;
        if (gs->shndx == SHN_UNDEF) {
            sym->referenced = 1;
            if (!sym->defined && bind == STB_WEAK && !sym->weak && !sym->referenced_strong) {}
            if (!sym->defined) {
                /* record weakness of the reference: a reference is weak only if all are weak */
                if (bind != STB_WEAK) sym->referenced_strong = 1;
            }
            continue;
        }
        if (gs->shndx == SHN_COMMON) {
            if (sym->defined && !sym->common) continue;       /* real definition wins */
            if (sym->common && sym->size >= gs->size) continue;
            sym->defined = 1; sym->common = 1; sym->obj = o; sym->symidx = i;
            sym->size = gs->size; sym->common_align = gs->value; sym->bind = bind; sym->type = type;
            sym->vis = ELF32_ST_VISIBILITY(gs->other) ? ELF32_ST_VISIBILITY(gs->other) : sym->vis;
            continue;
        }
        if (sym->defined && !sym->common) {
            if (bind == STB_WEAK) continue;                       /* existing wins */
            if (sym->weak || sym->script) {
                /* strong replaces weak; a script assignment is kept */
                if (sym->script) continue;
            } else {
                fatal("%s: multiple definition of '%s' (first defined in %s)", name, gs->name, sym->obj ? sym->obj->name : "linker script");
            }
        }
        sym->defined = 1; sym->common = 0; sym->weak = bind == STB_WEAK;
        sym->obj = o; sym->symidx = i; sym->sec = sec_of_sym(o, gs);
        sym->value = gs->value; sym->size = gs->size; sym->bind = bind; sym->type = type;
        if (ELF32_ST_VISIBILITY(gs->other)) sym->vis = ELF32_ST_VISIBILITY(gs->other);
    }
}

static int member_defines_needed(GElf *e)
{
    for (int i = 1; i < e->nsyms; i++) {
        GSym *gs = &e->syms[i];
        Symbol *sym;
        if (ELF32_ST_BIND(gs->info) == STB_LOCAL || gs->shndx == SHN_UNDEF || !gs->name[0]) continue;
        if (ELF32_ST_TYPE(gs->info) == STT_SECTION || ELF32_ST_TYPE(gs->info) == STT_FILE) continue;
        sym = sym_lookup(gs->name, 0);
        if (sym && !sym->defined && sym->referenced) return 1;
        if (sym && sym->common && gs->shndx != SHN_COMMON) return 1;
    }
    return 0;
}

typedef struct { Archive *ar; GElf **members; int *loaded; } ArState;
static PArr archives;       /* ArState* of the current group */

static int archive_pass(ArState *as)
{
    int any = 0;
    for (int i = 0; i < as->ar->nmembers; i++) {
        ArMember *m = &as->ar->members[i];
        if (as->loaded[i]) continue;
        if (!as->members[i]) {
            archive_load_member(as->ar, m);
            if (is_archive(m->data, m->size)) { warning("%s: nested archive member %s ignored", as->ar->path, m->name); as->loaded[i] = 1; continue; }
            if (m->size < 4 || memcmp(m->data, ELFMAG, 4)) { as->loaded[i] = 1; continue; }
            as->members[i] = gelf_read(xasprintf("%s(%s)", as->ar->path, m->name), m->data, m->size, 0);
        }
        if (member_defines_needed(as->members[i])) {
            add_object(as->members[i]->path, as->members[i]);
            as->loaded[i] = 1; any = 1;
        }
    }
    return any;
}

static void load_archive(const char *path, uint8_t *image, size_t size, int whole)
{
    Archive *ar = archive_read(path, image, size);
    ArState *as = xcalloc(1, sizeof *as);
    as->ar = ar;
    as->members = xcalloc(ar->nmembers ? ar->nmembers : 1, sizeof *as->members);
    as->loaded = xcalloc(ar->nmembers ? ar->nmembers : 1, sizeof *as->loaded);
    if (whole) {
        for (int i = 0; i < ar->nmembers; i++) {
            ArMember *m = &ar->members[i];
            archive_load_member(ar, m);
            if (is_archive(m->data, m->size)) {
                load_archive(m->path ? m->path : m->name, m->data, m->size, 1);
                continue;
            }
            if (m->size < 4 || memcmp(m->data, ELFMAG, 4)) continue;
            add_object(xasprintf("%s(%s)", path, m->name), gelf_read(xasprintf("%s(%s)", path, m->name), m->data, m->size, 0));
        }
        return;
    }
    parr_add(&archives, as);
    while (archive_pass(as)) ;
}

static void load_input(const char *path, int whole)
{
    size_t size;
    uint8_t *image = read_file(path, &size);
    if (is_archive(image, size)) { load_archive(path, image, size, whole); return; }
    add_object(path, gelf_read(path, image, size, 1));
}

static void group_resolve(void)
{
    /* iterate all archives of the group until nothing new is pulled */
    int any;
    do {
        any = 0;
        ArState *as;
        PARR_FOREACH(ArState *, as, archives) if (archive_pass(as)) any = 1;
    } while (any);
}

/* ---------------------------------------------------------------- output sections */
static OSec *osec_find(const char *name)
{
    OSec *os;
    PARR_FOREACH(OSec *, os, L.osecs) if (!os->is_reloc_sec && !strcmp(os->name, name)) return os;
    return NULL;
}

static OSec *osec_new(const char *name, OutSecDef *def)
{
    OSec *os = xcalloc(1, sizeof *os);
    os->name = xstrdup(name); os->def = def; os->order = L.osecs.n;
    os->alloc = 1;
    if (def) {
        os->sectype = def->sectype;
        for (Stmt *c = def->children; c; c = c->next) {
            if (c->kind == S_INPUT) os->has_input_spec = 1;
            if (c->kind == S_DOT_ASSIGN) {
                int trivial = 0;
                Expr *e = c->expr;
                if (e->kind == E_NUM && e->num == 0) trivial = 1;
                if (e->kind == E_BINARY && e->op == '+' && e->a->kind == E_DOT && e->b->kind == E_NUM && e->b->num == 0) trivial = 1;
                if (!trivial) os->has_dot_assign = 1;
            }
        }
        if (def->children && def->children->kind == S_DOT_ASSIGN) os->first_is_dot_assign = 1;
    }
    parr_add(&L.osecs, os);
    return os;
}

static void osec_insert_after(OSec *os, OSec *after)
{
    /* move os (currently last) to just after 'after' */
    int i, j;
    for (i = 0; i < L.osecs.n; i++) if (L.osecs.p[i] == after) break;
    for (j = L.osecs.n - 1; j > i + 1; j--) L.osecs.p[j] = L.osecs.p[j - 1];
    if (i + 1 < L.osecs.n) L.osecs.p[i + 1] = os;
    for (i = 0; i < L.osecs.n; i++) ((OSec *)L.osecs.p[i])->order = i;
}

static int is_debug_name(const char *n)
{
    return !strncmp(n, ".debug", 6) || !strncmp(n, ".zdebug", 7) || !strncmp(n, ".stab", 5) || !strcmp(n, ".line") ||
           !strncmp(n, ".gnu.linkonce.wi.", 17) || !strncmp(n, ".gnu.debuglto_", 14);
}

static int sec_is_input_candidate(InSec *s)
{
    GSec *g = s->g;
    if (s->idx == 0) return 0;
    if (s->linker_created) return 1;
    if (g->type == SHT_SYMTAB || g->type == SHT_STRTAB || g->type == SHT_REL || g->type == SHT_RELA ||
        g->type == SHT_GROUP || g->type == SHT_SYMTAB_SHNDX) return 0;
    if (!strcmp(g->name, ".note.gnu.property")) return 0;   /* GNU ld merges these into its own note; we drop them */
    if (s->idx == s->obj->e->shstrndx) return 0;
    if (!L.relocatable) {
        if (g->flags & SHF_EXCLUDE) return 0;
        if (!strcmp(g->name, ".note.GNU-stack")) return 0;
        if (L.strip_debug && is_debug_name(g->name)) return 0;
    }
    return 1;
}

static int file_matches(InputSpec *sp, Obj *o)
{
    if (!sp->filepat || !strcmp(sp->filepat, "*")) return 1;
    if (glob_match(sp->filepat, o->name)) return 1;
    /* archive members: match against the member name too */
    {
        const char *p = strrchr(o->name, '(');
        if (p) {
            char *m = xstrndup(p + 1, strlen(p + 1) - 1);
            int r = glob_match(sp->filepat, m);
            free(m);
            if (r) return 1;
        }
        p = strrchr(o->name, '/');
        if (p && glob_match(sp->filepat, p + 1)) return 1;
    }
    return 0;
}

static int spec_matches(InputSpec *sp, InSec *s)
{
    if (sp->kind_filter) {
        int nobits = s->g->type == SHT_NOBITS, alloc = (s->g->flags & SHF_ALLOC) != 0;
        if (nobits != sp->kind_nobits || alloc != sp->kind_alloc) return 0;
    }
    for (int i = 0; i < sp->nsecpats; i++)
        if (glob_match(sp->secpats[i], s->g->name)) return 1;
    return 0;
}

/* an existing output section 'name' compatible with input section s (GNU: same ALLOC/LOAD) */
static OSec *osec_find_compatible(const char *name, InSec *s)
{
    OSec *os;
    int nobits = s->g->type == SHT_NOBITS, alloc = (s->g->flags & SHF_ALLOC) != 0;
    PARR_FOREACH(OSec *, os, L.osecs) {
        InSec *f;
        if (os->is_reloc_sec || strcmp(os->name, name)) continue;
        if (os->def) return os;
        if (!os->insecs.n) return os;
        f = os->insecs.p[0];
        if ((f->g->type == SHT_NOBITS) == nobits && ((f->g->flags & SHF_ALLOC) != 0) == alloc) return os;
    }
    return NULL;
}

static int cmp_insec_name(const void *a, const void *b)
{
    const InSec *x = *(InSec * const *)a, *y = *(InSec * const *)b;
    int r = strcmp(x->g->name, y->g->name);
    return r ? r : (x->obj->order - y->obj->order ? x->obj->order - y->obj->order : x->idx - y->idx);
}
static int cmp_insec_align(const void *a, const void *b)
{
    const InSec *x = *(InSec * const *)a, *y = *(InSec * const *)b;
    if (x->g->addralign != y->g->addralign) return x->g->addralign > y->g->addralign ? -1 : 1;
    return (x->obj->order - y->obj->order) ? x->obj->order - y->obj->order : x->idx - y->idx;
}

static InSec *common_sec;
static Obj *make_synthetic_obj(void)
{
    if (L.synthetic_obj) return L.synthetic_obj;
    Obj *o = xcalloc(1, sizeof *o);
    GElf *e = xcalloc(1, sizeof *e);
    o->name = xstrdup("<linker>"); o->e = e; e->path = o->name; e->is64 = L.is64; e->e_machine = L.machine;
    e->secs = xcalloc(64, sizeof *e->secs); e->nsecs = 1;
    o->secs = xcalloc(64, sizeof *o->secs);
    o->symmap = xcalloc(1, sizeof *o->symmap); o->outsym = xcalloc(1, sizeof *o->outsym);
    o->order = 1 << 29;
    L.synthetic_obj = o;
    parr_add(&L.objs, o);
    return o;
}

static InSec *add_synthetic_section(const char *name, uint32_t type, uint64_t flags, uint64_t align, uint64_t entsize, uint64_t size)
{
    Obj *o = make_synthetic_obj();
    GElf *e = o->e;
    int i = e->nsecs;
    if (i >= 64) fatal("too many linker-created sections");
    e->secs[i].name = xstrdup(name); e->secs[i].type = type; e->secs[i].flags = flags;
    e->secs[i].addralign = align; e->secs[i].entsize = entsize; e->secs[i].size = size;
    e->secs[i].data = type == SHT_NOBITS ? NULL : xcalloc(size ? size : 1, 1); e->secs[i].data_owned = 1;
    e->nsecs = i + 1;
    o->secs[i].obj = o; o->secs[i].g = &e->secs[i]; o->secs[i].idx = i; o->secs[i].size = size;
    o->secs[i].linker_created = 1;
    return &o->secs[i];
}

static void assign_sections(void)
{
    int order = 0;
    Obj *o;
    InSec *s;
    /* commons */
    {
        Symbol *sym; uint64_t off = 0, maxal = 1;
        int n = 0;
        PARR_FOREACH(Symbol *, sym, L.symlist) if (sym->common) n++;
        if (n) {
            common_sec = add_synthetic_section("COMMON", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 1, 0, 0);
            PARR_FOREACH(Symbol *, sym, L.symlist) if (sym->common) {
                uint64_t al = sym->common_align ? sym->common_align : 1;
                off = (off + al - 1) & ~(al - 1);
                sym->value = off; sym->sec = common_sec; sym->common = 2; /* allocated */
                off += sym->size;
                if (al > maxal) maxal = al;
            }
            common_sec->g->size = common_sec->size = off; common_sec->g->addralign = maxal;
        }
    }
    /* linker-created sections come first, like GNU ld's stub file */
    if (L.synthetic_obj && L.objs.p[0] != L.synthetic_obj) {
        int k;
        for (k = 0; k < L.objs.n; k++) if (L.objs.p[k] == L.synthetic_obj) break;
        for (; k > 0; k--) L.objs.p[k] = L.objs.p[k - 1];
        L.objs.p[0] = L.synthetic_obj;
        PARR_FOREACH(Obj *, o, L.objs) o->order = o_i;
    }
    for (Stmt *st = L.sc->stmts; st; st = st->next) {
        OSec *os;
        if (st->kind != S_OUTSEC) continue;
        os = osec_new(st->os->name, st->os);
        if (st->os->discard) os->removed = 1;
        for (Stmt *c = st->os->children; c; c = c->next) {
            PArr matched = { 0 };
            if (c->kind != S_INPUT) continue;
            order++;
            PARR_FOREACH(Obj *, o, L.objs) {
                if (!file_matches(c->spec, o)) continue;
                for (int i = 1; i < o->e->nsecs; i++) {
                    s = &o->secs[i];
                    if (s->assigned || !sec_is_input_candidate(s)) continue;
                    if (c->spec->common ? (s == common_sec) : spec_matches(c->spec, s)) {
                        s->assigned = 1; s->spec = c->spec; s->spec_order = order;
                        parr_add(&matched, s);
                    }
                }
            }
            if (c->spec->sort == 1 || c->spec->sort == 3) qsort(matched.p, matched.n, sizeof(void *), cmp_insec_name);
            else if (c->spec->sort == 2) qsort(matched.p, matched.n, sizeof(void *), cmp_insec_align);
            PARR_FOREACH(InSec *, s, matched) {
                if (st->os->discard) { s->discarded = 1; continue; }
                s->out = os; s->keep = c->spec->keep;
                parr_add(&os->insecs, s);
            }
            free(matched.p);
        }
    }
    /* orphans */
    PARR_FOREACH(Obj *, o, L.objs) {
        for (int i = 1; i < o->e->nsecs; i++) {
            OSec *os, *after = NULL, *cand;
            s = &o->secs[i];
            if (s->assigned || !sec_is_input_candidate(s)) continue;
            if (s->g->size == 0 && !L.relocatable && !osec_find_compatible(s->g->name, s)) { s->assigned = 1; s->discarded = 1; continue; }
            s->assigned = 1;
            os = osec_find_compatible(s->g->name, s);
            if (!os) {
                os = osec_new(s->g->name, NULL);
                if (s->g->flags & SHF_ALLOC) {
                    /* after the last alloc section of the same class */
                    int want_x = (s->g->flags & SHF_EXECINSTR) != 0, want_w = (s->g->flags & SHF_WRITE) != 0, want_bss = s->g->type == SHT_NOBITS;
                    OSec *last_alloc = NULL;
                    PARR_FOREACH(OSec *, cand, L.osecs) {
                        if (cand == os || cand->removed || !cand->alloc || cand->is_reloc_sec) continue;
                        if (!(cand->flags & SHF_ALLOC) && cand->insecs.n == 0) continue;
                        if (cand->insecs.n == 0) continue;
                        {
                            InSec *f = cand->insecs.p[0];
                            int x = (f->g->flags & SHF_EXECINSTR) != 0, w = (f->g->flags & SHF_WRITE) != 0, bss = f->g->type == SHT_NOBITS;
                            if (!(f->g->flags & SHF_ALLOC)) continue;
                            last_alloc = cand;
                            if (x == want_x && w == want_w && bss == want_bss) after = cand;
                        }
                    }
                    if (!after) after = last_alloc;
                    if (after) osec_insert_after(os, after);
                    if (L.orphan_warn && !L.relocatable)
                        warning("orphan section `%s' from `%s' being placed in section `%s'", s->g->name, o->name, os->name);
                } else {
                    os->alloc = 0;
                }
            }
            s->out = os;
            parr_add(&os->insecs, s);
        }
    }
}

/* ---------------------------------------------------------------- section merging (SHF_MERGE) */
/* length of the entry starting at 'off' in a mergeable section: entsize-sized records, or
   NUL-terminated strings of entsize-wide characters */
static uint64_t mg_entry_len(MergeGroup *mg, const uint8_t *data, uint64_t off, uint64_t size)
{
    uint64_t es = mg->entsize, len = 0;
    if (!(mg->flags & SHF_STRINGS)) return off + es <= size ? es : size - off;
    while (off + len + es <= size) {
        int zero = 1;
        for (uint64_t k = 0; k < es; k++) if (data[off + len + k]) zero = 0;
        len += es;
        if (zero) break;
    }
    return len;
}

static int mg_find_ent(MergeGroup *mg, const char *s, int len)
{
    /* linear search is fine for the small kernel string sections; use a hash for big ones */
    for (int i = 0; i < mg->nents; i++)
        if (mg->ents[i].len == len && !memcmp(mg->ents[i].s, s, len)) return i;
    return -1;
}

static void merge_strings(void)
{
    OSec *os;
    if (L.relocatable) return;
    PARR_FOREACH(OSec *, os, L.osecs) {
        PArr groups = { 0 };
        InSec *s;
        MergeGroup *mg;
        if (os->removed) continue;
        PARR_FOREACH(InSec *, s, os->insecs) {
            GSec *g = s->g;
            if (!(g->flags & SHF_MERGE) || g->type == SHT_NOBITS || s->relsec || g->entsize == 0) continue;
            if ((g->flags & SHF_STRINGS) && g->entsize > 4) continue;
            if (!g->data) continue;
            {
                int gi;
                mg = NULL;
                for (gi = 0; gi < groups.n; gi++) {
                    MergeGroup *cand = groups.p[gi];
                    if (cand->flags == g->flags && cand->entsize == g->entsize && cand->align == g->addralign) { mg = cand; break; }
                }
            }
            if (!mg) {
                mg = xcalloc(1, sizeof *mg);
                mg->flags = g->flags; mg->entsize = g->entsize; mg->align = g->addralign ? g->addralign : 1;
                parr_add(&groups, mg);
                s->mg_first = 1;
            }
            s->mg = mg;
            parr_add(&mg->secs, s);
        }
        PARR_FOREACH(MergeGroup *, mg, groups) {
            /* collect entries in input order; dedupe exact duplicates */
            uint64_t es = mg->entsize;
            PARR_FOREACH(InSec *, s, mg->secs) {
                uint64_t off = 0;
                while (off < s->g->size) {
                    uint64_t len = mg_entry_len(mg, s->g->data, off, s->g->size);
                    if (!len) break;
                    if (mg_find_ent(mg, (const char *)s->g->data + off, (int)len) < 0) {
                        if (mg->nents == mg->cap) { mg->cap = mg->cap ? mg->cap * 2 : 64; mg->ents = xrealloc(mg->ents, mg->cap * sizeof *mg->ents); }
                        mg->ents[mg->nents].s = (const char *)s->g->data + off;
                        mg->ents[mg->nents].len = (int)len;
                        mg->ents[mg->nents].suffix_of = -1;
                        mg->nents++;
                    }
                    off += len;
                }
            }
            /* suffix merging (only for byte strings): a string that is a suffix of another */
            if (es == 1 && (mg->flags & SHF_STRINGS)) {
                for (int i = 0; i < mg->nents; i++) {
                    for (int j = 0; j < mg->nents; j++) {
                        if (i == j || mg->ents[j].suffix_of >= 0) continue;
                        if (mg->ents[j].len > mg->ents[i].len && (mg->ents[j].len - mg->ents[i].len) % mg->align == 0 &&
                            !memcmp(mg->ents[j].s + mg->ents[j].len - mg->ents[i].len, mg->ents[i].s, mg->ents[i].len)) {
                            mg->ents[i].suffix_of = j; break;
                        }
                    }
                }
            }
            /* layout */
            {
                uint64_t pos = 0;
                for (int i = 0; i < mg->nents; i++) {
                    if (mg->ents[i].suffix_of >= 0) continue;
                    pos = (pos + mg->align - 1) & ~(mg->align - 1);
                    mg->ents[i].outoff = pos;
                    pos += mg->ents[i].len;
                }
                mg->size = pos;
                mg->data = xcalloc(pos ? pos : 1, 1);
                for (int i = 0; i < mg->nents; i++) {
                    if (mg->ents[i].suffix_of >= 0) continue;
                    memcpy(mg->data + mg->ents[i].outoff, mg->ents[i].s, mg->ents[i].len);
                }
                for (int i = 0; i < mg->nents; i++) {
                    int j = i, guard = 0;
                    while (mg->ents[j].suffix_of >= 0 && guard++ < 1000) j = mg->ents[j].suffix_of;
                    if (j != i) mg->ents[i].outoff = mg->ents[j].outoff + (mg->ents[j].len - mg->ents[i].len);
                }
            }
            PARR_FOREACH(InSec *, s, mg->secs) s->size = s->mg_first ? mg->size : 0;
        }
        free(groups.p);
    }
}

/* map an offset inside a merged input section to the output offset within the group */
static uint64_t merge_map(InSec *s, uint64_t off)
{
    MergeGroup *mg = s->mg;
    uint64_t p = 0;
    if (off >= s->g->size) return mg->size + (off - s->g->size);
    while (p < s->g->size) {
        uint64_t len = mg_entry_len(mg, s->g->data, p, s->g->size);
        if (!len) break;
        if (off < p + len) {
            int e = mg_find_ent(mg, (const char *)s->g->data + p, (int)len);
            return mg->ents[e].outoff + (off - p);
        }
        p += len;
    }
    return off;
}

/* address of input section s (output), accounting for merging */
static InSec *mg_carrier(InSec *s)
{
    if (!s->mg) return s;
    return (InSec *)s->mg->secs.p[0];
}

static uint64_t insec_addr(InSec *s)
{
    InSec *c = mg_carrier(s);
    return c->out->addr + c->outoff;
}

/* ---------------------------------------------------------------- expressions */
static uint64_t dot;
static OSec *cur_os;        /* for '.' relativity */
static int in_section;      /* evaluating inside an output section body */
static int undefined_in_expr;

static Val mkabs(uint64_t v) { Val r = { v, NULL, 1 }; return r; }
static Val mkrel(uint64_t v, OSec *s) { Val r = { v, s, 1 }; return r; }

/* GNU ld's section_for_dot(): outside an output section, an expression relative to '.' is
   relative to the output section containing dot (end inclusive), else the first one after it. */
static OSec *section_for_dot(void)
{
    OSec *os, *follow = NULL, *last = NULL;
    if (in_section) return cur_os;
    PARR_FOREACH(OSec *, os, L.osecs) {
        if (os->removed || os->is_reloc_sec || !os->insecs.n || !(os->flags & SHF_ALLOC)) continue;
        if (os->addr <= dot && dot <= os->addr + os->size) return os;
        if (os->addr > dot && (!follow || os->addr < follow->addr)) follow = os;
        last = os;
    }
    return follow ? follow : last;
}

static uint64_t align_up(uint64_t v, uint64_t a) { return a > 1 ? (v + a - 1) & ~(a - 1) : v; }

static uint64_t symbol_address(Symbol *sym, OSec **secp);

static Val eval(Expr *e)
{
    Val a, b;
    switch (e->kind) {
    case E_NUM: return mkabs(e->num);
    case E_DOT: return mkrel(dot, section_for_dot());
    case E_SYM: {
        Symbol *sym = sym_lookup(e->name, 0);
        OSec *sec = NULL;
        uint64_t v;
        if (!sym || !sym->defined) {
            if (L.final) { if (sym) fatal("undefined symbol `%s' referenced in expression", e->name); else fatal("undefined symbol `%s' referenced in expression", e->name); }
            undefined_in_expr = 1;
            { Val r = { 0, NULL, 0 }; return r; }
        }
        v = symbol_address(sym, &sec);
        return mkrel(v, sec);
    }
    case E_UNARY:
        a = eval(e->a);
        switch (e->op) {
        case '-': return mkabs(-a.v);
        case '~': return mkabs(~a.v);
        case '!': return mkabs(!a.v);
        default: return a;
        }
    case E_BINARY:
        a = eval(e->a);
        if (e->op == T_ANDAND) { if (!a.v) return mkabs(0); b = eval(e->b); return mkabs(b.v != 0); }
        if (e->op == T_OROR) { if (a.v) return mkabs(1); b = eval(e->b); return mkabs(b.v != 0); }
        b = eval(e->b);
        switch (e->op) {
        case '+': return a.sec ? mkrel(a.v + b.v, a.sec) : b.sec ? mkrel(a.v + b.v, b.sec) : mkabs(a.v + b.v);
        case '-': return (a.sec && !b.sec) ? mkrel(a.v - b.v, a.sec) : mkabs(a.v - b.v);
        case '*': return mkabs(a.v * b.v);
        case '/': if (!b.v) { if (L.final) fatal("division by zero in linker script"); return mkabs(0); } return mkabs((int64_t)a.v / (int64_t)b.v);
        case '%': if (!b.v) { if (L.final) fatal("modulo by zero in linker script"); return mkabs(0); } return mkabs((int64_t)a.v % (int64_t)b.v);
        case T_LSH: return mkabs(a.v << b.v);
        case T_RSH: return mkabs(a.v >> b.v);
        case '<': return mkabs(a.v < b.v);
        case '>': return mkabs(a.v > b.v);
        case T_LE: return mkabs(a.v <= b.v);
        case T_GE: return mkabs(a.v >= b.v);
        case T_EQ: return mkabs(a.v == b.v);
        case T_NE: return mkabs(a.v != b.v);
        case '&': return mkabs(a.v & b.v);
        case '|': return mkabs(a.v | b.v);
        case '^': return mkabs(a.v ^ b.v);
        }
        fatal("bad binary operator");
    case E_TERNARY:
        a = eval(e->a);
        return a.v ? eval(e->b) : eval(e->c);
    case E_ASSERT:
        a = eval(e->a);
        if (L.final && !a.v) fatal("%s", e->name);
        return a;
    case E_FUNC:
        switch (e->op) {
        case F_ABSOLUTE: a = eval(e->a); return mkabs(a.v);
        case F_ADDR: case F_LOADADDR: case F_SIZEOF: case F_ALIGNOF: {
            OSec *os = osec_find(e->name);
            if (!os) { if (L.final) fatal("undefined section `%s' referenced in expression", e->name); return mkabs(0); }
            if (e->op == F_ADDR) return mkrel(os->addr, os);
            if (e->op == F_LOADADDR) return mkabs(os->lma);
            if (e->op == F_ALIGNOF) return mkabs(os->align);
            return mkabs(os->removed ? 0 : os->size);
        }
        case F_SIZEOF_HEADERS: return mkabs(welf_ehdr_size(L.is64) + (uint64_t)L.nphdrs_est * welf_phdr_size(L.is64));
        case F_ALIGN:
            if (e->b) { a = eval(e->a); b = eval(e->b); return a.sec ? mkrel(align_up(a.v, b.v), a.sec) : mkabs(align_up(a.v, b.v)); }
            a = eval(e->a);
            return mkrel(align_up(dot, a.v), section_for_dot());
        case F_BLOCK: a = eval(e->a); return mkrel(align_up(dot, a.v), section_for_dot());
        case F_DEFINED: { Symbol *sym = sym_lookup(e->name, 0); return mkabs(sym && sym->defined); }
        case F_MAX: a = eval(e->a); b = eval(e->b); return mkabs(a.v > b.v ? a.v : b.v);
        case F_MIN: a = eval(e->a); b = eval(e->b); return mkabs(a.v < b.v ? a.v : b.v);
        case F_LOG2CEIL: { a = eval(e->a); uint64_t v = a.v, r = 0; while ((1ULL << r) < v) r++; return mkabs(r); }
        case F_CONSTANT:
            if (!strcmp(e->name, "MAXPAGESIZE")) return mkabs(L.maxpage);
            if (!strcmp(e->name, "COMMONPAGESIZE")) return mkabs(0x1000);
            fatal("unknown CONSTANT(%s)", e->name);
        case F_DATA_SEGMENT_ALIGN: a = eval(e->a); return mkrel(align_up(dot, a.v), section_for_dot());
        case F_DATA_SEGMENT_END: return eval(e->a);
        case F_DATA_SEGMENT_RELRO_END: a = eval(e->a); b = eval(e->b); return mkrel(a.v + b.v, b.sec);
        case F_SEGMENT_START: return eval(e->a);
        default: fatal("unsupported linker script function");
        }
    }
    fatal("bad expression");
    return mkabs(0);
}

/* current absolute address of a symbol and the output section it is relative to */
static uint64_t symbol_address(Symbol *sym, OSec **secp)
{
    if (secp) *secp = NULL;
    if (sym->script) { if (secp) *secp = sym->osec; return sym->value; }
    if (sym->sec) {
        InSec *s = sym->sec;
        if (s->discarded || !s->out) { return 0; }
        if (secp) *secp = mg_carrier(s)->out;
        if (s->mg) return insec_addr(s) + merge_map(s, sym->value);
        return L.relocatable ? s->outoff + sym->value : insec_addr(s) + sym->value;
    }
    return sym->value; /* absolute */
}

/* ---------------------------------------------------------------- layout */
static int layout_changed;

/* symbols named in linker script expressions count as referenced (for PROVIDE) */
static void mark_expr_references(Expr *e)
{
    if (!e) return;
    if (e->kind == E_SYM) { Symbol *sym = sym_lookup(e->name, 1); sym->referenced = 1; }
    if (e->kind == E_FUNC && e->op == F_DEFINED) return;
    mark_expr_references(e->a); mark_expr_references(e->b); mark_expr_references(e->c);
}
static void mark_script_references(Stmt *st)
{
    for (; st; st = st->next) {
        if (st->expr && (st->kind == S_ASSIGN || st->kind == S_DOT_ASSIGN || st->kind == S_ASSERT || st->kind == S_DATA || st->kind == S_FILL)) mark_expr_references(st->expr);
        if (st->kind == S_OUTSEC) {
            OutSecDef *d = st->os;
            mark_expr_references(d->addr); mark_expr_references(d->at); mark_expr_references(d->align); mark_expr_references(d->fill);
            mark_script_references(d->children);
        }
    }
}

static void define_script_symbol(Stmt *st)
{
    Symbol *sym;
    Val v;
    int was_defined;
    uint64_t oldv; OSec *oldsec;
    if (st->provide) {
        sym = sym_lookup(st->name, 0);
        if (!sym) return;
        if (sym->defined && !sym->script) return;
        if (!sym->referenced && !sym->script) return;
    }
    sym = sym_lookup(st->name, 1);
    was_defined = sym->defined;
    oldv = sym->value; oldsec = sym->osec;
    if (st->op != OP_SET) {
        Val cur;
        OSec *sec;
        uint64_t cv = sym->defined ? symbol_address(sym, &sec) : 0;
        cur = sym->defined && sec ? mkrel(cv, sec) : mkabs(cv);
        v = eval(st->expr);
        switch (st->op) {
        case OP_ADD: v = cur.sec ? mkrel(cur.v + v.v, cur.sec) : v.sec ? mkrel(cur.v + v.v, v.sec) : mkabs(cur.v + v.v); break;
        case OP_SUB: v = (cur.sec && !v.sec) ? mkrel(cur.v - v.v, cur.sec) : mkabs(cur.v - v.v); break;
        case OP_MUL: v = mkabs(cur.v * v.v); break;
        case OP_DIV: v = mkabs(v.v ? cur.v / v.v : 0); break;
        case OP_LSH: v = mkabs(cur.v << v.v); break;
        case OP_RSH: v = mkabs(cur.v >> v.v); break;
        case OP_AND: v = mkabs(cur.v & v.v); break;
        case OP_OR: v = mkabs(cur.v | v.v); break;
        }
    } else {
        v = eval(st->expr);
    }
    if (!v.valid && !L.final) { /* unresolved yet: leave as is but mark defined so later refs work in pass 2 */
        if (!sym->defined) { sym->defined = 1; sym->script = 1; sym->value = 0; sym->osec = NULL; layout_changed = 1; }
        return;
    }
    if (sym->defined && !sym->script && !st->provide) {
        /* overriding a symbol defined in an object: becomes a script symbol */
    }
    sym->defined = 1; sym->script = 1; sym->common = 0;
    sym->value = v.v; sym->osec = v.sec; sym->sec = NULL; sym->obj = NULL;
    if (st->hidden) sym->vis = STV_HIDDEN;
    if (!was_defined) { sym->bind = STB_GLOBAL; sym->type = STT_NOTYPE; }
    /* `a = b` copies the ELF type of b */
    if (st->op == OP_SET && st->expr->kind == E_SYM) {
        Symbol *src = sym_lookup(st->expr->name, 0);
        if (src && src->defined && src != sym && !src->script) { sym->type = src->type; }
    }
    if (!was_defined || oldv != sym->value || oldsec != sym->osec) layout_changed = 1;
}

static int osec_advances(OSec *os)
{
    if (os->def && (os->def->sectype == OST_INFO || os->def->sectype == OST_DSECT)) return 0;
    if (os->insecs.n == 0) return 1;
    return (os->flags & SHF_ALLOC) != 0;
}

/* An orphan output section (no script definition: osec_new(name, NULL) in assign_sections,
   placed in L.osecs after the last section of its class) is laid out where it sits in the
   list: its inputs one after the other at their alignment, at dot when it is allocated,
   like GNU ld's orphan placement.  The relocatable (-r) auto script gives every section a
   definition, so this only serves -T scripts missing a section and the default scripts. */
static void layout_orphan(OSec *os, OSec **prev_alloc)
{
    InSec *s;
    uint64_t start, cur, old_addr = os->addr, old_size = os->size, old_lma = os->lma;
    if (os->removed || os->is_reloc_sec) return;
    os->align = 1;
    PARR_FOREACH(InSec *, s, os->insecs) if (s->g->addralign > os->align) os->align = s->g->addralign;
    start = os->alloc ? align_up(dot, os->align) : 0;
    os->addr = start;
    cur = start;
    PARR_FOREACH(InSec *, s, os->insecs) {
        uint64_t al;
        if (s->mg && !s->mg_first) { s->outoff = cur - start; continue; }
        al = s->g->addralign ? s->g->addralign : 1;
        cur = align_up(cur, al);
        s->outoff = cur - start;
        cur += s->size;
    }
    os->size = cur - start;
    if (*prev_alloc && (*prev_alloc)->lma != (*prev_alloc)->addr && os->alloc) os->lma = os->addr + ((*prev_alloc)->lma - (*prev_alloc)->addr);
    else os->lma = os->addr;
    if (os->alloc) {
        dot = start + os->size;
        if (os->flags & SHF_ALLOC) *prev_alloc = os;
    }
    if (os->addr != old_addr || os->size != old_size || os->lma != old_lma) layout_changed = 1;
}

/* the orphans that follow 'after' in L.osecs (NULL: the ones before the first scripted section) */
static void layout_orphans_after(OSec *after, OSec **prev_alloc)
{
    int i = 0;
    if (after) { for (i = 0; i < L.osecs.n && L.osecs.p[i] != after; i++) ; i++; }
    for (; i < L.osecs.n; i++) {
        OSec *os = L.osecs.p[i];
        if (os->def) break;
        layout_orphan(os, prev_alloc);
    }
}

static void layout_pass(void)
{
    OSec *os, *prev_alloc = NULL;
    Stmt *st;
    int first = 1;
    dot = 0; cur_os = NULL; in_section = 0;
    L.nphdrs_est = L.sc->nphdrs ? L.sc->nphdrs : L.nphdrs_est;
    PARR_FOREACH(OSec *, os, L.osecs) { os->datacmds.n = 0; }
    for (st = L.sc->stmts; st; st = st->next) {
        switch (st->kind) {
        case S_ASSIGN: define_script_symbol(st); break;
        case S_DOT_ASSIGN: {
            Val v = eval(st->expr);
            if (v.valid) {
                if (v.v < dot && L.final && 0) fatal("cannot move location counter backwards");
                dot = v.v;
            }
            break;
        }
        case S_ASSERT: if (L.final) { Val v = eval(st->expr); if (!v.v) fatal("%s", st->name); } break;
        case S_ENTRY: case S_NOP: break;
        case S_OUTSEC: {
            OutSecDef *d = st->os;
            uint64_t start, cur, old_addr, old_size, old_lma, dot_before = dot;
            InSec *s;
            os = NULL;
            PARR_FOREACH(OSec *, os, L.osecs) if (os->def == d) break;
            if (!os) break;
            if (first) { first = 0; layout_orphans_after(NULL, &prev_alloc); }
            if (os->removed && !d->discard) {
                /* evaluate assignments inside for symbol definitions (fixed up later) */
                OSec *saved = cur_os; cur_os = os; in_section = 1;
                for (Stmt *c = d->children; c; c = c->next) if (c->kind == S_ASSIGN) define_script_symbol(c);
                in_section = 0; cur_os = os; (void)saved;
                break;
            }
            if (d->discard) break;
            old_addr = os->addr; old_size = os->size; old_lma = os->lma;
            /* alignment */
            os->align = 1;
            PARR_FOREACH(InSec *, s, os->insecs) if (s->g->addralign > os->align) os->align = s->g->addralign;
            if (d->align) { Val v = eval(d->align); if (v.v > os->align) os->align = v.v; }
            if (d->addr) { Val v = eval(d->addr); start = v.valid ? v.v : dot; }
            else start = align_up(dot, os->align);
            /* non-alloc sections and INFO/NOLOAD do not consume address space the same way */
            os->addr = start;
            cur_os = os; in_section = 1; dot = start;
            os->fill_set = 0;
            if (d->fill) { Val v = eval(d->fill); os->fill_set = 1; os->fill = v.v; }
            cur = start;
            for (Stmt *c = d->children; c; c = c->next) {
                switch (c->kind) {
                case S_INPUT:
                    PARR_FOREACH(InSec *, s, os->insecs) {
                        if (s->spec != c->spec) continue;
                        if (s->mg && !s->mg_first) { s->outoff = cur - start; continue; }
                        {
                            uint64_t al = s->g->addralign ? s->g->addralign : 1;
                            if (d->subalign) { Val v = eval(d->subalign); if (v.v > al) al = v.v; }
                            cur = align_up(cur, al);
                            s->outoff = cur - start;
                            cur += s->size;
                        }
                    }
                    dot = cur;
                    break;
                case S_ASSIGN: dot = cur; define_script_symbol(c); break;
                case S_DOT_ASSIGN: {
                    Val v;
                    dot = cur;
                    v = eval(c->expr);
                    if (v.valid) {
                        uint64_t nd = v.sec ? v.v : start + v.v;
                        if (nd < cur && L.final) fatal("%s: cannot move location counter backwards (from 0x%llx to 0x%llx)", os->name, (unsigned long long)cur, (unsigned long long)nd);
                        cur = nd; dot = cur;
                    }
                    break;
                }
                case S_DATA: {
                    DataCmd *dc = xcalloc(1, sizeof *dc);
                    dc->off = cur - start; dc->size = c->size; dc->e = c->expr;
                    parr_add(&os->datacmds, dc);
                    cur += c->size; dot = cur;
                    break;
                }
                case S_FILL: { Val v = eval(c->expr); os->fill_set = 1; os->fill = v.v; break; }
                case S_ASSERT: if (L.final) { Val v = eval(c->expr); if (!v.v) fatal("%s", c->name); } break;
                default: break;
                }
            }
            os->size = cur - start;
            in_section = 0;
            /* LMA */
            if (d->at) { Val v = eval(d->at); os->lma = v.valid ? v.v : os->addr; os->has_at = 1; }
            else if (prev_alloc && prev_alloc->lma != prev_alloc->addr && osec_advances(os)) os->lma = os->addr + (prev_alloc->lma - prev_alloc->addr);
            else os->lma = os->addr;
            /* does this section advance dot? */
            if (osec_advances(os)) {
                dot = start + os->size;
                if (os->flags & SHF_ALLOC) prev_alloc = os;
            } else {
                dot = dot_before;
            }
            if (os->addr != old_addr || os->size != old_size || os->lma != old_lma) layout_changed = 1;
            layout_orphans_after(os, &prev_alloc);
            break;
        }
        default: break;
        }
    }
    if (first) layout_orphans_after(NULL, &prev_alloc);
}

/* flags/type of each output section from its inputs */
static void compute_osec_flags(void)
{
    OSec *os;
    PARR_FOREACH(OSec *, os, L.osecs) {
        InSec *s;
        uint64_t flags = 0; int all_nobits = 1, any = 0, merge_ok = 1; uint64_t entsize = 0, mflags = 0;
        uint32_t first_type = SHT_PROGBITS, first_type_nonempty = 0; int have_nonempty = 0;
        if (os->is_reloc_sec) continue;
        PARR_FOREACH(InSec *, s, os->insecs) {
            GSec *g = s->g;
            flags |= g->flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR | SHF_TLS);
            if (!any) first_type = g->type;
            if (g->size && !have_nonempty) { have_nonempty = 1; first_type_nonempty = g->type; }
            if (g->type != SHT_NOBITS && g->size) all_nobits = 0;
            if (!any) { entsize = g->entsize; mflags = g->flags & (SHF_MERGE | SHF_STRINGS); }
            else if (entsize != g->entsize || mflags != (g->flags & (SHF_MERGE | SHF_STRINGS))) merge_ok = 0;
            any = 1;
        }
        if (any) {
            if (merge_ok && (mflags & SHF_MERGE)) { flags |= mflags; os->entsize = entsize; }
            /* type: NOBITS if nothing has contents, otherwise the first (non-empty) input's type */
            if (all_nobits) os->type = have_nonempty ? SHT_NOBITS : (first_type == SHT_NOBITS ? SHT_NOBITS : first_type);
            else {
                uint32_t t = have_nonempty ? first_type_nonempty : first_type;
                os->type = t == SHT_NOBITS ? SHT_PROGBITS : t;
            }
            if (os->type != SHT_PROGBITS && os->type != SHT_NOBITS && os->type != SHT_NOTE && os->type != SHT_INIT_ARRAY && os->type != SHT_FINI_ARRAY && os->type != SHT_PREINIT_ARRAY) {
                /* linker-created dynamic sections keep their own type */
                InSec *f = os->insecs.p[0];
                if (!f->linker_created) os->type = SHT_PROGBITS;
            }
            if (os->insecs.n == 1 && ((InSec *)os->insecs.p[0])->linker_created) os->entsize = ((InSec *)os->insecs.p[0])->g->entsize;
            if (L.relocatable && !strcmp(os->name, ".note.GNU-stack")) os->type = SHT_NOTE;   /* bfd quirk */
            if (os->def && os->def->sectype == OST_NOLOAD) os->type = SHT_NOBITS;
        } else {
            os->type = SHT_PROGBITS;
            flags = SHF_WRITE;
            if (os->def && os->def->sectype == OST_NOLOAD) os->type = SHT_NOBITS;
        }
        if (os->def && (os->def->sectype == OST_INFO || os->def->sectype == OST_DSECT)) flags &= ~SHF_ALLOC;
        os->flags = flags;
        os->alloc = (flags & SHF_ALLOC) != 0;
        if (!any && os->def && os->def->sectype != OST_INFO) os->alloc = 1; /* empty script sections still take dot */
        if (os->def && os->def->addr == NULL && !any) os->alloc = 1;
        os->is_debug = is_debug_name(os->name);
    }
}

static void remove_empty_sections(void)
{
    OSec *os;
    PARR_FOREACH(OSec *, os, L.osecs) {
        int keep = 0;
        InSec *s;
        if (os->removed || os->is_reloc_sec) continue;
        if (os->size) continue;
        if (os->has_dot_assign) keep = 1;
        PARR_FOREACH(InSec *, s, os->insecs) { if (s->keep || s->linker_created) keep = 1; }
        if (L.relocatable && os->insecs.n) keep = 1;
        if (!keep) os->removed = 1;
        else if (!os->insecs.n) {
            /* kept empty section: flags like GNU ld */
            os->flags = SHF_WRITE | (os->first_is_dot_assign ? 0 : SHF_ALLOC);
            os->type = SHT_PROGBITS;
        }
    }
}

/* ---------------------------------------------------------------- relocations */
static int reloc_width(uint32_t type)
{
    if (L.machine == EM_X86_64) {
        switch (type) {
        case R_X86_64_64: case R_X86_64_PC64: return 8;
        case R_X86_64_8: case R_X86_64_PC8: return 1;
        case R_X86_64_16: case R_X86_64_PC16: return 2;
        case R_X86_64_NONE: return 0;
        default: return 4;
        }
    } else {
        switch (type) {
        case R_386_8: case R_386_PC8: return 1;
        case R_386_16: case R_386_PC16: return 2;
        case R_386_NONE: return 0;
        default: return 4;
        }
    }
}

static const char *reloc_name(uint32_t type)
{
    static char buf[32];
    if (L.machine == EM_X86_64) {
        switch (type) {
        case R_X86_64_NONE: return "R_X86_64_NONE"; case R_X86_64_64: return "R_X86_64_64"; case R_X86_64_PC32: return "R_X86_64_PC32";
        case R_X86_64_GOT32: return "R_X86_64_GOT32"; case R_X86_64_PLT32: return "R_X86_64_PLT32"; case R_X86_64_GOTPCREL: return "R_X86_64_GOTPCREL";
        case R_X86_64_32: return "R_X86_64_32"; case R_X86_64_32S: return "R_X86_64_32S"; case R_X86_64_16: return "R_X86_64_16";
        case R_X86_64_PC16: return "R_X86_64_PC16"; case R_X86_64_8: return "R_X86_64_8"; case R_X86_64_PC8: return "R_X86_64_PC8";
        case R_X86_64_PC64: return "R_X86_64_PC64"; case R_X86_64_GOTPCRELX: return "R_X86_64_GOTPCRELX"; case R_X86_64_REX_GOTPCRELX: return "R_X86_64_REX_GOTPCRELX";
        case R_X86_64_GOTOFF64: return "R_X86_64_GOTOFF64"; case R_X86_64_GOTPC32: return "R_X86_64_GOTPC32";
        }
    } else {
        switch (type) {
        case R_386_NONE: return "R_386_NONE"; case R_386_32: return "R_386_32"; case R_386_PC32: return "R_386_PC32";
        case R_386_GOT32: return "R_386_GOT32"; case R_386_PLT32: return "R_386_PLT32"; case R_386_GOTOFF: return "R_386_GOTOFF";
        case R_386_GOTPC: return "R_386_GOTPC"; case R_386_16: return "R_386_16"; case R_386_PC16: return "R_386_PC16";
        case R_386_8: return "R_386_8"; case R_386_PC8: return "R_386_PC8"; case R_386_GOT32X: return "R_386_GOT32X";
        }
    }
    snprintf(buf, sizeof buf, "reloc type %u", type);
    return buf;
}

static int reloc_errors;
static void reloc_error(InSec *s, uint64_t off, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: %s:(%s+0x%llx): ", progname, s->obj->name, s->g->name, (unsigned long long)off);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    reloc_errors++;
}

/* value (S) of the symbol referenced by relocation r of object o; *insecp = defining input section or NULL */
static int resolve_reloc_symbol(InSec *s, GRel *r, uint64_t *S, Symbol **symp, InSec **defsec, int *is_section_sym)
{
    Obj *o = s->obj;
    GSym *gs;
    *symp = NULL; *defsec = NULL; *is_section_sym = 0; *S = 0;
    if (r->sym >= (uint32_t)o->e->nsyms) { reloc_error(s, r->offset, "bad symbol index %u", r->sym); return 0; }
    gs = &o->e->syms[r->sym];
    if (r->sym == 0) { return 1; }
    if (ELF32_ST_BIND(gs->info) == STB_LOCAL) {
        InSec *ds;
        if (gs->shndx == SHN_ABS) { *S = gs->value; return 1; }
        ds = sec_of_sym(o, gs);
        if (!ds) { *S = gs->value; return 1; }
        if (ds->discarded || !ds->out || ds->out->removed) {
            if (!(s->g->flags & SHF_ALLOC)) { *S = 0; return 1; }
            if (ds->discarded) { reloc_error(s, r->offset, "`%s' referenced in section `%s' of %s: defined in discarded section `%s' of %s", gs->name, s->g->name, o->name, ds->g->name, o->name); return 0; }
            *S = 0; return 1;
        }
        *defsec = ds;
        *is_section_sym = ELF32_ST_TYPE(gs->info) == STT_SECTION;
        if (*is_section_sym) *S = insec_addr(ds);   /* addend holds the offset (mapped by the caller if merged) */
        else *S = ds->mg ? insec_addr(ds) + merge_map(ds, gs->value) : insec_addr(ds) + gs->value;
        return 1;
    } else {
        Symbol *sym = o->symmap[r->sym];
        if (!sym) { *S = gs->value; return 1; }
        *symp = sym;
        sym->reloc_referenced = 1;
        if ((L.machine == EM_X86_64 && r->type == R_X86_64_PLT32) || (L.machine == EM_386 && r->type == R_386_PLT32)) sym->plt_referenced = 1;
        if (!sym->defined) {
            if (sym->weak || !sym->referenced_strong) { *S = 0; return 1; }
            reloc_error(s, r->offset, "undefined reference to `%s'", sym->name);
            return 0;
        }
        if (sym->sec && (sym->sec->discarded || !sym->sec->out)) { *S = 0; return 1; }
        *defsec = sym->sec;
        *S = symbol_address(sym, NULL);
        return 1;
    }
}

static int fits_signed(int64_t v, int bits) { int64_t lim = (int64_t)1 << (bits - 1); return v >= -lim && v < lim; }
static int fits_unsigned_or_signed(int64_t v, int bits) { return fits_signed(v, bits) || (v >= 0 && (uint64_t)v < ((uint64_t)1 << bits)); }

static void apply_relocs(InSec *s, uint8_t *out)
{
    GSec *rs = s->relsec;
    for (int i = 0; i < rs->nrels; i++) {
        GRel *r = &rs->rels[i];
        uint64_t S, P, A = (uint64_t)r->addend, V = 0;
        Symbol *sym; InSec *ds; int secsym;
        uint8_t *f;
        int w = reloc_width(r->type);
        if (r->offset + w > s->g->size) { reloc_error(s, r->offset, "relocation offset out of range"); continue; }
        if (!resolve_reloc_symbol(s, r, &S, &sym, &ds, &secsym)) continue;
        if (secsym && ds && ds->mg) { uint64_t A0 = A; A = merge_map(ds, A); if (getenv("TLD_DEBUG")) fprintf(stderr, "merge-map %s %s+%llx -> %llx (S=%llx carrier=%s first=%d)\n", ds->obj->name, ds->g->name, (unsigned long long)A0, (unsigned long long)A, (unsigned long long)S, mg_carrier(ds)->obj->name, ds->mg_first); }
        f = out + (s->mg ? merge_map(s, r->offset) : r->offset);
        P = insec_addr(s) + (s->mg ? merge_map(s, r->offset) : r->offset);
        if (L.machine == EM_X86_64) {
            switch (r->type) {
            case R_X86_64_NONE: continue;
            case R_X86_64_64: V = S + A; if (L.pie && ds && !L.shared) { reloc_error(s, r->offset, "%s against `%s' cannot be used when making a PIE object (no run-time relocations)", reloc_name(r->type), sym ? sym->name : ds->g->name); continue; } break;
            case R_X86_64_32: V = S + A; if (L.pie && ds) { reloc_error(s, r->offset, "%s against `%s' cannot be used when making a PIE object", reloc_name(r->type), sym ? sym->name : ds->g->name); continue; }
                if (V > 0xffffffffULL) { reloc_error(s, r->offset, "relocation truncated to fit: %s against `%s'", reloc_name(r->type), sym ? sym->name : ds ? ds->g->name : "*ABS*"); continue; } break;
            case R_X86_64_32S: V = S + A; if (L.pie && ds) { reloc_error(s, r->offset, "%s against `%s' cannot be used when making a PIE object", reloc_name(r->type), sym ? sym->name : ds->g->name); continue; }
                if (!fits_signed((int64_t)V, 32)) { reloc_error(s, r->offset, "relocation truncated to fit: %s against `%s'", reloc_name(r->type), sym ? sym->name : ds ? ds->g->name : "*ABS*"); continue; } break;
            case R_X86_64_PC32: case R_X86_64_PLT32:
                V = S + A - P;
                if (!fits_signed((int64_t)V, 32)) { reloc_error(s, r->offset, "relocation truncated to fit: %s against `%s'", reloc_name(r->type), sym ? sym->name : ds ? ds->g->name : "*ABS*"); continue; } break;
            case R_X86_64_PC64: V = S + A - P; break;
            case R_X86_64_16: V = S + A; if (!fits_unsigned_or_signed((int64_t)V, 16)) { reloc_error(s, r->offset, "relocation truncated to fit: %s", reloc_name(r->type)); continue; } break;
            case R_X86_64_8: V = S + A; if (!fits_unsigned_or_signed((int64_t)V, 8)) { reloc_error(s, r->offset, "relocation truncated to fit: %s", reloc_name(r->type)); continue; } break;
            case R_X86_64_PC16: V = S + A - P; if (!fits_signed((int64_t)V, 16)) { reloc_error(s, r->offset, "relocation truncated to fit: %s", reloc_name(r->type)); continue; } break;
            case R_X86_64_PC8: V = S + A - P; if (!fits_signed((int64_t)V, 8)) { reloc_error(s, r->offset, "relocation truncated to fit: %s", reloc_name(r->type)); continue; } break;
            case R_X86_64_GOTPCREL: case R_X86_64_GOTPCRELX: case R_X86_64_REX_GOTPCRELX: {
                /* relax: the GOT entry would hold S; turn the GOT load into a direct reference */
                uint64_t fo = f - out;
                uint8_t *opc = f - 2;
                if (fo < 2) { reloc_error(s, r->offset, "GOTPCREL relocation at the start of a section"); continue; }
                if (opc[0] == 0x8b && (opc[1] & 0xc7) == 0x05) {
                    int rex = (fo >= 3 && (opc[-1] & 0xf0) == 0x40) ? opc[-1] : 0;
                    if (!L.pie && !L.shared && r->type != R_X86_64_GOTPCREL && (rex & 8 ? fits_signed((int64_t)(S + A + 4), 32) : (S + A + 4) <= 0xffffffffULL)) {
                        /* mov sym@GOTPCREL(%rip), %reg -> mov $sym, %reg (R_X86_64_32S / R_X86_64_32) */
                        opc[0] = 0xc7; opc[1] = 0xc0 | ((opc[1] >> 3) & 7);
                        if (rex) opc[-1] = (rex & ~4) | ((rex & 4) ? 1 : 0);
                        V = S + A + 4;
                        wr32(f, (uint32_t)V);
                        continue;
                    }
                    opc[0] = 0x8d;                                        /* -> lea sym(%rip), %reg */
                } else if (opc[0] == 0xff && opc[1] == 0x15) {           /* call *sym@GOTPCREL(%rip) -> addr32 call sym */
                    opc[0] = 0x67; opc[1] = 0xe8;
                } else if (opc[0] == 0xff && opc[1] == 0x25) {           /* jmp *sym@GOTPCREL(%rip) -> jmp sym; nop */
                    opc[0] = 0xe9; f[4] = 0x90;
                    /* the displacement field moves one byte earlier */
                    V = S + A - (P - 1);
                    if (!fits_signed((int64_t)V, 32)) { reloc_error(s, r->offset, "relocation truncated to fit: %s", reloc_name(r->type)); continue; }
                    wr32(f - 1, (uint32_t)V);
                    continue;
                } else if (r->type != R_X86_64_GOTPCREL && (opc[0] == 0x85 || (opc[0] >= 0x01 && opc[0] <= 0x3b && (opc[0] & 7) == 3)) && (opc[1] & 0xc7) == 0x05) {
                    /* test/binop sym@GOTPCREL(%rip), %reg -> op $sym, %reg (needs the address to fit in 32 bits) */
                    int reg = (opc[1] >> 3) & 7;
                    if (!fits_signed((int64_t)(S + A + 4), 32)) { reloc_error(s, r->offset, "cannot relax %s: address does not fit in 32 bits", reloc_name(r->type)); continue; }
                    if (opc[0] == 0x85) { opc[0] = 0xf7; opc[1] = 0xc0 | reg; }
                    else { int op = opc[0] >> 3; opc[0] = 0x81; opc[1] = 0xc0 | (op << 3) | reg; }
                    V = S + A + 4;  /* the addend of GOTPCREL is -4 for the PC adjustment */
                    wr32(f, (uint32_t)V);
                    continue;
                } else {
                    reloc_error(s, r->offset, "unsupported instruction for %s relaxation (no GOT is created)", reloc_name(r->type));
                    continue;
                }
                V = S + A - P;
                if (!fits_signed((int64_t)V, 32)) { reloc_error(s, r->offset, "relocation truncated to fit: %s against `%s'", reloc_name(r->type), sym ? sym->name : "?"); continue; }
                break;
            }
            default:
                reloc_error(s, r->offset, "unsupported relocation type %s", reloc_name(r->type));
                continue;
            }
        } else {
            switch (r->type) {
            case R_386_NONE: continue;
            case R_386_32: V = S + A; break;
            case R_386_PC32: case R_386_PLT32: V = S + A - P; break;
            case R_386_16: V = S + A; if (!fits_unsigned_or_signed((int64_t)(int32_t)V, 16)) { reloc_error(s, r->offset, "relocation truncated to fit: %s against `%s'", reloc_name(r->type), sym ? sym->name : "?"); continue; } break;
            case R_386_8: V = S + A; if (!fits_unsigned_or_signed((int64_t)(int32_t)V, 8)) { reloc_error(s, r->offset, "relocation truncated to fit: %s", reloc_name(r->type)); continue; } break;
            case R_386_PC16: V = S + A - P; if (!fits_signed((int64_t)(int32_t)V, 16)) { reloc_error(s, r->offset, "relocation truncated to fit: %s", reloc_name(r->type)); continue; } break;
            case R_386_PC8: V = S + A - P; if (!fits_signed((int64_t)(int32_t)V, 8)) { reloc_error(s, r->offset, "relocation truncated to fit: %s", reloc_name(r->type)); continue; } break;
            default:
                reloc_error(s, r->offset, "unsupported relocation type %s", reloc_name(r->type));
                continue;
            }
        }
        switch (w) {
        case 8: wr64(f, V); break;
        case 4: wr32(f, (uint32_t)V); break;
        case 2: wr16(f, (uint16_t)V); break;
        case 1: f[0] = (uint8_t)V; break;
        }
    }
}

/* ---------------------------------------------------------------- dynamic sections (-shared) */
static int version_index_for(Symbol *sym)
{
    /* returns 0 = not exported (local), or 2+ version node index; 1 = global unversioned */
    Script *sc = L.sc;
    int has_local_star = 0;
    if (!sc->nversions) return 1;
    for (int v = 0; v < sc->nversions; v++) {
        VersionNode *vn = sc->versions[v];
        for (int i = 0; i < vn->nglobals; i++) if (glob_match(vn->globals[i], sym->name)) return v + 2;
    }
    for (int v = 0; v < sc->nversions; v++) {
        VersionNode *vn = sc->versions[v];
        for (int i = 0; i < vn->nlocals; i++) if (glob_match(vn->locals[i], sym->name)) { has_local_star = 1; }
    }
    return has_local_star ? 0 : 1;
}

static int cmp_dynsym_bucket(const void *a, const void *b)
{
    Symbol *x = *(Symbol * const *)a, *y = *(Symbol * const *)b;
    uint32_t hx = gnu_hash(x->name) % L.gnuhash_nbuckets_tmp, hy = gnu_hash(y->name) % L.gnuhash_nbuckets_tmp;
    if (hx != hy) return hx < hy ? -1 : 1;
    return x->dynidx - y->dynidx;
}

static int bucket_count(int nsyms)
{
    static const int tab[] = { 1, 3, 17, 37, 67, 97, 131, 197, 263, 521, 1031, 2053, 4099, 8209, 16411, 32771, 0 };
    int best = 1;
    for (int i = 0; tab[i]; i++) if (nsyms >= tab[i]) best = tab[i];
    return best;
}

static void create_dynamic_sections(void)
{
    Symbol *sym;
    int n = 0, nbuckets, i, maskbitslog2, maskwords;
    if (!L.shared) return;
    strtab_init(&L.dynstr);
    PARR_FOREACH(Symbol *, sym, L.symlist) {
        if (!sym->defined || sym->bind == STB_LOCAL || sym->vis == STV_HIDDEN || sym->vis == STV_INTERNAL) continue;
        if (sym->type == STT_SECTION || sym->type == STT_FILE) continue;
        {
            int vi = version_index_for(sym);
            if (vi == 0) continue;
            sym->versym = (uint16_t)vi;
            sym->dynamic_export = 1;
            sym->dynidx = n++;
            parr_add(&L.dynsyms, sym);
        }
    }
    /* version definition symbols (GNU adds an absolute symbol per version node) */
    for (i = 0; i < L.sc->nversions; i++) {
        VersionNode *vn = L.sc->versions[i];
        Symbol *vs = sym_lookup(vn->name, 1);
        if (!vs->defined) {
            vs->defined = 1; vs->script = 1; vs->value = 0; vs->osec = NULL; vs->bind = STB_GLOBAL; vs->type = STT_OBJECT;
        }
        vs->versym = (uint16_t)(i + 2); vs->dynamic_export = 1; vs->dynidx = n++;
        parr_add(&L.dynsyms, vs);
    }
    nbuckets = bucket_count(n ? n : 1);
    L.gnuhash_nbuckets_tmp = nbuckets;
    qsort(L.dynsyms.p, L.dynsyms.n, sizeof(void *), cmp_dynsym_bucket);
    for (i = 0; i < L.dynsyms.n; i++) ((Symbol *)L.dynsyms.p[i])->dynidx = i + 1;
    if (L.soname) L.dynstr_soname_off = strtab_add(&L.dynstr, L.soname);
    PARR_FOREACH(Symbol *, sym, L.dynsyms) sym->dynstr_off = strtab_add(&L.dynstr, sym->name);
    for (i = 0; i < L.sc->nversions; i++) L.sc->versions[i]->dynstr_off = strtab_add(&L.dynstr, L.sc->versions[i]->name);
    /* sizes */
    L.dynsym_sec = add_synthetic_section(".dynsym", SHT_DYNSYM, SHF_ALLOC, L.is64 ? 8 : 4, welf_sym_size(L.is64), (uint64_t)(n + 1) * welf_sym_size(L.is64));
    L.dynstr_sec = add_synthetic_section(".dynstr", SHT_STRTAB, SHF_ALLOC, 1, 0, L.dynstr.len);
    memcpy(L.dynstr_sec->g->data, L.dynstr.buf, L.dynstr.len);
    L.hash_sec = add_synthetic_section(".hash", SHT_HASH, SHF_ALLOC, L.is64 ? 8 : 4, 4, 8 + 4ULL * nbuckets + 4ULL * (n + 1));
    maskbitslog2 = 0; while ((1 << maskbitslog2) < nbuckets) maskbitslog2++; maskbitslog2++;
    if (maskbitslog2 < 3) maskbitslog2 = 5;
    else if ((1 << (maskbitslog2 - 2)) & nbuckets) maskbitslog2 += 3;
    else maskbitslog2 += 2;
    if (L.is64 && maskbitslog2 < 6) maskbitslog2 = 6;
    maskwords = 1 << (maskbitslog2 - (L.is64 ? 6 : 5));
    L.gnuhash_maskwords = maskwords; L.gnuhash_shift = maskbitslog2;
    L.gnuhash_sec = add_synthetic_section(".gnu.hash", SHT_GNU_HASH, SHF_ALLOC, L.is64 ? 8 : 4, 0, 16 + (uint64_t)maskwords * (L.is64 ? 8 : 4) + 4ULL * nbuckets + 4ULL * n);
    if (L.sc->nversions) {
        L.versym_sec = add_synthetic_section(".gnu.version", SHT_GNU_versym, SHF_ALLOC, 2, 2, 2ULL * (n + 1));
        L.verdef_sec = add_synthetic_section(".gnu.version_d", SHT_GNU_verdef, SHF_ALLOC, L.is64 ? 8 : 4, 0, 28ULL * (L.sc->nversions + 1));
    }
    {
        int ndyn = 1 + (L.soname ? 1 : 0) + (L.bsymbolic ? 2 : 0) + 6 + (L.sc->nversions ? 3 : 0) + 5 /* GNU-like spare */;
        L.dynamic_sec = add_synthetic_section(".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, L.is64 ? 8 : 4, L.is64 ? 16 : 8, (uint64_t)ndyn * (L.is64 ? 16 : 8));
    }
    /* _DYNAMIC */
    {
        Symbol *d = sym_lookup("_DYNAMIC", 1);
        d->defined = 1; d->script = 0; d->sec = L.dynamic_sec; d->value = 0; d->bind = STB_LOCAL; d->type = STT_OBJECT; d->linker_local = 1;
    }
}

static void fill_dynamic_sections(void)
{
    Symbol *sym;
    int n = L.dynsyms.n, i, nbuckets = L.gnuhash_nbuckets_tmp;
    uint8_t *p;
    if (!L.shared) return;
    /* .dynsym */
    p = L.dynsym_sec->g->data;
    PARR_FOREACH(Symbol *, sym, L.dynsyms) {
        GSym gs; OSec *sec = NULL;
        memset(&gs, 0, sizeof gs);
        gs.name = (char *)(uintptr_t)sym->dynstr_off;
        gs.value = symbol_address(sym, &sec);
        gs.size = sym->size;
        gs.info = (unsigned char)ELF32_ST_INFO(sym->bind == STB_WEAK ? STB_WEAK : STB_GLOBAL, sym->type);
        gs.other = (unsigned char)sym->vis;
        gs.shndx = (sec && !sec->removed && sec->idx) ? (uint16_t)sec->idx : SHN_ABS;
        welf_put_sym(L.is64, p + (size_t)sym->dynidx * welf_sym_size(L.is64), &gs);
    }
    /* .hash */
    p = L.hash_sec->g->data;
    wr32(p, nbuckets); wr32(p + 4, n + 1);
    {
        uint32_t *buckets = (uint32_t *)(p + 8), *chains = (uint32_t *)(p + 8 + 4 * nbuckets);
        for (i = 0; i < nbuckets; i++) wr32((uint8_t *)&buckets[i], 0);
        for (i = 0; i <= n; i++) wr32((uint8_t *)&chains[i], 0);
        PARR_FOREACH(Symbol *, sym, L.dynsyms) {
            uint32_t h = elf_hash(sym->name) % nbuckets;
            uint32_t idx = (uint32_t)sym->dynidx;
            /* insert at the head of the chain */
            uint32_t head = rd32((uint8_t *)&buckets[h]);
            wr32((uint8_t *)&chains[idx], head);
            wr32((uint8_t *)&buckets[h], idx);
        }
    }
    /* .gnu.hash */
    p = L.gnuhash_sec->g->data;
    {
        int maskwords = L.gnuhash_maskwords, shift = L.gnuhash_shift, wsz = L.is64 ? 8 : 4, c = L.is64 ? 64 : 32;
        uint8_t *bloom = p + 16, *buckets = bloom + maskwords * wsz, *chains = buckets + 4 * nbuckets;
        wr32(p, nbuckets); wr32(p + 4, 1); wr32(p + 8, maskwords); wr32(p + 12, shift);
        memset(bloom, 0, maskwords * wsz);
        memset(buckets, 0, 4 * nbuckets);
        for (i = 1; i <= n; i++) {
            Symbol *s2 = L.dynsyms.p[i - 1];
            uint32_t h = gnu_hash(s2->name);
            uint32_t b = h % nbuckets;
            uint64_t word = (h / c) & (maskwords - 1);
            uint64_t mask = (1ULL << (h % c)) | (1ULL << ((h >> shift) % c));
            if (L.is64) wr64(bloom + word * 8, rd64(bloom + word * 8) | mask);
            else wr32(bloom + word * 4, (uint32_t)(rd32(bloom + word * 4) | mask));
            if (rd32(buckets + 4 * b) == 0) wr32(buckets + 4 * b, (uint32_t)i);
            /* chain value: hash with low bit = end of chain */
            {
                int last = (i == n) || (gnu_hash(((Symbol *)L.dynsyms.p[i])->name) % nbuckets != b);
                wr32(chains + 4 * (i - 1), (h & ~1u) | (last ? 1 : 0));
            }
        }
    }
    /* versions */
    if (L.versym_sec) {
        p = L.versym_sec->g->data;
        wr16(p, 0);
        PARR_FOREACH(Symbol *, sym, L.dynsyms) wr16(p + 2 * sym->dynidx, sym->versym);
        p = L.verdef_sec->g->data;
        for (i = 0; i <= L.sc->nversions; i++) {
            uint8_t *vd = p + 28 * i, *va = vd + 20;
            const char *name = i == 0 ? (L.soname ? L.soname : L.output) : L.sc->versions[i - 1]->name;
            uint32_t nameoff = i == 0 ? (L.soname ? L.dynstr_soname_off : 0) : L.sc->versions[i - 1]->dynstr_off;
            wr16(vd, 1); wr16(vd + 2, i == 0 ? 1 : 0); wr16(vd + 4, (uint16_t)(i + 1)); wr16(vd + 6, 1);
            wr32(vd + 8, elf_hash(name)); wr32(vd + 12, 20); wr32(vd + 16, i == L.sc->nversions ? 0 : 28);
            wr32(va, nameoff); wr32(va + 4, 0);
        }
    }
    /* .dynamic */
    {
        int esz = L.is64 ? 16 : 8, k = 0;
        p = L.dynamic_sec->g->data;
#define DYN(tag, val) do { if (L.is64) { wr64(p + k * esz, tag); wr64(p + k * esz + 8, val); } else { wr32(p + k * esz, (uint32_t)(tag)); wr32(p + k * esz + 4, (uint32_t)(val)); } k++; } while (0)
        if (L.soname) DYN(DT_SONAME, L.dynstr_soname_off);
        if (L.bsymbolic) DYN(DT_SYMBOLIC, 0);
        DYN(DT_HASH, insec_addr(L.hash_sec));
        DYN(DT_GNU_HASH, insec_addr(L.gnuhash_sec));
        DYN(DT_STRTAB, insec_addr(L.dynstr_sec));
        DYN(DT_SYMTAB, insec_addr(L.dynsym_sec));
        DYN(DT_STRSZ, L.dynstr.len);
        DYN(DT_SYMENT, welf_sym_size(L.is64));
        if (L.versym_sec) { DYN(DT_VERDEF, insec_addr(L.verdef_sec)); DYN(DT_VERDEFNUM, L.sc->nversions + 1); }
        if (L.bsymbolic) DYN(DT_FLAGS, DF_SYMBOLIC);
        if (L.versym_sec) DYN(DT_VERSYM, insec_addr(L.versym_sec));
        DYN(DT_NULL, 0);
#undef DYN
    }
}

/* ---------------------------------------------------------------- output */
static void nop_fill(uint8_t *p, uint64_t n)
{
    static const uint8_t nop1[] = { 0x90 }, nop2[] = { 0x66, 0x90 }, nop3[] = { 0x0f, 0x1f, 0x00 }, nop4[] = { 0x0f, 0x1f, 0x40, 0x00 },
        nop5[] = { 0x0f, 0x1f, 0x44, 0x00, 0x00 }, nop6[] = { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 }, nop7[] = { 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00 },
        nop8[] = { 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 }, nop9[] = { 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
        nop10[] = { 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t *const nops[] = { nop1, nop2, nop3, nop4, nop5, nop6, nop7, nop8, nop9, nop10 };
    if (L.machine == EM_X86_64) {
        /* measured from GNU ld 2.47: 10-byte nops, then one shorter nop for the remainder */
        while (n >= 10) { memcpy(p, nop10, 10); p += 10; n -= 10; }
        if (n) memcpy(p, nops[n - 1], n);
    } else {
        while (n >= 2) { p[0] = 0x66; p[1] = 0x90; p += 2; n -= 2; }
        if (n) p[0] = 0x90;
    }
}

static int cmp_osec_order(const void *a, const void *b) { return (*(OSec * const *)a)->order - (*(OSec * const *)b)->order; }

static int is_c_ident(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}

static void provide_start_stop(void)
{
    OSec *os;
    if (L.relocatable) return;
    PARR_FOREACH(OSec *, os, L.osecs) {
        char *n1, *n2; Symbol *a, *b;
        if (os->removed || os->is_reloc_sec || !is_c_ident(os->name)) continue;
        n1 = xasprintf("__start_%s", os->name); n2 = xasprintf("__stop_%s", os->name);
        a = sym_lookup(n1, 0); b = sym_lookup(n2, 0);
        if (a && !a->defined && a->referenced) { a->defined = 1; a->script = 1; a->value = os->addr; a->osec = os; a->bind = STB_GLOBAL; a->vis = STV_PROTECTED; a->startstop = 1; layout_changed = 1; }
        if (b && !b->defined && b->referenced) { b->defined = 1; b->script = 1; b->value = os->addr + os->size; b->osec = os; b->bind = STB_GLOBAL; b->vis = STV_PROTECTED; b->startstop = 1; layout_changed = 1; }
        if (a && a->startstop) { a->value = os->addr; a->osec = os; }
        if (b && b->startstop) { b->value = os->addr + os->size; b->osec = os; }
        free(n1); free(n2);
    }
}

/* GNU ld's fix_syms: symbols assigned relative to an excluded (empty) output section are
   moved to a neighbouring kept section */
static OSec *fix_excluded_section(OSec *s, uint64_t value)
{
    OSec *op1 = NULL, *op = NULL, *c;
    int i, si = -1;
    uint64_t sflags = s->has_input_spec ? (SHF_ALLOC | SHF_WRITE) : SHF_WRITE;  /* "LOAD" ~ ALLOC here */
    int s_alloc = s->has_input_spec, s_load = s->has_input_spec;
    for (i = 0; i < L.osecs.n; i++) if (L.osecs.p[i] == s) si = i;
    for (i = si - 1; i >= 0; i--) { c = L.osecs.p[i]; if (!c->removed && !c->is_reloc_sec && c->idx) { op1 = c; break; } }
    for (i = si + 1; i < L.osecs.n; i++) { c = L.osecs.p[i]; if (!c->removed && !c->is_reloc_sec && c->idx) { op = c; break; } }
    (void)sflags;
    if (!op1) return op;   /* may be NULL -> absolute */
    if (!op) return op1;
    {
#define ALLOC(x) (((x)->flags & SHF_ALLOC) != 0)
#define LOAD(x) (ALLOC(x) && (x)->type != SHT_NOBITS)
#define RO(x) (((x)->flags & SHF_WRITE) == 0)
#define CODE(x) (((x)->flags & SHF_EXECINSTR) != 0)
        int s_ro = 0, s_code = 0;
        if ((ALLOC(op1) != s_alloc) || (LOAD(op1) != s_load) || (RO(op1) != s_ro)) {
            if ((ALLOC(op) != s_alloc) || (LOAD(op1) && !LOAD(op))) return op1;
            return op;
        } else if (RO(op1) != s_ro) {
            if (RO(op) != s_ro) return op1;
            return op;
        } else if (CODE(op1) != s_code) {
            if (CODE(op) != s_code) return op1;
            return op;
        } else {
            if (value < op->addr) return op1;
            return op;
        }
#undef ALLOC
#undef LOAD
#undef RO
#undef CODE
    }
}

static void write_output(void)
{
    OSec *os;
    InSec *s;
    Obj *o;
    Symbol *sym;
    int idx = 1, i;
    WElf w;
    PArr outsecs = { 0 };   /* OSec* in file order */
    PArr symbuf = { 0 };    /* GSym* for the output symtab */
    StrTab strtab = { 0 };
    uint64_t pos;
    int secsym_count = 0;

    memset(&w, 0, sizeof w);
    strtab_init(&strtab);
    w.is64 = L.is64; w.e_machine = (uint16_t)L.machine;
    w.e_type = L.relocatable ? ET_REL : (L.shared || L.pie) ? ET_DYN : ET_EXEC;

    /* assign section header indices: alloc sections in order, then non-alloc, then reloc sections (-r) */
    for (int round = 0; round < 2; round++) {
        PARR_FOREACH(OSec *, os, L.osecs) {
            int alloc_like = (os->flags & SHF_ALLOC) || os->insecs.n == 0;
            if (os->removed || os->is_reloc_sec || os->idx) continue;
            if (!L.relocatable && round == 0 && !alloc_like) continue;
            if (!L.relocatable && round == 1 && alloc_like) continue;
            if (L.relocatable && round == 1) continue;
            os->idx = idx++;
            parr_add(&outsecs, os);
            if ((L.relocatable || L.emit_relocs) && (L.relocatable || (os->flags & SHF_ALLOC))) {
                int nrels = 0;
                PARR_FOREACH(InSec *, s, os->insecs) if (s->relsec && s->g->type != SHT_NOBITS) nrels += s->relsec->nrels;
                if (nrels) {
                    int rela = L.machine == EM_X86_64;
                    OSec *ro = osec_new(xasprintf("%s%s", rela ? ".rela" : ".rel", os->name), NULL);
                    ro->is_reloc_sec = 1; ro->rel_target = os; ro->type = rela ? SHT_RELA : SHT_REL;
                    ro->flags = SHF_INFO_LINK; ro->align = L.is64 ? 8 : 4; ro->entsize = L.is64 ? (rela ? 24 : 16) : (rela ? 12 : 8);
                    ro->idx = idx++;
                    os->relsec_out = ro;
                    parr_add(&outsecs, ro);
                }
            }
        }
    }
    if (L.shared) fill_dynamic_sections();
    /* assemble section contents */
    PARR_FOREACH(OSec *, os, outsecs) {
        if (os->type == SHT_NOBITS) { os->data = NULL; continue; }
        os->data = xmalloc(os->size ? os->size : 1);
        /* fill */
        if (os->fill_set) { uint8_t fb[8]; int n = 1; uint64_t f = os->fill; if (f > 0xff) n = f > 0xffff ? 4 : 2; for (i = 0; i < n; i++) fb[i] = (uint8_t)(f >> (8 * (n - 1 - i))); for (uint64_t k = 0; k < os->size; k++) os->data[k] = fb[k % n]; }
        else {
            memset(os->data, 0, os->size);
            if (os->flags & SHF_EXECINSTR) {
                /* like GNU ld: gaps in code sections get NOP patterns */
                uint64_t pos = 0;
                PArr sorted = { 0 };
                PARR_FOREACH(InSec *, s, os->insecs) if (!(s->mg && !s->mg_first)) parr_add(&sorted, s);
                for (int a2 = 1; a2 < sorted.n; a2++) for (int b2 = a2; b2 > 0 && ((InSec *)sorted.p[b2])->outoff < ((InSec *)sorted.p[b2 - 1])->outoff; b2--) { void *t = sorted.p[b2]; sorted.p[b2] = sorted.p[b2 - 1]; sorted.p[b2 - 1] = t; }
                for (int k = 0; k <= sorted.n; k++) {
                    uint64_t gap_end = k < sorted.n ? ((InSec *)sorted.p[k])->outoff : os->size;
                    if (gap_end > pos) nop_fill(os->data + pos, gap_end - pos);
                    if (k < sorted.n) pos = ((InSec *)sorted.p[k])->outoff + ((InSec *)sorted.p[k])->size;
                }
                free(sorted.p);
            }
        }
        PARR_FOREACH(InSec *, s, os->insecs) {
            if (s->mg) {
                if (s->mg_first) memcpy(os->data + s->outoff, s->mg->data, s->mg->size);
                continue;
            }
            if (s->g->type == SHT_NOBITS) { memset(os->data + s->outoff, 0, s->size); continue; }
            if (s->g->data) memcpy(os->data + s->outoff, s->g->data, s->g->size);
        }
    }
    /* relocations */
    if (!L.relocatable) {
        PARR_FOREACH(OSec *, os, outsecs) {
            if (os->type == SHT_NOBITS) continue;
            PARR_FOREACH(InSec *, s, os->insecs) {
                if (!s->relsec || s->g->type == SHT_NOBITS) continue;
                if (s->mg && !s->mg_first) {
                    /* relocations of a non-carrier merged section apply into the carrier's data */
                    apply_relocs(s, os->data + mg_carrier(s)->outoff);
                    continue;
                }
                apply_relocs(s, os->data + s->outoff);
            }
        }
        /* data commands */
        PARR_FOREACH(OSec *, os, outsecs) {
            DataCmd *dc;
            cur_os = os; dot = os->addr;
            PARR_FOREACH(DataCmd *, dc, os->datacmds) {
                Val v = eval(dc->e);
                if (os->type == SHT_NOBITS) continue;
                switch (dc->size) {
                case 1: os->data[dc->off] = (uint8_t)v.v; break;
                case 2: wr16(os->data + dc->off, (uint16_t)v.v); break;
                case 4: wr32(os->data + dc->off, (uint32_t)v.v); break;
                case 8: wr64(os->data + dc->off, v.v); break;
                }
            }
        }
        if (reloc_errors) fatal("link failed: %d relocation error(s)", reloc_errors);
    }

    /* ---- symbol table */
    if (L.relocatable) {
        /* mark the symbols that relocations reference, so that undefined ones are kept */
        PARR_FOREACH(OSec *, os, outsecs) PARR_FOREACH(InSec *, s, os->insecs) {
            GSec *rs = s->relsec;
            if (!rs) continue;
            for (i = 0; i < rs->nrels; i++) {
                uint32_t si = rs->rels[i].sym;
                if (si < (uint32_t)s->obj->e->nsyms && s->obj->symmap[si]) s->obj->symmap[si]->reloc_referenced = 1;
            }
        }
    }
    {
        GSym *g0 = xcalloc(1, sizeof *g0);
        parr_add(&symbuf, g0);
    }
    /* section symbols (relocatable output or --emit-relocs) */
    if (L.relocatable || L.emit_relocs) {
        PARR_FOREACH(OSec *, os, outsecs) {
            GSym *g;
            if (os->is_reloc_sec) continue;
            g = xcalloc(1, sizeof *g);
            g->info = (unsigned char)ELF32_ST_INFO(STB_LOCAL, STT_SECTION); g->shndx = (uint16_t)os->idx;
            g->value = L.relocatable ? 0 : os->addr;
            g->name = (char *)(uintptr_t)0;
            os->secsym_idx = symbuf.n;
            parr_add(&symbuf, g);
            secsym_count++;
        }
    }
    /* local symbols of each object */
    PARR_FOREACH(Obj *, o, L.objs) {
        int has_file = 0, has_locals = 0;
        for (i = 1; i < o->e->nsyms; i++) {
            GSym *gs = &o->e->syms[i];
            if (ELF32_ST_BIND(gs->info) != STB_LOCAL) break;
            if (ELF32_ST_TYPE(gs->info) == STT_FILE) has_file = 1;
            else if (ELF32_ST_TYPE(gs->info) != STT_SECTION && gs->name[0] && gs->shndx != SHN_UNDEF) {
                InSec *ds = sec_of_sym(o, gs);
                if (gs->shndx == SHN_ABS || (ds && !ds->discarded && ds->out && !ds->out->removed)) has_locals = 1;
            }
        }
        if (has_locals && !has_file && !L.relocatable && o != L.synthetic_obj) {
            GSym *g = xcalloc(1, sizeof *g);
            const char *b = strrchr(o->name, '/'); b = b ? b + 1 : o->name;
            if (strchr(b, '(')) { b = strchr(b, '(') + 1; }
            {
                char *nm = xstrdup(b); char *pc = strchr(nm, ')'); if (pc) *pc = 0;
                g->name = (char *)(uintptr_t)strtab_add(&strtab, nm); free(nm);
            }
            g->info = (unsigned char)ELF32_ST_INFO(STB_LOCAL, STT_FILE); g->shndx = SHN_ABS;
            parr_add(&symbuf, g);
        }
        for (i = 1; i < o->e->nsyms; i++) {
            GSym *gs = &o->e->syms[i], *g;
            int type = ELF32_ST_TYPE(gs->info);
            InSec *ds;
            if (ELF32_ST_BIND(gs->info) != STB_LOCAL) continue;
            if (type == STT_SECTION) {
                /* -r: map to the output section symbol */
                ds = sec_of_sym(o, gs);
                if ((L.relocatable || L.emit_relocs) && ds && ds->out && !ds->discarded && !ds->out->removed) o->outsym[i] = ds->out->secsym_idx;
                continue;
            }
            if (!gs->name[0] && type != STT_FILE) continue;
            if (gs->shndx == SHN_UNDEF) continue;
            ds = sec_of_sym(o, gs);
            if (gs->shndx != SHN_ABS && gs->shndx != SHN_COMMON && (!ds || ds->discarded || !ds->out || ds->out->removed)) continue;
            if (ds && L.strip_debug && ds->out && ds->out->is_debug) continue;
            if (ds && !L.relocatable && (ds->g->flags & SHF_MERGE) && gs->name[0] == '.' && gs->name[1] == 'L') continue;
            g = xcalloc(1, sizeof *g);
            *g = *gs;
            g->name = (char *)(uintptr_t)strtab_add(&strtab, gs->name);
            if (ds) {
                OSec *oo = mg_carrier(ds)->out;
                g->shndx = (uint16_t)oo->idx;
                g->value = ds->mg ? merge_map(ds, gs->value) : gs->value;
                g->value += L.relocatable ? mg_carrier(ds)->outoff : insec_addr(ds);
            } else if (gs->shndx == SHN_COMMON) continue;
            o->outsym[i] = symbuf.n;
            parr_add(&symbuf, g);
        }
    }
    /* linker-created locals (_DYNAMIC), preceded by an empty FILE symbol like GNU ld */
    {
        int any = 0;
        PARR_FOREACH(Symbol *, sym, L.symlist) if (sym->linker_local && sym->defined) any = 1;
        if (any) { GSym *g = xcalloc(1, sizeof *g); g->info = (unsigned char)ELF32_ST_INFO(STB_LOCAL, STT_FILE); g->shndx = SHN_ABS; parr_add(&symbuf, g); }
    }
    PARR_FOREACH(Symbol *, sym, L.symlist) {
        if (!sym->linker_local || !sym->defined) continue;
        {
            GSym *g = xcalloc(1, sizeof *g);
            OSec *sec = NULL;
            g->name = (char *)(uintptr_t)strtab_add(&strtab, sym->name);
            g->value = symbol_address(sym, &sec);
            g->info = (unsigned char)ELF32_ST_INFO(STB_LOCAL, sym->type);
            g->shndx = sec ? (uint16_t)sec->idx : SHN_ABS;
            sym->outidx = symbuf.n;
            parr_add(&symbuf, g);
        }
    }
    /* globals (hidden symbols become local in a final link: two rounds) */
    for (int round = 0; round < 2; round++) {
        if (round == 1) L.first_global = symbuf.n;
        PARR_FOREACH(Symbol *, sym, L.symlist) {
            GSym *g;
            OSec *sec = NULL;
            uint64_t v;
            /* GNU ld: a hidden symbol that other objects reference becomes LOCAL (with default visibility) */
            /* GNU ld (_bfd_elf_fix_symbol_flags): in a PIC link a hidden symbol that needs a PLT
               (PLT32 references) is forced local; a version script's "local:" also forces local */
            int make_local = !L.relocatable && sym->defined &&
                             ((((L.pie || L.shared) && (sym->vis == STV_HIDDEN || sym->vis == STV_INTERNAL) && sym->plt_referenced))
                              || (L.shared && L.sc->nversions && !sym->dynamic_export && !sym->linker_local));
            if (sym->linker_local) continue;
            if (make_local != (round == 0)) continue;
            if (!sym->defined) {
                if (!sym->referenced) continue;
                if (!L.relocatable && !sym->reloc_referenced) continue;
                if (!(sym->weak || !sym->referenced_strong) && !L.relocatable) continue; /* error already reported */
                g = xcalloc(1, sizeof *g);
                g->name = (char *)(uintptr_t)strtab_add(&strtab, sym->name);
                g->info = (unsigned char)ELF32_ST_INFO(sym->referenced_strong ? STB_GLOBAL : STB_WEAK, STT_NOTYPE);
                g->shndx = SHN_UNDEF;
                sym->outidx = symbuf.n;
                parr_add(&symbuf, g);
                continue;
            }
            if (sym->sec && (sym->sec->discarded || !sym->sec->out || sym->sec->out->removed)) continue;
            if (sym->sec && L.strip_debug && sym->sec->out->is_debug) continue;
            g = xcalloc(1, sizeof *g);
            g->name = (char *)(uintptr_t)strtab_add(&strtab, sym->name);
            v = symbol_address(sym, &sec);
            if (sym->script && sym->osec && sym->osec->removed) {
                OSec *fix = fix_excluded_section(sym->osec, v);
                sec = fix;
            }
            g->value = v;
            g->size = sym->size;
            g->info = (unsigned char)ELF32_ST_INFO(make_local ? STB_LOCAL : (sym->weak || sym->bind == STB_WEAK) ? STB_WEAK : STB_GLOBAL, sym->type);
            g->other = (unsigned char)(make_local ? 0 : sym->vis);
            if (sym->startstop) g->other = STV_PROTECTED;
            g->shndx = sec ? (uint16_t)sec->idx : SHN_ABS;
            if (!sym->script && sym->sec && L.relocatable) g->value = mg_carrier(sym->sec)->outoff + (sym->sec->mg ? merge_map(sym->sec, sym->value) : sym->value);
            sym->outidx = symbuf.n;
            parr_add(&symbuf, g);
        }
    }

    /* ---- relocation output sections (-r / --emit-relocs) */
    if (L.relocatable || L.emit_relocs) {
        int rela = L.machine == EM_X86_64;
        size_t esz = L.is64 ? (rela ? 24 : 16) : (rela ? 12 : 8);
        PARR_FOREACH(OSec *, os, outsecs) {
            PArr rels = { 0 };
            GRel *r;
            if (os->is_reloc_sec || !os->relsec_out) continue;
            PARR_FOREACH(InSec *, s, os->insecs) {
                GSec *rs = s->relsec;
                if (!rs || s->g->type == SHT_NOBITS) continue;
                for (i = 0; i < rs->nrels; i++) {
                    GRel *ir = &rs->rels[i];
                    GSym *gs;
                    int outi;
                    r = xcalloc(1, sizeof *r);
                    *r = *ir;
                    r->offset = (L.relocatable ? 0 : os->addr) + s->outoff + ir->offset;
                    if (ir->sym >= (uint32_t)s->obj->e->nsyms) fatal("bad relocation symbol index");
                    gs = &s->obj->e->syms[ir->sym];
                    if (ir->sym == 0) outi = 0;
                    else if (ELF32_ST_BIND(gs->info) == STB_LOCAL) {
                        outi = s->obj->outsym[ir->sym];
                        if (ELF32_ST_TYPE(gs->info) == STT_SECTION) {
                            InSec *ds = sec_of_sym(s->obj, gs);
                            if (ds && ds->out) {
                                if (rela) r->addend += (L.relocatable ? 0 : ds->out->addr) + ds->outoff;
                                else if (L.relocatable) {
                                    /* REL: adjust the addend in place */
                                    uint8_t *f = os->data + s->outoff + ir->offset;
                                    int wdt = reloc_width(ir->type);
                                    uint64_t v = ds->outoff;
                                    if (wdt == 4) wr32(f, rd32(f) + (uint32_t)v); else if (wdt == 2) wr16(f, rd16(f) + (uint16_t)v); else if (wdt == 1) f[0] += (uint8_t)v;
                                }
                                if (!L.relocatable && !rela) { /* emit-relocs REL: data already final */ }
                            }
                        }
                        if (outi < 0) { outi = 0; }
                    } else {
                        Symbol *sym = s->obj->symmap[ir->sym];
                        outi = sym ? sym->outidx : -1;
                        if (outi < 0) outi = 0;
                    }
                    r->sym = (uint32_t)outi;
                    parr_add(&rels, r);
                }
            }
            if (!rels.n) { free(rels.p); continue; }
            {
                OSec *ro = os->relsec_out;
                if (!ro) fatal("internal error: no relocation section reserved for %s", os->name);
                ro->size = (uint64_t)rels.n * esz;
                ro->data = xcalloc(ro->size, 1);
                for (i = 0; i < rels.n; i++) welf_put_rel(L.is64, rela, ro->data + i * esz, (GRel *)rels.p[i]);
            }
        }
    }

    /* ---- program headers and file offsets */
    {
        PArr phdrs = { 0 };
        uint64_t hdr_end;
        int nph = 0;
        typedef struct { WPhdr ph; PArr secs; int filehdr, phdrs_in; int has_secs; PhdrDef *def; } SegB;
        PArr segs = { 0 };
        SegB *sg;
        if (!L.relocatable) {
            if (L.sc->nphdrs) {
                for (i = 0; i < L.sc->nphdrs; i++) {
                    sg = xcalloc(1, sizeof *sg);
                    sg->def = L.sc->phdrs[i]; sg->ph.type = sg->def->type;
                    sg->ph.flags = sg->def->has_flags ? sg->def->flags : PF_R;
                    sg->filehdr = sg->def->filehdr; sg->phdrs_in = sg->def->phdrs;
                    parr_add(&segs, sg);
                }
                /* assign sections: explicit :phdr lists, otherwise inherit from the previous alloc section */
                {
                    int cur[16], ncur = 0;
                    PARR_FOREACH(OSec *, os, outsecs) {
                        if (!(os->flags & SHF_ALLOC) || os->is_reloc_sec) continue;
                        if (os->def && os->def->nphdrs) {
                            ncur = 0;
                            for (i = 0; i < os->def->nphdrs; i++) {
                                int k;
                                for (k = 0; k < L.sc->nphdrs; k++) if (!strcmp(L.sc->phdrs[k]->name, os->def->phdrs[i])) break;
                                if (k == L.sc->nphdrs) fatal("section `%s' assigned to unknown program header `%s'", os->name, os->def->phdrs[i]);
                                if (!strcmp(os->def->phdrs[i], "NONE")) continue;
                                cur[ncur++] = k;
                            }
                        } else if (!ncur) {
                            /* first section without a phdr: use the first PT_LOAD */
                            for (i = 0; i < L.sc->nphdrs; i++) if (L.sc->phdrs[i]->type == PT_LOAD) { cur[ncur++] = i; break; }
                        }
                        for (i = 0; i < ncur; i++) { sg = segs.p[cur[i]]; parr_add(&sg->secs, os); sg->has_secs = 1; }
                    }
                }
            } else {
                /* default: build PT_LOADs from the alloc sections in order */
                OSec *last = NULL; uint64_t last_end = 0;
                sg = NULL;
                PARR_FOREACH(OSec *, os, outsecs) {
                    int newseg = 0;
                    if (!(os->flags & SHF_ALLOC) || os->is_reloc_sec || os->size == 0) continue;
                    if (!last) newseg = 1;
                    else if (align_up(last->lma + last->size, L.maxpage) < align_up(os->lma, L.maxpage)) newseg = 1;
                    else if (os->lma < last->lma) newseg = 1;
                    else if (last->type == SHT_NOBITS && os->type != SHT_NOBITS) newseg = 1;
                    else if (!(sg->ph.flags & PF_W) && (os->flags & SHF_WRITE)) {
                        if (align_up(last->lma + last->size, L.maxpage) <= (os->lma & ~(L.maxpage - 1))) newseg = 1;
                    } else if ((((last->flags & SHF_EXECINSTR) != 0) != ((os->flags & SHF_EXECINSTR) != 0))) {
                        if (align_up(last->lma + last->size, L.maxpage) <= (os->lma & ~(L.maxpage - 1))) newseg = 1;
                    }
                    if (newseg) {
                        sg = xcalloc(1, sizeof *sg); sg->ph.type = PT_LOAD; sg->ph.flags = PF_R;
                        parr_add(&segs, sg);
                    }
                    sg->ph.flags |= (os->flags & SHF_WRITE ? PF_W : 0) | (os->flags & SHF_EXECINSTR ? PF_X : 0);
                    parr_add(&sg->secs, os); sg->has_secs = 1;
                    last = os; last_end = os->lma + os->size;
                }
                (void)last_end;
                if (L.noexecstack || 1) {
                    sg = xcalloc(1, sizeof *sg); sg->ph.type = PT_GNU_STACK; sg->ph.flags = PF_R | PF_W; sg->ph.align = 16; sg->has_secs = 1;
                    parr_add(&segs, sg);
                }
            }
            /* drop empty non-LOAD/non-STACK phdrs */
            {
                PArr kept = { 0 };
                PARR_FOREACH(SegB *, sg, segs) {
                    if (!sg->has_secs && sg->ph.type != PT_GNU_STACK && sg->ph.type != PT_LOAD) continue;
                    if (sg->ph.type == PT_LOAD && !sg->has_secs && !sg->filehdr) continue;
                    parr_add(&kept, sg);
                }
                segs = kept;
            }
            nph = segs.n;
        }
        hdr_end = welf_ehdr_size(L.is64) + (uint64_t)nph * welf_phdr_size(L.is64);
        pos = hdr_end;
        /* file offsets for alloc sections */
        if (!L.relocatable) {
            uint64_t delta = 0; int have_delta = 0;
            OSec *prev = NULL;
            SegB *cursg = NULL;
            PARR_FOREACH(OSec *, os, outsecs) {
                SegB *mysg = NULL;
                if (!(os->flags & SHF_ALLOC) || os->is_reloc_sec) continue;
                /* which PT_LOAD contains this section (first one) */
                PARR_FOREACH(SegB *, sg, segs) { int k; if (sg->ph.type != PT_LOAD) continue; for (k = 0; k < sg->secs.n; k++) if (sg->secs.p[k] == os) { mysg = sg; break; } if (mysg) break; }
                if (mysg != cursg || !have_delta) {
                    cursg = mysg;
                    if (mysg && mysg->filehdr) { delta = os->addr - 0; /* headers at vaddr addr - pos? */
                        /* the segment starts at file offset 0: vaddr of offset 0 = first section vaddr - its offset */
                        if (os->addr < pos) fatal("section %s does not leave room for the file headers", os->name);
                        os->offset = os->addr; delta = 0; have_delta = 1;
                    } else {
                        uint64_t want = os->addr & (L.maxpage - 1);
                        uint64_t off = (pos & ~(L.maxpage - 1)) | want;
                        if (off < pos) off += L.maxpage;
                        if (os->size == 0 && os->type != SHT_NOBITS) off = pos; /* empty: no gap */
                        os->offset = off; delta = os->addr - off; have_delta = 1;
                    }
                } else {
                    if (os->type == SHT_NOBITS) os->offset = pos;
                    else os->offset = os->addr - delta;
                    if (os->offset < pos && os->type != SHT_NOBITS) {
                        /* overlapping addresses (shouldn't happen); push forward */
                        os->offset = align_up(pos, os->align ? os->align : 1);
                    }
                }
                if (os->type != SHT_NOBITS && os->offset + os->size > pos) pos = os->offset + os->size;
                if (os->type == SHT_NOBITS && os->offset > pos) pos = os->offset;
                prev = os;
            }
            (void)prev;
            /* program header values */
            PARR_FOREACH(SegB *, sg, segs) {
                OSec *first = sg->secs.n ? sg->secs.p[0] : NULL, *lastp = NULL, *lastsec = NULL;
                if (sg->ph.type == PT_GNU_STACK) { sg->ph.align = sg->ph.align ? sg->ph.align : 16; continue; }
                for (i = 0; i < sg->secs.n; i++) { OSec *x = sg->secs.p[i]; lastsec = x; if (x->type != SHT_NOBITS) lastp = x; }
                if (sg->ph.type == PT_LOAD) {
                    sg->ph.align = L.maxpage;
                    if (sg->filehdr) { sg->ph.offset = 0; sg->ph.vaddr = first ? first->addr - first->offset : 0; sg->ph.paddr = first ? first->lma - first->offset : 0; }
                    else if (first) { sg->ph.offset = first->offset; sg->ph.vaddr = first->addr; sg->ph.paddr = first->lma; }
                    if (lastp) sg->ph.filesz = lastp->offset + lastp->size - sg->ph.offset;
                    else sg->ph.filesz = 0;
                    if (lastsec) sg->ph.memsz = lastsec->addr + lastsec->size - sg->ph.vaddr;
                    if (sg->ph.memsz < sg->ph.filesz) sg->ph.memsz = sg->ph.filesz;
                } else {
                    sg->ph.align = first ? first->align : 1;
                    if (first) { sg->ph.offset = first->offset; sg->ph.vaddr = first->addr; sg->ph.paddr = first->lma; }
                    if (lastp) sg->ph.filesz = lastp->offset + lastp->size - sg->ph.offset;
                    if (lastsec) sg->ph.memsz = lastsec->addr + lastsec->size - sg->ph.vaddr;
                }
                if (sg->def && sg->def->at) { Val v; cur_os = NULL; dot = 0; v = eval(sg->def->at); sg->ph.paddr = v.v; }
            }
            w.nphdrs = segs.n;
            w.phdrs = xcalloc(segs.n ? segs.n : 1, sizeof *w.phdrs);
            for (i = 0; i < segs.n; i++) w.phdrs[i] = ((SegB *)segs.p[i])->ph;
            (void)phdrs;
        }
        /* non-alloc sections */
        PARR_FOREACH(OSec *, os, outsecs) {
            if ((os->flags & SHF_ALLOC) && !L.relocatable) continue;
            if (L.relocatable && os->is_reloc_sec) continue;
            if (os->type == SHT_NOBITS) { os->offset = pos; continue; }
            pos = align_up(pos, os->align > 1 ? os->align : 1);
            os->offset = pos; pos += os->size;
        }
        /* -r: relocation sections after all others */
        if (L.relocatable) {
            PARR_FOREACH(OSec *, os, outsecs) {
                if (!os->is_reloc_sec) continue;
                pos = align_up(pos, os->align); os->offset = pos; pos += os->size;
            }
        }
    }

    /* ---- build-id */
    if (L.buildid_sec && L.buildid_sec->out && !L.buildid_sec->out->removed) {
        OSec *bo = L.buildid_sec->out;
        uint8_t *note = bo->data + L.buildid_sec->outoff;
        wr32(note, 4); wr32(note + 4, 20); wr32(note + 8, NT_GNU_BUILD_ID); memcpy(note + 12, "GNU", 4);
        memset(note + 16, 0, 20);
        /* hash the contents of all output sections with file contents */
        {
            size_t total = 0; uint8_t *buf; size_t off = 0; uint8_t dig[20];
            PARR_FOREACH(OSec *, os, outsecs) if (os->type != SHT_NOBITS && os->data) total += os->size;
            buf = xmalloc(total ? total : 1);
            PARR_FOREACH(OSec *, os, outsecs) if (os->type != SHT_NOBITS && os->data) { memcpy(buf + off, os->data, os->size); off += os->size; }
            sha1(buf, total, dig);
            memcpy(note + 16, dig, 20);
            free(buf);
        }
    }

    /* ---- symtab / strtab */
    {
        int nsec = outsecs.n;
        WSec *ws = xcalloc(nsec + 3, sizeof *ws);
        int k = 1, symtab_idx, strtab_idx;
        uint64_t symsize = (uint64_t)symbuf.n * welf_sym_size(L.is64);
        uint8_t *symdata = xcalloc(symsize ? symsize : 1, 1);
        for (i = 0; i < symbuf.n; i++) welf_put_sym(L.is64, symdata + (size_t)i * welf_sym_size(L.is64), (GSym *)symbuf.p[i]);
        PARR_FOREACH(OSec *, os, outsecs) {
            WSec *x = &ws[k++];
            x->name = os->name; x->type = os->type; x->flags = os->flags;
            x->addr = (!L.relocatable && ((os->flags & SHF_ALLOC) || (os->def && os->insecs.n == 0))) ? os->addr : 0;
            x->offset = os->offset; x->size = os->size; x->addralign = os->align ? os->align : 1; x->entsize = os->entsize;
            x->data = os->data;
            if (os->is_reloc_sec) { x->link = 0; x->info = (uint32_t)os->rel_target->idx; }
            if (os == (L.dynsym_sec ? L.dynsym_sec->out : NULL) && L.dynsym_sec->out->insecs.n == 1) { x->link = (uint32_t)L.dynstr_sec->out->idx; x->info = 1; }
            if (L.hash_sec && os == L.hash_sec->out) x->link = (uint32_t)L.dynsym_sec->out->idx;
            if (L.gnuhash_sec && os == L.gnuhash_sec->out) x->link = (uint32_t)L.dynsym_sec->out->idx;
            if (L.versym_sec && os == L.versym_sec->out) x->link = (uint32_t)L.dynsym_sec->out->idx;
            if (L.verdef_sec && os == L.verdef_sec->out) { x->link = (uint32_t)L.dynstr_sec->out->idx; x->info = (uint32_t)(L.sc->nversions + 1); }
            if (L.dynamic_sec && os == L.dynamic_sec->out) x->link = (uint32_t)L.dynstr_sec->out->idx;
        }
        if (!L.strip_all) {
            symtab_idx = k; strtab_idx = k + 1;
            ws[k].name = ".symtab"; ws[k].type = SHT_SYMTAB; ws[k].size = symsize; ws[k].data = symdata; ws[k].addralign = L.is64 ? 8 : 4;
            ws[k].entsize = welf_sym_size(L.is64); ws[k].link = (uint32_t)strtab_idx; ws[k].info = (uint32_t)L.first_global;
            pos = align_up(pos, ws[k].addralign); ws[k].offset = pos; pos += symsize;
            k++;
            ws[k].name = ".strtab"; ws[k].type = SHT_STRTAB; ws[k].size = strtab.len; ws[k].data = (uint8_t *)strtab.buf; ws[k].addralign = 1;
            ws[k].offset = pos; pos += strtab.len;
            k++;
            PARR_FOREACH(OSec *, os, outsecs) if (os->is_reloc_sec) ws[os->idx].link = (uint32_t)symtab_idx;
        }
        w.secs = ws; w.nsecs = k;
        /* entry */
        if (!L.relocatable) {
            Symbol *es = L.entry_sym ? sym_lookup(L.entry_sym, 0) : NULL;
            if (es && es->defined) w.e_entry = symbol_address(es, NULL);
            else if (L.entry_sym && !L.shared) {
                char *end; uint64_t v = strtoull(L.entry_sym, &end, 0);
                if (*end == 0 && end != L.entry_sym) w.e_entry = v;
                else {
                    OSec *t = osec_find(".text");
                    warning("cannot find entry symbol %s; defaulting to %016llx", L.entry_sym, (unsigned long long)(t ? t->addr : 0));
                    w.e_entry = t ? t->addr : 0;
                }
            }
        }
        welf_write(&w, L.output);
        if (!L.relocatable) chmod(L.output, 0755);
    }
}

/* ---------------------------------------------------------------- default scripts */
static Script *default_script_relocatable(void)
{
    /* one output section per distinct input section name, in order of first appearance */
    Script *sc = script_new();
    Stmt **tail = &sc->stmts;
    PArr names = { 0 };
    Obj *o;
    static const char *const seeds[] = { ".text", ".rodata", ".data", ".bss", ".comment", ".note.GNU-stack", NULL };
    /* pass 0..5: the sections GNU ld's default -r script names, in its order; pass 6: the rest */
    for (int pass = 0; pass < 7; pass++)
    PARR_FOREACH(Obj *, o, L.objs) {
        for (int i = 1; i < o->e->nsecs; i++) {
            InSec *s = &o->secs[i];
            const char *n;
            int k, seeded = -1;
            if (!sec_is_input_candidate(s)) continue;
            n = s->g->name;
            for (k = 0; seeds[k]; k++) if (!strcmp(seeds[k], n)) seeded = k;
            if (pass < 6 ? seeded != pass : seeded >= 0) continue;
            {
                /* key: name + NOBITS-ness + ALLOC-ness (GNU keeps those apart in -r output) */
                char *key = xasprintf("%s|%d|%d", n, s->g->type == SHT_NOBITS, (s->g->flags & SHF_ALLOC) != 0);
                for (k = 0; k < names.n; k++) if (!strcmp(names.p[k], key)) break;
                if (k < names.n) { free(key); continue; }
                parr_add(&names, key);
            }
            {
                Stmt *st = xcalloc(1, sizeof *st), *in = xcalloc(1, sizeof *in);
                OutSecDef *d = xcalloc(1, sizeof *d);
                st->kind = S_OUTSEC; st->os = d; d->name = xstrdup(n);
                in->kind = S_INPUT; in->spec = xcalloc(1, sizeof *in->spec);
                in->spec->filepat = xstrdup("*"); in->spec->secpats = xmalloc(sizeof(char *)); in->spec->secpats[0] = xstrdup(n); in->spec->nsecpats = 1;
                in->spec->kind_filter = 1; in->spec->kind_nobits = s->g->type == SHT_NOBITS; in->spec->kind_alloc = (s->g->flags & SHF_ALLOC) != 0;
                d->children = in;
                *tail = st; tail = &st->next;
            }
        }
    }
    sc->has_sections = 1;
    return sc;
}

static Script *default_script_exec(void)
{
    /* a small GNU-like default for executables without -T */
    const char *text =
        "SECTIONS {\n"
        " . = SEGMENT_START(\"text-segment\", 0x400000) + SIZEOF_HEADERS;\n"
        " .text : { *(.text .text.*) }\n"
        " .rodata : { *(.rodata .rodata.* .data.ro) }\n"
        " . = ALIGN(CONSTANT(MAXPAGESIZE));\n"
        " .data : { *(.data .data.*) }\n"
        " .bss : { *(.bss .bss.*) *(COMMON) }\n"
        " .comment 0 : { *(.comment) }\n"
        "}\n";
    char path[] = "/tmp/tld-default-XXXXXX";
    int fd = mkstemp(path);
    FILE *f;
    Script *sc;
    if (fd < 0) fatal("cannot create a temporary file");
    f = fdopen(fd, "w"); fputs(text, f); fclose(f);
    sc = script_parse_file(path);
    unlink(path);
    if (!L.entry_sym) L.entry_sym = xstrdup("_start");
    return sc;
}

/* ---------------------------------------------------------------- driver */
static void usage(void) { fprintf(stderr, "usage: %s [options] file...\n", progname); exit(1); }

int ld_main(int argc, char **argv)
{
    int i, whole = 0, group_depth = 0, print_version = 0;
    PArr inputs = { 0 }, input_whole = { 0 };
    OSec *os;

    L.maxpage = 0x1000;
    L.orphan_warn = 0;
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        char *val = NULL;
#define ARG() (val ? val : (i + 1 < argc ? argv[++i] : (fatal("option %s needs an argument", a), (char *)0)))
        if (a[0] != '-' || !strcmp(a, "-")) { parr_add(&inputs, a); parr_add(&input_whole, (void *)(intptr_t)whole); continue; }
        if ((val = strchr(a, '='))) { *val++ = 0; }
        if (!strcmp(a, "-o")) L.output = ARG();
        else if (!strcmp(a, "-m")) { char *m = ARG(); if (!strcmp(m, "elf_x86_64")) { L.machine = EM_X86_64; L.is64 = 1; L.maxpage = 0x1000; } else if (!strcmp(m, "elf_i386")) { L.machine = EM_386; L.is64 = 0; L.maxpage = 0x1000; } else fatal("unsupported emulation %s", m); }
        else if (!strcmp(a, "-T") || !strcmp(a, "--script") || !strcmp(a, "-dT") || !strcmp(a, "--default-script")) parr_add(&L.script_files, ARG());
        else if (!strncmp(a, "-T", 2) && a[2]) parr_add(&L.script_files, a + 2);
        else if (!strcmp(a, "-r") || !strcmp(a, "--relocatable") || !strcmp(a, "-i")) L.relocatable = 1;
        else if (!strcmp(a, "-shared") || !strcmp(a, "--shared") || !strcmp(a, "-Bshareable")) L.shared = 1;
        else if (!strcmp(a, "-pie") || !strcmp(a, "--pie") || !strcmp(a, "--pic-executable")) L.pie = 1;
        else if (!strcmp(a, "-soname") || !strcmp(a, "--soname") || !strcmp(a, "-h")) L.soname = ARG();
        else if (!strcmp(a, "-z")) {
            char *z = ARG();
            if (!strncmp(z, "max-page-size", 13)) { char *eq = strchr(z, '='); if (eq) L.maxpage = strtoull(eq + 1, NULL, 0); }
            else if (!strcmp(z, "noexecstack")) L.noexecstack = 1;
            /* RELR is not implemented; say so by name: the kernel's scripts/tools-support-relr.sh
               greps the linker's message for it to decide CONFIG_TOOLS_SUPPORT_RELR */
            else if (!strcmp(z, "pack-relative-relocs")) fatal("-z pack-relative-relocs is not supported");
        }
        else if (!strncmp(a, "-z", 2) && a[2]) { if (!strncmp(a + 2, "max-page-size", 13) && val) L.maxpage = strtoull(val, NULL, 0); }
        else if (!strcmp(a, "--whole-archive")) whole = 1;
        else if (!strcmp(a, "--no-whole-archive")) whole = 0;
        else if (!strcmp(a, "--start-group") || !strcmp(a, "-(")) { group_depth++; parr_add(&inputs, "--start-group"); parr_add(&input_whole, 0); }
        else if (!strcmp(a, "--end-group") || !strcmp(a, "-)")) { group_depth--; parr_add(&inputs, "--end-group"); parr_add(&input_whole, 0); }
        else if (!strcmp(a, "--strip-debug") || !strcmp(a, "-S")) L.strip_debug = 1;
        else if (!strcmp(a, "--strip-all") || !strcmp(a, "-s")) L.strip_all = 1;
        else if (!strcmp(a, "--build-id")) L.build_id = 1;
        else if (!strcmp(a, "--orphan-handling")) { char *v = ARG(); L.orphan_warn = !strcmp(v, "warn") || !strcmp(v, "error"); }
        else if (!strcmp(a, "--no-warn-rwx-segments") || !strcmp(a, "--warn-rwx-segments") || !strcmp(a, "--no-ld-generated-unwind-info") ||
                 !strcmp(a, "--no-dynamic-linker") || !strcmp(a, "--eh-frame-hdr") || !strcmp(a, "--hash-style") || !strcmp(a, "--gc-sections") ||
                 !strcmp(a, "--no-gc-sections") || !strcmp(a, "-X") || !strcmp(a, "--discard-locals") || !strcmp(a, "-N") || !strcmp(a, "-n") ||
                 !strcmp(a, "--fatal-warnings") || !strcmp(a, "--no-warn-mismatch") || !strcmp(a, "--no-relax") || !strcmp(a, "--relax") ||
                 !strcmp(a, "-static") || !strcmp(a, "--static") || !strcmp(a, "-Bstatic") || !strcmp(a, "--no-warnings") || !strcmp(a, "-w") ||
                 !strcmp(a, "--sort-common") || !strcmp(a, "--no-check-sections") || !strcmp(a, "--warn-common") || !strcmp(a, "--no-undefined-version") ||
                 !strcmp(a, "--as-needed") || !strcmp(a, "--no-as-needed") || !strcmp(a, "--no-undefined") || !strcmp(a, "-nostdlib") ||
                 !strcmp(a, "--emit-relocs") || !strcmp(a, "-q") || !strcmp(a, "-Bsymbolic") || !strcmp(a, "-Bsymbolic-functions") ||
                 !strcmp(a, "--version-script") || !strcmp(a, "-Map") || !strcmp(a, "-e") || !strcmp(a, "--entry") || !strcmp(a, "-u") ||
                 !strcmp(a, "--undefined") || !strcmp(a, "-L") || !strcmp(a, "-l") || !strcmp(a, "--verbose") || !strcmp(a, "-v") || !strcmp(a, "-V") ||
                 !strcmp(a, "--version") || !strcmp(a, "--print-map") || !strcmp(a, "--no-eh-frame-hdr") || !strcmp(a, "--defsym") ||
                 !strcmp(a, "--enable-new-dtags") || !strcmp(a, "--disable-new-dtags") || !strcmp(a, "--rosegment") || !strcmp(a, "--no-rosegment")) {
            if (!strcmp(a, "--emit-relocs") || !strcmp(a, "-q")) L.emit_relocs = 1;
            else if (!strcmp(a, "-Bsymbolic") || !strcmp(a, "-Bsymbolic-functions")) L.bsymbolic = 1;
            else if (!strcmp(a, "--no-undefined")) L.no_undefined = 1;
            else if (!strcmp(a, "--eh-frame-hdr")) L.eh_frame_hdr = 1;
            else if (!strcmp(a, "--hash-style")) { ARG(); }
            else if (!strcmp(a, "--version-script")) parr_add(&L.script_files, ARG());
            else if (!strcmp(a, "-Map")) { ARG(); }
            else if (!strcmp(a, "-e") || !strcmp(a, "--entry")) L.entry_opt = ARG();
            else if (!strcmp(a, "-u") || !strcmp(a, "--undefined")) parr_add(&L.undefs_cmdline, ARG());
            else if (!strcmp(a, "-L")) parr_add(&L.lib_dirs, ARG());
            else if (!strcmp(a, "-l")) parr_add(&L.libs, ARG());
            else if (!strcmp(a, "--defsym")) { char *d = ARG(); char *eq = strchr(d, '='); if (!eq) fatal("bad --defsym"); *eq = 0; { Symbol *s = sym_lookup(d, 1); s->defined = 1; s->script = 1; s->value = strtoull(eq + 1, NULL, 0); s->bind = STB_GLOBAL; } }
            else if (!strcmp(a, "-v") || !strcmp(a, "-V") || !strcmp(a, "--version") || !strcmp(a, "--verbose")) print_version = 1;
        }
        else if (!strncmp(a, "-Map", 4)) {}
        else if (!strncmp(a, "-L", 2)) parr_add(&L.lib_dirs, a + 2);
        else if (!strncmp(a, "-l", 2)) parr_add(&L.libs, a + 2);
        else if (!strncmp(a, "-e", 2) && a[2]) L.entry_opt = a + 2;
        else if (!strncmp(a, "-u", 2) && a[2]) parr_add(&L.undefs_cmdline, a + 2);
        else if (!strncmp(a, "-O", 2)) {}
        else if (!strncmp(a, "-m", 2) && a[2]) { if (!strcmp(a + 2, "elf_x86_64")) { L.machine = EM_X86_64; L.is64 = 1; L.maxpage = 0x1000; } else if (!strcmp(a + 2, "elf_i386")) { L.machine = EM_386; L.is64 = 0; L.maxpage = 0x1000; } else fatal("unsupported emulation %s", a + 2); }
        else { fprintf(stderr, "%s: unrecognized option '%s'\n", progname, a); return 1; }
#undef ARG
    }
    if (print_version) {
        printf("GNU ld (tld) 2.47\n");
        if (!inputs.n) return 0;
    }
    if (!inputs.n) { if (print_version) return 0; fatal("no input files"); }
    if (!L.output) L.output = "a.out";
    if (L.pie && L.shared) L.pie = 0;

    /* load inputs */
    for (i = 0; i < inputs.n; i++) {
        const char *in = inputs.p[i];
        if (!strcmp(in, "--start-group")) { archives.n = 0; continue; }
        if (!strcmp(in, "--end-group")) { group_resolve(); archives.n = 0; continue; }
        load_input(in, (int)(intptr_t)input_whole.p[i]);
    }
    {
        char *lib;
        PARR_FOREACH(char *, lib, L.libs) {
            char *dir; int found = 0;
            PARR_FOREACH(char *, dir, L.lib_dirs) {
                char *p = xasprintf("%s/lib%s.a", dir, lib);
                FILE *f = fopen(p, "rb");
                if (f) { fclose(f); load_input(p, 0); found = 1; break; }
            }
            if (!found) fatal("cannot find -l%s", lib);
        }
    }
    group_resolve();
    if (!L.objs.n) fatal("no input objects");
    {
        char *u;
        PARR_FOREACH(char *, u, L.undefs_cmdline) { Symbol *s = sym_lookup(u, 1); s->referenced = 1; s->referenced_strong = 1; }
        if (L.undefs_cmdline.n) group_resolve();
    }

    /* script */
    L.sc = script_new();
    {
        char *f;
        PARR_FOREACH(char *, f, L.script_files) script_parse_into(L.sc, f);
        for (int k = 0; k < L.sc->ninputs; k++) load_input(L.sc->inputs[k], 0);
    }
    if (!L.sc->has_sections) {
        Script *d = L.relocatable ? default_script_relocatable() : default_script_exec();
        /* keep assignments/version/phdrs parsed from files */
        Stmt **tail = &d->stmts; while (*tail) tail = &(*tail)->next;
        *tail = L.sc->stmts;
        d->phdrs = L.sc->phdrs; d->nphdrs = L.sc->nphdrs; d->versions = L.sc->versions; d->nversions = L.sc->nversions;
        if (L.sc->entry) d->entry = L.sc->entry;
        L.sc = d;
    }
    L.entry_sym = L.entry_opt ? L.entry_opt : L.sc->entry ? L.sc->entry : (L.relocatable || L.shared) ? NULL : "_start";
    mark_script_references(L.sc->stmts);
    if (L.entry_sym) { Symbol *es = sym_lookup(L.entry_sym, 0); if (es) es->referenced = 1; }

    /* linker-created input sections */
    if (L.build_id && !L.relocatable) {
        L.buildid_sec = add_synthetic_section(".note.gnu.build-id", SHT_NOTE, SHF_ALLOC, 4, 0, 36);
    }
    create_dynamic_sections();

    /* mark symbol references */
    {
        Symbol *sym;
        PARR_FOREACH(Symbol *, sym, L.symlist) if (!sym->defined && sym->referenced) {
            /* keep */
        }
    }

    assign_sections();
    compute_osec_flags();
    merge_strings();

    /* layout iterations */
    L.nphdrs_est = L.sc->nphdrs ? L.sc->nphdrs : 4;
    for (L.pass = 0; L.pass < 32; L.pass++) {
        layout_changed = 0;
        L.final = 0;
        layout_pass();
        provide_start_stop();
        if (L.pass == 0) { remove_empty_sections(); layout_changed = 1; }
        if (L.pass > 0 && !layout_changed) break;
    }
    L.final = 1;
    layout_changed = 0;
    layout_pass();
    provide_start_stop();
    /* undefined symbols are reported by the relocation pass (GNU ld only complains about
       undefined symbols that relocations use); -u symbols must be defined though */
    {
        char *u;
        PARR_FOREACH(char *, u, L.undefs_cmdline) { Symbol *s = sym_lookup(u, 0); if (!s || !s->defined) fatal("undefined symbol `%s' (from -u)", u); }
    }
    /* final output section flags for empty kept sections (GNU ld quirks) */
    PARR_FOREACH(OSec *, os, L.osecs) {
        if (os->removed || os->is_reloc_sec || os->insecs.n) continue;
        if (os->datacmds.n) { os->flags = SHF_WRITE | SHF_ALLOC; os->type = SHT_PROGBITS; }
        else if (os->size) { os->flags = SHF_WRITE | SHF_ALLOC; os->type = SHT_NOBITS; }
        else os->flags = SHF_WRITE | (os->first_is_dot_assign ? 0 : SHF_ALLOC);
    }
    write_output();
    return 0;
}
