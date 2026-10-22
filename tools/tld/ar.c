/* SPDX-License-Identifier: BSD-2-Clause */
/* ar: create/replace members, thin archives (T), deterministic (D), full paths (P), symbol index (s),
   move members (m with a/b/i position, Kbuild's built-in-fixup.a). */
#include "tld.h"
#include <errno.h>

static void put_field(char *dst, int width, const char *s)
{
    int n = (int)strlen(s);
    memset(dst, ' ', width);
    memcpy(dst, s, n > width ? width : n);
}

/* split a path into normalized components ("." dropped, "a/.." folded) */
static int path_components(const char *path, char **comps, int max)
{
    char *copy = xstrdup(path), *tok = strtok(copy, "/");
    int n = 0;
    while (tok) {
        if (!strcmp(tok, ".")) {}
        else if (!strcmp(tok, "..") && n > 0 && strcmp(comps[n - 1], "..")) free(comps[--n]);
        else if (n < max) comps[n++] = xstrdup(tok);
        tok = strtok(NULL, "/");
    }
    free(copy);
    return n;
}

/* GNU ar stores thin-archive member names relative to the archive's directory when both
   paths are relative (bfd's adjust_relative_path); absolute archive paths store the name as given */
static char *thin_member_name(const char *archive, const char *member)
{
    char *ac[256], *mc[256], *out, *p;
    int na, nm, i, common = 0;
    const char *aslash = strrchr(archive, '/');
    char *adir;
    if (archive[0] == '/' || member[0] == '/') return xstrdup(member);
    adir = aslash ? xstrndup(archive, aslash - archive) : xstrdup("");
    na = path_components(adir, ac, 256);
    nm = path_components(member, mc, 256);
    free(adir);
    while (common < na && common < nm && !strcmp(ac[common], mc[common])) common++;
    out = xmalloc(3 * (na - common) + strlen(member) + 2);
    p = out;
    for (i = common; i < na; i++) { memcpy(p, "../", 3); p += 3; }
    for (i = common; i < nm; i++) { size_t l = strlen(mc[i]); memcpy(p, mc[i], l); p += l; if (i + 1 < nm) *p++ = '/'; }
    *p = 0;
    for (i = 0; i < na; i++) free(ac[i]);
    for (i = 0; i < nm; i++) free(mc[i]);
    return out;
}

