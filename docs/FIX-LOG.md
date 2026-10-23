# Fix log

Every change made to tinycc (and the kernel) for this port, in the order it was found, with
the problem, the fix and how it was verified. The entries were written while working in
the original checkouts (`/home/nika/Documents/tinycc` at mob e23dabed and
`/home/nika/Documents/linux` at f47d2236); the patch series in `../patches/` was split from
that work, so one patch may hold several entries and one entry may span two patches.

## Fix number to patch

| Fix | Patch | Fix | Patch | Fix | Patch |
|-----|-------|-----|-------|-----|-------|
| 1 | tinycc 0001 | 20 | tinycc 0020 | 39 | tinycc 0038 |
| 2 | tinycc 0002 | 21 | tinycc 0021 | 40 | tinycc 0039 |
| 3 | tinycc 0003 | 22 | tinycc 0022 | 41 | tinycc 0040 |
| 4 | tinycc 0004 | 23 | tinycc 0023 | 42 | tinycc 0037 |
| 5 | tinycc 0005 | 24 | tinycc 0024 | 43 | tinycc 0036 |
| 6 | tinycc 0006 | 25 | tinycc 0025 | 44 | tinycc 0035 |
| 7 | tinycc 0007 | 26 | tinycc 0026 | 45 | tinycc 0041 |
| 8 | tinycc 0008 | 27 | tinycc 0027 | 46 | tinycc 0042 |
| 9 | tinycc 0009 | 28 | tinycc 0028 | 47 | tinycc 0043 |
| 10 | tinycc 0049 | 29 | tinycc 0029 | 48 | tinycc 0044 |
| 11 | tinycc 0011 | 30 | tinycc 0030 | 49 | tinycc 0045 |
| 12 | tinycc 0010 | 31 | tinycc 0031 | 50 | tinycc 0046 |
| 13 | tinycc 0012 | 32 | tinycc 0025 | 51 | tinycc 0047 |
| 14 | tinycc 0013 | 33 | tinycc 0032 | 52 | tinycc 0048 |
| 15 | tinycc 0014 | 34 | tinycc 0033 | 53 | tinycc 0050 |
| 16 | tinycc 0015 | 35 | tinycc 0034 | 54 | tinycc 0037 |
| 17 | tinycc 0016 | 36 | tinycc 0035 | 55 | tinycc 0051, 0052 |
| 18 | tinycc 0017 | 37 | tinycc 0036 | 56 | tinycc 0053 |
| 19 | tinycc 0018, 0019 | 38 | tinycc 0037 | 57 | tinycc 0054 |
| 58 | tinycc 0055 | 63 | tinycc 0060 | 68 | tinycc 0065 |
| 59 | tinycc 0056 | 64 | tinycc 0061 | 69 | tinycc 0066 |
| 60 | tinycc 0057, 0069 | 65 | tinycc 0062 | 70 | tinycc 0067 |
| 61 | tinycc 0058 | 66 | tinycc 0063, 0068 | 71 | tinycc 0070 |
| 62 | tinycc 0059 | 67 | tinycc 0064 | 72 | tinycc 0071 |
| | | | | 73 | tinycc 0072 |
| 74 | tinycc 0073 | 80 | tinycc 0074 | 86 | tinycc 0077 |
| 75 | tinycc 0073 | 81 | tinycc 0074 | 87 | tinycc 0076 |
| 76 | tinycc 0074 | 82 | tinycc 0075 | 88 | tinycc 0078 |
| 77 | tinycc 0074 | 83 | tinycc 0076 | 89 | tinycc 0078 |
| 78 | tinycc 0075 | 84 | tinycc 0075 | 90 | tinycc 0074 |
| 79 | tinycc 0074 | 85 | tinycc 0077 | 91 | tinycc 0074 |
| | | | | 92 | tinycc 0074 |
| | | | | 93 | tinycc 0074 |

Kernel: K1 = linux 0001, K2 = 0002, K3 = 0003, K4 = 0004, K5 and K6 = 0005, K7 = 0006,
K8 = 0007, K9 = 0008, K10 = 0009, K11 = 0010, K12 = 0011, K13 = 0012, K14 = 0013,
K15 = 0014, K16 = 0015.

Fixes 57-70 and K10-K12 (2026-10-22) made the kernel build independent of gcc and
binutils: tcc assembles every `.S` file, compiles the real-mode code with `-m16`, produces
the asm-offsets text with `-S`, and the project's own `tools/tld` links (see
`../tools/tld/README.md`); they are committed in `src/tinycc` / `src/linux` and exported,
so their status is COMMITTED. Fixes 71-73 and K13 (the same evening) came with the SMP
kernel and the self-hosting image (the kernel rebuilt inside the guest by the guest's own
musl-built tcc, tld and GNU make; see `PORT-NOTES.md`, "Self-hosting"). Fixes 74-93,
K14-K16 and the tld changes of 2026-10-23 are the i386 target (`ARCH=i386`, the 32-bit
kernel; see `PORT-NOTES.md`, "The i386 target"): position-independent code for the
decompressor and the vDSO, the 32-bit kernel's assembler forms and its freestanding
arithmetic, four code generator bugs found by booting it (85, 88, 89, 91), and what musl
and busybox asked of the inline assembler and tcc's own linker (90, 92, 93).

## Log

Running log of tinycc fixes (repo /home/nika/Documents/tinycc, branch `mob`, base commit
e23dabed as of 2026-10-20)..

Format per entry: date, status (IDENTIFIED / APPLIED / COMMITTED), file:line, problem, fix,
verification.

1. 2026-10-20, APPLIED (uncommitted, working tree), `lib/Makefile:99`, rule `$(TOP)/bcheck_run.o : bcheck.c $(TCC)`.
   Problem: the recipe compiled the literal path `bcheck.c` instead of `$<`, so out-of-tree
   builds (e.g. `mkdir build && cd build && ../configure && make`) failed with
   "tcc: error: file 'bcheck.c' not found" because the source is found only via VPATH.
   Fix (one word): recipe line `$S$(XCC) -c bcheck.c -o $@ ...` -> `$S$(XCC) -c $< -o $@ ...`.
   Verification: `make -C lib ../bcheck_run.o` rebuilds the object both in-tree and in
   `build-stage2/` (VPATH expands `$<` to `../../lib/bcheck.c`); a fresh out-of-tree
   self-hosted `configure && make` completes with no manual step.
   Patch: `git diff lib/Makefile` in the tinycc checkout (1 insertion, 1 deletion).

2. 2026-10-20, APPLIED (uncommitted, working tree), `tests/Makefile:7` (was `VPATH = $(TOPSRC)/tests $(TOPSRC)`).
   Problem: with a build directory separate from the source, the blanket VPATH also matched
   generated files (`test.ref`, `libtcc_test`, `libtcc_test_mt`, `asm-c-connect`) left in the
   source `tests/` dir by an earlier in-tree `make test`; make treated them as up to date, and
   recipes that run `./libtcc_test` etc. in the build dir failed with exit 127 / "diff: test.ref:
   No such file".
   Fix: replace the VPATH line with `vpath %.c $(TOPSRC)/tests $(TOPSRC)` and
   `vpath %.S $(TOPSRC)/tests $(TOPSRC)` plus a 3-line comment, so only sources are searched in
   the source tree. `%.c` still matches directory-prefixed names (`examples/ex3.c`,
   `../examples/ex1.c`), checked with a scratch Makefile.
   Verification: in-tree `make test` -> ALL TESTS PASSED (leaves stale artifacts in src/tests);
   then out-of-tree `make test` in `build-stage2/` with those artifacts present -> ALL TESTS
   PASSED, no cleanup needed.
   Patch: `git diff tests/Makefile` in the tinycc checkout.

--- Fixes 3+ come from the "build the Linux kernel with tcc" effort ([[kernel-with-tcc-project]]) ---

3. 2026-10-20, APPLIED (uncommitted), `libtcc.c` case TCC_OPTION_Wp (~line 2084).
   Problem: `-Wp,-MMD,<file>` (what Kbuild passes) was split on commas into `-MMD` and `<file>`,
   so `<file>` became an input file ("cannot specify output file with -c many files"), although
   tcc already understands the single-arg spelling `-MMD,<file>`.
   Fix: when the -Wp, argument starts with `-MD,` or `-MMD,`, rewrite it to `-MD,-MF,<rest>` /
   `-MMD,-MF,<rest>` before splitting, so the file goes to -MF.
   Verification: `tcc -c x.c -o x.o -Wp,-MMD,x.d` writes x.d; `make test` passes.

4. 2026-10-20, APPLIED (uncommitted), `x86_64-gen.c` gen_gotpcrel() and its 3 callers
   (gen_modrm_impl TREG_MEM path, load(), store()).
   Problem: every non-static global was accessed through the GOT with plain R_X86_64_GOTPCREL,
   which forces GNU ld to create GOT entries; the Linux kernel link asserts SIZEOF(.got)==0.
   Fix: gen_gotpcrel() takes a `relax` flag; when the instruction is `mov sym@GOTPCREL(%rip),%reg`
   with a REX prefix it emits R_X86_64_REX_GOTPCRELX instead, which GNU ld relaxes to
   `lea sym(%rip),%reg` (no GOT entry). tcc's own linker already handled the X variants.
   Verification: objdump shows REX_GOTPCRELX; `ld -static` of a tcc object yields no .got
   section; `make test` passes.

5. 2026-10-20, APPLIED (uncommitted), `include/tccdefs.h` after __builtin_extract_return_addr.
   Problem: kernel headers use __builtin_object_size, __builtin_dynamic_object_size,
   __builtin_prefetch, __builtin_expect_with_probability, __builtin_bswap16/32/64 and
   __builtin_trap, which tcc did not know (implicit function declarations -> link errors).
   Fix: macros with the documented fallback semantics (object_size -> 0 / (size_t)-1, prefetch
   -> (void), bswap -> shift/mask expressions [evaluate the argument more than once, documented],
   trap -> `ud2` on x86). Interim: a native bswap would be better.
   Verification: tcc -run test prints correct swaps; `make test` passes.

6. 2026-10-20, APPLIED (uncommitted), `tccgen.c` struct_decl(), enum branch after skip('}').
   Problem: `enum e {...} __attribute__((packed));` was ignored (sizeof stayed 4); the kernel's
   arch/x86/include/asm/cpuid/types.h static_asserts sizeof(enum _cache_table_type)==1.
   Fix: parse the trailing attribute into struct_decl's own AttributeDef (as already done for
   structs) and, if packed, pick the smallest integer type holding all values: unsigned
   char/short/int when no negative value (GCC semantics), signed char/short/int otherwise;
   values beyond int still use long long as before.
   Verification: sizes 1/2/1/4/4/1 for the respective enums, struct { enum a; char } is 2 bytes,
   values round-trip; `make test` passes.

7. 2026-10-20, APPLIED (uncommitted), GNU `__auto_type`: `tcctok.h` (TOK___AUTO_TYPE),
   `tcc.h` (AttributeDef.auto_type), `tccgen.c` parse_btype() case + decl_initializer_alloc().
   Problem: `__auto_type` was unknown; the kernel's min()/max() and cleanup helpers use it in
   nearly every translation unit.
   Fix: parse_btype marks the declaration (placeholder type int); decl_initializer_alloc saves
   the initializer tokens (skip_or_save_block, same mechanism as the unknown-array-size dry run),
   parses them once with nocode_wanted via expr_type(expr_eq) to get the type, drops storage
   bits, decays arrays/functions to pointers, reduces a bit-field to its integer type, then
   rewinds and parses the initializer for real. Errors without initializer or with `{...}`.
   Verification: int/const/double/pointer/struct/array/function/string/statement-expression/
   bit-field/global cases give the same sizes and values as gcc; initializer side effects run
   once; `make test` passes. (tcc is more permissive than gcc: gcc rejects bit-field initializers
   and multiple declarators.)

