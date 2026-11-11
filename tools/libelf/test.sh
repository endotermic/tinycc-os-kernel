#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# test.sh - compare the project's objtool (tcc + tools/libelf, linked by tld) with a reference
# objtool built by gcc against the host's elfutils libelf, over every object of a kernel build
# tree that Kbuild runs objtool on: each object is compiled afresh with the command its .cmd
# file recorded (objtool rewrites objects in place, so the ones in the tree are already
# processed), then both objtools run on a copy of it, with Kbuild's options; exit codes,
# diagnostics and the written files must be identical.
#
# Usage: [BUILD=<kernel build dir>] [REF=<reference objtool>] sh test.sh [N]
#   N: test only the first N objects (sorted by path); default: all
# Needs: the build tree (default ../../build/x86_64/linux: the .*.o.cmd files and
# tools/objtool/objtool, the one the kernel step built with tcc-hostcc), the kernel source tree
# (SRCTREE; default: ../../src/linux), the host tcc of the repository (TCC; default
# ../../src/tinycc/tcc) and, for the reference, gcc with libelf-dev (built into the work
# directory unless REF names one).
# Env: BUILD, REF, SRCTREE, TCC, OBJTOOL (the objtool under test; default $BUILD/tools/objtool/objtool),
#      W (work directory; default: a fresh mktemp -d /tmp/libelf-test.XXXXXX), KEEP=1 keeps it
case ${1:-} in -h|--help) sed -n '3,/^[^#]/p' "$0" | sed '$d' | cut -c3-; exit 0;; esac
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-$HERE/../../build/x86_64/linux}
BUILD=$(cd "$BUILD" && pwd) || exit 1
SRCTREE=${SRCTREE:-$HERE/../../src/linux}
SRCTREE=$(cd "$SRCTREE" && pwd) || exit 1
TCC=${TCC:-$HERE/../../src/tinycc/tcc}
[ -x "$TCC" ] || { echo "no host tcc $TCC (scripts/build.sh host-tcc)" >&2; exit 1; }
export TCC
KCC_TCC=${KCC_TCC:-$TCC}; export KCC_TCC   # the target compiler tcc-kcc runs (i386: the cross compiler)
OBJTOOL=${OBJTOOL:-$BUILD/tools/objtool/objtool}
[ -x "$OBJTOOL" ] || { echo "no $OBJTOOL (scripts/build.sh kernel builds it)" >&2; exit 1; }
W=${W:-$(mktemp -d /tmp/libelf-test.XXXXXX)}; mkdir -p "$W"
LIMIT=${1:-0}
if [ -z "$REF" ]; then
    REF=$W/ref/tools/objtool/objtool
    echo "building the reference objtool with gcc and the host libelf into $W/ref"
    mkdir -p "$W/ref/tools"
    make -s -C "$SRCTREE/tools" O="$W/ref" subdir=tools objtool HOSTCC=gcc >"$W/ref.log" 2>&1 \
        || { echo "the reference build failed (gcc, libelf-dev?): $W/ref.log" >&2; exit 1; }
fi
[ -x "$REF" ] || { echo "no reference objtool $REF" >&2; exit 1; }
n=0; same=0; diffbytes=0; diffexit=0; differr=0; warned=0; nocompile=0
: > "$W/fail.txt"; : > "$W/warnings.txt"
cd "$BUILD" || exit 1
for cmd in $(find . -name '.*.o.cmd' | xargs grep -l "tools/objtool/objtool " | sort); do
    [ "$LIMIT" -gt 0 ] && [ $n -ge "$LIMIT" ] && break
    # "savedcmd_<obj> := <compile command> ; <objtool> <options> <obj>"
    line=$(grep -m1 "^savedcmd_" "$cmd") || continue
    compile=${line#*:= }; compile=${compile%% ; *}
    objline=${line##* ; }
    args=$(echo "$objline" | sed 's,^[^ ]*tools/objtool/objtool ,,; s, *[^ ]*$,,')
    d=$(dirname "$cmd"); f=$(basename "$cmd"); obj=$d/${f#.}; obj=${obj%.cmd}
    n=$((n+1))
    # the same compile into the work directory (the wrappers read TCC; KCC_TCC as the kernel step set it)
    rm -f "$W/x.o"
    if ! (eval "$(echo "$compile" | sed "s| -o [^ ]* | -o $W/x.o |")") >"$W/cc.err" 2>&1 || [ ! -f "$W/x.o" ]; then
        nocompile=$((nocompile+1)); echo "COMPILE $obj: $(head -1 "$W/cc.err")" >> "$W/fail.txt"; continue
    fi
    cp "$W/x.o" "$W/ref.o"; cp "$W/x.o" "$W/tcc.o"
    "$REF" $args "$W/ref.o" > "$W/ref.err" 2>&1; e1=$?
    "$OBJTOOL" $args "$W/tcc.o" > "$W/tcc.err" 2>&1; e2=$?
    sed -i 's,ref\.o,X.o,g' "$W/ref.err"; sed -i 's,tcc\.o,X.o,g' "$W/tcc.err"
    ok=1
    [ "$e1" = "$e2" ] || { ok=0; diffexit=$((diffexit+1)); echo "EXIT $obj: reference $e1, tcc $e2" >> "$W/fail.txt"; }
    cmp -s "$W/ref.err" "$W/tcc.err" || { ok=0; differr=$((differr+1)); echo "DIAG $obj" >> "$W/fail.txt"; diff "$W/ref.err" "$W/tcc.err" | head -6 >> "$W/fail.txt"; }
    cmp -s "$W/ref.o" "$W/tcc.o" || { ok=0; diffbytes=$((diffbytes+1)); echo "BYTES $obj: $(cmp "$W/ref.o" "$W/tcc.o" 2>&1 | head -1)" >> "$W/fail.txt"; }
    [ $ok = 1 ] && same=$((same+1))
    [ -s "$W/ref.err" ] && { warned=$((warned+1)); { echo "== $obj"; cat "$W/ref.err"; } >> "$W/warnings.txt"; }
done
echo "objects $n, identical $same, with warnings $warned; differing: bytes $diffbytes, exit codes $diffexit, diagnostics $differr; not compiled $nocompile"
if [ $same = $n ] && [ $n -gt 0 ]; then
    echo "PASS"; [ -n "$KEEP" ] || rm -rf "$W"; exit 0
fi
echo "FAIL (see $W/fail.txt)"; head -20 "$W/fail.txt"; exit 1