int ar_main(int argc, char **argv)
{
    const char *ops, *archive;
    int thin = 0, create = 0, replace = 0, symidx = 1, quick = 0, i, nmem = 0, has_t = 0, has_x = 0;
    int move = 0, pos_after = 0, has_pos = 0, s_given = 0;
    const char *posname = NULL;
    char **mem;
    StrTab names = { 0 };
    uint8_t *out; size_t outcap = 1 << 20, outlen = 0;
    PArr idx_names = { 0 }; PArr idx_offs = { 0 };
    size_t *member_off;

    if (argc < 3) fatal("usage: ar [-]{crsTDP...} archive members...");
    ops = argv[1];
    if (ops[0] == '-') ops++;
    for (const char *p = ops; *p; p++) {
        switch (*p) {
        case 'c': create = 1; break;
        case 'r': replace = 1; break;
        case 'q': quick = 1; break;
        case 's': symidx = 1; s_given = 1; break;
        case 'm': move = 1; break;
        case 'a': pos_after = 1; has_pos = 1; break;
        case 'b': case 'i': has_pos = 1; break;
        case 'S': symidx = 0; break;
        case 'T': thin = 1; break;
        case 'D': case 'P': case 'u': case 'v': case 'U': break;
        case 't': has_t = 1; break;
        case 'x': has_x = 1; break;
        case '-': break;
        default: fatal("unsupported ar operation/modifier '%c'", *p);
        }
    }
    if (!strcmp(ops, "--version")) { printf("GNU ar (tld) 2.47\n"); return 0; }
    if (move && has_pos) {
        if (argc < 4) fatal("usage: ar m[abi] posname archive members...");
        posname = argv[2]; archive = argv[3]; mem = argv + 4; nmem = argc - 4;
    } else {
        archive = argv[2];
        mem = argv + 3; nmem = argc - 3;
    }
    if (move && !s_given) symidx = 0; /* like GNU ar: an archive without an index stays without */
    if (has_t || has_x) {
        size_t size; uint8_t *img = read_file(archive, &size);
        Archive *ar = archive_read(archive, img, size);
        for (i = 0; i < ar->nmembers; i++) {
            if (has_t) printf("%s\n", ar->members[i].name);
            if (has_x) {
                FILE *f; const char *b = strrchr(ar->members[i].name, '/'); b = b ? b + 1 : ar->members[i].name;
                archive_load_member(ar, &ar->members[i]);
                f = fopen(b, "wb"); if (!f) fatal("cannot create %s", b);
                fwrite(ar->members[i].data, 1, ar->members[i].size, f); fclose(f);
            }
        }
        return 0;
    }
    if (!replace && !quick && !move) fatal("only 'r'/'q'/'m' (create/replace/move) are implemented");
    (void)create;
    /* We always rebuild the archive from scratch: existing members are kept unless replaced. */
    {
        PArr all = { 0 };     /* member paths */
        PArr all_data = { 0 }; PArr all_size = { 0 };
        FILE *f = fopen(archive, "rb");
        if (f) {
            size_t size; uint8_t *img;
            fclose(f);
            img = read_file(archive, &size);
            if (is_archive(img, size)) {
                Archive *ar = archive_read(archive, img, size);
                for (i = 0; i < ar->nmembers; i++) {
                    int dup = 0;
                    if (!move)
                        for (int k = 0; k < nmem; k++) { char *rn = thin_member_name(archive, mem[k]); if (!strcmp(mem[k], ar->members[i].name) || !strcmp(rn, ar->members[i].name)) dup = 1; free(rn); }
                    if (dup) continue;
                    archive_load_member(ar, &ar->members[i]);
                    parr_add(&all, ar->members[i].name); parr_add(&all_data, ar->members[i].data); parr_add(&all_size, (void *)(uintptr_t)ar->members[i].size);
                }
                thin = thin || ar->thin;
            }
        }
        if (move) {
            /* reorder: the named members, in the order given, go before (b/i) or
               after (a) the member 'posname', or to the end without a position */
            PArr n2 = { 0 }, d2 = { 0 }, s2 = { 0 };
            int *moved = xcalloc(all.n ? all.n : 1, sizeof *moved), pos = -1, k;
            for (k = 0; k < nmem; k++) {
                for (i = 0; i < all.n; i++) if (!strcmp(all.p[i], mem[k])) break;
                if (i == all.n) fatal("no member named '%s' in %s", mem[k], archive);
                moved[i] = k + 1;
            }
            for (i = 0; i < all.n; i++) {
                if (moved[i]) continue;
                if (posname && !strcmp(all.p[i], posname)) pos = n2.n;
                parr_add(&n2, all.p[i]); parr_add(&d2, all_data.p[i]); parr_add(&s2, all_size.p[i]);
            }
            if (posname && pos < 0) fatal("no member named '%s' in %s", posname, archive);
            if (pos < 0) pos = n2.n; else if (pos_after) pos++;
            /* insert at pos */
            for (k = nmem - 1; k >= 0; k--) {
                for (i = 0; i < all.n; i++) if (moved[i] == k + 1) break;
                parr_add(&n2, NULL); parr_add(&d2, NULL); parr_add(&s2, NULL);
                memmove(n2.p + pos + 1, n2.p + pos, (n2.n - pos - 1) * sizeof *n2.p);
                memmove(d2.p + pos + 1, d2.p + pos, (d2.n - pos - 1) * sizeof *d2.p);
                memmove(s2.p + pos + 1, s2.p + pos, (s2.n - pos - 1) * sizeof *s2.p);
                n2.p[pos] = all.p[i]; d2.p[pos] = all_data.p[i]; s2.p[pos] = all_size.p[i];
            }
            all = n2; all_data = d2; all_size = s2;
            free(moved);
        } else
        for (i = 0; i < nmem; i++) {
            size_t size; uint8_t *d = read_file(mem[i], &size);
            parr_add(&all, thin ? thin_member_name(archive, mem[i]) : (void *)mem[i]); parr_add(&all_data, d); parr_add(&all_size, (void *)(uintptr_t)size);
        }
        /* long names table */
        strtab_init(&names); names.len = 0;
        {
            int *nameoff = xcalloc(all.n ? all.n : 1, sizeof *nameoff);
            for (i = 0; i < all.n; i++) {
                const char *n = all.p[i];
                char *entry = xasprintf("%s/\n", n);
                nameoff[i] = (int)names.len;
                while (names.len + strlen(entry) + 1 > names.cap) { names.cap *= 2; names.buf = xrealloc(names.buf, names.cap); }
                memcpy(names.buf + names.len, entry, strlen(entry)); names.len += strlen(entry);
                free(entry);
            }
            out = xmalloc(outcap);
            member_off = xcalloc(all.n ? all.n : 1, sizeof *member_off);
#define EMIT(ptr, n) do { while (outlen + (n) > outcap) { outcap *= 2; out = xrealloc(out, outcap); } memcpy(out + outlen, ptr, n); outlen += (n); } while (0)
            EMIT(thin ? "!<thin>\n" : "!<arch>\n", 8);
            /* symbol index placeholder: written at the end (we need member offsets) */
            {
                /* collect symbol names */
                for (i = 0; i < all.n; i++) {
                    uint8_t *d = all_data.p[i]; size_t sz = (size_t)(uintptr_t)all_size.p[i];
                    if (sz >= 4 && !memcmp(d, ELFMAG, 4)) {
                        GElf *e = gelf_read(all.p[i], d, sz, 0);
                        for (int k = 1; k < e->nsyms; k++) {
                            GSym *s = &e->syms[k];
                            if (ELF32_ST_BIND(s->info) == STB_LOCAL || s->shndx == SHN_UNDEF || !s->name[0]) continue;
                            if (ELF32_ST_TYPE(s->info) == STT_SECTION || ELF32_ST_TYPE(s->info) == STT_FILE) continue;
                            parr_add(&idx_names, xstrdup(s->name)); parr_add(&idx_offs, (void *)(uintptr_t)i);
                        }
                    }
                }
            }
            {
                /* layout: [symbol index member] [// long names] members... ; compute sizes first */
                size_t symtab_size = 0, hdr = 60;
                size_t names_size = names.len + (names.len & 1);   /* GNU ar pads the table and counts the pad */
                if (names.len & 1) { while (names.len + 1 > names.cap) { names.cap *= 2; names.buf = xrealloc(names.buf, names.cap); } names.buf[names.len++] = '\n'; }
                size_t off;
                char h[60];
                if (symidx) {
                    symtab_size = 4 + 4 * idx_names.n;
                    for (i = 0; i < idx_names.n; i++) symtab_size += strlen(idx_names.p[i]) + 1;
                }
                off = 8;
                if (symidx) off += hdr + symtab_size + (symtab_size & 1);
                off += hdr + names_size + (names_size & 1);
                for (i = 0; i < all.n; i++) {
                    member_off[i] = off;
                    off += hdr;
                    if (!thin) { size_t sz = (size_t)(uintptr_t)all_size.p[i]; off += sz + (sz & 1); }
                }
                if (symidx) {
                    uint8_t *st = xcalloc(symtab_size + 1, 1); size_t p = 4;
                    char num[24];
                    uint32_t n = (uint32_t)idx_names.n;
                    st[0] = n >> 24; st[1] = n >> 16; st[2] = n >> 8; st[3] = n;
                    for (i = 0; i < idx_names.n; i++) { uint32_t o = (uint32_t)member_off[(int)(uintptr_t)idx_offs.p[i]]; st[p] = o >> 24; st[p+1] = o >> 16; st[p+2] = o >> 8; st[p+3] = o; p += 4; }
                    for (i = 0; i < idx_names.n; i++) { size_t l = strlen(idx_names.p[i]) + 1; memcpy(st + p, idx_names.p[i], l); p += l; }
                    memset(h, ' ', 60); put_field(h, 16, "/"); put_field(h + 16, 12, "0"); put_field(h + 28, 6, "0"); put_field(h + 34, 6, "0"); put_field(h + 40, 8, "0");
                    snprintf(num, sizeof num, "%zu", symtab_size); put_field(h + 48, 10, num); memcpy(h + 58, "`\n", 2);
                    EMIT(h, 60); EMIT(st, symtab_size); if (symtab_size & 1) EMIT("\n", 1);
                    free(st);
                }
                {
                    char num[24];
                    memset(h, ' ', 60); put_field(h, 16, "//"); put_field(h + 16, 12, ""); put_field(h + 28, 6, ""); put_field(h + 34, 6, ""); put_field(h + 40, 8, "");
                    snprintf(num, sizeof num, "%zu", names_size); put_field(h + 48, 10, num); memcpy(h + 58, "`\n", 2);
                    EMIT(h, 60); EMIT(names.buf, names_size);
                }
                for (i = 0; i < all.n; i++) {
                    char num[24], nm[24];
                    size_t sz = (size_t)(uintptr_t)all_size.p[i];
                    memset(h, ' ', 60);
                    snprintf(nm, sizeof nm, "/%d", nameoff[i]); put_field(h, 16, nm);
                    put_field(h + 16, 12, "0"); put_field(h + 28, 6, "0"); put_field(h + 34, 6, "0"); put_field(h + 40, 8, "644");
                    snprintf(num, sizeof num, "%zu", sz); put_field(h + 48, 10, num); memcpy(h + 58, "`\n", 2);
                    EMIT(h, 60);
                    if (!thin) { EMIT(all_data.p[i], sz); if (sz & 1) EMIT("\n", 1); }
                }
            }
#undef EMIT
        }
    }
    {
        FILE *f = fopen(archive, "wb");
        if (!f) fatal("cannot create %s: %s", archive, strerror(errno));
        fwrite(out, 1, outlen, f);
        if (fclose(f)) fatal("cannot write %s", archive);
    }
    return 0;
}