8. 2026-10-20, APPLIED (uncommitted), C23 `typeof_unqual` / `__typeof_unqual` / `__typeof_unqual__`:
   `tcctok.h` (3 tokens after TOK___AUTO_TYPE), `tccgen.c` parse_btype() shares the typeof case
   and additionally clears VT_CONSTANT|VT_VOLATILE|VT_RESTRICT.
   Problem: unknown identifier -> only an implicit-declaration warning, so Kconfig's
   CC_HAS_TYPEOF_UNQUAL probe passed and 21 kernel objects then failed on real uses.
   Verification: const volatile int -> plain int (sizeof, assignable); plain typeof unchanged;
   `make test` passes.

9. 2026-10-20, APPLIED (uncommitted), `include/tccdefs.h`: `__builtin_add_overflow`,
   `__builtin_sub_overflow`, `__builtin_mul_overflow` as statement-expression macros
   (helpers __tcc_ovf_neg/__tcc_ovf_mag/__tcc_ovf_store): exact result as sign+magnitude in
   64 bits, compared with the range of *r; operands evaluated once.
   Problem: kernel overflow.h (check_*_overflow, size_mul, kmalloc_array...) needs them;
   141 files in the built directories.
   Verification: harness of 1,311,264 (type-pair x value-pair x result-type) cases compiled
   with tcc gives the same output hash as gcc's native builtins (scratch: scratchpad/ovf/);
   `make test` passes. Limitation: operands wider than 64 bits unsupported (tcc has none).

10. 2026-10-20, APPLIED (uncommitted), GCC flag output constraints `"=@cc<cond>"`: `tcc.h`
    (ASMOperand.flag_cc), `i386-asm.c` (constraint_priority '@', asm_parse_cc(), asm_gen_setcc(),
    asm_compute_constraints case '@' allocating a general register [al..bl only on i386],
    asm_gen_code emitting `setcc %reg8; movzbl %reg8,%reg32` before the output store).
    Problem: the kernel's bitops (variable_test_bit etc., 11 files) use "=@ccc"/"=@ccz" and
    tcc rejected the '@' constraint.
    Verification: bt/cmp/add/test cases incl. 5 flag outputs in one statement, aliases (z/nz/e/
    ne/s/o/l/c) and an output via pointer give the same results as gcc; `make test` passes.

11. 2026-10-20, APPLIED (uncommitted), `tccasm.c` .section/.pushsection: accepts the 'M' and
    'S' flags (SHF_MERGE/SHF_STRINGS) and the trailing entry size
    (`.pushsection name,"aM",@progbits,14`), stored in sh_entsize.
    Problem: the kernel emits that form for .altinstructions, __ex_table, .discard.* ->
    "end of line expected".
    Verification: readelf shows entsize on the created section; `make test` passes.

