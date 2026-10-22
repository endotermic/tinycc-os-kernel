/* SPDX-License-Identifier: BSD-2-Clause */
/* nm: GNU-compatible subset (default, -n, -S, -u, -p, -g, -A/-o not needed) */
#include "tld.h"

typedef struct { GSym *s; char type; const char *name; } NmEnt;
static int opt_numeric, opt_size, opt_undef_only, opt_defined_only, opt_nosort, opt_reverse, opt_extern_only;

static char sym_class(GElf *e, GSym *s)
{
    int bind = ELF32_ST_BIND(s->info), type = ELF32_ST_TYPE(s->info);
    char c;
    if (s->shndx == SHN_UNDEF) return bind == STB_WEAK ? (type == STT_OBJECT ? 'v' : 'w') : 'U';
    if (bind == STB_WEAK) return type == STT_OBJECT ? 'V' : 'W';
    if (s->shndx == SHN_ABS) c = 'A';
    else if (s->shndx == SHN_COMMON) c = 'C';
    else if (s->shndx >= SHN_LORESERVE || s->shndx >= e->nsecs) c = '?';
    else {
        GSec *sec = &e->secs[s->shndx];
        if (type == STT_GNU_IFUNC) c = 'I';
        else if (sec->flags & SHF_EXECINSTR) c = 'T';
        else if (!(sec->flags & SHF_ALLOC)) c = sec->type == SHT_PROGBITS || sec->type == SHT_NOTE || sec->type == SHT_NOBITS ? 'N' : '?';
        else if (sec->type == SHT_NOBITS) c = 'B';
        else if (!(sec->flags & SHF_WRITE)) c = 'R';
        else c = 'D';
        /* sections that are neither loaded nor allocated in a meaningful way */
        if (!(sec->flags & SHF_ALLOC) && c == 'N' && (sec->flags & SHF_WRITE)) c = '?';
    }
    if (bind == STB_LOCAL && c != '?') c = (char)(c + ('a' - 'A'));
    if (bind == STB_GNU_UNIQUE) c = 'u';
    return c;
}

