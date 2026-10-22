/* SPDX-License-Identifier: BSD-2-Clause */
/* readelf: only -r/--relocs (and -W) are implemented, with GNU-like output. */
#include "tld.h"

static const char *rtype_name(int machine, uint32_t t)
{
    if (machine == EM_X86_64) {
        static const char *n[] = { "R_X86_64_NONE", "R_X86_64_64", "R_X86_64_PC32", "R_X86_64_GOT32", "R_X86_64_PLT32", "R_X86_64_COPY",
            "R_X86_64_GLOB_DAT", "R_X86_64_JUMP_SLOT", "R_X86_64_RELATIVE", "R_X86_64_GOTPCREL", "R_X86_64_32", "R_X86_64_32S", "R_X86_64_16",
            "R_X86_64_PC16", "R_X86_64_8", "R_X86_64_PC8", "R_X86_64_DTPMOD64", "R_X86_64_DTPOFF64", "R_X86_64_TPOFF64", "R_X86_64_TLSGD",
            "R_X86_64_TLSLD", "R_X86_64_DTPOFF32", "R_X86_64_GOTTPOFF", "R_X86_64_TPOFF32", "R_X86_64_PC64", "R_X86_64_GOTOFF64", "R_X86_64_GOTPC32" };
        if (t < sizeof n / sizeof n[0]) return n[t];
        if (t == 41) return "R_X86_64_GOTPCRELX";
        if (t == 42) return "R_X86_64_REX_GOTPCRELX";
    } else if (machine == EM_386) {
        static const char *n[] = { "R_386_NONE", "R_386_32", "R_386_PC32", "R_386_GOT32", "R_386_PLT32", "R_386_COPY", "R_386_GLOB_DAT",
            "R_386_JUMP_SLOT", "R_386_RELATIVE", "R_386_GOTOFF", "R_386_GOTPC", "R_386_32PLT" };
        if (t < sizeof n / sizeof n[0]) return n[t];
        if (t == 20) return "R_386_16";
        if (t == 21) return "R_386_PC16";
        if (t == 22) return "R_386_8";
        if (t == 23) return "R_386_PC8";
        if (t == 43) return "R_386_GOT32X";
    }
    return "unknown";
}

int readelf_main(int argc, char **argv)
{
    PArr files = { 0 };
    int i, want_relocs = 0;
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-') { parr_add(&files, a); continue; }
        if (!strcmp(a, "--relocs")) want_relocs = 1;
        else if (!strcmp(a, "--wide")) {}
        else if (!strcmp(a, "--version") || !strcmp(a, "-v")) { printf("GNU readelf (tld) 2.47\n"); return 0; }
        else if (a[1] != '-') { for (char *c = a + 1; *c; c++) { if (*c == 'r') want_relocs = 1; else if (*c == 'W') {} else fatal("unsupported readelf option -%c (only -r/-W are implemented)", *c); } }
        else fatal("unsupported readelf option %s", a);
    }
    if (!want_relocs) fatal("only --relocs is implemented");
    for (i = 0; i < files.n; i++) {
        GElf *e = gelf_open(files.p[i]);
        int any = 0;
        if (files.n > 1) printf("\nFile: %s\n", (char *)files.p[i]);
        for (int k = 0; k < e->nsecs; k++) {
            GSec *s = &e->secs[k];
            int rela;
            if (!gelf_is_rel_section(s)) continue;
            rela = s->type == SHT_RELA;
            any = 1;
            printf("\nRelocation section '%s' at offset 0x%llx contains %d entr%s:\n", s->name, (unsigned long long)s->offset, s->nrels, s->nrels == 1 ? "y" : "ies");
            if (e->is64) {
                if (rela) printf("    Offset             Info             Type               Symbol's Value  Symbol's Name + Addend\n");
                else printf("    Offset             Info             Type               Symbol's Value  Symbol's Name\n");
            } else {
                if (rela) printf(" Offset     Info    Type                Sym. Value  Symbol's Name + Addend\n");
                else printf(" Offset     Info    Type                Sym. Value  Symbol's Name\n");
            }
            for (int j = 0; j < s->nrels; j++) {
                GRel *r = &s->rels[j];
                const char *symname = "";
                uint64_t symval = 0;
                int type = 0;
                GElf *src = e;
                if (s->link < (uint32_t)e->nsecs) {
                    /* the symbol table may be .dynsym; our reader only decodes .symtab, so decode on the fly */
                    GSec *st = &e->secs[s->link];
                    GSec *strs = st->link < (uint32_t)e->nsecs ? &e->secs[st->link] : NULL;
                    size_t ent = e->is64 ? 24 : 16;
                    if (st->data && strs && strs->data && (size_t)(r->sym + 1) * ent <= st->size) {
                        const uint8_t *p = st->data + r->sym * ent;
                        uint32_t no = rd32(p);
                        if (e->is64) { symval = rd64(p + 8); type = ELF32_ST_TYPE(p[4]); if (type == STT_SECTION) { uint16_t sh = rd16(p + 6); symname = sh < e->nsecs ? e->secs[sh].name : ""; } else symname = no < strs->size ? (const char *)strs->data + no : ""; }
                        else { symval = rd32(p + 4); type = ELF32_ST_TYPE(p[12]); if (type == STT_SECTION) { uint16_t sh = rd16(p + 14); symname = sh < e->nsecs ? e->secs[sh].name : ""; } else symname = no < strs->size ? (const char *)strs->data + no : ""; }
                    }
                }
                (void)src;
                if (e->is64) {
                    printf("%016llx  %016llx %-18s ", (unsigned long long)r->offset, (unsigned long long)(((uint64_t)r->sym << 32) | r->type), rtype_name(e->e_machine, r->type));
                    if (r->sym) printf("%016llx %s", (unsigned long long)symval, symname); else printf("                 ");
                    if (rela) { if (r->sym) printf(" %c %llx", r->addend < 0 ? '-' : '+', (unsigned long long)(r->addend < 0 ? -r->addend : r->addend)); else printf("%llx", (unsigned long long)r->addend); }
                } else {
                    printf("%08llx  %08llx %-18s ", (unsigned long long)r->offset, (unsigned long long)((r->sym << 8) | (r->type & 0xff)), rtype_name(e->e_machine, r->type));
                    if (r->sym) printf("%08llx   %s", (unsigned long long)symval, symname);
                    if (rela && r->sym) printf(" %c %llx", r->addend < 0 ? '-' : '+', (unsigned long long)(r->addend < 0 ? -r->addend : r->addend));
                }
                printf("\n");
            }
        }
        if (!any) printf("\nThere are no relocations in this file.\n");
    }
    return 0;
}