12. 2026-10-20, APPLIED (uncommitted), `tccasm.c`: two-pass assembly of inline asm statements
    (asm_pass_* helpers near the top; hooks in asm_new_label1 [record], asm_expr_unary forward
    labels [predict], asm_expr_sum [resolve], tcc_assemble_inline [pass loop];
    asm_section_sym excluded from recording).
    Problem: GNU as resolves differences of labels defined later in the statement by relaxation
    (".byte 775f-774f", the ".skip" padding of the kernel's ALTERNATIVE()); tcc's single pass
    errored "invalid operation with label".
    Fix: pass 1 assembles with such differences as 0 and records label positions, the output
    is rolled back (section data_offset + relocs, but NOT .symtab/.strtab/.hash: truncating
    those aliased ELF symbol indexes, found by debugging), pass 2+ uses the recorded values
    for forward references and repeats until positions are stable (max 8 passes).
    Verification: a copy of the kernel's ALTERNATIVE() asm yields a .altinstructions section
    byte-identical to gcc's (lengths 7/7, flags AM, entsize 14), PC32 relocs, runs; `make test`
    passes. Limitation: debug line entries emitted during pass 1 are discarded with the data.

13. 2026-10-20, APPLIED (uncommitted), `tccasm.c` tcc_assemble_internal: `.2byte`, `.4byte`,
    `.8byte` (lexed as pp-numbers because they start with ".<digit>") are mapped to the
    .short/.long/.quad directive handlers. Kernel ALTERNATIVE() uses `.4byte ft_flags`.
    Verification: part of the alternatives test above.

14. 2026-10-20, APPLIED (uncommitted), `tccgen.c`: GCC "cast to union" extension
    (gen_cast_to_union() after cast_error(); dispatched from the cast site in unary() when the
    target is a union and the value is not a struct). Finds the non-bitfield member whose type
    matches, stores the value into a stack temporary of the union type and yields it as an
    lvalue; errors for incomplete unions, no matching member, or use outside a function.
    Problem: arch/x86/events/amd/ibs.c: `(union ibs_fetch_ctl)config`.
    Verification: scalar/double/initializer-list cases give gcc's results; the no-member case
    errors as in gcc; `make test` passes.

15. 2026-10-20, APPLIED (uncommitted), `tccasm.c` subst_asm_operands + `i386-asm.c`
    subst_asm_operand: GCC operand modifier `%a<op>` ("print as address": no '$' on a constant
    or symbol). Kernel percpu.h this_cpu_read_stable uses `%a[var]` with "i"(&var) (27
    objects failed with "invalid operand reference after %").
    Verification: `movq %a[src],%[val]` with "i"(&var) assembles to `mov var,%reg` with
    R_X86_64_32S (same as gcc); `make test` passes.

16. 2026-10-20, APPLIED (uncommitted), `i386-asm.c` asm_compute_constraints: a register
    variable (`register void *tos asm("r11")`) may appear as several operands of one asm
    statement (kernel irq_stack.h call_on_stack: "+r"(tos) and [tos] "r"(tos)); previously
    "asm regvar requests register that's taken already". The register is shared when the
    earlier operand holding it is the same variable (same vt->sym).
    Verification: test with "+r"/"r" of the same r11 variable; `make test` passes.

17. 2026-10-20, APPLIED (uncommitted), `tccgen.c`: forward `__attribute__((alias("t")))`
    (target defined later in the unit; every kernel SYSCALL_DEFINEx). PendingAlias list +
    resolve_pending_aliases() called from tccgen_compile() after gen_inline_functions();
    undefined targets are reported with tcc_error_noabort.
    Verification: function and variable aliases to later static definitions work; missing
    target errors; `make test` passes.

18. 2026-10-20, APPLIED (uncommitted), `tccasm.c` subst_asm_operands: an array object given
    to an "m" constraint is substituted as a memory reference (`arr(%rip)`) instead of its
    address constant (`$arr`), which produced "bad operand with opcode 'btl'" for the kernel's
    x86_this_cpu_variable_test_bit ([var] "m"(cpu_info.x86_capability)).
    Verification: btl with "rI"/"Ir" constant and variable bit numbers compiles; `make test`.

19. 2026-10-20, APPLIED (uncommitted), `x86_64-asm.h`: new instructions rdtscp, clac, stac,
    xgetbv, xsetbv, monitor, mwait (0F 01 group), rdrand/rdseed (16/32/64-bit register
    forms), rdpid; and the pre-existing `swapgs` entry (0x0f01 /7 OPC_MODRM, which failed with
    "bad MODR/M opcode without operands") rewritten as an OPC_0F01 entry.
    Verification: every encoding byte-identical to GNU as; `make test` passes. (x86_64 table
    only; i386-asm.h has a different layout and was not touched.)

20. 2026-10-20, APPLIED (uncommitted), `tccgen.c` block() label definition: MISCOMPILE FIX.
    When a label was reached both by plain forward gotos and by gotos leaving a cleanup scope
    (`__attribute__((cleanup))`), only the pending-cleanup list was resolved and the plain
    jump chain was left unpatched (jumps to garbage / crash; objtool: "can't find jump dest"
    in fs/select.o do_sys_poll). Now `gsym(s->jnext)` always runs after the pending list.
    Verification: scratch gc.c crashed under `tcc -run` before, now prints the expected
    values; `make test` passes.

21. 2026-10-20, APPLIED (uncommitted), `tccpp.c` + `tcctok.h`: `_Pragma("...")` operator
    (was not implemented at all: "identifier expected"). next() became next1(); next() handles
    TOK___PRAGMA: GCC/clang pragmas are ignored silently (like #pragma GCC diagnostic),
    anything else gets an "ignored" warning. Kernel __diag_push()/__diag_ignore() use it.
    Limitation: `_Pragma("pack(...)")` is not applied (warning).

22. 2026-10-20, APPLIED (uncommitted), `tccgen.c` unary() + `tcctok.h` (TOK_builtin_strlen,
    TOK_strlen) + `include/tccdefs.h` (strlen removed from the __BOTH list, keeps __BOUND):
    `__builtin_strlen("literal")` folds to a constant (kernel MODULE_INFO/module_param use it
    in _Static_assert: "constant expression expected" in 12 objects); other arguments call
    strlen() with a size_t result (REG_IRET pushed as for other helper calls).
    Verification: static assert and initializer fold; runtime cases 6 5 0; tests2/117 passes.

23. 2026-10-20, APPLIED (uncommitted), `i386-asm.c` asm_opcode: a segment register name used as
    an instruction prefix on the same line ("ds clflush %0", "cs nop") emits the prefix byte
    and continues with the instruction (synthetic ';' like lock/rep).

24. 2026-10-20, APPLIED (uncommitted), `x86_64-asm.h` + `i386-asm.c`: monitorx, mwaitx,
    clflushopt, clwb, and invpcid via a new OPC_0F38 instr_type flag (three-byte opcode map;
    the table's opcode field is 16 bits so the map byte cannot be in the opcode).
    Verification: all byte-identical to GNU as.

25. 2026-10-20, APPLIED (uncommitted), GCC `__attribute__((transparent_union))`: `tcc.h`
    (SymAttr.transparent), `tcctok.h`, `tccgen.c` parse_attribute/struct_decl/gfunc_param_typed
    (argument converted with gen_cast_to_union, which now decays arrays/functions before
    matching a member). Kernel: release_pages(union release_pages_arg) in fs/splice.c, mm/.
    Verification: pointer, array and int cases equal gcc's results; `make test` passes.

26. 2026-10-20, APPLIED (uncommitted), `tccasm.c` asm_fixup_local_label_relocs(): after an
    inline asm statement, relocations against local labels ("L..N", ".L*") become
    section-relative (section label symbol + offset), as GNU as emits them; objtool reads
    .discard.annotate_insn/.altinstructions entries that way ("bad .discard.annotate_insn
    entry: 0 of type 8"). REL targets adjust the in-place addend.
    Verification: alt.o relocs now `L..text + 12` like gcc's `.text + e`; runs; cross-test ok.

27. 2026-10-20, APPLIED (uncommitted), `tccasm.c` + `tcctok.h` + `tcc.h`/`tccgen.c` (asm_free_macros
    from tccgen_finish): GNU as macro facility for inline asm: `.macro name p[:req]..`/`.endm`/
    `.purgem`, `\param`, `\@`, `\()`, keyword and positional arguments; `.irp var,items`/`.endr`;
    `.if expr`/`.ifc a,b`/`.ifnc`/`.else`/`.endif` (false branches skipped with nesting);
    `.error "msg"`. Bodies are kept as text (GNU as semantics are textual) and assembled again
    from a buffer (asm_assemble_text); '\' tokens are accepted while assembling
    (PARSE_FLAG_ACCEPT_STRAYS). Needed by the kernel's _ASM_EXTABLE_TYPE_REG (msr.h, segment.h,
    gsseg.h, fpu/legacy.h: 17 objects).
    Verification: the kernel's extable_type_reg macro produces the expected 12-byte entry
    with the register number of the register actually used; nested .irp/.if produce the
    expected bytes; `.error` fires; `make test` passes.

28. 2026-10-20, APPLIED (uncommitted), `x86_64-asm.h`: xsave/xsave64, xrstor/xrstor64,
    xsaveopt/xsaveopt64, xsavec/xsavec64, xsaves/xsaves64, xrstors/xrstors64 (OPC_48 for the
    64-bit forms, like fxsaveq). Kernel arch/x86/kernel/fpu.

29. 2026-10-20, APPLIED (uncommitted), `tccgen.c` unary() + `tcctok.h` + `include/tccdefs.h`:
    `__builtin_{clz,ctz,popcount,ffs}{,l,ll}` are compiler builtins now: folded for constant
    arguments (kernel ilog2() in array designators -> "constant expression expected"), else a
    call to the same-named libtcc1 function as before (declarations removed from tccdefs.h;
    clrsb/parity still declared there).
    Verification: designators and _Static_assert fold; runtime values equal gcc's; `make test`.

30. 2026-10-20, APPLIED (uncommitted), `tccgen.c` decl0 function definition: a function placed
    in a named section (`__attribute__((section(".noinstr.text")))`) always makes that section
    executable (flags |= text's), not only when the attribute created the section. Before, a
    prior declaration with the attribute created the section as data-only (A), so objtool did
    not decode .noinstr.text/.init.text ("bad .discard.annotate_insn entry") and the kernel
    would have had non-executable code sections.

31. 2026-10-21, APPLIED (uncommitted), `x86_64-asm.h`: rdfsbase, rdgsbase, wrfsbase, wrgsbase
    (F3 0F AE /0../3, 32- and 64-bit register forms). Kernel arch/x86/include/asm/fsgsbase.h.
    Verification: 9 encodings (eax/rax/ecx/rcx/edx/rdx/r8d/r8/r15) byte-identical to GNU as;
    `make test` passes.

32. 2026-10-21, APPLIED (uncommitted), `tccgen.c` decl0 right after type_decl():
    `__attribute__((transparent_union))` placed after the declarator
    (`typedef union {...} release_pages_arg __attribute__((__transparent_union__));`, kernel
    include/linux/mm.h) or before the type now marks the union type (type.ref->a.transparent);
    fix 25 only saw the attribute directly after the union body.
    Verification: pointer args of both member types and a compound literal give gcc's output;
    `make test` passes.

33. 2026-10-21, APPLIED (uncommitted), `tccasm.c` asm_instr(): when nocode_wanted is set
    (after return/goto, dead branch of a constant condition, inside sizeof/typeof) the asm
    statement is consumed syntactically (balanced parentheses + ';') and not assembled, as
    GCC never assembles unreachable asm. Kernel arch/x86/kernel/cpu/common.c
    flag_is_changeable_p() has 32-bit `pushfl` asm after `if (IS_ENABLED(CONFIG_X86_64)) return`.
    Verification: dead `pushfl`, asm in `if (0)`, after `return` in a switch, and inside
    sizeof(({...})) compile and the program prints gcc's output; `make test` passes.

34. 2026-10-21, APPLIED (uncommitted), `tccelf.c` tccelf_end_file()/update_relocs() +
    is_asm_local_label(): assembler local labels (numeric `1:` = "L..<n>", and ".L*") that are
    LOCAL NOTYPE and defined are no longer written to the output symbol table, as GNU as does.
    A referenced one is replaced by an STT_SECTION symbol of its section (created on demand,
    one per section per file) with the label value added to the addend (RELA) or in place
    (REL, add32le). Input symbols are processed from a copy because the symtab is rewritten in
    place and the extra section symbols would overtake unread entries (found: corrupted
    symbols, 3 `.text` section symbols). Cause: the two-pass inline assembler (fix 12) rolled
    back section data but not the symtab, so each discarded pass left its `L..773` symbols,
    one in the middle of a `fxrstor64` -> objtool "L..773(): can't find starting instruction"
    (arch/x86/kernel/fpu/signal.o).
    Verification: signal.o has 0 local labels (was 400 of 724 symbols), objtool exit 0; a
    two-file program with `.quad 1b, 3b-1b` data relocs links and runs; `make test` passes.
    Note: tcc's own anonymous symbols are "L.<n>" (one dot) and are untouched; `.eh_frame`
    section symbols from tccdbg.c are pre-existing.

35. 2026-10-21, APPLIED (uncommitted), GCC global register variables
    (`register unsigned long current_stack_pointer asm("rsp");`, kernel arch/x86/include/asm/asm.h,
    also used as the asm output operand ASM_CALL_CONSTRAINT in ~17 headers): `tcc.h`
    (SymAttr.regvar replaces the unused bit), `tccgen.c` parse_btype (TOK_REGISTER sets it),
    merge_symattr, decl0 file-scope path (register + asm label naming a register ->
    `VT_STATIC` per-unit slot, VT_EXTERN cleared; error if the label is not a register) and
    unary() (every read first emits `store(reg, slot)`, i.e. `mov %rsp, slot(%rip)`, unless
    nocode_wanted). Stores and asm operands use the slot as for local register variables.
    Problem: tcc treated the declaration as a tentative global definition -> every object
    defined a global `rsp` in .bss (483 "multiple definition of `rsp'" at the vmlinux.o link)
    and reads returned the memory contents (0), breaking on_thread_stack().
    Verification: two-TU program: reads equal `mov %rsp` within a frame, "+r"(regvar) asm
    with a call works, link has one LOCAL `rsp` slot per object; tcc `make test` passes;
    kernel init/main.o, kernel/entry/common.o rebuilt: `rsp` LOCAL, objtool clean.
    Limitation: a C store to the variable updates only the slot, not the register (GCC writes
    the register); the kernel never assigns current_stack_pointer in C.

--- Fixes 36-41 (2026-10-21): dead code that gcc never emits referenced undefined symbols at the
vmlinux link (link-time assertions, stubs of disabled subsystems). All verified with a gcc-oracle
program (scratch dce.c: identical output, no undefined references) and `make test`. ---

36. 2026-10-21, APPLIED (uncommitted), `tccgen.c` block() TOK_WHILE/TOK_FOR/TOK_DO +
    dead_loop_tokens()/replay_end(): a loop in unreachable code (nocode_wanted == CODE_OFF_BIT)
    used to get its whole body generated because gind() at the loop head turns code on (a label
    inside may be a goto target: tests2/87_dead_code.c). Now the rest of the statement is saved
    (head; body = block or simple statement; do-while tail), scanned for labels (identifier
    followed by ':' or __label__, conservatively), and if there is none the loop stays dead
    (CODE_OFF after each gind()); the tokens are then parsed from the saved copy. Bodies that
    are if/for/while/do/switch/case/default statements are not analysed (generated as before).
    Kernel: fs/super.c `if (IS_ENABLED(CONFIG_BLOCK)) while (... bdev_thaw(...))`.
    A first version that generated the condition at the back edge when a label made the body
    live was wrong (the body part before the label was missing) and was replaced. Second bug:
    when the body cannot be delimited the tokens read so far must be given back as a
    self-ending macro (0 terminator, begin_macro alloc 1, return NULL) so the parser continues
    with the file; with an explicit TOK_EOF the parser stopped ("expression expected before
    '<eof>'" in mm/vmalloc.c, oom_kill.c, hrtimer.c: scoped_cond_guard() bodies start with if).

37. 2026-10-21, APPLIED (uncommitted), `tccgen.c` TOK_SWITCH/TOK_CASE/TOK_DEFAULT +
    switch_fold_scan(), switch_t.fold/ncase/match/match_ind: a switch on a constant value
    (`switch (sizeof(x))` in percpu.h, uaccess.h, cmpxchg.h, atomic) with a braced body is
    pre-scanned from the saved tokens (case values via expr_const64; nested switch -> no fold)
    for the case that is taken; only that case label starts code, `default` only when no case
    matches, fall-through still works, and the dispatch is one direct jump. Removes the
    references to __bad_size_call_parameter, __xchg/__cmpxchg/__xadd_wrong_size,
    __put_user_bad, __get_user_bad (~700 at the link).

38. 2026-10-21, APPLIED (uncommitted), `tcc.h` FuncAttr.func_fold:2/func_fval:13 (the old
    spare bits), `tccgen.c` inline_fold_check() (called where a static inline body is saved)
    and the call site in unary(): a `static inline` function whose body is `{ }` (void) or
    `{ return <constant>; }` where the expression consists only of integer constants, enum
    values, typedef/type keywords, sizeof/alignof and side-effect free operators (no parameter
    names) and whose value is in -4096..4095, is folded at direct calls: arguments are still
    evaluated (side effects), then dropped, and the constant of the return type is pushed; no
    reference to the function. Kernel: IS_ENABLED()-style stubs (efi_enabled -> efi_init,
    is_x32_frame -> x32_setup_rt_frame, folio_test_hugetlb -> free_huge_folio, use_dma_iommu ->
    iommu_dma_*, shmem_mapping -> shmem_writeout, empty apply_retpolines(__retpoline_sites..)).
    Bug found on the way: CValue.i is unsigned, the range check needed (int64_t) casts.

39. 2026-10-21, APPLIED (uncommitted), `tcctok.h` (TOK_builtin_strcmp, helper TOK_strcmp) +
    `tccgen.c` unary(): `__builtin_strcmp(a, b)` with two string literals folds to -1/0/1,
    otherwise it calls strcmp() (arguments saved as tokens and replayed). Kernel
    kernel/sched/fair.c vruntime_cmp()/vruntime_op() select code with it (else
    __BUILD_BUG_vruntime_cmp()). Bug found on the way: cstr_cat(.., -1) copies without the NUL
    (use 0), which made the comparison read garbage.

40. 2026-10-21, APPLIED (uncommitted), `x86_64-gen.c` gen_bitscan() + `tcc.h` + `tccgen.c`:
    on x86_64 the non-constant `__builtin_clz/ctz/ffs{,l,ll}` are generated inline (bsr/bsf,
    `xor $bits-1` for clz, jnz/mov $-1/add $1 for ffs(0)==0) instead of calling the libtcc1
    function (the kernel has no libtcc1; fs/seq_file.c uses __builtin_clzll). popcount still
    calls. Verified against gcc for 8 values x 32/64-bit, incl. ffs(0).

41. 2026-10-21, APPLIED (uncommitted), `include/tccdefs.h` x86_64: `__builtin_va_arg` fetches
    integer-class arguments of 1..8 bytes inline (`__tcc_va_arg_gp`: gp_offset < 48 ->
    reg_save_area, else overflow_arg_area, both advanced by 8) and only calls __va_arg() for
    the other classes; the kernel (lib/vsprintf.c, kasprintf.c) has no __va_arg. Zero-sized
    structs must keep the __va_arg path (tcctest stdarg_for_struct: they take no slot), hence
    `sizeof(t) - 1 < 8`.

42. 2026-10-21, APPLIED (uncommitted), `tccgen.c` inline_fold_check() rewritten: instead of a
    token allow-list, the return expression is parsed for real with the parameters pushed into
    scope (sym_push as VT_LOCAL at local_scope+1, popped after), nocode_wanted = 1 (no code, no
    data), and side effects are detected while parsing: fold_side_effect() is called from
    vstore(), vpush_helper_func(), gv() of a volatile lvalue and the call site of unary() for
    non-folded calls, and counts only at the probe's own nocode level (dead arms of ?:, &&, ||
    and sizeof are deeper). A quick token look only rejects statements/asm/unknown names
    (member names after '.', '->', ',' and tags after struct/union/enum are fine). This folds
    transitively: `return 0 || is_ia32_compat_frame(ksig)`, `return is_vma_hugetlb_flags(&vma->
    flags)`, `return cpu_feature_enabled(X)` (constant ?: with calls in the dead arm). Value range
    is now -2048..2047 (func_fval:12, one bit went to func_used).
    Later refinements (same day): the probe is fold_probe_expr(fs, cast_to, term, &val); names
    starting with "__builtin_" are known (builtin tokens sit above TOK_UIDENT, so sym_find()
    fails for them: use_fxsr()/cpu_feature_enabled() were never probed); `({ ... })` statement
    expressions are allowed (a probe scope `cur_scope` is set up, `loc` saved) so dead arms with
    READ_ONCE() pass (is_rd_overutilized); a void body `{ if (<constant 0 expr>) <stmt> }`
    folds to empty (alternative.c __apply_fineibt); no probing with -g (debug_modes).
    Also dead_loop_tokens(): a label targeted only by a `goto` inside the saved loop and not
    referenced before (label_find) keeps the loop dead (scoped_cond_guard's `_label: break;`).
    Bug found on the way: a string literal inside the probe leaves an exhausted unget_tok()
    level on the macro stack (popped lazily on the next read), so end_macro() popped the wrong
    level ("declaration expected" after `static inline const char *f(void) { return "x"; }`,
    kernel string_choices.h, tcc tests/tcctest.h). New `end_macro_to(str)` in tccpp.c unwinds to
    the replayed string; used by replay_end() everywhere.

43. 2026-10-21, APPLIED (uncommitted), `tccgen.c` switch_fold_scan(): a nested braced switch
    inside the body is skipped (its cases are its own) instead of giving up; the kernel's
    this_cpu_xchg() nests `switch (sizeof)` in a case of another one (8 __bad_size_call_parameter).

44. 2026-10-21, APPLIED (uncommitted), `tccgen.c` save_stmt_tokens()/save_paren_tokens() used by
    dead_loop_tokens(): the loop body is saved by a recursive statement grammar (block; if/else,
    while, for, switch with their sub-statements; do-while; otherwise up to ';'), so bodies that
    start with `if` (the kernel's scoped_cond_guard() expansion) are analysed too; `else`, `case`,
    `default`, `__label__` at the start give up.

45. 2026-10-21, APPLIED (uncommitted), `tccgen.c` decl0 (non-deferred definitions) + unary() call
    site: a non-weak void function whose body is `{ }` gets func_fold = 1 (peeked with next()/
    unget_tok('{')), so direct calls to it are dropped and their arguments not referenced: the
    kernel's `void __init_or_module noinline apply_retpolines(s32 *start, s32 *end) { }` stubs
    called with __retpoline_sites/__return_sites/__cfi_sites/__ibt_endbr_seal (linker-script
    symbols that exist only with the mitigation).

46. 2026-10-21, APPLIED (uncommitted), `tccgen.c` decl0 + gen_inline_functions() + `tcc.h`
    (InlineFunc.sec/is_static, FuncAttr.func_used from attribute((used)) in parse_attribute/
    merge_funcattr): static non-inline functions are recorded like static inline ones and only
    emitted at the end of the unit when referenced (sym->c), as gcc -O2 drops unused static
    functions; exceptions: attribute used/constructor/destructor/alias definitions. The
    section attribute is now honoured for deferred functions (fn->sec, flags made executable),
    which also fixes `static inline __init` functions that landed in .text. An alias target that
    is a function gets its ELF symbol created at the alias declaration so it is emitted.
    Kernel: static helpers that are only called from folded-away code (mm/rmap.c
    try_to_unmap_poisoned_hugetlb_one -> huge_pte_dirty, kernel/sched/fair.c eenv_pd_max_util).

47. 2026-10-21, APPLIED (uncommitted), `tccgen.c` gen_opic(): `x & 0` and `x * 0` with a
    non-constant x (not in constant expressions) leave x evaluated (volatile x is loaded) and
    yield the constant 0; `mapping->host && (mapping->host->i_flags & S_DAX)` with S_DAX 0 then
    folds (mm/truncate.c -> dax_*).

Verification of 42-47: scratch dce2.c (transitive stubs with parameters, impure/volatile stubs
not folded, parameter shadowing a function name, side effects in dead arms, empty/weak functions,
unused/used/section/alias/address-taken/forward-declared statics, nested constant switch, dead
for-loop with if body) gives gcc's output and exactly the expected undefined symbols; `make test`.

48. 2026-10-21, APPLIED (uncommitted), `tccpp.c` pragma_parse() + `tcc.h` (TCCState.vis_stack/
    vis_stack_ptr) + `tccgen.c` decl0 (file-scope declarations without an explicit visibility
    attribute get the pragma's) + `x86_64-gen.c` sym_is_local() used by load()/store()/
    gen_modrm_impl(): `#pragma GCC visibility push(hidden|default|internal|protected)` / `pop`
    is implemented (it was silently ignored), and hidden/internal symbols are addressed like
    static ones (`lea sym(%rip)`, direct RIP-relative loads/stores, R_X86_64_PC32) instead of
    through the GOT. Problem: the kernel's position independent code (arch/x86/boot/startup/*.pi.o,
    the decompressor) force-includes include/linux/hidden.h; with tcc the `mov sym@GOTPCREL(%rip)`
    (fix 4) was relaxed by the non-PIE vmlinux link to an absolute `mov $sym` (48 c7 c0 imm32), so
    __startup_64() -> arch_test_bit(boot_cpu_data...) touched the kernel virtual address before
    paging was set up: page fault -> triple fault, no console output (first boot of the tcc-built
    bzImage, survey 13). gcc emits RIP-relative accesses there because of the pragma.
    Verification: hidden -> R_X86_64_PC32 and ELF HIDDEN, default -> REX_GOTPCRELX; two-file
    program with hidden/default globals gives gcc's output; `make test` passes.

49. 2026-10-21, APPLIED (uncommitted), `i386-asm.c` subst_asm_operand(): a constant "i"/"n"
    asm operand was printed with "%d" of an int, truncating 64-bit constants. The kernel's
    runtime_const_ptr() (arch/x86/include/asm/runtime-const.h, used for dentry_cache in
    fs/dcache.c) emits `mov %1,%0` with "i"(0x0123456789abcdefull) and later patches the 8
    immediate bytes at `1b - 8`; tcc produced `mov $-1985229329,%rax` (7 bytes, sign-extended
    imm32), so the patch overwrote the preceding `jmp`'s displacement -> jump to
    __d_alloc+0x18 with 0x80000000 added -> oops "unable to handle page fault for address
    ffffffff012e322b", panic in vfs_caches_init (second boot attempt, survey 14). Now "%lld".

50. 2026-10-21, APPLIED (uncommitted), `i386-asm.c` parse_operand()/asm_opcode(): an immediate
    in 2^31..2^32-1 also gets the OP_IM64 bit (it is an imm32 for 32-bit instructions but
    `mov $0x80000000,%rdi` sign-extended it to 0xffffffff80000000), and for a 64-bit register
    destination the generic 0xb0 (register-sized) mov entry is always skipped, so values that
    fit int32 take the 0xc7 form and the others the imm64 (movabs) entry. 10 encodings
    byte-identical to GNU as (movabs is still not a known mnemonic; the kernel's C code does
    not use it). `make test` passes.

--- Fixes 51+ come from building musl 1.2.6 and busybox with tcc for the tcc-linux initramfs ---

51. 2026-10-21, APPLIED (uncommitted), `tccasm.c` asm_expr_unary() identifier case.
    Problem: the assembler rejected `call exp2l@PLT` / `jmp setjmp@PLT` ("end of line
    expected"); musl's hand-written x86_64 asm (src/math/x86_64/expl.s,
    src/signal/x86_64/sigsetjmp.s) and gcc -fPIC output use the suffix.
    Fix: after a symbol, accept `@PLT` and ignore it (call/jmp already emit R_X86_64_PLT32);
    any other `@suffix` is a clear "unsupported symbol suffix" error instead of a syntax error.
    Verification: objdump -dr byte- and reloc-identical to GNU as for call/jmp sym@PLT;
    `tests/asmtest.S` gained `call L4@PLT` / `jmp L4@PLT` (compared against gas by the asmtest
    target); `make test` ALL TESTS PASSED.

52. 2026-10-21, APPLIED (uncommitted), `i386-asm.c` (constraint_priority, asm_compute_constraints,
    subst_asm_operand, asm_gen_code, asm_clobber) + `x86_64-gen.c` NB_ASM_REGS 16 -> 26 with
    NB_ASM_GREGS 16 for the general-register loops.
    Problem: inline asm constraints "x" (SSE register), "t"/"u" (x87 st(0)/st(1)), "X" (any
    operand) and clobbers "st"/"st(N)"/"xmmN" were "unknown constraint" / "invalid clobber
    register"; all 18 musl src/math/x86_64/*.c files (fabs, sqrt, lrint, llrint, rintl, fmodl,
    remainderl, remquol, ...) use them.
    Fix (x86_64 only, #ifdef TCC_TARGET_X86_64): the asm allocator tracks xmm0-7 (TREG_XMM0..7)
    and st(0)/st(1) (TREG_ST0, pseudo TREG_ST1) besides the 16 GPRs; "x" needs a float/double
    operand and takes a free xmm register (load()/store() already know movd/movq for xmm);
    "t"/"u" need a long double, "u" is input-only; operands print as %xmmN / %st / %st(1);
    loads happen in three passes (GPR/xmm, then st(1), then st(0)) so the x87 stack is in the
    order the asm expects; after the asm, "=t"/"+t" outputs are stored with store() (keeps
    st(0)) followed by `fstp %st(0)`, and every x87 input not tied to an output is popped
    unless the asm clobbered "st"/"st(1)" (GCC's rule: a clobbered x87 input was popped by the
    asm itself), so lrintl's `"fistpll %0" : "=m"(r) : "t"(x) : "st"` gets no extra pop.
    "X" behaves like "g". st/xmm clobbers do not block allocation (tcc holds no live values
    in them across an asm statement thanks to save_regs()).
    Verification: a test file with all 13 musl patterns (plus operands through pointers and
    1000 iterations to check x87 stack balance) gives the same output as gcc, except remquol's
    quotient where tcc matches glibc (3 / -4) and gcc's build of the musl snippet does not;
    generated fmodl code is `fldt y; fldt x; fprem; fnstsw; fld %st; fstpt x; mov %ax; fstp;
    fstp`; `make test` ALL TESTS PASSED.

53. 2026-10-21, APPLIED (uncommitted), `libtcc.c` tcc_set_linker()/args_parser_add_group()/
    tcc_add_group() + `tcc.c` main file loop + `tcc.h` (struct filespec.group, PUB_FUNC
    tcc_add_group).
    Problem: `-Wl,--start-group ... -Wl,--end-group` was "unsupported linker option"; busybox
    links 30 per-directory lib.a archives with circular references (applets -> coreutils ->
    libbb -> ...) inside a group, and without it tcc's single pass left ~400 `*_main` and
    libbb symbols unresolved (archives are only searched when they are reached).
    Fix: the two options become marker entries in s->files; at --end-group the archives of the
    group (-l entries and plain files starting with "!<arch>\n"; sources and objects are
    skipped) are added again until a pass changes neither the number of symbols nor the number
    of undefined ones, like GNU ld's rescan. --end-group without --start-group is an error.
    PUB_FUNC because tcc.c calls it (the dlltest links tcc.c against libtcc.so).
    Verification: a 3-object circular case (main -> liba.a:a -> libb.a:b -> liba.a:a_helper)
    fails without and links with the group, also with -l names and with objects inside the
    group; GNU ld behaves the same; busybox links; `make test` ALL TESTS PASSED.

54. 2026-10-21, APPLIED (uncommitted), `tccgen.c` inline_fold_check(), decl0() empty-function
    case, unary() call site.
    Problem: the trivial-body fold (fixes 38/45) lived only in the function *type* (FuncAttr
    func_fold), so every symbol sharing that type Sym was folded too: musl's exit.c has
    `static void dummy(){}` and `weak_alias(dummy, __stdio_exit)` = `extern __typeof(dummy)
    __stdio_exit __attribute__((weak, alias("dummy")))`; the call `__stdio_exit()` in exit()
    was dropped although the link resolves the weak alias to the real __stdio_exit.o, so
    stdout was never flushed at exit: `hexdump -C`/`od` lost their last partial buffer.
    Fix: the defining symbol's token is stored in the function type Sym's `c` field (unused
    for function types, -1) when func_fold is set, and unary() folds only a direct call whose
    symbol token matches; weak aliases and `extern __typeof(f) g;` declarations keep the call.
    Verification: test with a weak alias overridden by a strong definition and a `__typeof`
    declared second function: tcc output equals gcc -O2 (calls kept, `dummy()`/`zero()` still
    folded); musl's exit() calls __stdio_exit again (objdump); busybox hexdump/od output equals
    the gcc build's; `make test` ALL TESTS PASSED.

55. 2026-10-21, APPLIED (uncommitted), `x86_64-asm.h` (table), `i386-asm.c` (OPC_0F3A, map byte
    insertion, xmm8-15 operands, sha256rnds2 3-operand form), `i386-tok.h` (xmm8-15 tokens),
    `tccasm.c` + `tcctok.h` (`.octa`), `tests/asmtest.S`.
    Problem: busybox's SHA1/SHA256 hwaccel `.S` files (CONFIG_SHA*_HWACCEL, defconfig) need
    xorps, movhlps, shufps, pshufb (SSSE3), palignr, pinsrd, pextrd (SSE4.1), sha1rnds4,
    sha1nexte, sha1msg1/2, sha256rnds2 (with an explicit `%xmm0` first operand), sha256msg1/2
    (SHA-NI), `%xmm8`-`%xmm15`, and `.octa` 128-bit constants for the shuffle masks; movaps/
    movups register-to-register forms were "bad operand" because the table gave the load forms
    an `OPT_EA | OPT_REG32` source (a general register is invalid there, an xmm register is).
    Fix: new table entries (0x66 prefix in the high opcode byte as for the existing 0x66..
    entries, map from the new OPC_0F3A flag or OPC_0F38); the 0F 38/0F 3A map bytes are ORed
    into the opcode right after the OPC_0F expansion so that the later special cases (imul,
    int3, arith `v <= 5`) never see e.g. pshufb's 0x00; `%xmm8-15` are tokens outside the
    8-per-class register block, parse_operand() gives them OP_SSE with reg 8-15 and asm_rex()
    already emits REX.R/REX.B for them; `sha256rnds2 %xmm0, src, dst` drops the implicit
    operand (error if it is not %xmm0); `.octa` parses hex/decimal literals (optionally
    negated) into two 64-bit halves without __int128 (tcc self-hosts) and emits 16 bytes LE.
    Verification: a 38-instruction test file (all forms incl. memory, RIP-relative and
    xmm8-15 operands) disassembles identically to GNU as; the `.text.sha1_process_block64_shaNI`
    (508 bytes) and `.text.sha256_process_block64_shaNI` (661 bytes) sections of the two
    busybox objects and the 6 `.octa` constants are byte-identical to GNU as; busybox
    sha1sum/sha256sum of 14 sizes (0..1000000 bytes) match coreutils on a SHA-NI host (so the
    hwaccel code actually ran) and the "abc" test vectors; the forms are appended to
    tests/asmtest.S (that target's gas reference is already broken by the host binutils on
    `lar`, unrelated); `make test` ALL TESTS PASSED.

56. 2026-10-21, APPLIED (uncommitted), `tccpp.c` parse_define().
    Problem: in a `.S` file a macro body is tokenized with PARSE_FLAG_ASM_FILE cleared (so
    that `#` is not a comment there), and with dollars_in_identifiers (on by default) `$`
    then starts an identifier: `#define IMM $0x50` + `mov IMM, %eax` silently assembled to
    `mov 0x0,%eax` with an R_X86_64_32S relocation against a symbol named `$0x50`;
    `#define SHUF(a,b,c,d) $((a)+((b)<<2)...)` gave "register expected" (busybox's shufps).
    Fix: while parsing a #define in an asm file, `$` is temporarily removed from the identifier
    characters (set_idnum('$', 0), restored at the end, like the existing '.' handling for
    macro parameter lists), so it stays the immediate marker exactly as when assembling.
    Verification: `mov IMM, %eax` through a macro gives `b8 50 00 00 00` like GNU as; the
    SHUF() macro assembles; C files still accept `$` in identifiers (`int $x`);
    `make test` ALL TESTS PASSED.

Wrapper/config (not tcc): `minimal/tcc-kcc` passes -U__OPTIMIZE__ (kernel compiler_types.h
documents it: __compiletime_assert -> undefined __compiletime_assert_N when the compiler does
not fold); `minimal/tcc.config` sets SECTION_MISMATCH_WARN_ONLY=y (static inline __init-only
helpers are real .text functions with tcc -> benign modpost mismatches).
K5. 2026-10-21, APPLIED (uncommitted), `include/linux/seqlock.h`: `|| defined(__TINYC__)` in the
    #if that selects the empty `__scoped_seqlock_bug()` stub (the extern canary assumes the
    compiler proves ss_done impossible after inlining).
K6. 2026-10-21, APPLIED (uncommitted), `include/linux/seqlock.h`: under `__TINYC__`
    `__scoped_seqlock_invalid_target()` is an empty static inline instead of the extern canary
    (the switch on the constant target is only folded after inlining).
K7. 2026-10-21, APPLIED (uncommitted, semantics-preserving, unguarded), `arch/x86/kernel/fpu/
    signal.c`: `ia32_fxstate_possible()` macro (X86_32 || IA32_EMULATION) used where
    ia32_fxstate is tested (copy_fpstate_to_sigframe, __fpu_restore_sig), so the code paths
    with convert_to/from_fxsr() are dead without propagating the constant through the variable.
K8. 2026-10-21, APPLIED (uncommitted, semantics-preserving), `kernel/dma/direct.c`:
    `if (IS_ENABLED(CONFIG_ARCH_HAS_DMA_SET_UNCACHED) && set_uncached)` (arch_dma_set_uncached).
K9. 2026-10-21, APPLIED (uncommitted, semantics-preserving), `kernel/dma/direct.c`:
    `if (!IS_ENABLED(CONFIG_DMA_COHERENT_POOL)) { WARN_ON_ONCE(1); return NULL; }` instead of
    `if (WARN_ON_ONCE(!IS_ENABLED(..)))` (the constant does not survive WARN_ON_ONCE's local).

Port notes: PORT-NOTES.md in this directory; the patch series: ../patches/ (formerly the diffs in
/home/nika/Documents/linux-claude/patches/).

Wrapper (not tcc): until fix 60 `tcc-kcc` routed the arch/x86/entry/vdso/ C files to gcc
(the vDSO is userspace linked without libc and tcc emitted memcpy calls there); since
2026-10-22 nothing goes to gcc (see the entries below and `kernels/linux/tcc-kcc`).

57. 2026-10-22, COMMITTED, `x86_64-asm.h`: lretq (0x48cb, also with an imm16), movabs/movabsq
    (always the 10-byte REX.W B8+r imm64 form, like GNU as), `ud1 r/m, r` (0F B9 /r, the
    kernel's "ud1 (%edx), %rdi" reachability annotation in arch/x86/entry/entry.S), sysretl
    (plain 0F 07); later in the day (fix 68) also bswap of a 64-bit register, the SSE2
    integer forms movdqu, movdqa, paddq, pshufd, pslldq, psrldq, punpcklqdq, punpckhqdq.
    Verification: encodings byte-compared with GNU as on snippets of every form.

58. 2026-10-22, COMMITTED, `libtcc.c`, `tcc.c`, `tccasm.c`, `tcc.h` (asm_text_only): `-S`.
    Problem: Kbuild makes include/generated/asm-offsets.h (and bounds.h, rq-offsets.h,
    devicetable-offsets.h) from `cc -S` output: the `.ascii "->SYM $value comment"` lines that
    `asm volatile("\n.ascii \"->" #sym " %0 " #val "\"" : : "i"(val))` produces. tcc had no -S
    at all ("invalid option").
    Fix: -S is accepted like -c for the compiler action (the "i" operands must be evaluated);
    in that mode every asm statement is written to the output file as text after operand
    substitution instead of being assembled, and -MD/-MMD still write the dependency file.
    Verification: the kernel's asm-offsets pattern gives exactly gcc's `.ascii` lines; Kbuild's
    sed turns them into the same #defines; the kernel's four offsets files are generated by tcc.

59. 2026-10-22, COMMITTED, `tccasm.c`, `tccpp.c`, `tcctok.h`: assembling the kernel's `.S` files.
    Problem: the kernel's x86 assembly (entry_64.S, head_64.S, the lib/ routines, the boot and
    realmode code, the alternative/annotation headers) uses GNU as features tcc lacked: macro
    bodies had to be re-created from tokens (which lost adjacency: `\op\()_safe_regs`), quoted
    macro arguments kept their quotes, arguments separated by blanks or in parentheses were
    not split like GNU as (`idtentry 0 asm_exc_de exc_de has_error_code=0`,
    `ftr2=ALT_NOT(X86_FEATURE_ALWAYS)`), no `:req`/`:vararg`/defaults, no `.elseif`/`.ifdef`/
    `.ifb`/`.ifeq` family, no `.incbin`, `.comm`, `.local`, `.zero`, `.uleb128`, `.equ`,
    `.print`, `.err`, `.cfi_*`; `.type name STT_FUNC` without a comma; `.section` ignored
    `@nobits` (the decompressor's `.pgtable` became PROGBITS with 24 KB of zeros) and gave
    every new section SHF_ALLOC; `.word`/`.byte` of a symbol were errors; `.if \base == %rsp`
    (unwind hints) and `$0x0700a169U` (cpufeaturemasks.h) were expression errors; the `.skip`
    padding of the ALTERNATIVE macros subtracts labels defined later in the *file*, which only
    the inline-asm two-pass handled; tccpp did not predefine `__CHAR_BIT__`/`__SIZEOF_*__` for
    assembler files (asm-generic/bitsperlong.h: "Inconsistent word size") and a `##` paste
    that does not form one token (`.L__sym_size_##\name`) inserted a blank.
    Fix: macro bodies and invocation lines are kept as text reconstructed from the
    preprocessed tokens with blanks preserved (PARSE_FLAG_SPACES) and strings as written;
    GNU-as argument splitting; parameters with qualifiers and defaults; `.exitm`, `.irpc`,
    `.rept` on text; the conditional family with proper nesting; the directives above;
    `.section` flags replace the defaults, which depend on the name as in GNU as, and the type
    is honoured; 8/16-bit relocations; registers compare by identity in expressions; C
    integer suffixes; a whole file is assembled twice when needed, via tccpp's new
    `preprocess_restart()` (re-reads the file keeping the token table and symbols, dropping
    the file's #defines and the include-once cache); asm-mode predefines; glued pastes.
    Verification: all 42 x86_64 `.S` files of the kernel assemble; `make test` passes; the
    kernel built from them boots (see K10-K12 and the port notes).

60. 2026-10-22, COMMITTED, `tccgen.c`, `x86_64-gen.c` (gen_struct_fill), `libtcc.c`
    (-fno-builtin, -ffreestanding), `i386-gen.c` (gen_struct_fill, fix 67).
    Problem: the vDSO is linked with --no-undefined and without libc; lib/vdso/getrandom.c
    copies with `__builtin_memcpy(&v, src, sizeof v)` and tcc called memcpy/memset for those
    and for the zero-filling of local initializers (init_putz). GCC expands constant-size
    memcpy/memset inline.
    Fix: a call of memcpy or memset (by either name, also the `__builtin_` declarations with
    asm("memcpy")) whose length is an integer constant up to 4096 becomes the struct-copy
    sequence or `rep stosb`, with the destination as the expression value; init_putz uses the
    fill; -fno-builtin/-ffreestanding (the kernel's real-mode code wants its own memcpy) and
    bounds checking keep the calls. Patch 0069: the do_bounds_check test needs
    CONFIG_TCC_BCHECK (the guest tcc is configured without it).
    Verification: against gcc on a program covering all size classes, return values, nested
    calls, a 0-length copy with a side-effecting argument, struct copies, a 4800-byte memset
    (stays a call); vgetrandom.o has no undefined memcpy/memset; `make test`.

61. 2026-10-22, COMMITTED, `include/limits.h` (new).
    Problem: with `__GNUC__` defined, glibc's <limits.h> expects the compiler's own limits.h
    (GCC's `_GCC_LIMITS_H_` guard, `#include_next`) and defines INT_MAX & co. only when the
    compiler is not GCC. The kernel's host tools (tools/include) need `__GNUC__` (for
    `__must_be_array`), so `kernels/linux/tcc-hostcc` claims GCC 4.2 and host tools failed with
    "include file 'limits.h' not found" / INT_MAX undeclared.
    Fix: a limits.h with the C limits and `#include_next <limits.h>`, no include guard of its
    own (the directory is on the search path twice, -I and -B, and each copy must pass on).
    Verification: INT_MAX/LONG_MAX/PATH_MAX match gcc with and without the GCC claim.

62. 2026-10-22, COMMITTED, `libtcc.c`, `tcc.h`: `-m16`, `-mregparm=N`, GCC `-m` options.
    Problem: the kernel's arch/x86/boot/*.c are compiled with REALMODE_CFLAGS (`-m16
    -mregparm=3 -march=i386 -mpreferred-stack-boundary=2 ...`) and the x86_64 tcc could not
    produce 16-bit i386 code.
    Fix: `-m16` on the x86_64 tcc execs `i386-tcc -m16` (like -m32 already did); the i386 tcc
    sets the 16-bit code mode for the code generator and the assembler (`.code16gcc`
    semantics); `-mregparm=N` makes GCC's register argument passing the default calling
    convention; the other `-m` options are accepted. `scripts/build.sh` builds `i386-tcc` in
    the host-tcc step.

63. 2026-10-22, COMMITTED, `i386-asm.c`, `i386-tok.h`, `x86_64-asm.h`, `Makefile`, `tccelf.c`,
    `i386-link.c`: one x86 assembler with runtime `.code16`/`.code16gcc`/`.code32`/`.code64`.
    Problem: the kernel switches code mode inside one object (compressed/head_64.S and
    la57toggle.S: .code32 then .code64 in an ELF64 object; realmode/trampoline_64.S,
    reboot.S, boot/pmjump.S: .code16/.code32/.code64 in an ELF32 object); tcc's assembler
    decided everything at compile time from the target.
    Fix: one instruction table (x86_64-asm.h, also for the i386 build; i386-asm.h removed)
    and one token set; 0x66/0x67 rules, GNU `.code16gcc` defaults (call/ret/jmp/push/pop
    32-bit), rel16/disp16 with R_386_16/PC16/8/PC8, REX and 64-bit registers only in 64-bit
    mode, OPC_NO64/OPC_ONLY64 entries, GNU as's prefix order for same-line lock/rep/addr32/
    data16, GNU relocation types in .code32 of ELF64 (R_X86_64_32/PC32), ~40 new mnemonics,
    REL in-place addends added in the field width (rel_add_addend).
    Verification (`tests/m16-check.sh asm`): 168 .code16gcc, 168 .code16, 174 .code32 (i386),
    174 .code32 (x86_64) and 108 .code64 (ELF32) snippets byte-identical to GNU as, including
    relocations.

64. 2026-10-22, COMMITTED, `tccgen.c`: a tentative definition followed by an initialized
    definition in a named section (`static __videocard video_vga;` ... `= { ... }` in the
    boot video drivers) made the section NOBITS without data (upstream tcc crashes too).
    Fix: the section becomes PROGBITS again and the definition reuses the tentative slot (no
    zeroed duplicate entry in `.videocards`).

65. 2026-10-22, COMMITTED, `i386-gen.c`, `tccgen.c` (post_type): 16-bit code generation and
    GCC's regparm.
    Fix: with -m16 every instruction gets the operand/address size prefixes GCC's -m16 output
    has (prolog, inline-asm register loads, flag-output movzbl included); GCC's regparm
    assignment (dword-wise, long long = 2 registers, structs in registers while they fit,
    the first non-fitting argument stops, floats skip, the sret pointer is the first argument
    in %eax and not callee-popped, variadic functions on the stack); -mregparm's default is
    applied in post_type; indirect regparm calls go through `call *disp(%ebp)` (all three
    allocatable registers carry arguments); inline `rep movsl` struct copies (and, fix 67,
    `rep stosb` fills).
    Verification (`tests/m16-check.sh codegen regparm`): all 20 arch/x86/boot/*.c compiled
    with -m16 and -m32 give identical instruction streams (objdump -M i8086 vs -M i386);
    gcc-main + tcc-callee, tcc-main + gcc-callee and tcc + tcc 32-bit programs pass 12
    regparm cases; the kernel's boot objects copy.o, realmode header.o and stack.o are byte-
    and relocation-identical to GNU as's.

66. 2026-10-22, COMMITTED, `tests/m16-check.sh` (+ m16-asm-cmp.sh, m16-file-cmp.sh, the
    snippet lists m16-asm-16/32/64.txt, tests/m16/regparm_*.c): the verification of fixes
    62-65 as a script (codegen, asm, files, regparm, link).

    Follow-up 2026-10-22 (before publishing): m16-file-cmp.sh had the kernel build directory
    hard-coded as an absolute path of this machine; it now takes $KBUILD like m16-check.sh
    (which exports KBUILD and KSRC), default ../../build/x86_64/linux relative to the tcc tree.
67. 2026-10-22, COMMITTED, `tccasm.c`, `i386-asm.c`, `i386-gen.c`: fixups after merging the
    -m16 work with fix 59: a named label defined later in the file is known in the second
    pass (`.skip 32 - (.Lret1 - 1f)` in entry_64.S); `sym - 1f` with a forward local label of
    the current section is PC-relative like with a defined one (`.byte start_of_setup-1f` in
    the boot header); gen_expr8/16 use gen_expr32's PC-relative convention; gen_struct_fill
    for i386.
    Verification: a file of .long/.word/.byte "sym - label" expressions across sections links
    to GNU as's bytes for ELF64 and ELF32.

68. 2026-10-22, COMMITTED, `tccasm.c`, `i386-asm.c`, `x86_64-asm.h`, `tcc.c`: the SSSE3
    crypto assembly and the linker scripts.
    Problem: lib/crypto/x86/sha256-ssse3-asm.S, sha256-ni-asm.S and blake2s-core.S define
    register aliases (`X0 = %xmm4`, `CTX = %rdi`, rotated inside macros: `X_ = X0`), use
    `&&` in `.if`, write displacements as `(\i + 12)*4(%rsi)` and need movdqu/movdqa & co.;
    Kbuild preprocesses the linker scripts with `cc -E -MMD` and tcc wrote no dependency file
    for -E (fixdep failed).
    Fix: register aliases usable wherever a register is (also as base/index); `&&`/`||`
    (results 1/0 like GNU as); a parenthesized displacement is an expression, not `(%reg)`;
    the SSE2 forms (see 57); -E writes -MD/-MMD.
    Verification: the three files assemble to the same sizes as GNU as (instruction streams
    equal except jump encodings and alignment padding); a snippet of every new form matches
    GNU as byte for byte.

69. 2026-10-22, COMMITTED, `i386-asm.c` (gen_expr32), `tccdbg.c` (lazy .eh_frame).
    Problem: `addq $(init_top_pgt - __START_KERNEL_map), %rax` in head_64.S is
    init_top_pgt + 0x80000000; the addend went through gen_addr32()'s int and became
    - 0x80000000, which does not fit R_X86_64_32S at link time. Every assembler file also got
    a 24-byte `.eh_frame` with a CIE and no FDE (an orphan section for the kernel's linker
    script).
    Fix: the 64-bit assembler emits the relocation with the 64-bit addend as computed; the
    .eh_frame section and its CIE are created at the first FDE, so files without functions
    have none (GNU as emits none).
    Verification: relocations identical to GNU as's for the three forms (32S immediate, mov
    imm, .quad); C output unchanged; `make test`.

70. 2026-10-22, COMMITTED, `tccasm.c` (asm_expr_sum order), `i386-asm.c` (parse_operand).
    Problem: `leal rva(gdt)(%ebp), %eax` (rva(X) = X - startup_32) and `.quad gdt - gdt64` in
    the compressed head_64.S subtract labels of one section where the first is defined later;
    the PC-relative conversion ran before the two-pass resolution and produced absolute
    R_X86_64_32/64 relocations, which the -pie link of the decompressor refuses (and the
    .quad had the wrong value); a parenthesized displacement lost its pcrel flag.
    Fix: the difference of two labels of one section is resolved to a constant whenever the
    second pass knows them, before the PC-relative forms; bracketed displacements keep pcrel;
    `.quad sym - label` across sections gets R_X86_64_PC64.
    Verification: the compressed head_64.o has only PC32/PLT32 relocations like GNU as's
    object; the PC-relative data test still links to GNU as's bytes; `make test`; the
    decompressor links with -pie and the kernel boots.

71. 2026-10-22, COMMITTED, `i386-asm.c` (imm_type, imm_truncated, asm_opcode).
    Problem: with CONFIG_SMP (the self-hosting image needs `make -j` in the guest),
    `arch/x86/kernel/head_64.S` has `andl $(~STARTUP_PARALLEL_MASK), %ecx` with the mask
    0xFF000000; the 64-bit assembler evaluates `~0xFF000000` as 0xFFFFFFFF00FFFFFF, classified
    it as a 64-bit immediate and found no encoding: "bad operand with opcode 'andl'". GNU as
    takes a constant in the operand size of a b/w/l operation (andl $0x00ffffff).
    Fix: when an instruction-table entry takes an immediate smaller than 64 bits and the
    operand size is known (the suffix, else a register operand; only the arith and b/w/l/q
    families, not `ret $N`/`int $N`/shift counts), a constant is truncated to that size and
    taken as signed in it, so `andw $0xffff` becomes the imm8 form (66 83 e1 ff) like GNU as.
    A 64-bit operation whose constant does not fit a sign-extended imm32 (`andq $0xffffffff`)
    is now rejected instead of silently sign-extended; GNU as rejects it too.
    Verification: 70 immediate forms in 64-bit mode and 14 in 32-bit mode byte-compared with
    GNU as 2.47 (all equal except the pre-existing `shll $1` long form and constructs GNU as
    rejects); `make test`; `tests/m16-check.sh asm`; the SMP kernel builds and boots.

72. 2026-10-22, COMMITTED, `tccgen.c` (the section attribute).
    Problem: the CONFIG_SMP link failed the linker script's `ASSERT(__per_cpu_hot_end -
    __per_cpu_hot_start <= 64, "percpu cache hot data too large")`: every cache-hot per-cpu
    variable has a section of its own (`.data..percpu..hot..<name>`, sorted by alignment), and
    tcc's `new_section()` gives every section the pointer size as alignment, so the bool, u16
    and int variables took 8 bytes each: 9 x 8 = 72 bytes where gcc's objects (section
    alignment = the largest alignment of the contents, as GNU as computes it) give 55.
    Fix: a section created by a `section` attribute starts at alignment 1 and the objects
    placed in it raise it (`section_add`), as the assembler already did for `.section`.
    Verification: a test file with char/short/int/long/struct/aligned(64) variables in named
    sections has the same section alignments as gcc's object; `make test`; the SMP kernel
    links and boots. Also makes tcc's other table sections (`__ex_table`, `.modinfo`, ...)
    pack like gcc's.

73. 2026-10-22, COMMITTED, `tccasm.c` (asm_expr_unary).
    Problem: inside the self-hosting guest, tcc (built against musl) failed on
    `lib/crypto/x86/sha256-ssse3-asm.S`: "local label '0' not found backward" at `pshufd
    $0b11111010, ...`. The assembler left numbers to `strtoull(p, &p, 0)`: glibc 2.38+ accepts
    the C23 binary prefix, so the host tcc assembled the constant by luck; musl's strtoull stops
    at the `b`, which then read as the backward reference to local label 0.
    Fix: `0b`/`0B` followed by a binary digit is parsed as a base-2 constant explicitly; `0b`
    alone stays the local label reference.
    Verification: binary constants and `0b`/`1b`/`1f` references byte-compared with GNU as; the
    musl tcc produces the same object as the glibc tcc for the SHA-256 file; the guest build.
    Lesson for the self-hosting image: the host tcc is compiled by gcc against glibc, the guest's
    by tcc against musl, so libc-dependent parsing differs; `scripts/selfhost-test.sh host` does
    not catch that class (it uses the host tcc), only the guest run does.

74. 2026-10-23, COMMITTED, `i386-gen.c` (PIC, gen_gotoff32, got_sym, the prologue written by
    gfunc_epilog), `libtcc.c` (-f options), `tccpp.c` (predefines), `tcc.h` (TCCState.pic).
    Problem: the 32-bit kernel's decompressor is compiled with -fPIE (it runs where the
    bootloader loaded it and copies itself to the end of its buffer) and the 32-bit vDSO with
    -fpic (a shared object mapped into every process); the decompressor's linker script
    refuses any run-time relocation and vdso2c fails on them. tcc's i386 code generator only
    produced absolute addresses (R_386_32) and ignored the options; x86_64 needed nothing
    here (RIP-relative addressing).
    Fix: -fPIC/-fpic/-fPIE/-fpie set `s->pic` (-fno-pic/-fno-PIE clear it). With it the i386
    code generator addresses every symbol `sym@GOTOFF(%ebx)` (R_386_GOTOFF): gen_modrm for
    memory references, load() for addresses (`lea sym@GOTOFF(%ebx)` instead of `mov $sym`);
    the prologue saves %ebx and loads the GOT address into it (`call 1f; 1: pop %ebx; add
    $_GLOBAL_OFFSET_TABLE_+3, %ebx`, R_386_GOTPC), the epilogue restores it; calls stay
    R_386_PC32. That is gcc's -fPIE model with hidden symbols (no GOT, no PLT), which is what
    both kernel users compile with (hidden.h, -Bsymbolic); -fPIC gets the same code.
    `__PIC__`/`__pic__` (2) and `__PIE__`/`__pie__` are defined like gcc. Not with -m16 (the
    real-mode code is never position-independent). Inline asm using %ebx is unaffected: tcc
    saves and restores it around the asm (asm_gen_code).
    Verification: a libc-free test program (globals, statics, strings, function pointers,
    long long, double, struct copies, 64-bit shifts, bit scans) linked with `tld -pie` runs
    correctly as a PIE and gives the same output as the absolute build and as gcc; the
    decompressor and the vDSO link under tld's PIE check (no R_386_32 against a defined
    symbol); the kernel boots.

75. 2026-10-23, COMMITTED, `tccasm.c` (asm_expr_unary: `@GOTOFF`), `i386-asm.c` (gen_expr32:
    R_386_GOTOFF, `_GLOBAL_OFFSET_TABLE_` as R_386_GOTPC, asm_insn_start), `tcc.h`
    (ExprValue.gotoff).
    Problem: compressed/head_32.S computes the GOT address with `call 1f; 1: popl %edx; addl
    $_GLOBAL_OFFSET_TABLE_+(.-1b), %edx` and addresses everything `sym@GOTOFF(%edx)`; the
    assembler rejected `@GOTOFF` and took `_GLOBAL_OFFSET_TABLE_` for an ordinary symbol.
    Fix: `sym@GOTOFF` marks the expression (ExprValue.gotoff) and gen_expr32 emits
    R_386_GOTOFF; the symbol `_GLOBAL_OFFSET_TABLE_` emits R_386_GOTPC, and like GNU as the
    addend grows by the offset of the field within the instruction (GNU as takes the symbol's
    value relative to the instruction start: the kernel's `addl $_GLOBAL_OFFSET_TABLE_+(.-1b)`
    gets the addend 3, not 1), for which asm_opcode records where the instruction began.
    Verification: head_32.S's forms byte-compared with GNU as (identical apart from the known
    symbol-vs-section spelling of local relocations); tld links the decompressor; the kernel
    boots.

76. 2026-10-23, COMMITTED, `i386-asm.c` (asm_clobber).
    Problem: the kernel's shared x86 headers clobber "rax", "rcx", "rdx" (nospec-branch.h) in
    32-bit code too, and busybox's tls_pstm_mul_comba.c spells clobbers "%eax"; gcc accepts
    both, tcc said "invalid clobber register".
    Fix: the 64-bit register names mean the 32-bit registers in 32-bit code, and a leading
    '%' is skipped.
    Verification: both files compile; the registers are saved around the asm as before.

77. 2026-10-23, COMMITTED, `i386-asm.c` (asm_opcode: size inference, operand size prefix).
    Problem: entry_32.S's `mov PT_FS(%esp), %fs` has no suffix and no sized register:
    "cannot infer opcode suffix".
    Fix: a mov between a segment register and memory is a 16-bit one (GNU as), and, also
    like GNU as, the store form (`mov %fs, mem`, opcode 0x8c) never takes the 0x66 prefix
    (the load form 0x8e already did not); push/pop of a segment register keep their rule.
    Verification: the eight mov forms with segment registers byte-identical to GNU as;
    `tests/m16-check.sh asm` (push/pop %seg in every mode) passes.

78. 2026-10-23, COMMITTED, `tccgen.c` (gen_opic, the symbol + constant case).
    Problem: arch/x86/kernel/doublefault_32.c initializes `.__cr3 = __pa_nodebug(
    swapper_pg_dir)` = `(unsigned long)swapper_pg_dir - 0xC0000000`: "initializer element is
    not constant". The folder refuses symbol addends that do not fit 32 bits signed, and
    -0xC0000000 does not.
    Fix: on a 32-bit target the addend wraps like the address arithmetic does (`l2 =
    (int)l2`): sym - 0xC0000000 is sym + 0x40000000 in 32 bits.
    Verification: the file compiles with the relocation addend 0x40000000, as with gcc; the
    kernel boots (the double fault TSS's CR3 is that value).

79. 2026-10-23, COMMITTED, `x86_64-asm.h`.
    Problem: mnemonics of the 32-bit kernel missing from the table: `sysenter`
    (vdso32/system_call.S), `ud2a` (vdso32/sigreturn.S), and `rdpid %eax` (segment.h's
    vdso_read_cpunode) and `invpcid mem, %eax` (invpcid.h) with a 32-bit register, which the
    table only had with a 64-bit one.
    Fix: ud2a (= ud2), sysenter (0f 34), sysexit (0f 35), and the 32-bit register
    alternatives of rdpid and invpcid.
    Verification: byte-compared with GNU as in 32-bit mode; the 64-bit forms unchanged.

80. 2026-10-23, COMMITTED, `i386-asm.c` (asm_opcode).
    Problem: the 32-bit vDSO's system call is `ALTERNATIVE("ds;ds;ds;int $0x80", "call
    __kernel_vsyscall", ...)` (asm/vdso/sys_call.h): a segment register name as a statement
    is its prefix byte; tcc took `ds` for an opcode with a bad operand.
    Fix: `es`..`gs` alone on a statement emit the override prefix; followed by an instruction
    on the same line they prefix it (GNU as behaviour).
    Verification: `ds`, `ds;ds;int $0x80`, `ds int $0x80` byte-identical to GNU as; the vDSO
    links.

81. 2026-10-23, COMMITTED, `tccasm.c` (.previous).
    Problem: vdso32/sigreturn.S ends with `.previous` after a `.text` directive, having never
    used `.section`: "no previous section referenced".
    Fix: like GNU as, the section in force at the start (.text) is the previous one of a file
    that has not switched sections yet.
    Verification: the file assembles; `make test`.

82. 2026-10-23, COMMITTED, `include/tccdefs.h` (`__builtin_mul_overflow`).
    Problem: every 32-bit kernel object referenced `__udivdi3`: the macro checks the overflow
    of the 64-bit product with a division (`__m / __ma != __mb`), a libtcc1 call on a 32-bit
    target, and the kernel's size_mul()/check_mul_overflow() (overflow.h) are everywhere.
    Fix: the division is compiled only when the operands' sizes add up to more than 8 bytes
    (two 32-bit magnitudes cannot overflow a 64-bit product); the condition is a constant, so
    tcc does not generate the dead arm.
    Verification: `__builtin_mul_overflow` on 32- and 64-bit operands gives gcc's results
    (value and overflow flag); size_mul() no longer references `__udivdi3`.

83. 2026-10-23, COMMITTED, `i386-gen.c` (gen_shift64), `tccgen.c` (gen_opl), `tcc.h`.
    Problem: a long long shifted by a variable count called `__ashldi3`/`__lshrdi3`/
    `__ashrdi3` on i386; the vDSO (vclock_gettime.c) and the kernel have no such helpers,
    gcc inlines them.
    Fix: gcc's sequence inline: the value in %edx:%eax, the count in %cl, `shld`+`shl` (or
    `shrd`+`shr`, `shrd`+`sar`), `test $32, %cl` and the fix-up of the halves; with -m16 the
    operand-size prefixes.
    Verification: the three shifts of nine 64-bit values by 0..63 give gcc's results
    (absolute and PIE builds; the -m16 encoding inspected); `make test`.

84. 2026-10-23, COMMITTED, `tccgen.c` (gen_opl).
    Problem: an unsigned 64-bit division or modulo by a constant power of two called
    `__udivdi3`/`__umoddi3` (page sizes and alignments in the kernel).
    Fix: a shift or a mask, as gcc does at every optimization level (signed division keeps
    the call: it needs the rounding fix-up).
    Verification: x / 4096, x % 4096, x / 1, x % 1, x / 2^63 on nine values against gcc.

85. 2026-10-23, COMMITTED, `tccgen.c` (block_cleanup). A pre-existing upstream bug; it
    crashed the x86_64 compiler on the same code too.
    Problem: kernel/futex/pi.c crashed tcc (SIGSEGV in gsym_addr) in
    futex_get_value_locked(): `guard(pagefault)()` (a cleanup variable) plus
    get_user_inline(), whose expansion `goto`es to a `__label__` local label of a statement
    expression from nested cleanup scopes. When that label's statement expression ended,
    label_pop marked it LABEL_GONE (3); block_cleanup tested `r & LABEL_FORWARD` (1), took the
    gone label for a pending forward one and followed the pending nodes freed when the label
    was defined, patching a stale jump index.
    Fix: `== LABEL_FORWARD`.
    Verification: the reduced test case and the kernel file compile on i386 and x86_64; a
    destructor-order test (gotos out of nested cleanup scopes, local labels, statement
    expressions) gives gcc's output; `make test`.

86. 2026-10-23, COMMITTED, `tccgen.c` (decl_initializer_alloc).
    Problem: found with the test of fix 85: a `goto` out of the initializer of a cleanup
    variable ran that variable's cleanup (it was registered before the initializer was
    parsed); gcc does not, the variable's lifetime has not begun.
    Fix: the cleanup is registered after the initializer.
    Verification: the destructor-order test matches gcc on every path.

87. 2026-10-23, COMMITTED, `i386-gen.c` (gen_bitscan, gen_bitscan64), `tccgen.c`, `tcc.h`.
    Problem: `__builtin_clzll` (seq_file.c, lib/math/div64.c, bitfield.h) and the other
    bit-scan builtins were library calls on i386 (fix 47 inlined them for x86_64 only).
    Fix: `bsr`/`bsf` inline for 32-bit values (the x86_64 sequence) and, for 64-bit values on
    i386, the halves in %edx:%eax with a scan of the second half when the first is zero
    (ffs: 0 for 0); with -m16 the prefixes.
    Verification: clz/ctz/ffs of 7 32-bit and 10 64-bit values (and ffs(0)) against gcc,
    absolute and PIE; `make test`.

88. 2026-10-23, COMMITTED, `i386-gen.c` (gfunc_call). A bug in the regparm code of fix 62/65.
    Problem: `entry->read(entry, &out)` under -mregparm=3 (cpuid_parser.c, the first use of
    cpuid in the 32-bit kernel) called address 0: the function pointer's address stayed in
    %eax while the arguments were popped into %eax/%edx, and was dereferenced afterwards.
    gfunc_call's stack-slot path for such calls rotated the wrong way (vrott brings the first
    argument to the top, not the function) and spilled the argument instead.
    Fix: vrotb/vrott swapped: the function value is loaded and spilled to a stack slot before
    the arguments, and the call goes through `call *disp(%ebp)`.
    Verification: indirect regparm calls through struct fields, array elements, globals and
    locals give the right results; the kernel boots.

89. 2026-10-23, COMMITTED, `i386-gen.c` (gen_struct_fill). A bug in fix 67's i386 fill.
    Problem: __init_waitqueue_head() wrote the spinlock through a NULL pointer: `*lock =
    __SPIN_LOCK_UNLOCKED(lock)` zero-fills a compound literal with `rep stosb` and copies it;
    the fill moved the value into %eax behind the register allocator's back and clobbered
    the destination pointer of the copy that the allocator held in %eax.
    Fix: the value is allocated to %eax by gv2(RC_INT, RC_EAX), like the x86_64 version's
    RC_RAX, which spills whatever else lives there.
    Verification: struct assignments from zero-initialized compound literals (4 and 41 bytes,
    with regparm and PIE) give the expected memory contents; the kernel boots.

90. 2026-10-23, COMMITTED, `i386-asm.c` (constraints 't'/'u', ASM_ST0/ASM_ST1), `i386-gen.c`
    (NB_ASM_REGS 10).
    Problem: musl's i386 math (`fabs`: `"+t"(x)`, `fmod`: `"+t"(x) : "u"(y)`) uses the x87
    constraints, which fix 52 implemented for x86_64 only, and for long double only.
    Fix: on i386 st(0)/st(1) are asm register numbers 8 and 9 (TREG_ST0 is 4, the asm number
    of %esp), mapped to TREG_ST0 for the loads and stores, and any floating type is accepted
    (the x87 is the float unit there); the "st"/"st(0)"/"st(1)" clobbers too.
    Verification: fabs/fabsl/fmod/fmodl/fsqrt through these constraints give the right
    results; musl's math builds.

91. 2026-10-23, COMMITTED, `i386-asm.c` (asm_opcode, segment prefix).
    Problem: musl's i386 system call is `call *%gs:16` (the vDSO pointer in the TLS). The `*`
    was parsed with the segment register and lost when the memory operand was re-parsed
    after the `:`, so tcc assembled a direct `gs call 16` (a PC32 relocation against the
    absolute symbol, which tcc's own linker then rejected as an invalid relocation entry in
    libc.a).
    Fix: the indirection flag survives the re-parse.
    Verification: `call *%gs:16`, `call *%gs:(%eax)`, `jmp *%gs:0x10`, `call *16`, `call *foo`
    byte-identical to GNU as; musl's hello program links and runs.

92. 2026-10-23, COMMITTED, `i386-asm.c` (asm_compute_constraints: the 'm' case and the copy).
    Problem: busybox's i386 comba multiply (tls_pstm_mul_comba.c, MULADD) has three "=r"
    outputs, "%eax"/"%edx" clobbers and two "m" inputs reached through pointers
    (`*tmpx++`, `*tmpy--`): tcc needs a register for the address of such an operand, and
    with eight general registers none was left ("asm constraint 7 ('m') could not be
    satisfied"); gcc addresses the operands with whatever it has.
    Fix: an "m" input of scalar integer or pointer type that gets no register is copied to a
    stack temporary before the asm, and the temporary is the operand (`disp(%ebp)`): for an
    input that is invisible. The copy is emitted at the end of asm_compute_constraints, before
    the operand text is substituted; every register is free there (save_regs(0) opens the asm
    statement), %eax is the scratch register; long long values are copied in two halves.
    Verification: the MULADD loop over three 0xffffffff digits gives the 96-bit result of a
    64-bit reference computation (3 fffffffa 2); busybox builds; `make test`.

93. 2026-10-23, COMMITTED, `i386-link.c` (gotplt_entry_type, relocate).
    Problem: tcc's own linker (the musl-tcc wrapper links busybox with `i386-tcc`) stopped at
    musl's sigsetjmp.o: "Unknown relocation type for got: 23". R_386_PC8 comes from `jecxz
    1f` (a rel8-only jump) and the other 8-/16-bit relocations from the assembler work of fix
    63; the link tables only knew R_386_16/PC16, and applied those only to 16-bit binary
    output.
    Fix: R_386_8/PC8 classified like 16/PC16, and all four applied for any output format,
    with GNU ld's overflow check.
    Verification: a program with a forward `jecxz` in inline asm linked and run by tcc
    itself; busybox links; `make test`.

tld (tools/tld, 2026-10-23, for the i386 target): `ld` applies R_386_GOTOFF and R_386_GOTPC
against a linker-defined `_GLOBAL_OFFSET_TABLE_` (absolute 0 unless the script defines it;
no GOT is built) and refuses R_386_32 against a defined symbol in a PIE like the x86_64
check. `objcopy` recomputes a PT_LOAD's p_filesz/p_memsz from the sections it still holds
(GNU objcopy behaviour): the kernel strips `.modinfo` out of vmlinux.unstripped, an alloc
section the linker script places after .bss/.brk, and while it is there the data segment's
file size spans the NOBITS sections (GNU ld lays it out the same way); with the headers
copied unchanged the 32-bit decompressor's parse_elf() copied 2.8 MB of "file" over its own
relocated code and stack. Both builds had filesz == memsz; the x86_64 decompressor sits
higher in memory and survived.

Kernel-side changes (separate from tcc, kept to a minimum; repo /home/nika/Documents/linux):
K14. 2026-10-23, COMMITTED (upstreamable), `init/Kconfig` (CC_IS_TCC), `lib/Kconfig`,
    `lib/math/Makefile`, `lib/math/divdi3.c`, `arch/x86/Kconfig`: GENERIC_LIB_DIVDI3 provides
    `__divdi3`, `__moddi3`, `__udivdi3` and `__umoddi3` on top of lib/math/div64.c, selected
    for X86_32 when the compiler is tcc (CC_IS_TCC: `$(CC) -v` says "tcc version"). gcc
    strength-reduces every 64-bit division by a constant on 32-bit x86, so no gcc build links
    them; tcc emits the calls (`/ 8`, `/ 100`, ... in a dozen objects of mm, fs and sched).
K15. 2026-10-23, COMMITTED (upstreamable, semantics-preserving), `include/linux/swiotlb.h`: the
    three swiotlb_*() wrappers test `IS_ENABLED(CONFIG_SWIOTLB) && unlikely(pool)`: without
    SWIOTLB (32-bit x86 does not select it) swiotlb_find_pool() is a stub returning NULL and
    the __swiotlb_*() functions are not built; a compiler that does not propagate the NULL
    through the local keeps the calls (undefined references from kernel/dma/mapping.o).
K16. 2026-10-23, COMMITTED (upstreamable), `arch/x86/include/asm/pgtable.h`:
    __HAVE_ARCH_PMDP_TEST_AND_CLEAR_YOUNG and the declaration of pmdp_test_and_clear_young()
    only under the conditions pgtable.c defines it (THP or ARCH_HAS_NONLEAF_PMD_YOUNG),
    pudp_test_and_clear_young() only with THP; otherwise the generic stub applies. On 32-bit
    x86 (two-level page tables, no THP) the arch declared a function it never defined, and
    clear_refs_pte_range()'s dead THP branch referenced it.
K2. 2026-10-20, APPLIED (uncommitted), `mm/slab.h`: `typedef u128 freelist_full_t;` now under
    `#ifdef __SIZEOF_INT128__`, else a 16-byte `struct { u64 lo, hi; }` (only its size is used
    when system_has_cmpxchg128 is undefined).
K3. 2026-10-20, APPLIED (uncommitted), `arch/x86/include/asm/asm.h`: under `#ifdef __TINYC__`,
    rip_rel_ptr() is a statement-expression macro so the "i" operand is the caller's constant
    (tcc does not inline __always_inline functions).
K4. 2026-10-20, APPLIED (uncommitted), `arch/x86/include/asm/cpufeature.h`: under
    `#ifdef __TINYC__`, `_static_cpu_has(bit)` is `boot_cpu_has(bit)` (dynamic test) because
    __static_cpu_has()'s asm goto needs "i" immediates that only exist after inlining.
K10. 2026-10-22, COMMITTED (upstreamable), `Makefile`, `scripts/Makefile.lib`: a make variable
    `OBJTOOL=<path>` names the objtool to run instead of building tools/objtool (which needs
    libelf). `kernels/linux/objtool-stub` (exit 0) is used: with UNWINDER_GUESS, no jump
    labels, no mitigations and no IBT objtool only validates and records static-call sites;
    without `.static_call_sites` the kernel keeps calling through the trampolines.
K11. 2026-10-22, COMMITTED (upstreamable), `arch/x86/Kconfig.assembler`, `lib/crypto/Makefile`,
    `lib/crypto/x86/{sha256.h,blake2s.h,blake2s-core.S}`: the CONFIG_AS_AVX2 / CONFIG_AS_AVX512
    probes (removed in v5.8) are back and gate sha256-avx-asm.S, sha256-avx2-asm.S and the
    AVX-512 BLAKE2s function; tcc's assembler has no VEX/EVEX encodings, the glue falls back
    to the SSSE3 and generic code. With binutils nothing changes.
K12. 2026-10-22, COMMITTED (upstreamable), `arch/x86/Kconfig`, `arch/x86/boot/Makefile`,
    `arch/x86/boot/video.c`: `X86_BOOT_VIDEO_MODES` (default y, hidden unless EXPERT) builds the
    BIOS video mode setting (vga=, the menu, the VGA/VESA/BIOS drivers, EDID); without it
    set_video() only records the firmware's text mode. tcc's 16-bit code is about twice the
    size of gcc -Os's (39 488 vs 19 399 bytes of setup) and the setup must fit in 32 KB; with
    the option off the tcc setup is 27 328 bytes. `kernels/linux/tcc.config` sets it off:
    `vga=` has no effect on the direct boot (VGA text console); the ISO's framebuffer comes
    from GRUB's gfxpayload and is unaffected.
K13. 2026-10-22, COMMITTED (upstreamable), `include/linux/gfp.h`: `vma_thp_gfp_mask()` gets an
    inline stub returning 0 without CONFIG_TRANSPARENT_HUGEPAGE (it is defined in
    mm/huge_memory.c, THP-only, but declared unconditionally). With CONFIG_SHMEM/TMPFS
    (the self-hosting image's build directory) shmem_get_folio_gfp() calls it under
    `if (orders > 0)` where orders is the 0 of an inline helper; tcc keeps the call and the
    link failed with an undefined reference.
K1. 2026-10-20, APPLIED (uncommitted), `arch/x86/include/asm/cmpxchg_64.h`: wrapped the
    128-bit section (union __u128_halves .. system_has_cmpxchg128) in `#ifdef __SIZEOF_INT128__`,
    because <linux/types.h> only defines u128 under that macro and tcc has no __int128.
    Kconfig already reports CC_HAS_INT128=n for tcc, so nothing else uses 128-bit arithmetic.
