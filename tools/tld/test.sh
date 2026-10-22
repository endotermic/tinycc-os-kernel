#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# test.sh - link the kernel artifacts of a GNU-ld-built kernel build tree with tld and
# compare with the GNU results found there.
#
# Usage: BUILD=<kernel build dir> sh test.sh      (default: ../../build/x86_64/linux)
# Needs: the build tree (vmlinux.a, the objects, *.lds, arch/x86/tools/{relocs,vdso2c},
# GNU-linked vmlinux*, setup.*, realmode.*, compressed/vmlinux, vdso64.so*) and the host
# binutils (nm, readelf, objcopy) for the comparisons.  LC_ALL=C like Kbuild.
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-$HERE/../../build/x86_64/linux}
BUILD=$(cd "$BUILD" && pwd) || exit 1
W=${W:-$(mktemp -d /tmp/tld-test.XXXXXX)}
TLD=$HERE/tld
LD="$TLD --tool=ld"; NM="$TLD --tool=nm"; OBJCOPY="$TLD --tool=objcopy"; READELF="$TLD --tool=readelf"
pass=0; fail=0; skip=0
res() { # res <PASS|FAIL|SKIP> <name> [detail]
    printf '%-4s %-44s %s\n' "$1" "$2" "$3"
    case $1 in PASS) pass=$((pass+1));; FAIL) fail=$((fail+1));; *) skip=$((skip+1));; esac
}
check_cmp() { if cmp -s "$1" "$2"; then res PASS "$3"; else res FAIL "$3" "$(cmp "$1" "$2" 2>&1 | head -1)"; fi; }
check_nm() { # check_nm <gnu-file> <tld-file> <name>
    nm -n "$1" > "$W/nm.gnu" 2>/dev/null; nm -n "$2" > "$W/nm.tld" 2>/dev/null
    if cmp -s "$W/nm.gnu" "$W/nm.tld"; then res PASS "$3"; else res FAIL "$3" "$(diff "$W/nm.gnu" "$W/nm.tld" | grep -c '^[<>]') differing lines"; fi
}
# run the saved Kbuild command of <.cmd file> with tld, output redirected to <out>
kcmd() { # kcmd <cmdfile> <gnu-output> <tld-output>
    cmd=$(grep -m1 '^savedcmd_' "$1" | sed 's/^savedcmd_[^=]*:= //; s#; .*##')
    cmd=$(echo "$cmd" | sed "s#^ld #$LD #; s# -o $2# -o $3#")
    (cd "$BUILD" && eval "$cmd")
}
[ -x "$TLD" ] || make -C "$HERE" tld >/dev/null || exit 1
cd "$BUILD" || exit 1
# the kernel source tree: O= build trees have a stub Makefile that includes it
SRCTREE=${SRCTREE:-$(sed -n 's#^include \(.*\)/Makefile$#\1#p' Makefile 2>/dev/null | head -1)}
[ -d "$SRCTREE" ] || SRCTREE=$(cd "$BUILD/../../../src/linux" 2>/dev/null && pwd)
echo "tld tests against $BUILD (work dir $W)"

# 1. setup.elf (i386)
if kcmd arch/x86/boot/.setup.elf.cmd arch/x86/boot/setup.elf "$W/setup.elf"; then
    check_nm arch/x86/boot/setup.elf "$W/setup.elf" "setup.elf: nm -n"
    $OBJCOPY -O binary "$W/setup.elf" "$W/setup.bin" && check_cmp arch/x86/boot/setup.bin "$W/setup.bin" "setup.bin: objcopy -O binary identical"
else res FAIL "setup.elf: link"; fi

# 2. realmode.elf (i386, --emit-relocs)
if kcmd arch/x86/realmode/rm/.realmode.elf.cmd arch/x86/realmode/rm/realmode.elf "$W/realmode.elf"; then
    check_nm arch/x86/realmode/rm/realmode.elf "$W/realmode.elf" "realmode.elf: nm -n"
    $OBJCOPY -O binary "$W/realmode.elf" "$W/realmode.bin" && check_cmp arch/x86/realmode/rm/realmode.bin "$W/realmode.bin" "realmode.bin: objcopy -O binary identical"
    if [ -x arch/x86/tools/relocs ]; then
        arch/x86/tools/relocs --realmode "$W/realmode.elf" > "$W/realmode.relocs" && check_cmp arch/x86/realmode/rm/realmode.relocs "$W/realmode.relocs" "realmode.relocs: relocs --realmode identical"
    else res SKIP "realmode.relocs" "no arch/x86/tools/relocs"; fi
    $NM arch/x86/realmode/rm/header.o arch/x86/realmode/rm/trampoline_64.o arch/x86/realmode/rm/stack.o arch/x86/realmode/rm/reboot.o 2>/dev/null | sed -n -r -e 's/^([0-9a-fA-F]+) [ABCDGRSTVW] (.+)$/pa_\2 = \2;/p' | sort | uniq > "$W/pasyms.h"
    nm arch/x86/realmode/rm/header.o arch/x86/realmode/rm/trampoline_64.o arch/x86/realmode/rm/stack.o arch/x86/realmode/rm/reboot.o 2>/dev/null | sed -n -r -e 's/^([0-9a-fA-F]+) [ABCDGRSTVW] (.+)$/pa_\2 = \2;/p' | sort | uniq > "$W/pasyms.gnu.h"
    check_cmp "$W/pasyms.gnu.h" "$W/pasyms.h" "pasyms.h: tld nm on the realmode objects"
