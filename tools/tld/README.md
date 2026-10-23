# tld — a small binutils replacement for the tinycc-os-kernel link

`tld` is one C99 program (no libelf, no GNU extensions; it compiles with gcc and with the
project's tcc) that provides the parts of GNU binutils the Linux x86_64 kernel build needs
after the compiler:

| tool (chosen by `argv[0]` basename or `--tool=NAME`) | what is implemented |
|---|---|
| `ld` (also when the binary is called `tld`) | static linker for ELF64 x86-64 and ELF32 i386 relocatable inputs, GNU linker scripts (the subset in the kernel's `vmlinux.lds`, `setup.ld`, `realmode.lds`, `compressed/vmlinux.lds`, `vdso64.lds`), `-r`, `--emit-relocs`, `-shared` with VERSION scripts (the vDSO), `-pie` as a static link that refuses run-time relocations |
| `nm` | default, `-n`, `-S`, `-u`, `-g`, `-p`, `-r`, archives; GNU symbol classes and output format |
| `objcopy` / `strip` | `-O binary` (with `-R`, `-j`, `-S`), ELF→ELF copy with `-R/--remove-section=glob` (incl. `!glob`), `-S`, `--strip-debug`, `--set-section-flags SEC=…`, `-w --strip-unneeded-symbol=glob`, in-place operation |
| `readelf` | `-r` / `-rW` (relocations), GNU-like output; everything else is unimplemented |
| `ar` | `c r q s S T D P t x`; thin archives byte-identical to GNU ar's deterministic output (member names relative to the archive directory, like bfd) |

`ld -v`/`--version` print `GNU ld (tld) 2.47` so that `scripts/ld-version.sh` and the
`ld-option` Kconfig probe work; an unknown option makes `ld -v` fail, a known one succeed.

## Building and testing

    make            # builds tld and the ld/nm/objcopy/readelf/ar/strip symlinks
    make test       # = sh test.sh: links the artifacts of a GNU-built kernel tree with tld
                    #   and compares with the GNU results (BUILD=<tree> to choose it)

`test.sh` needs a kernel build tree that was linked with GNU ld (the project's
`build/x86_64/linux` after `scripts/build.sh kernel`), the host binutils for the
comparisons, and `LC_ALL=C` (Kbuild sets it; nm's sort order depends on it).  It checks,
for setup.elf, realmode.elf, the compressed vmlinux, vmlinux(.unstripped), vmlinux.o and
the vDSO: `nm -n` identical, `objcopy -O binary` images identical (setup.bin, realmode.bin,
boot/vmlinux.bin, the vmlinux image without the build-id note), `relocs --realmode`,
`modpost`, `check-function-names.sh`, `vdso2c`, `zoffset.h`/`voffset.h`/`pasyms.h`/
`System.map` derived with tld's nm, `modules.builtin.modinfo`, and `ar` output bytes.
All 28 checks pass against the reference tree of 2026-10-22.

## How the linker works

1. Inputs are read into a generic 64-bit form (`input.c`); archives (thin or not, nested)
   are scanned member by member, members are pulled when they define a symbol that is
   referenced but undefined (`--whole-archive` pulls all; `--start-group` iterates).
2. The linker script is parsed into an AST (`script.c`).  Without `-T`, `-r` uses a
   generated script with one output section per input section name (kept apart by
   NOBITS/ALLOC kind, in GNU's default order `.text .rodata .data .bss .comment
   .note.GNU-stack`, then first appearance); other links use a minimal default script.
3. Input sections are assigned to output sections in script order; within one input
   spec the sections keep input order unless `SORT`/`SORT_BY_ALIGNMENT`.  Orphans go
   after the last section of the same class and warn with `--orphan-handling=warn`;
   they are laid out where they sit, their inputs one after the other at their alignment
   (since 2026-10-22; before, an orphan with contents was dropped as empty, which the
   kernel links never noticed because their only orphans are empty `.init.*` sections).
   GNU ld orders orphans of different classes among themselves differently (read-only data
   before writable data); the layout is valid either way.
4. `SHF_MERGE` sections without relocations are merged (exact duplicates; suffix merging
   for byte strings), placed at the first member's position, like GNU ld.
5. Layout is iterated until addresses and symbol values converge; expressions carry a
   section (relative) or none (absolute) with GNU ld's rules (`.` outside a section is
   relative to the section containing dot, `ABSOLUTE()`, `ADDR()`, `ALIGN()`, `SIZEOF`,
   `SIZEOF_HEADERS`, `AT()`, `PROVIDE`, `ASSERT`, …).  Empty output sections are removed
   unless they assign to `.` or contain KEEP'd/linker-created input; symbols assigned
   relative to a removed section are moved to a neighbouring section like bfd's fix_syms.
6. Relocations: R_X86_64_{64,32,32S,PC32,PLT32,PC64,16,PC16,8,PC8} and R_386_{32,PC32,
   PLT32,16,PC16,8,PC8,GOTOFF,GOTPC} (the GOT-relative pair of the 32-bit decompressor and
   vDSO, computed against a linker-defined `_GLOBAL_OFFSET_TABLE_`; no GOT is built).  GOTPCREL/GOTPCRELX/REX_GOTPCRELX are relaxed exactly like GNU ld
   (no GOT is ever created): `mov sym@GOTPCREL(%rip),%reg` becomes `mov $sym,%reg` in a
   non-PIC link and `lea sym(%rip),%reg` in `-pie`/`-shared`; `call/jmp *sym@GOTPCREL(%rip)`
   become direct branches.  In `-pie` an absolute relocation against a defined symbol is
   an error (the kernel's script asserts there are none).  Relocations into NOBITS
   sections are dropped (tcc emits some).
7. Output: program headers from `PHDRS` or from the sections (one PT_LOAD per flag change
   on a new page, plus PT_GNU_STACK), file offsets congruent to the address modulo the
   max page size, gaps in code sections filled with the same NOP patterns as GNU ld
   (measured: 10-byte NOPs plus a remainder on x86-64, `66 90` pairs on i386), `.symtab`
   with FILE/local symbols per object then globals (hidden symbols referenced through a
   PLT become local in PIC links, version-script locals become local, `.L` labels in
   merge sections are dropped), `--build-id=sha1` over the section contents, `-shared`
   dynamic sections (`.dynsym .dynstr .hash .gnu.hash .gnu.version .gnu.version_d
   .dynamic`, `DT_SYMBOLIC` for `-Bsymbolic`).

Verified equal to GNU ld 2.47 on the kernel: every symbol address, the loadable images
byte for byte (the only differences are the build-id note contents, the file offset of
zero-sized sections, and `.strtab` sharing).

## Not implemented (not needed by the kernel configuration)

* `-z pack-relative-relocs` / `--pack-dyn-relocs=relr` (RELR): rejected with a message that
  names the option, which is what the kernel's `scripts/tools-support-relr.sh` probe expects
  from a linker without RELR (`CONFIG_TOOLS_SUPPORT_RELR=n`).

* GOT/PLT creation, dynamic relocations, TLS relocations, `-shared` symbol interposition
  (everything binds locally, as with `-Bsymbolic`), DT_NEEDED/shared library inputs.
* `.eh_frame_hdr` generation: `--eh-frame-hdr` is accepted, `.eh_frame` input is copied,
  but no header is built and the PT_GNU_EH_FRAME program header is dropped when empty.
  tcc produces no `.eh_frame`, so the tcc-built vDSO is unaffected; with gcc objects the
  vDSO layout differs from GNU's by the missing `.eh_frame_hdr` and `.got.plt` (`vdso2c`
  still produces the same image size, alternatives and symbol offsets).
* `.note.gnu.property` input sections are dropped (GNU ld merges them into one note).
* MEMORY regions, `--gc-sections`, `INSERT`, `EXCLUDE_FILE`, `-Map`, `--defsym` with
  expressions, `readelf` beyond `-r`, `objcopy -O elf32-x86-64`, `ar` operations other
  than create/replace/list/extract.

## Kernel-side notes

No kernel change is required.  Observed input quirks that tld tolerates: tcc emits
`.ref.data`/`.data..cacheline_aligned` as NOBITS with a relocation section (GNU ld keeps
such same-named sections apart in `-r` output and drops the relocations; tld does the
same), and `.L`-prefixed undefined global symbols from asm macros (ignored unless a
relocation uses them, as GNU ld does).
