/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * gelf.h - the class-independent ("generic") part of the libelf subset, as in elfutils:
 * the GElf_* types are the ELF64 types; the functions convert for ELF32 files.
 */
#ifndef _GELF_H
#define _GELF_H 1

#include "libelf.h"

typedef Elf64_Half GElf_Half;
typedef Elf64_Word GElf_Word;
typedef Elf64_Sword GElf_Sword;
typedef Elf64_Xword GElf_Xword;
typedef Elf64_Sxword GElf_Sxword;
typedef Elf64_Addr GElf_Addr;
typedef Elf64_Off GElf_Off;
typedef Elf64_Ehdr GElf_Ehdr;
typedef Elf64_Shdr GElf_Shdr;
typedef Elf64_Section GElf_Section;
typedef Elf64_Sym GElf_Sym;
typedef Elf64_Rel GElf_Rel;
typedef Elf64_Rela GElf_Rela;
typedef Elf64_Phdr GElf_Phdr;
typedef Elf64_Dyn GElf_Dyn;
typedef Elf64_Nhdr GElf_Nhdr;

#define GELF_ST_BIND(val) ELF64_ST_BIND(val)
#define GELF_ST_TYPE(val) ELF64_ST_TYPE(val)
#define GELF_ST_INFO(bind, type) ELF64_ST_INFO(bind, type)
#define GELF_ST_VISIBILITY(val) ELF64_ST_VISIBILITY(val)
#define GELF_R_SYM(info) ELF64_R_SYM(info)
#define GELF_R_TYPE(info) ELF64_R_TYPE(info)
#define GELF_R_INFO(sym, type) ELF64_R_INFO(sym, type)

int gelf_getclass(Elf *elf);

/* file header */
GElf_Ehdr *gelf_getehdr(Elf *elf, GElf_Ehdr *dst);
unsigned long gelf_newehdr(Elf *elf, int class);
int gelf_update_ehdr(Elf *elf, GElf_Ehdr *src);

/* section headers */
GElf_Shdr *gelf_getshdr(Elf_Scn *scn, GElf_Shdr *dst);
int gelf_update_shdr(Elf_Scn *scn, GElf_Shdr *src);

/* symbols: ndx counts entries of the given data block (not of the section) */
GElf_Sym *gelf_getsym(Elf_Data *data, int ndx, GElf_Sym *dst);
int gelf_update_sym(Elf_Data *data, int ndx, GElf_Sym *src);
GElf_Sym *gelf_getsymshndx(Elf_Data *symdata, Elf_Data *shndxdata, int ndx, GElf_Sym *dst, Elf32_Word *xshndx);
int gelf_update_symshndx(Elf_Data *symdata, Elf_Data *shndxdata, int ndx, GElf_Sym *src, Elf32_Word xshndx);

/* relocations */
GElf_Rel *gelf_getrel(Elf_Data *data, int ndx, GElf_Rel *dst);
GElf_Rela *gelf_getrela(Elf_Data *data, int ndx, GElf_Rela *dst);
int gelf_update_rel(Elf_Data *data, int ndx, GElf_Rel *src);
int gelf_update_rela(Elf_Data *data, int ndx, GElf_Rela *src);

/* sizes of the data types in the file for the descriptor's class */
size_t gelf_fsize(Elf *elf, Elf_Type type, size_t count, unsigned int version);

#endif /* gelf.h */