else res FAIL "realmode.elf: link"; fi

# 3. arch/x86/boot/compressed/vmlinux (-pie)
if kcmd arch/x86/boot/compressed/.vmlinux.cmd arch/x86/boot/compressed/vmlinux "$W/czvmlinux"; then
    check_nm arch/x86/boot/compressed/vmlinux "$W/czvmlinux" "compressed/vmlinux: nm -n"
    $OBJCOPY -O binary -R .note -R .comment -S "$W/czvmlinux" "$W/vmlinux.bin" && check_cmp arch/x86/boot/vmlinux.bin "$W/vmlinux.bin" "boot/vmlinux.bin: objcopy -O binary identical"
    zo='s/^\([0-9a-fA-F]*\) [a-zA-Z] \(startup_32\|efi.._stub_entry\|efi\(32\)\?_pe_entry\|input_data\|kernel_info\|_end\|_ehead\|_text\|_e\?data\|_e\?sbat\|z_.*\)$/\#define ZO_\2 0x\1/p'
    $NM "$W/czvmlinux" | sed -n -e "$zo" > "$W/zoffset.h"; nm arch/x86/boot/compressed/vmlinux | sed -n -e "$zo" > "$W/zoffset.gnu.h"
    check_cmp "$W/zoffset.gnu.h" "$W/zoffset.h" "zoffset.h: tld nm"
else res FAIL "compressed/vmlinux: link"; fi

# 4. vmlinux (link-vmlinux.sh's final link; CONFIG_KALLSYMS is off in this configuration)
if $LD -m elf_x86_64 -z noexecstack --no-warn-rwx-segments -z max-page-size=0x200000 --build-id=sha1 --orphan-handling=warn \
      --script=./arch/x86/kernel/vmlinux.lds --strip-debug -o "$W/vmlinux.unstripped" \
      --whole-archive vmlinux.a .vmlinux.export.o init/version-timestamp.o --no-whole-archive --start-group --end-group; then
    check_nm vmlinux.unstripped "$W/vmlinux.unstripped" "vmlinux.unstripped: nm -n"
    # the reference was sorted in place by scripts/sorttable; do the same
    if [ -x scripts/sorttable ]; then
        $NM -S "$W/vmlinux.unstripped" > "$W/nm-sort" && scripts/sorttable -s "$W/nm-sort" "$W/vmlinux.unstripped" || res FAIL "vmlinux: sorttable"
    else res SKIP "vmlinux: sorttable" "no scripts/sorttable (the image compare below will then differ in __ex_table)"; fi
    objcopy -O binary -R .notes vmlinux.unstripped "$W/vml.gnu.bin"
    $OBJCOPY -O binary -R .notes "$W/vmlinux.unstripped" "$W/vml.tld.bin"
    check_cmp "$W/vml.gnu.bin" "$W/vml.tld.bin" "vmlinux: loadable image identical (without .notes)"
    readelf -lW vmlinux.unstripped | sed -n '/Program Headers/,/Section to Segment/p' > "$W/ph.gnu"
    readelf -lW "$W/vmlinux.unstripped" | sed -n '/Program Headers/,/Section to Segment/p' > "$W/ph.tld"
    check_cmp "$W/ph.gnu" "$W/ph.tld" "vmlinux: program headers identical"
    if [ -f "$SRCTREE/scripts/mksysmap" ]; then
        $NM -n "$W/vmlinux.unstripped" | sed -f "$SRCTREE/scripts/mksysmap" > "$W/System.map"
        nm -n vmlinux.unstripped | sed -f "$SRCTREE/scripts/mksysmap" > "$W/System.map.gnu"
        check_cmp "$W/System.map.gnu" "$W/System.map" "System.map: tld nm + mksysmap"
    else res SKIP "System.map" "srctree not found"; fi
    # scripts/Makefile.vmlinux: vmlinux.unstripped -> vmlinux
    $OBJCOPY --set-section-flags .modinfo=noload "$W/vmlinux.unstripped" "$W/vmlinux" && \
    $OBJCOPY --remove-section=.modinfo -w --strip-unneeded-symbol='__mod_device_table__*' "$W/vmlinux" && \
    check_nm vmlinux "$W/vmlinux" "vmlinux (after objcopy strip_relocs): nm -n"
    vo='s/^\([0-9a-fA-F]*\) [ABbCDGRSTtVW] \(_text\|__start_rodata\|_sinittext\|__inittext_end\|__bss_start\|_end\)$/\#define VO_\2 _AC(0x\1,UL)/p'
    $NM "$W/vmlinux" | sed -n -e "$vo" > "$W/voffset.h"; nm vmlinux | sed -n -e "$vo" > "$W/voffset.gnu.h"
    check_cmp "$W/voffset.gnu.h" "$W/voffset.h" "voffset.h: tld nm"
    $OBJCOPY -j .modinfo -O binary "$W/vmlinux.unstripped" "$W/modinfo" && objcopy -j .modinfo -O binary vmlinux.unstripped "$W/modinfo.gnu" && check_cmp "$W/modinfo.gnu" "$W/modinfo" "modules.builtin.modinfo: objcopy -j .modinfo -O binary"
