/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * libelf.h - the subset of the libelf API that the kernel's objtool uses, for tinycc-os-kernel.
 *
 * API-compatible with elfutils' libelf for these functions (same names, types and
 * semantics, in particular the section layout that elf_update() computes when
 * ELF_F_LAYOUT is not set), so that tools/objtool builds against it unchanged.  Reads and
 * writes ELF32 and ELF64 files of the host's byte order; no program headers are written,
 * no archives, no byte swapping, no translation of data (d_buf is the file's own encoding).
 * See README.md for the list of functions.
 */
#ifndef _LIBELF_H
#define _LIBELF_H 1

#include <elf.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct Elf Elf;
typedef struct Elf_Scn Elf_Scn;

/* data element types (d_type); only the names matter here, the data is never translated */
typedef enum {
    ELF_T_BYTE,
    ELF_T_ADDR,
    ELF_T_DYN,
    ELF_T_EHDR,
    ELF_T_HALF,
    ELF_T_OFF,
    ELF_T_PHDR,
    ELF_T_RELA,
    ELF_T_REL,
    ELF_T_SHDR,
    ELF_T_SWORD,
    ELF_T_SYM,
    ELF_T_WORD,
    ELF_T_XWORD,
    ELF_T_SXWORD,
    ELF_T_NUM
} Elf_Type;

typedef struct {
    void *d_buf;
    Elf_Type d_type;
    unsigned int d_version;
    size_t d_size;
    int64_t d_off;
    size_t d_align;
} Elf_Data;

typedef enum {
    ELF_C_NULL,
    ELF_C_READ,
    ELF_C_RDWR,
    ELF_C_WRITE,
    ELF_C_CLR,
    ELF_C_SET,
    ELF_C_FDDONE,
    ELF_C_FDREAD,
    ELF_C_READ_MMAP,
    ELF_C_RDWR_MMAP,
    ELF_C_WRITE_MMAP,
    ELF_C_READ_MMAP_PRIVATE,
    ELF_C_EMPTY,
    ELF_C_NUM
} Elf_Cmd;

typedef enum {
    ELF_K_NONE,
    ELF_K_AR,
    ELF_K_COFF,
    ELF_K_ELF,
    ELF_K_NUM
} Elf_Kind;

enum {
    ELF_F_DIRTY = 0x1,
#define ELF_F_DIRTY ELF_F_DIRTY
    ELF_F_LAYOUT = 0x4,
#define ELF_F_LAYOUT ELF_F_LAYOUT
    ELF_F_PERMISSIVE = 0x8
#define ELF_F_PERMISSIVE ELF_F_PERMISSIVE
};

/* descriptors */
Elf *elf_begin(int fd, Elf_Cmd cmd, Elf *ref);
int elf_end(Elf *elf);
Elf_Kind elf_kind(Elf *elf);
unsigned int elf_version(unsigned int version);
unsigned int elf_flagelf(Elf *elf, Elf_Cmd cmd, unsigned int flags);
int64_t elf_update(Elf *elf, Elf_Cmd cmd);

/* errors */
int elf_errno(void);
const char *elf_errmsg(int error);

/* sections */
Elf_Scn *elf_getscn(Elf *elf, size_t index);
Elf_Scn *elf_newscn(Elf *elf);
Elf_Scn *elf_nextscn(Elf *elf, Elf_Scn *scn);
size_t elf_ndxscn(Elf_Scn *scn);
int elf_getshdrnum(Elf *elf, size_t *dst);
int elf_getshdrstrndx(Elf *elf, size_t *dst);
char *elf_strptr(Elf *elf, size_t index, size_t offset);

/* section data */
Elf_Data *elf_getdata(Elf_Scn *scn, Elf_Data *data);
Elf_Data *elf_newdata(Elf_Scn *scn);

/* the file header of a file of that class (NULL for the other class); read-only here: the
   header is written through gelf_update_ehdr() */
Elf32_Ehdr *elf32_getehdr(Elf *elf);
Elf64_Ehdr *elf64_getehdr(Elf *elf);

#endif /* libelf.h */
