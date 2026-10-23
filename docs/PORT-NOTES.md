# Port notes: building the Linux kernel with tinycc

State as of 2026-10-22 (evening: self-hosting). Layout and quick start are in `../README.md`;
every fix with its verification is in `FIX-LOG.md`. This file explains how the port works and
what is open.

## Result

The tinyconfig-based x86_64 kernel (`kernels/linux/minimal.config`) is built without gcc
and without binutils: tcc compiles every C file, assembles every `.S` file (42 of them),
compiles the real-mode boot code with `-m16` (through its `i386-tcc` cross compiler) and
produces the asm-offsets text with `-S`; the host tools (fixdep, kconfig, modpost,
sorttable, relocs, vdso2c, mkpiggy, build) are compiled by tcc too; `tools/tld` (our own
ld, nm, objcopy, readelf, ar; 4500 lines of C compiled by tcc) does every link and binary
conversion, including the `-pie` decompressor, the `--emit-relocs` realmode blob and the
`-shared` vDSO with its version script; objtool is a stub (it needs libelf). The build log
shows 1546 tcc and 0 gcc invocations, and the result boots in QEMU (2026-10-22: the
libc-free smoke image and the busybox shell). The one thing gcc still does is build tcc
itself on the host (tcc's own Makefile), which the guest build already does with tcc.
Since the evening of 2026-10-22 the system is self-hosting: the kernel is SMP (the per-cpu
cache-hot layout and head_64.S's `andl $(~mask)` needed tcc fixes 71 and 72), and the
self-hosting image rebuilds the kernel inside the guest with the guest's own tcc, i386-tcc,
tld and GNU make (the musl-built tcc needed fix 73, binary constants); the rebuilt kernel
boots as generation 2 (see "Self-hosting" below).

Before 2026-10-22, 124 gcc invocations remained for `.S` files, `-m16`/`-m32` boot and
realmode code, `-S` on real files (asm-offsets) and the vDSO (userspace linked without
libc, tcc emitted memcpy calls there), and GNU ld/objcopy/nm/ar did the links.
`vmlinux` and `bzImage` link with no errors and no undefined references. The initramfs
carries busybox 1.38.0 compiled and linked by tcc against musl 1.2.6 compiled by tcc, and
tcc itself (static, musl) so programs can be compiled inside the guest. A hybrid BIOS/UEFI
GRUB ISO of all that boots on SeaBIOS and on OVMF. Since 2026-10-21 the kernel also has the
VGA text console, a framebuffer console (sysfb + simplefb, fed by GRUB's gfxpayload on both
BIOS and UEFI) and the i8042 keyboard; that code (VT, fbcon, fbdev core, input core, atkbd)
compiled with tcc without any new fix.

Remaining noise in the kernel build: objtool warnings (`unreachable instruction` after
`ud2`, `missing __noreturn`, UACCESS), modpost section-mismatch warnings (kept non-fatal by
`kernels/linux/tcc.config`) and empty orphan `.init.text`/`.init.data` sections in the
decompressor objects (tcc creates a section as soon as a declaration carries the attribute).

## How tcc is presented to Kbuild: `kernels/linux/tcc-kcc`, `tcc-hostcc`, `objtool-stub`, `tld`

* tcc gets `-B<tccdir> -I<tccdir>/include -D__GNUC__=15 -D__GNUC_MINOR__=3
  -D__GNUC_PATCHLEVEL__=0` (scripts/cc-version.sh and compiler_types.h need a GCC
  identity); `-Wunsupported` is added when `-Werror` is present so that `cc-option` probes
  fail for flags tcc ignores.
* `-U__OPTIMIZE__`: the kernel's `__compiletime_assert()` (BUILD_BUG_ON & co.) becomes a
  no-op, as include/linux/compiler_types.h documents for compilers that do not fold;
  otherwise ~150 `__compiletime_assert_N` references survive (conditions that only fold
  after inlining). A gcc reference build still checks them.
* Nothing goes to gcc. `.S` files, `-S` (asm-offsets), `-E` (linker scripts), `-x
  assembler-with-cpp` from stdin (the `as-instr` probes, which honestly fail for AVX-512 and
  friends) all go to tcc; `-m16` goes to tcc as well, which execs the `i386-tcc` built next to
  it. The wrapper answers the queries Kbuild makes of its compiler: `-Wa,--version` (a "GNU
  assembler 2.47" line for scripts/as-version.sh), `-dumpmachine`, `-print-file-name`.
  `KCC_GCC` remains as a debugging hook (the routing case is empty).
* `tcc-hostcc` is HOSTCC: plain tcc against glibc, claiming GCC 4.2 (tools/include needs
  `__GNUC__`; a modest version keeps glibc's headers off newer gcc extensions), with
  `-U__REDIRECT*` (glibc redefines them), `-D__USER_LABEL_PREFIX__=` and `-D__NO_INLINE__`
  (glibc's gnu89 extern inlines would be duplicate definitions).
* `OBJTOOL=kernels/linux/objtool-stub` (kernel patch 0009): objtool needs libelf; with this
  config it only validates and records static-call sites, and the kernel works without them.
* `LD NM OBJCOPY READELF AR STRIP` are `build/<arch>/tld/*` (`scripts/build.sh tld`
  compiles `tools/tld/*.c` with the host tcc). `ld -v` prints "GNU ld (tld) 2.47" for
  scripts/ld-version.sh and the `ld-option` probes. tld's own test suite
  (`tools/tld/test.sh`) compares every link of the kernel with GNU ld's result on a
  GNU-linked tree: 28 checks, all passing on 2026-10-22.
* tcc's version string is `0.9.28rc <base>+<n>p` (GITHASH given to tcc's make); it is a -D
  define that make does not track, so `build.sh host-tcc` recompiles the object holding it
  when the patch count changed (`build/<arch>/tcc-tag`), which makes Kbuild rebuild
  everything with the new compiler.
* `KCC_LOG` (set by `scripts/build.sh` to `build/<arch>/logs/kcc.log`) gets one line per
  compiler invocation: tool, output file, arguments. It loses shell quoting
  (`-DKBUILD_MODNAME='"x"'`), harmless for most diagnosis.
* Kconfig honestly reports for tcc: no int128, no asm inline, no named address spaces, no
  retpoline/IBT. Kconfig is re-probed when `CC_VERSION_TEXT` changes; `build.sh` runs
  `olddefconfig` every time.

## Kernel-side changes (`patches/linux/`)

* workarounds, guarded by `__TINYC__` or `__SIZEOF_INT128__`: the 128-bit cmpxchg block and
  `freelist_full_t` (tcc has no `__int128`); `rip_rel_ptr()` as a macro and
  `_static_cpu_has()` as the dynamic test (tcc does not inline, so `"i"` operands from
  function parameters never become immediates); the seqlock canaries as stubs (need the
  optimizer to prove states impossible).
* upstreamable, semantics-preserving: `ia32_fxstate_possible()` in fpu/signal.c and two
  `IS_ENABLED()` conditions in dma/direct.c, where gcc propagates a constant through a
  local variable or an inlined callee and tcc does not.
* upstreamable, toolchain capability (2026-10-22): `OBJTOOL=` names an external objtool
  (0009); `CONFIG_AS_AVX2`/`CONFIG_AS_AVX512` probes gate the AVX SHA-256 files and the
  AVX-512 BLAKE2s function (0010; tcc has no VEX/EVEX); `X86_BOOT_VIDEO_MODES` makes the
  BIOS video mode setting of the real-mode setup optional (0011; tcc's 16-bit code is twice
  gcc -Os's and the setup must stay under 32 KB: 39 488 bytes with it, 27 328 without).

Pattern for new finds: anything that needs an `"i"` constraint from a function parameter
(`__always_inline` helpers) needs a macro form under `__TINYC__`; anything that needs a
constant to flow through a variable can usually be rewritten so the constant is tested
directly, which is also better code for gcc.

## What the tcc patches do, in one paragraph each kind

* Language: `__auto_type`, `typeof_unqual`, packed enums, cast to union, `transparent_union`,
  forward `alias`, `_Pragma`, `#pragma GCC visibility`, global register variables, the
  builtins (`object_size`, `bswap`, `overflow`, `clz/ctz/popcount/ffs`, `strlen`, `strcmp`
  folding, inline `va_arg`).
* Inline assembler: two-pass assembly for forward label differences, GNU as macros,
  `.section` flags/entsize, `.2byte/.4byte/.8byte`, `.octa`, `%a` modifier, `"=@cc"`
  flag outputs, `"x"/"t"/"u"/"X"` constraints, segment prefixes, `sym@PLT`, local labels
  handled like GNU as (section-relative relocs, dropped from the symbol table), asm in dead
  code not assembled, and ~40 new x86_64 mnemonics.
* Code generation like an optimizing compiler where freestanding code depends on it:
  REX_GOTPCRELX (no GOT), hidden symbols RIP-relative, unreferenced static functions not
  emitted, constant switches generate only the taken case, dead loops stay dead, trivial
  stubs folded at call sites, `x & 0` folds, inline `bsr`/`bsf`.
* Linker/driver: `-Wl,--start-group`, `-Wp,-MMD,<file>`, `-S` (asm-offsets text), `-E -MD`,
  `-m16` (execs i386-tcc), `-mregparm=N`, `-fno-builtin`/`-ffreestanding`, `include/limits.h`
  for code that makes tcc claim to be GCC.
* Assembler for the kernel's `.S` files (2026-10-22): GNU as macro semantics (text bodies,
  argument splitting, `:req`/`:vararg`/defaults, `\@`, `\()`, `.exitm`, `.irpc`), the full
  `.if` family, `.incbin`, `.comm`, `.section` types and name-dependent default flags,
  register aliases (`X0 = %xmm4`), `&&`/`||`, 8/16/64-bit PC-relative data, two-pass
  assembly of whole files, runtime `.code16`/`.code16gcc`/`.code32`/`.code64` in both the
  i386 and the x86_64 assembler from one instruction table, `-m16` code generation with
  GCC's regparm convention, and the SSE2/SHA-NI forms the lib/crypto code needs.
* Inline constant-size memcpy/memset (the vDSO links without libc).
* Real bugs found on the way (they would bite any user): a miscompiled goto chain, `swapgs`
  encoding, `mov $imm32,%r64` sign extension, 64-bit asm constants truncated, movaps
  register forms, `$` in `.S` macro bodies, stale local labels, non-executable code
  sections, out-of-tree build breakage.

## Open items

1. Quality: objtool is a stub now (the real one needs libelf), so its validation is gone:
   the earlier warnings (code after `__builtin_unreachable`/`ud2`, `missing __noreturn`,
   UACCESS) are no longer reported; modpost mismatches from non-inlined `__init`-only
   helpers; empty `.init.*` sections created by declarations with a section attribute
   (tccgen should create the section only for definitions).
2. Assembler: no VEX/EVEX encodings (AVX, AVX2, AVX-512: kernel patch 0010 gates the code
   that needs them); jumps to forward labels are not relaxed to rel8 and same-section
   references to labels keep their relocations where GNU as resolves them (semantically
   equal, a few bytes bigger: the kernel .S objects differ from GNU as's in exactly that);
   `.altmacro` is accepted but its `%expr`/`<...>` forms are not implemented; `-m16` code is
   about twice the size of gcc -Os's (no register allocation), the reason for kernel patch
   0011; the real objtool could not be built without libelf.
3. tld: no `.eh_frame_hdr` generation, no GOT/PLT/dynamic relocations/TLS (the kernel needs
   none), `.strtab` not tail-merged, `readelf` only `-r`; see `../tools/tld/README.md`.
4. tcc polish: `__builtin_popcount*` still calls libtcc1; `_Pragma("pack")` only warns; the
   inline-stub fold is disabled with `-g`; `do { } while` in dead code only when
   delimitable.
5. Coverage: only `minimal.config` on QEMU was tested; a larger config (SMP, modules,
   KALLSYMS, ORC unwinder, jump labels/static calls) will meet more constructs; KALLSYMS
   would also exercise tld's `nm` in link-vmlinux.sh.
6. Semantics worth a second look before upstreaming: patch 0042 drops unused static
   functions regardless of -O (gcc only at -O1+); patch 0034 global register variables
   update only the slot on C stores; patch 0037 folds trivial static functions also
   without -O; patch 0057 inlines memcpy/memset of constant size up to 4096 bytes also
   without -O (gcc does it at -O1+ with a smaller limit).
7. Self-hosting (done 2026-10-22, see the section below): what is left is making the
   guest-built tcc the one that builds the next kernel by default (`build-toolchain` installs
   it in /usr/local and `build-kernel` prefers it, but the automatic run `--auto=all` is the
   only path exercising it), a generation-3 run from inside generation 2, and `make test` of
   tcc in the guest (needs the test suite's expectations of a glibc host reviewed).
8. Targets: arm64 (see README), other kernels; on i386 the self-hosting image is untested
   and the setup code's early serial output doubles every character (see "The i386 target").

## The i386 target

`ARCH=i386` (2026-10-23; `install.sh --arch i386`, `ARCH=i386 ./run.sh`) builds the 32-bit
kernel from the same sources, patches and fragments: `scripts/build.sh` gives the kernel
`ARCH=i386` (which forces `CONFIG_64BIT=n`; `kernels/linux/i386.config` only switches highmem
off), runs the `i386-tcc` cross compiler as the target compiler through `tcc-kcc` (`KCC_TCC`;
`TCC` stays the host compiler of `tcc-hostcc`; `-dumpmachine` answers i686-linux-gnu), links
with the same tld (`-m elf_i386`), and builds musl, busybox and the guest tcc (configured
`--cpu=i386`, no cross compiler needed in the guest: `tcc -m16` is native) with `i386-tcc`
and `i386-libtcc1.a`. The libc-free init has an `int $0x80` syscall path. Numbers of the
first boot: 1900 tcc and 0 gcc invocations, a 1.8 MB bzImage, the kernel reports
`Linux 7.3.0+ i686`.

What the 32-bit kernel needed from the toolchain (fixes 74-93, K14-K16, docs/FIX-LOG.md):

* Position-independent code. The decompressor is compiled `-fPIE` (it relocates itself to
  the end of its buffer) and the vDSO `-fpic`; neither may contain run-time relocations.
  On x86_64 tcc's RIP-relative addressing made that free; i386 has no PC-relative data
  addressing, so tcc got a real PIC mode (fix 74): every symbol is addressed
  `sym@GOTOFF(%ebx)` with `%ebx` loaded once per function from `_GLOBAL_OFFSET_TABLE_`
  (R_386_GOTPC), calls stay PC-relative. That is gcc's `-fPIE` + hidden-symbols model
  (`include/linux/hidden.h`, `-Bsymbolic`): no GOT, no PLT. The assembler takes `sym@GOTOFF`
  and `_GLOBAL_OFFSET_TABLE_` as GNU as does (fix 75), and tld applies R_386_GOTOFF and
  R_386_GOTPC against a linker-defined `_GLOBAL_OFFSET_TABLE_` (absolute 0; the two
  relocations only need a common base) and refuses absolute relocations in a PIE.
* Freestanding 64-bit arithmetic. The kernel has no libgcc: tcc now inlines 64-bit shifts by
  a variable count (83) and the bit-scan builtins (87) on i386, folds unsigned 64-bit
  division by a power of two (84) and no longer divides in `__builtin_mul_overflow` for
  32-bit operands (82, every object had `__udivdi3` from `size_mul()`). The dozen real
  64-bit divisions by other constants (`/ 8`, `/ 100`, ... which gcc strength-reduces) come
  from kernel patch K14: `GENERIC_LIB_DIVDI3` (`lib/math/divdi3.c` on top of `div64.c`),
  selected for `X86_32 && CC_IS_TCC`; `CC_IS_TCC` is new too (`$(CC) -v`).
* The 32-bit kernel's assembler forms: `mov PT_FS(%esp), %fs` (77), `"rax"` clobbers in
  shared headers (76), `ds;ds;ds;int $0x80` and `.previous` in the vDSO (80, 81),
  `sysenter`/`ud2a`/`rdpid %eax`/`invpcid` (79), and `(unsigned long)&sym - 0xC0000000` in
  a static initializer (78: the addend wraps in 32 bits).
* Four code generator bugs found by booting: a pre-existing upstream crash on `goto` out of
  cleanup scopes to a local label (85, `kernel/futex/pi.c`; it also crashes the x86_64 tcc on
  the reduced case), and in this project's own earlier work the regparm indirect call that
  dereferenced its function pointer after the argument registers were loaded (88,
  `entry->read(entry, &out)` in cpuid_parser.c called address 0), the struct zero-fill that
  clobbered `%eax` (89, `__init_waitqueue_head` wrote its spinlock to NULL) and musl's
  `call *%gs:16` assembled as a direct call (91). 86 (a goto out of a cleanup variable's
  initializer ran the cleanup) came out of the test written for 85.
* tld's objcopy recomputes PT_LOAD sizes from the sections it keeps. The kernel strips
  `.modinfo` out of `vmlinux.unstripped`; the linker script places it after `.bss`/`.brk`,
  so while it is there the data segment's file size spans the NOBITS sections (GNU ld lays it
  out the same way) and only objcopy brings it back. With the headers copied unchanged the
  32-bit decompressor's `parse_elf()` copied 2.8 MB of "file" over its own relocated code and
  stack: the first boot died with zeros where `zlib_adler32` had been. (The x86_64 vmlinux
  had the same `p_filesz == p_memsz`; its decompressor sits above the data segment's end.)
* Kernel patches K15 (swiotlb wrappers under `IS_ENABLED(CONFIG_SWIOTLB)`) and K16 (x86's
  `pmdp_test_and_clear_young()` declared only where it is defined) remove undefined
  references from dead branches that gcc's constant propagation eliminates; both are
  semantics-preserving and upstreamable.
* musl and busybox on i386: the x87 `"t"`/`"u"` constraints of musl's math (90, any floating
  type on i386), `"%eax"` clobbers (76), an `"m"` input operand when every register is taken
  (busybox's comba multiply: three register outputs, two clobbers, two pointer-based memory
  inputs; the value is copied to a stack temporary, 92), and tcc's own linker learning the
  8- and 16-bit relocations (93: `jecxz 1f` in musl's sigsetjmp).

Debugging a 32-bit boot that prints nothing: `ARCH=i386 ./run.sh --nolibc -S -gdb tcp::1234`
and gdb with `add-symbol-file build/i386/linux/arch/x86/boot/compressed/vmlinux -o <base>`
for the decompressor (base = `0x1000000 + init_size - ZO__end`, from `arch/x86/boot/zoffset.h`
and the bzImage header; software breakpoints set before its self-copy are overwritten by
it, use `hbreak` for the first one) or `add-symbol-file build/i386/linux/vmlinux 0` for the
kernel; tcc's frames always have `%ebp`, so the saved frame pointers give a call chain even
without unwind info (`exc_page_fault`'s `pt_regs` is in `%eax` under `-mregparm=3`). Under
TCG (`KVM=0`) `-d int` logs every exception with its address.

Open on i386: the self-hosting image (`ARCH=i386 scripts/build.sh selfhost` and the guest's
`build-kernel`, which now picks `ARCH=i386` from `uname -m`, are wired but have not been
run); the real-mode setup's early serial console (`earlyprintk=serial`) prints every
character twice ("eeaarrllyy ccoonnssoollee iinn sseettuupp ccooddee"), a -m16 code generation
problem that the x86_64 boot (same setup code) does not show, not yet investigated.

## Self-hosting

The self-hosting image (`scripts/build.sh selfhost` -> `out/<arch>/initramfs-selfhost.cpio`,
30 MB; `install.sh --selfhost`; on the ISO as menu entries 4 and 5) is the busybox image plus:

* `/usr/src/linux-7.3.0.tar.gz`: the kernel source pruned to this configuration, 16 MB
  compressed, 8602 files, 92 MB unpacked (the full tree is 96 000 files, 1.8 GB). The list
  (`build/<arch>/selfhost/linux-files.txt`, made by `kernel_source_list` in build.sh) is: every
  file the host build read, from the `.*.cmd` dependency lists Kbuild saved (2978 files, the
  `..`/`//` in paths normalized); the Makefile/Kbuild files of every directory Kbuild
  descended into (the build tree mirrors them) with their helpers (`*.sh`, syscall `*.tbl`,
  `timeconst.bc`, linker scripts, `*_shipped` sources, `cp437.uni`, `defkeymap.map`, awk
  generators: inputs of generated files that no dependency list records); every Kconfig
  file (Kconfig sources them all); `scripts/` without dtc/coccinelle/gdb/..., but with every
  Makefile under it (Kbuild descends into scripts/dtc even with CONFIG_DTC=n);
  `include/{linux,asm-generic,uapi,vdso}`, `arch/x86/{include,boot,realmode,entry,tools,
  configs,lib}`, `arch/um/scripts/Makefile.rules` (fs/hostfs includes it), `init/`, `usr/`,
  `kernel/configs/`, `tools/include`, `tools/arch/x86/include`. The 15 kernel patches are
  applied (the tree is `src/linux` on branch `patched`); `patches/` is shipped too.
  `scripts/selfhost-test.sh host` proves the list by building the tarball on the host with the
  image's toolchain: 49 s on 8 cores, 229 MB of build directory, a bzImage of the same
  1 473 024 bytes as the reference. Missing-file errors found that way: `scripts/dtc/Makefile`,
  `defkeymap.c_shipped`, `arch/um/scripts/Makefile.rules`.
* `/usr/src/tinycc.tar.gz` (the patched tree, `git archive`), `tld.tar.gz` (tools/tld),
  `make-4.4.1.tar.gz` (GNU make's release tarball; `sources.lock` pins it by sha256 and
  `fetch.sh` downloads it, unpacks it into `src/make` and commits it there as branch `base`:
  the git tree of make needs autotools and gnulib; busybox has no make applet).
* The toolchain, all static musl binaries compiled by the host tcc: `/usr/bin/tcc` and
  `/usr/bin/i386-tcc` (step guest-tcc, `make cross-i386`; `tcc -m16` execs `i386-tcc` from
  the directory of its own argv[0], so the two live side by side), `/usr/bin/tld` with the
  `ld nm objcopy readelf ar strip` links, `/usr/bin/make` (step guest-make: GNU make 4.4.1
  configured `--disable-nls --without-guile --disable-load`, builds with tcc in a second),
  the Kbuild wrappers `tcc-kcc`, `tcc-hostcc`, `objtool-stub` (unchanged files; the wrappers
  pass `-B` only when the tcc is an in-tree one, detected by `include/tccdefs.h` next to it),
  `flex` and `bison` stand-ins that copy the lexer and parser generated on the host into
  `scripts/kconfig/` (`/usr/share/tinycc-os-kernel/kconfig/`), the Kconfig fragments, and
  the scripts of `userland/selfhost/`.
* `build-kernel`: mounts a tmpfs on /build (85% of RAM), unpacks the tree, `tinyconfig` +
  `merge_config.sh` of the fragments + `olddefconfig`, then the same make line as
  `scripts/build.sh step_kernel` (CC=tcc-kcc HOSTCC=tcc-hostcc OBJTOOL=objtool-stub
  LD/NM/OBJCOPY/READELF/AR/STRIP=tld LEX=flex YACC=bison) with `-j nproc`,
  `KBUILD_BUILD_VERSION=<running kernel's build number + 1>`, `KBUILD_BUILD_USER=tcc`,
  `KBUILD_BUILD_HOST=tinycc-os-guest`; reports time, invocation counts, memory. It prefers
  `/usr/local/bin/tcc` when `build-toolchain` has made one. `send-file` writes files to
  `/dev/ttyS1` (raw, `>>> send-file NAME SIZE MD5` header lines); `run.sh --selfhost` captures
  that port to `out/<arch>/selfhost/ttyS1.bin` and `scripts/recv-file.sh` cuts the files out
  and checks the sums. `run.sh --selfhost --auto` puts `tcc.selfhost=auto` on the command line
  and `/init` runs build-kernel, send-file and reboots (`--auto=all`: build-toolchain first).
* Kernel configuration for it (`minimal.config`): `CONFIG_SMP=y`/`NR_CPUS=8` (make -j; the
  trampoline and the SMP paths compiled without new fixes, but head_64.S and the per-cpu hot
  section needed tcc fixes 71 and 72), `SHMEM`/`TMPFS` (the build directory with a size
  limit; needed kernel patch 0012, the `vma_thp_gfp_mask()` stub), two 8250 UARTs,
  `KERNEL_GZIP` instead of tinyconfig's xz (busybox's `xz` only decompresses; its `gzip`
  takes the kernel's `-n -f -9`). `run.sh`
  passes `-smp N,sockets=N,cores=1,threads=1`: without ACPI the CPUs come from SeaBIOS's MP
  table, and SeaBIOS lists only the first logical CPU of each package there (QEMU's default
  topology is one package), so one socket per CPU; `-accel kvm` when /dev/kvm is writable.
* What the guest run found that the host check could not: the guest's tcc is compiled by
  tcc against musl, the host's by gcc against glibc, so libc-dependent parsing differs
  (fix 73: `0b1010` constants, which glibc's strtoull accepts and musl's does not); the
  Linux UAPI headers must sit next to musl's in `/usr/include` for `tools/include`
  (`<asm/types.h>`); the `ld` crashes in the guest's console log were the
  `tools-support-relr.sh` probe (tld now rejects `-z pack-relative-relocs` by name, and
  lays out orphan output sections, which that probe's `-shared` link needs; the kernel
  links are unchanged: identical symbol tables before and after).
* Memory and time (2026-10-22, 8 virtual CPUs under KVM on an 8-core host, `MEM=1G`): the
  37 MB image is unpacked into the rootfs (RAM); `build-kernel` unpacks the tree (83 MB on
  tmpfs) and builds in 1 min 34 s (1546 tcc invocations, 0 gcc, 30 host-tool compiles),
  223 MB of tmpfs in all, 625 MB still available at the end; `send-file` moves the 2.0 MB
  bzImage out in 21 s (QEMU's UART runs at the host's pace, not 115200 baud); the whole
  `selfhost-test.sh qemu` run is 2 minutes. The guest-built kernel (uname -v `#2 SMP
  PREEMPT`, builder `tcc@tinycc-os-guest`) boots with the same initramfs images and prints
  "generation 2" in the banner. Its bzImage is 2 065 920 bytes against the host's 2 008 576:
  busybox's gzip compresses differently and the version string differs (no `+`: no git in
  the guest, so setlocalversion adds nothing).

## Debugging recipes

```bash
scripts/build.sh kernel                 # build/x86_64/logs/kernel.log, kcc.log
L=build/x86_64/logs
grep -oE ' error: .*' $L/kernel.log | sed -E "s/'[^']*'/'X'/g; s/[0-9]+/N/g" | sort | uniq -c | sort -rn
grep -oE "undefined reference to \`[^']*'" $L/kernel.log | sort | uniq -c | sort -rn
# rebuild one object with tcc outside Kbuild (objtool deletes failed objects); path as Kbuild prints it
cmd=$(grep -E "^tcc (.*/)?fs/select.o ::" $L/kcc.log | head -1 | sed 's/^tcc [^:]*:: //')
(cd build/x86_64/linux && TCC=$PWD/../../../src/tinycc/tcc eval "../../../kernels/linux/tcc-kcc $(echo "$cmd" | sed 's| -o [^ ]*\.o| -o /tmp/x.o|')")
# preprocess instead (swap -c for -E) to see the inline asm text a header produces
build/x86_64/linux/tools/objtool/objtool --static-call --hacks=jump_label,noinstr /tmp/x.o
echo 'rdgsbase %rax' > t.s && as t.s -o t.o && objdump -d t.o   # compare an encoding with GNU as
sh src/tinycc/tests/m16-check.sh all     # -m16 codegen vs -m32, .code16/32/64 encodings vs GNU as, regparm, boot objects
grep -c '^gcc ' build/x86_64/logs/kcc.log   # must stay 0
BUILD=<gnu-linked tree> sh tools/tld/test.sh   # tld vs GNU ld on every kernel link (needs a GNU-linked build tree)
./run.sh --nolibc                        # smoke test: banner, reboot, QEMU exits
./run.sh -d int,cpu_reset -D qemu.log    # a hang or triple fault
./run.sh -S -gdb tcp::1234               # + gdb build/x86_64/linux/vmlinux
```

After a tcc change: `make -C src/tinycc && make -C src/tinycc test`, commit, then
`scripts/build.sh host-tcc kernel`: the host-tcc step rebuilds tcc and i386-tcc with the
new version tag, which makes Kbuild rebuild every object (a plain `make` in src/tinycc
leaves the version string stale and Kbuild then keeps the old objects). Rebuilding tcc
while a kernel build runs makes that build inconsistent.

## Methodology notes

* objtool runs on every object (`--static-call --hacks=jump_label,noinstr`) and caught
  real tcc bugs (goto chains, local-label relocs, section flags).
* tcc used to assemble inline asm in dead code; it now skips it like gcc (patch 0032). Asm
  in a dead arm of a non-constant-folded expression is still assembled.
* The order of discovery: 15 objects compiled, then 36, 289, 508, 530, all 1669; then the
  vmlinux.o link, modpost, the vmlinux link with ~1000 undefined references reduced to 0;
  first boot: triple fault in `__startup_64()` (patch 0044); second boot: panic in
  `__d_alloc()` (patches 0045, 0046); third boot: init runs.
* 2026-10-22, removing gcc and binutils: the `.S` files were assembled one by one against
  the gcc command lines in the build tree (`.*.o.cmd`), the -m16 code generator was proved
  by comparing the instruction streams of `-m16` and `-m32` compiles of the boot files, the
  assembler modes by byte comparison with GNU as, the linker by relinking the GNU-built
  tree and comparing symbol tables and images, and the whole by booting. The kernel build
  then failed in order at: a 64-bit addend truncated to int (head_64.S), forward labels
  turned into absolute relocations the -pie decompressor link refuses, and a 39 KB setup
  against a 32 KB limit.