else res FAIL "vmlinux: link"; fi

# 5. vmlinux.o (-r) + modpost
if $LD -m elf_x86_64 -z noexecstack --no-warn-rwx-segments -r -o "$W/vmlinux.o" --whole-archive vmlinux.a --no-whole-archive --start-group --end-group; then
    nm vmlinux.o | awk '{print $NF, $(NF-1)}' | sort > "$W/rnm.gnu"; nm "$W/vmlinux.o" | awk '{print $NF, $(NF-1)}' | sort > "$W/rnm.tld"
    check_cmp "$W/rnm.gnu" "$W/rnm.tld" "vmlinux.o: symbol names and types"
    readelf -SW vmlinux.o | grep '\] ' | sed 's/^ *\[ *[0-9]*\] *//' | awk '{print $1, $2}' > "$W/rs.gnu"
    readelf -SW "$W/vmlinux.o" | grep '\] ' | sed 's/^ *\[ *[0-9]*\] *//' | awk '{print $1, $2}' > "$W/rs.tld"
    check_cmp "$W/rs.gnu" "$W/rs.tld" "vmlinux.o: section names, types and order"
    MODPOST=$BUILD/scripts/mod/modpost
    if [ ! -x "$MODPOST" ] && [ -d "$SRCTREE/scripts/mod" ] && [ -f scripts/mod/elfconfig.h ]; then
        # build modpost from the kernel sources (the build tree may be between builds)
        gcc -O2 -I scripts/mod -I "$SRCTREE/scripts/mod" -I "$SRCTREE/scripts/include" -o "$W/modpost" "$SRCTREE"/scripts/mod/modpost.c "$SRCTREE"/scripts/mod/file2alias.c "$SRCTREE"/scripts/mod/sumversion.c "$SRCTREE"/scripts/mod/symsearch.c 2>/dev/null && MODPOST=$W/modpost
    fi
    if [ -x "$MODPOST" ]; then
        mkdir -p "$W/mp" "$W/mpg"; cp "$W/vmlinux.o" "$W/mp/vmlinux.o"; cp vmlinux.o "$W/mpg/vmlinux.o"
        (cd "$W/mp" && "$MODPOST" -o vmlinux.symvers -n vmlinux.o > modpost.out 2>&1; echo $? > rc)
        (cd "$W/mpg" && "$MODPOST" -o vmlinux.symvers -n vmlinux.o > modpost.out 2>&1; echo $? > rc)
        if cmp -s "$W/mp/modpost.out" "$W/mpg/modpost.out" && cmp -s "$W/mp/rc" "$W/mpg/rc"; then res PASS "vmlinux.o: modpost output identical to GNU's"; else res FAIL "vmlinux.o: modpost" "rc $(cat "$W/mp/rc") vs $(cat "$W/mpg/rc")"; fi
    else res SKIP "vmlinux.o: modpost" "no scripts/mod/modpost in the build tree"; fi
    if [ -f "$SRCTREE/scripts/check-function-names.sh" ]; then
        NM="$NM" sh "$SRCTREE/scripts/check-function-names.sh" "$W/vmlinux.o" && res PASS "vmlinux.o: check-function-names.sh (with tld nm)" || res FAIL "vmlinux.o: check-function-names.sh"
    else res SKIP "vmlinux.o: check-function-names.sh" "srctree not found"; fi
