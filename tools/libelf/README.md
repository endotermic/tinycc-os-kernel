# libelf: the subset the kernel's objtool needs

`tools/libelf` is a small implementation of the libelf API (the elfutils flavour, which
`tools/objtool` in the Linux tree is written against), so that the real objtool can be built
by tcc on a system without elfutils: the host build (`scripts/build.sh libelf`, then Kbuild
builds `tools/objtool` with `HOSTCC=tcc-hostcc` during `prepare`) and the self-hosting guest,
whose `/usr/lib/libelf.a` and `/usr/include/{libelf,gelf}.h` are made from these files
(static musl). Before it, objtool was a stub and the kernel needed a patch for an
`OBJTOOL=` make variable; now the vanilla tree builds with no kernel patch.

One C99 file, `libelf.c` (about 900 lines, no dependencies beyond the C library and
`<elf.h>`), and the two headers, `libelf.h` and `gelf.h`.

| Function | Notes |
|---|---|
| `elf_version`, `elf_begin`, `elf_end`, `elf_kind`, `elf_flagelf`, `elf_errno`, `elf_errmsg` | `elf_begin` reads the whole file (`ELF_C_READ`, `ELF_C_READ_MMAP`, `ELF_C_RDWR`, the `_MMAP` variants read too) or starts an empty one (`ELF_C_WRITE`) |
| `elf_getscn`, `elf_newscn`, `elf_nextscn`, `elf_ndxscn`, `elf_getshdrnum`, `elf_getshdrstrndx`, `elf_strptr` | `elf_newscn` numbers from 1, `elf_nextscn(elf, NULL)` is section 1, as in elfutils; the extended section count and `SHN_XINDEX` string table index are understood and written |
| `elf_getdata`, `elf_newdata` | every section read from the file has one data block pointing into the file image (`d_buf` NULL for `SHT_NOBITS` and empty sections), `elf_newdata` appends blocks; `d_type` follows the section type (`ELF_T_SYM`, `ELF_T_RELA`, `ELF_T_REL`, `ELF_T_WORD`, else `ELF_T_BYTE`), the data is never translated |
| `elf_update` | `ELF_C_NULL` computes, `ELF_C_WRITE` writes the whole file and truncates it. The layout is elfutils' without `ELF_F_LAYOUT`: sections in index order after the ELF header, each at `max(sh_addralign, d_align of its blocks)`, each block at its `d_align` (`d_off` set), `sh_size` = the blocks' total, `SHT_NOBITS` takes no space, the section header table after the last section at the class alignment. No program headers |
| `gelf_getclass`, `gelf_getehdr`, `gelf_newehdr`, `gelf_update_ehdr`, `elf32_getehdr`, `elf64_getehdr` | headers are kept in the ELF64 form and converted for ELF32 files; `gelf_newehdr` also creates section 0 |
| `gelf_getshdr`, `gelf_update_shdr` | |
| `gelf_getsym`, `gelf_update_sym`, `gelf_getsymshndx`, `gelf_update_symshndx`, `gelf_getrel`, `gelf_getrela`, `gelf_update_rel`, `gelf_update_rela`, `gelf_fsize` | the index counts the entries of the given data block, as in libelf; the block's `d_type` must match |

Limits: native byte order only (an `ELFDATA2MSB` file on this host is refused), no archives,
no program headers on output (objtool and `klp-post-link` write object files; `klp-sympos`
and `orc_dump` only read), none of the functions objtool does not call (`elf_rawdata`,
`elf_getphdrnum`, `elf_fill`, `elf_cntl`, ...).

`pkg-config.c` answers `pkg-config libelf --cflags --libs` for a build of this library (the
directory is compiled in); `tools/objtool/Makefile` asks `$(HOSTPKG_CONFIG)`, which
`scripts/build.sh` sets to it for the host build. The guest has no pkg-config and the
Makefile's fallback, `-lelf`, finds `/usr/lib/libelf.a`.

```sh
make CC=tcc AR="tcc -ar" PREFIX=out     # out/include/{libelf,gelf}.h, out/lib/libelf.a, out/pkg-config
sh test.sh                              # see below
```

## Test

`test.sh` compares the objtool built by the project (tcc + this libelf, `tld -r` for the
link, `build/x86_64/linux/tools/objtool/objtool`) with a reference objtool built from the same
kernel tree by gcc against the host's elfutils: both run on a copy of every object Kbuild runs
objtool on in the build tree (found by their `.cmd` files, with Kbuild's options) and the
exit codes, the diagnostics and the written files must be identical. 2026-11-11: 1573
objects, all identical (164 of them with the same warnings from both: `missing __noreturn`,
`unreachable instruction`, UACCESS).