static int cmp_name(const void *a, const void *b)
{
    const NmEnt *x = a, *y = b;
    int r = strcmp(x->name, y->name);
    if (r) return r;
    return x->s->value < y->s->value ? -1 : x->s->value > y->s->value ? 1 : 0;
}
static int cmp_num(const void *a, const void *b)
{
    const NmEnt *x = a, *y = b;
    int xu = x->s->shndx == SHN_UNDEF, yu = y->s->shndx == SHN_UNDEF;
    if (xu != yu) return xu ? -1 : 1;     /* undefined first in numeric sort */
    if (x->s->value != y->s->value) return x->s->value < y->s->value ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void nm_file(const char *path, int multi)
{
    size_t size;
    uint8_t *image = read_file(path, &size);
    GElf *e;
    NmEnt *ents;
    int n = 0, i, width;
    if (is_archive(image, size)) {
        Archive *ar = archive_read(path, image, size);
        for (i = 0; i < ar->nmembers; i++) {
            ArMember *m = &ar->members[i];
            archive_load_member(ar, m);
            if (m->size < 4 || memcmp(m->data, ELFMAG, 4)) continue;
            printf("\n%s:\n", m->name);
            {
                GElf *me = gelf_read(m->name, m->data, m->size, 0);
                NmEnt *me_ents = xcalloc(me->nsyms ? me->nsyms : 1, sizeof *me_ents);
                int k = 0;
                for (int j = 1; j < me->nsyms; j++) {
                    GSym *s = &me->syms[j];
                    int type = ELF32_ST_TYPE(s->info);
                    if (type == STT_SECTION || type == STT_FILE) continue;
                    me_ents[k].s = s; me_ents[k].name = s->name; me_ents[k].type = sym_class(me, s); k++;
                }
                if (!opt_nosort) qsort(me_ents, k, sizeof *me_ents, opt_numeric ? cmp_num : cmp_name);
                for (int j = 0; j < k; j++) {
                    GSym *s = me_ents[j].s;
                    int w = me->is64 ? 16 : 8;
                    if (s->shndx == SHN_UNDEF) printf("%*s", w, "");
                    else printf("%0*llx", w, (unsigned long long)s->value);
                    if (opt_size && s->size) printf(" %0*llx", w, (unsigned long long)s->size);
                    printf(" %c %s\n", me_ents[j].type, me_ents[j].name);
                }
                free(me_ents);
            }
        }
        return;
    }
    e = gelf_read(path, image, size, 1);
    if (multi) printf("\n%s:\n", path);
    if (e->nsyms == 0) { fprintf(stderr, "%s: %s: no symbols\n", progname, path); return; }
    ents = xcalloc(e->nsyms, sizeof *ents);
    for (i = 1; i < e->nsyms; i++) {
        GSym *s = &e->syms[i];
        int type = ELF32_ST_TYPE(s->info), bind = ELF32_ST_BIND(s->info);
        if (type == STT_SECTION || type == STT_FILE) continue;
        if (!s->name[0]) continue;
        if (opt_undef_only && s->shndx != SHN_UNDEF) continue;
        if (opt_defined_only && s->shndx == SHN_UNDEF) continue;
        if (opt_extern_only && bind == STB_LOCAL) continue;
        ents[n].s = s; ents[n].name = s->name; ents[n].type = sym_class(e, s); n++;
    }
    if (!opt_nosort) qsort(ents, n, sizeof *ents, opt_numeric ? cmp_num : cmp_name);
    if (opt_reverse) for (i = 0; i < n / 2; i++) { NmEnt t = ents[i]; ents[i] = ents[n - 1 - i]; ents[n - 1 - i] = t; }
    width = e->is64 ? 16 : 8;
    for (i = 0; i < n; i++) {
        GSym *s = ents[i].s;
        if (s->shndx == SHN_UNDEF) printf("%*s", width, "");
        else printf("%0*llx", width, (unsigned long long)s->value);
        if (opt_size && s->size) printf(" %0*llx", width, (unsigned long long)s->size);
        printf(" %c %s\n", ents[i].type, ents[i].name);
    }
    free(ents);
}

int nm_main(int argc, char **argv)
{
    PArr files = { 0 };
    int i;
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || !strcmp(a, "-")) { parr_add(&files, a); continue; }
        if (!strcmp(a, "-n") || !strcmp(a, "--numeric-sort") || !strcmp(a, "-v")) opt_numeric = 1;
        else if (!strcmp(a, "-S") || !strcmp(a, "--print-size")) opt_size = 1;
        else if (!strcmp(a, "-u") || !strcmp(a, "--undefined-only")) opt_undef_only = 1;
        else if (!strcmp(a, "--defined-only") || !strcmp(a, "-U")) opt_defined_only = 1;
        else if (!strcmp(a, "-p") || !strcmp(a, "--no-sort")) opt_nosort = 1;
        else if (!strcmp(a, "-r") || !strcmp(a, "--reverse-sort")) opt_reverse = 1;
        else if (!strcmp(a, "-g") || !strcmp(a, "--extern-only")) opt_extern_only = 1;
        else if (!strcmp(a, "--version") || !strcmp(a, "-V")) { printf("GNU nm (tld) 2.47\n"); return 0; }
        else if (a[1] != '-' && a[1]) {
            /* combined short options like -nS */
            for (char *c = a + 1; *c; c++) {
                switch (*c) {
                case 'n': case 'v': opt_numeric = 1; break;
                case 'S': opt_size = 1; break;
                case 'u': opt_undef_only = 1; break;
                case 'p': opt_nosort = 1; break;
                case 'r': opt_reverse = 1; break;
                case 'g': opt_extern_only = 1; break;
                case 'U': opt_defined_only = 1; break;
                default: fatal("unknown option -%c", *c);
                }
            }
        }
        else fatal("unknown option %s", a);
    }
    if (!files.n) parr_add(&files, "a.out");
    for (i = 0; i < files.n; i++) nm_file(files.p[i], files.n > 1);
    return 0;
}