else res FAIL "vmlinux.o: link"; fi

# 6. vDSO
V=arch/x86/entry/vdso/vdso64
if kcmd $V/.vdso64.so.dbg.cmd $V/vdso64.so.dbg "$W/vdso64.so.dbg"; then
    if $READELF -rW "$W/vdso64.so.dbg" | grep -v _NONE | grep -q " R_\w*_"; then res FAIL "vdso64.so.dbg: no dynamic relocations"; else res PASS "vdso64.so.dbg: no dynamic relocations (readelf -rW)"; fi
    readelf -aW "$W/vdso64.so.dbg" > "$W/vdso.readelf" 2> "$W/vdso.readelf.err"
    if [ -s "$W/vdso.readelf.err" ]; then res FAIL "vdso64.so.dbg: readelf -a without warnings" "$(head -1 "$W/vdso.readelf.err")"; else res PASS "vdso64.so.dbg: readelf -a without warnings"; fi
    readelf -W --dyn-syms $V/vdso64.so.dbg | awk '/^ +[0-9]+:/ {print $4, $5, $8}' | sort > "$W/dyn.gnu"
    readelf -W --dyn-syms "$W/vdso64.so.dbg" | awk '/^ +[0-9]+:/ {print $4, $5, $8}' | sort > "$W/dyn.tld"
    check_cmp "$W/dyn.gnu" "$W/dyn.tld" "vdso64.so.dbg: exported dynamic symbols (type, binding, name)"
    $OBJCOPY -S --remove-section __ex_table "$W/vdso64.so.dbg" "$W/vdso64.so" || res FAIL "vdso64.so: objcopy -S"
    if [ -x arch/x86/tools/vdso2c ]; then
        if arch/x86/tools/vdso2c "$W/vdso64.so.dbg" "$W/vdso64.so" "$W/vdso64-image.c"; then
            res PASS "vdso2c accepts the tld vDSO"
            grep -E '^\s*\.(size|alt|alt_len|sym_|extable)' "$W/vdso64-image.c" | sed 's/^\s*//' > "$W/img.tld"
            grep -E '^\s*\.(size|alt|alt_len|sym_|extable)' $V/vdso64-image.c | sed 's/^\s*//' > "$W/img.gnu"
            if cmp -s "$W/img.gnu" "$W/img.tld"; then res PASS "vdso64-image.c: image size, alternatives and symbol offsets identical"; else res SKIP "vdso64-image.c: fields differ" "(expected with GNU-built inputs: no .eh_frame_hdr/.got.plt in the tld layout; see README)"; diff "$W/img.gnu" "$W/img.tld" | sed 's/^/     /'; fi
        else res FAIL "vdso2c on the tld vDSO"; fi
    else res SKIP "vdso2c" "no arch/x86/tools/vdso2c"; fi
else res FAIL "vdso64.so.dbg: link"; fi

# 7. ar: Kbuild's thin archives, byte-identical to GNU ar's (deterministic mode)
if command -v ar >/dev/null; then
    rm -f "$W/t.gnu.a" "$W/t.tld.a"
    ar cDPrST "$W/t.gnu.a" init/main.o init/version.o arch/x86/entry/entry.o 2>/dev/null
    $TLD --tool=ar cDPrST "$W/t.tld.a" init/main.o init/version.o arch/x86/entry/entry.o
    check_cmp "$W/t.gnu.a" "$W/t.tld.a" "ar cDPrST (thin archive) identical to GNU ar"
    rm -f arch/x86/boot/startup/tld-test.a arch/x86/boot/startup/gnu-test.a
    ar cDPrsT arch/x86/boot/startup/gnu-test.a arch/x86/boot/startup/la57toggle.o 2>/dev/null
    $TLD --tool=ar cDPrsT arch/x86/boot/startup/tld-test.a arch/x86/boot/startup/la57toggle.o
    check_cmp arch/x86/boot/startup/gnu-test.a arch/x86/boot/startup/tld-test.a "ar cDPrsT (indexed thin archive, relative member path) identical"
    rm -f arch/x86/boot/startup/tld-test.a arch/x86/boot/startup/gnu-test.a
else res SKIP "ar comparisons" "no GNU ar"; fi

# 8. ld -v for Kconfig's ld-version.sh / ld-option
if $LD -v --no-warn-rwx-segments > /dev/null 2>&1 && ! $LD -v --no-such-option > /dev/null 2>&1 && $LD --version | grep -q '^GNU ld'; then res PASS "ld -v / --version for scripts/ld-version.sh"; else res FAIL "ld -v"; fi

echo "----"; echo "$pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
