#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# build.sh - build the pinned, patched sources (src/) into a kernel + initramfs images
# (+ ISO) for one architecture.  Every step is skipped when its result exists.
#
# Usage: scripts/build.sh [-n|--dry-run] [step...]        default: all
#   host-tcc    tinycc in src/tinycc (in-tree build; this compiler builds everything else)
#   tcc-test    run tinycc's own test suite (must print ALL TESTS PASSED)
#   tcc-archive install the host tcc into a staging tree and pack it as out/<arch>/tcc-*.tar.gz
#   tld         tools/tld (ld, nm, objcopy, readelf, ar, strip replacement) compiled by the host tcc
#   kernel      Linux: tinyconfig + kernels/linux/*.config, CC=kernels/linux/tcc-kcc, LD=tld
#   musl        musl libc, static, compiled by tcc (no src/complex: tcc has no _Complex)
#   busybox     static busybox compiled and linked by tcc against that musl
#   guest-tcc   tcc itself (+ i386-tcc), static, musl, for the initramfs (/usr/bin/tcc in the guest)
#   guest-make  GNU make, static, musl, compiled by the host tcc (the guest's make for self-hosting)
#   initramfs   the libc-free init + the two cpio images -> out/<arch>/
#   selfhost    the self-hosting image out/<arch>/initramfs-selfhost.cpio: the busybox image plus
#               the pruned kernel source, the tcc, tld and make sources, i386-tcc, tld, make, the
#               Kbuild wrappers, pre-generated Kconfig lexer/parser, and build-kernel /
#               build-toolchain / send-file (userland/selfhost/); needs a complete kernel build
#   iso         hybrid BIOS/UEFI GRUB ISO -> out/<arch>/ (boot/mkiso.sh; with the self-hosting
#               image's menu entries when it exists)
#   all         host-tcc tld kernel musl busybox guest-tcc initramfs
#   clean       remove build/<arch>/ and out/<arch>/ (the build trees, logs and results of ARCH);
#               src/ stays, and with it the host tcc, which is built in-tree in src/tinycc
#   clean-all   the same for every architecture: build/ and out/ as a whole
#   clean-src   remove the fetched component trees src/<name> and src/.downloads (scripts/fetch.sh +
#               scripts/patch.sh recreate them: a 2 GB download); refuses while a tree has
#               uncommitted changes, a stash, or more commits on top of 'base' than
#               patches/<name>/series lists (fixes are committed there and exported with
#               scripts/export-patches.sh before the tree may go)
# Every clean step prints what goes with its size (du -sh); -n/--dry-run (or DRY_RUN=1) prints
# that and removes nothing.  The whole command line is checked before the first step runs: an
# unknown step or option aborts with nothing done.
# Env: ARCH (x86_64, i386; arm64 is not implemented, see install.sh), JOBS, SRC/BUILD/OUT, DRY_RUN.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/lib.sh"
ARCH=${ARCH:-x86_64}
JOBS=${JOBS:-$(nproc)}
# KARCH: the kernel's ARCH= (i386 selects the 32-bit kernel); TCC_TARGET: the compiler of the
# target code (the i386 cross compiler built next to the host tcc for i386: "tcc -m32" would
# exec it anyway); LIBTCC1: its runtime archive, for the musl-tcc wrapper; TCC_CPU: tcc's
# configure --cpu for the guest compiler; the self-hosting image of the i386 guest needs no
# i386 cross compiler (tcc there is the i386 compiler, -m16 is native)
case $ARCH in
    x86_64) KARCH=x86;   KIMG=arch/x86/boot/bzImage; TCC_TARGET=tcc;      LIBTCC1=libtcc1.a;      TCC_CPU=x86_64; CROSS_I386=1;;
    i386)   KARCH=i386;  KIMG=arch/x86/boot/bzImage; TCC_TARGET=i386-tcc; LIBTCC1=i386-libtcc1.a; TCC_CPU=i386;   CROSS_I386=;;
    *) die "ARCH=$ARCH is not implemented (x86_64, i386; see ./install.sh --help)";;
esac
B=$BUILD/$ARCH; LOGS=$B/logs; O=$OUT/$ARCH
# the source copies under build/ must not see this repository's own .git (tcc's and
# busybox's version strings ask git about the tree they are built from)
export GIT_CEILING_DIRECTORIES=$BUILD
# progress-bar totals for the two builds whose step count "make -n" cannot tell; corrected
# from the real count after the first complete build (build/<arch>/steps/<name>)
KERNEL_STEPS_DEFAULT=1190
BUSYBOX_STEPS_DEFAULT=660
STEPS=$B/steps
# the build steps' directories (made right before a build step runs, so that the clean steps
# leave nothing behind)
prepare() { mkdir -p "$B" "$LOGS" "$O" "$STEPS"; }
TCCSRC=$SRC/tinycc; TCC=$TCCSRC/tcc; TCC_TARGET=$TCCSRC/$TCC_TARGET; LIBTCC1=$TCCSRC/$LIBTCC1
LINUX=$SRC/linux; MUSLSRC=$SRC/musl; BBSRC=$SRC/busybox
MUSLB=$B/musl; MUSLP=$MUSLB/install; KHDR=$B/kernel-headers; MUSL_TCC=$B/musl-tcc

need_src() {
    for n; do
        [ -d "$SRC/$n/.git" ] || die "src/$n is missing: run scripts/fetch.sh"
        if [ -f "$ROOT/patches/$n/series" ]; then
            [ "$(git -C "$SRC/$n" rev-parse --abbrev-ref HEAD)" = patched ] \
                || die "src/$n is not on the 'patched' branch: run scripts/patch.sh"
        fi
    done
}
fail() { tail -n 25 "$1" >&2; die "$2 (log: $1)"; }
# configured FILE STRING...: FILE (a configure output) exists and records every STRING.  The
# configure scripts write absolute paths (CC, srcdir, prefix) into config.mak / config.status;
# when the repository or its build tree has moved, those files exist but point to nowhere, so
# the steps re-run configure instead of trusting that the file is there.
configured() {
    f=$1; shift
    [ -f "$f" ] || return 1
    for sv; do grep -qF -- "$sv" "$f" && continue
        echo "   $f was configured for another path: re-running configure"
        rm -f "$f"; return 1
    done
}
# "<upstream commit>+<n> patches", e.g. "e23dabe+53p": tcc's version suffix (see step_host_tcc)
tcc_tag() { echo "$(git -C "$TCCSRC" rev-parse --short base)+$(git -C "$TCCSRC" rev-list --count base..HEAD)p"; }

step_host_tcc() {
    need_src tinycc
    msg "host tcc: src/tinycc (in-tree, $(git -C "$TCCSRC" rev-list --count base..HEAD) patches on $(git -C "$TCCSRC" rev-parse --short base))"
    cd "$TCCSRC"
    [ -f config.mak ] || ./configure >"$LOGS/tcc-configure.log" 2>&1 || fail "$LOGS/tcc-configure.log" "tcc configure failed"
    # a stable version string: Kbuild rebuilds everything when CC's version text changes,
    # and every re-application of the series gives the commits new hashes.  The string is a
    # -D define, which make does not track: recompile the object that embeds it when the
    # patch count changed since the last build
    if [ "$(cat "$B/tcc-tag" 2>/dev/null)" != "$(tcc_tag)" ]; then
        rm -f "$TCCSRC"/libtcc.o "$TCCSRC"/tcc.o "$TCCSRC"/i386-libtcc.o "$TCCSRC"/i386-tcc.o
        echo "$(tcc_tag)" > "$B/tcc-tag"
    fi
    : >"$LOGS/tcc-build.log"
    run_progress "tcc" "$(make_total GITHASH="$(tcc_tag)" GITMODF=)" "$LOGS/tcc-build.log" \
        make -j"$JOBS" GITHASH="$(tcc_tag)" GITMODF= || fail "$LOGS/tcc-build.log" "tcc build failed"
    # the i386 cross compiler: tcc -m16/-m32 exec it for the kernel's real-mode code
    make -j"$JOBS" cross-i386 GITHASH="$(tcc_tag)" GITMODF= >>"$LOGS/tcc-build.log" 2>&1 || fail "$LOGS/tcc-build.log" "i386-tcc build failed"
    echo "   $("$TCC" -v), $("$TCCSRC/i386-tcc" -v)"
}

step_tcc_test() {
    need_src tinycc
    msg "tcc test suite"
    : >"$LOGS/tcc-test.log"
    PROGRESS_STEP_RE='.'
    run_progress "tcc tests" 0 "$LOGS/tcc-test.log" make -C "$TCCSRC" -s test -k || true
    unset PROGRESS_STEP_RE
    grep -q 'ALL TESTS PASSED' "$LOGS/tcc-test.log" || fail "$LOGS/tcc-test.log" "tcc tests did not all pass"
    echo "   ALL TESTS PASSED"
}

step_tcc_archive() {
    need_src tinycc
    [ -x "$TCC" ] || die "no host tcc: run the host-tcc step first"
    D=$B/tcc-dist
    msg "tcc archive: make install into $D, packed into $O"
    rm -rf "$D"
    (cd "$TCCSRC" && make -s install DESTDIR="$D" GITHASH="$(tcc_tag)" GITMODF=) >"$LOGS/tcc-install.log" 2>&1 \
        || fail "$LOGS/tcc-install.log" "tcc install failed"
    prefix=$(sed -n 's/^prefix=//p' "$TCCSRC/config.mak" | head -1); prefix=${prefix:-/usr/local}
    doc=$D$prefix/share/doc/tinycc-os-kernel; mkdir -p "$doc"
    { echo "Tiny C Compiler $("$TCC" -v | awk '{print $3}') from tinycc-os-kernel"
      echo "upstream: $(lock_urls tinycc | awk '{print $1}') commit $(git -C "$TCCSRC" rev-parse base)"
      echo "plus these $(git -C "$TCCSRC" rev-list --count base..HEAD) patches (patches/tinycc/ in the repository):"
      git -C "$TCCSRC" log --reverse --format='  %s' base..HEAD
    } > "$doc/PATCHES"
    ar=$O/tcc-$("$TCC" -v | awk '{print $3}')-$(tcc_tag)-$ARCH-linux.tar.gz
    tar czf "$ar" -C "$D" .
    echo "   $(ls -l "$ar" | awk '{print $5}') bytes: $ar"
    echo "   contents: $(find "$D" -type f | wc -l) files under $prefix (bin/tcc, lib/tcc/, include/libtcc.h, share/doc)"
}

step_tld() {
    [ -x "$TCC" ] || die "no host tcc: run the host-tcc step first"
    TLD=$B/tld; mkdir -p "$TLD"
    msg "tld: tools/tld (ld, nm, objcopy, readelf, ar, strip) compiled by the host tcc -> $TLD"
    "$TCC" -B"$TCCSRC" -I"$TCCSRC/include" -std=gnu99 -O2 -o "$TLD/tld" "$ROOT"/tools/tld/*.c \
        || die "tld build failed"
    for t in ld nm objcopy readelf ar strip; do ln -sf tld "$TLD/$t"; done
    echo "   $("$TLD/ld" -v), $(ls -l "$TLD/tld" | awk '{print $5}') bytes"
}

step_kernel() {
    need_src linux
    [ -x "$TCC" ] || die "no host tcc: run the host-tcc step first"
    [ -x "$TCCSRC/i386-tcc" ] || die "no $TCCSRC/i386-tcc (the host-tcc step builds it): the real-mode code needs tcc -m16"
    KFRAGS="minimal.config $ARCH.config tcc.config"
    TLD=$B/tld; [ -x "$TLD/ld" ] || die "no $TLD/ld: run the tld step first"
    KB=$B/linux; mkdir -p "$KB"
    msg "kernel: ARCH=$KARCH tinyconfig + kernels/linux/{minimal,$ARCH,tcc}.config, CC=tcc-kcc ($TCC_TARGET) -> $KB"
    KCC_LOG=$LOGS/kcc.log
    : >"$KCC_LOG"
    # TCC and KCC_LOG are given to this make only (not exported: GNU make re-exports a
    # variable that came from the environment with its own value, which would break the
    # musl-tcc wrapper during the tcc builds)
    # HOSTCC: the host tools (fixdep, kconfig, modpost, sorttable, relocs, vdso2c, ...) are
    # compiled by tcc too; objtool needs libelf and is replaced by kernels/linux/objtool-stub
    # the binutils are tld's: LD/NM/OBJCOPY/READELF/AR/STRIP (no objdump is needed)
    # KBUILD_BUILD_VERSION=1: the build number counts the generations (uname -v "#1"); a kernel
    # built inside the guest by userland/selfhost/build-kernel is #2, and so on
    # KCC_TCC: the compiler tcc-kcc runs for the target code (TCC stays the host compiler for
    # tcc-hostcc); for i386 the cross compiler next to it
    MK="env TCC=$TCC KCC_TCC=$TCC_TARGET KCC_LOG=$KCC_LOG make -C $LINUX O=$KB ARCH=$KARCH CC=$ROOT/kernels/linux/tcc-kcc HOSTCC=$ROOT/kernels/linux/tcc-hostcc OBJTOOL=$ROOT/kernels/linux/objtool-stub LD=$TLD/ld NM=$TLD/nm OBJCOPY=$TLD/objcopy READELF=$TLD/readelf AR=$TLD/ar STRIP=$TLD/strip KBUILD_BUILD_VERSION=1"
    if [ ! -f "$KB/.config" ]; then
        $MK -s tinyconfig >"$LOGS/kernel-config.log" 2>&1 || fail "$LOGS/kernel-config.log" "tinyconfig failed"
    fi
    # the fragments are merged on every run, so that a line added to them reaches an existing
    # build tree too (olddefconfig then resolves the dependencies; Kbuild rebuilds what changed)
    for f in $KFRAGS; do
        "$LINUX/scripts/kconfig/merge_config.sh" -m -O "$KB" "$KB/.config" "$ROOT/kernels/linux/$f" >>"$LOGS/kernel-config.log" 2>&1
    done
    $MK -s olddefconfig >>"$LOGS/kernel-config.log" 2>&1 || fail "$LOGS/kernel-config.log" "olddefconfig failed"
    # every fragment line must survive olddefconfig
    for f in $KFRAGS; do
        grep -E '^(CONFIG_|# CONFIG_.* is not set)' "$ROOT/kernels/linux/$f" | while IFS= read -r line; do
            grep -qxF -- "$line" "$KB/.config" && continue
            case $line in
                "# CONFIG_"*" is not set")   # absent altogether = not available for this compiler
                    opt=${line#\# }; opt=${opt%% *}
                    grep -q "^$opt=" "$KB/.config" || continue;;
            esac
            echo "   NOT APPLIED ($f): $line"
        done
    done
    total=$(cat "$STEPS/kernel" 2>/dev/null || echo "$KERNEL_STEPS_DEFAULT")
    learn=1; [ ! -f "$KB/vmlinux" ] || learn=0   # only a complete build teaches the total
    : >"$LOGS/kernel.log"
    PROGRESS_COUNT_FILE=$STEPS/kernel.new
    run_progress "kernel" "$total" "$LOGS/kernel.log" $MK -j"$JOBS" \
        || { grep -E ' error: |Error [0-9]|undefined reference' "$LOGS/kernel.log" | head -20 >&2; fail "$LOGS/kernel.log" "kernel build failed"; }
    unset PROGRESS_COUNT_FILE
    [ $learn = 0 ] || [ "$(cat "$STEPS/kernel.new")" = 0 ] || mv "$STEPS/kernel.new" "$STEPS/kernel"
    cp "$KB/$KIMG" "$O/bzImage"
    echo "   $(grep -c '^tcc ' "$KCC_LOG") tcc and $(grep -c '^gcc ' "$KCC_LOG") gcc compiler invocations; $(ls -l "$O/bzImage" | awk '{print $5}') bytes bzImage"
}

step_musl() {
    need_src musl linux
    [ -x "$TCC" ] || die "no host tcc: run the host-tcc step first"
    msg "musl: CC=$(basename "$TCC_TARGET"), static, without src/complex -> $MUSLP"
    mkdir -p "$MUSLB"
    if [ ! -f "$MUSLP/lib/libc.a" ]; then
        cd "$MUSLB"
        configured config.mak "CC = $TCC_TARGET" "srcdir = $MUSLSRC" "prefix = $MUSLP" \
            || "$MUSLSRC/configure" CC="$TCC_TARGET" --prefix="$MUSLP" --disable-shared >"$LOGS/musl-configure.log" 2>&1 \
            || fail "$LOGS/musl-configure.log" "musl configure failed"
        srcdir=$(sed -n 's/^srcdir = //p' config.mak)
        malloc=$(sed -n 's/^MALLOC_DIR = //p' config.mak)
        dirs=$(for d in "$srcdir"/src/*; do [ "${d##*/}" = complex ] || printf '%s ' "$d"; done)
        dirs="$dirs $srcdir/src/malloc/$malloc $srcdir/crt $srcdir/ldso"
        : >"$LOGS/musl.log"
        run_progress "musl" "$(make_total SRC_DIRS="$dirs")" "$LOGS/musl.log" make -j"$JOBS" SRC_DIRS="$dirs" \
            || fail "$LOGS/musl.log" "musl build failed"
        make -s install SRC_DIRS="$dirs" >>"$LOGS/musl.log" 2>&1 || fail "$LOGS/musl.log" "musl install failed"
    fi
    if [ ! -f "$KHDR/include/linux/types.h" ]; then
        echo "   Linux UAPI headers (musl ships none; busybox needs <linux/*.h>)"
        make -s -C "$LINUX" O="$B/linux-hdr" ARCH=$KARCH headers_install INSTALL_HDR_PATH="$KHDR" >"$LOGS/kernel-headers.log" 2>&1 \
            || fail "$LOGS/kernel-headers.log" "headers_install failed"
    fi
    # the driver wrapper: tcc + musl headers + static link against musl's libc.a and tcc's libtcc1.a
    cat > "$MUSL_TCC" <<WRAP
#!/bin/sh
# musl-tcc: tcc driver for the tcc-built musl in $MUSLP (generated by scripts/build.sh).
# Compiles with musl's headers (+ Linux UAPI headers) only and links static against musl's
# libc.a; libtcc1.a (tcc's runtime: __fixxfdi, __va_arg, ...) sits between two copies of
# libc.a because musl's objects need libtcc1 helpers and libtcc1 needs abort() from libc.
# -c/-E/-S/-M*, -r and -nostdlib invocations are passed through without crt files or libs.
MUSL=$MUSLP
KHDR=$KHDR
TCC=\${MUSL_TCC_BIN:-$TCC_TARGET}
LIBTCC1=\${MUSL_TCC_LIBTCC1:-$LIBTCC1}
link=1
for a; do
    case \$a in -c|-E|-S|-M|-MM|-r|-nostdlib|--version|-v|-dumpversion|-print-*) link=0;; esac
done
if [ \$link = 0 ]; then
    exec "\$TCC" -nostdinc -isystem "\$MUSL/include" -isystem "\$KHDR/include" "\$@"
fi
exec "\$TCC" -nostdinc -isystem "\$MUSL/include" -isystem "\$KHDR/include" -nostdlib -static \\
    "\$MUSL/lib/crt1.o" "\$MUSL/lib/crti.o" "\$@" \\
    -L"\$MUSL/lib" "\$MUSL/lib/libc.a" "\$LIBTCC1" "\$MUSL/lib/libc.a" "\$MUSL/lib/crtn.o"
WRAP
    chmod +x "$MUSL_TCC"
    printf '#include <stdio.h>\nint main(void){puts("musl-tcc ok");return 0;}\n' > "$B/musl-hello.c"
    "$MUSL_TCC" -o "$B/musl-hello" "$B/musl-hello.c" && "$B/musl-hello" | sed 's/^/   /'
}

step_busybox() {
    need_src busybox
    [ -x "$MUSL_TCC" ] || die "no $MUSL_TCC: run the musl step first"
    BB=$B/busybox
    msg "busybox: defconfig + STATIC - TC, CC=musl-tcc -> $BB/busybox"
    mkdir -p "$BB"
    # build from a copy of the tracked files, so that src/busybox stays clean
    git -C "$BBSRC" ls-files -z | rsync -a -0 --files-from=- "$BBSRC/" "$BB/"
    cd "$BB"
    # quiet=quiet_: busybox's Makefile takes the "s" of make's --jobserver-auth flag for -s
    # and goes silent under -j, which would leave the progress bar without steps
    MAKE="make quiet=quiet_ CC=$MUSL_TCC HOSTCC=gcc"
    if [ ! -f .config ]; then
        $MAKE -s defconfig >"$LOGS/busybox-config.log" 2>&1
        # TC does not build against current kernel headers; busybox's platform.h defines
        # __attribute__(x) away unless __GNUC__ >= 2.7 (packed, noreturn, ...)
        sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/; s/^CONFIG_TC=y/# CONFIG_TC is not set/
                s/^CONFIG_EXTRA_CFLAGS=.*/CONFIG_EXTRA_CFLAGS="-D__GNUC__=4 -D__GNUC_MINOR__=2"/' .config
        $MAKE -s oldconfig </dev/null >>"$LOGS/busybox-config.log" 2>&1
    fi
    total=$(cat "$STEPS/busybox" 2>/dev/null || echo "$BUSYBOX_STEPS_DEFAULT")
    learn=1; [ ! -x busybox ] || learn=0
    : >"$LOGS/busybox.log"
    PROGRESS_COUNT_FILE=$STEPS/busybox.new
    run_progress "busybox" "$total" "$LOGS/busybox.log" $MAKE -j"$JOBS" || fail "$LOGS/busybox.log" "busybox build failed"
    unset PROGRESS_COUNT_FILE
    [ $learn = 0 ] || [ "$(cat "$STEPS/busybox.new")" = 0 ] || mv "$STEPS/busybox.new" "$STEPS/busybox"
    [ -x busybox ] || fail "$LOGS/busybox.log" "no busybox binary"
    echo "   $(./busybox | head -1 | cut -d' ' -f1-2), $(file -b busybox | cut -d, -f1-2), $(ls -l busybox | awk '{print $5}') bytes"
}

step_guest_tcc() {
    need_src tinycc
    [ -x "$MUSL_TCC" ] || die "no $MUSL_TCC: run the musl step first"
    G=$B/tcc-guest; STAGE=$G/stage
    msg "guest tcc: static, musl, compiled by the host tcc -> $STAGE"
    # tcc's sources #include "config.h" from the source directory first, so an out-of-tree
    # configure would pick up the host build's config.h: build from a copy of the tracked files
    mkdir -p "$G/src"
    git -C "$TCCSRC" ls-files -z | rsync -a -0 --files-from=- "$TCCSRC/" "$G/src/"
    # the version string is a -D define make does not track (see step_host_tcc)
    if [ "$(cat "$G/tcc-tag" 2>/dev/null)" != "$(tcc_tag)" ]; then
        rm -f "$G/src/tcc.o" "$G/src/libtcc.o"
        echo "$(tcc_tag)" > "$G/tcc-tag"
    fi
    cd "$G/src"
    if ! configured config.mak "CC=$MUSL_TCC"; then
        ./configure --cc="$MUSL_TCC" --cpu="$TCC_CPU" --ar=ar --prefix=/usr --tccdir=/usr/lib/tcc --config-musl \
            --sysincludepaths='{B}/include:/usr/include' --libpaths='{B}:/usr/lib' \
            --crtprefix=/usr/lib --tcc-switches=-static --extra-cflags=-O2 \
            --config-backtrace=no --config-bcheck=no >"$LOGS/tcc-guest-configure.log" 2>&1 \
            || fail "$LOGS/tcc-guest-configure.log" "guest tcc configure failed"
    fi
    # libtcc1's objects are normally compiled by the just-built tcc, whose include path is the
    # guest's /usr/include (glibc on the host); the usegcc switch uses $(CC) = musl-tcc instead
    : >"$LOGS/tcc-guest.log"
    run_progress "guest tcc" "$(make_total $TCC_CPU-libtcc1-usegcc=yes GITHASH="$(tcc_tag)" GITMODF=)" "$LOGS/tcc-guest.log" \
        make -j"$JOBS" $TCC_CPU-libtcc1-usegcc=yes GITHASH="$(tcc_tag)" GITMODF= || fail "$LOGS/tcc-guest.log" "guest tcc build failed"
    # the i386 cross compiler next to it (tcc -m16 execs it): the self-hosting image compiles
    # the kernel's real-mode code with it.  Its libtcc1.a is not needed by the kernel; build it
    # when it compiles against the host's headers, else only the compiler.  (The i386 guest's
    # tcc is the i386 compiler itself: nothing to add.)
    [ -z "$CROSS_I386" ] || make -j"$JOBS" cross-i386 GITHASH="$(tcc_tag)" GITMODF= >>"$LOGS/tcc-guest.log" 2>&1 \
        || make -j"$JOBS" i386-tcc GITHASH="$(tcc_tag)" GITMODF= >>"$LOGS/tcc-guest.log" 2>&1 \
        || fail "$LOGS/tcc-guest.log" "guest i386-tcc build failed"
    rm -rf "$STAGE"
    make -s install DESTDIR="$STAGE" >>"$LOGS/tcc-guest.log" 2>&1
    rm -rf "$STAGE/usr/share"
    cp -a "$MUSLP/include/." "$STAGE/usr/include/"
    cp -a "$MUSLP/lib/." "$STAGE/usr/lib/"
    # tcc links libc before libtcc1.a and libtcc1's helpers call abort(): merge libtcc1's
    # objects into the guest's libc.a so that the archive is self-contained
    tmp=$(mktemp -d); (cd "$tmp" && ar x "$STAGE/usr/lib/tcc/libtcc1.a") && ar rs "$STAGE/usr/lib/libc.a" "$tmp"/*.o 2>/dev/null; rm -rf "$tmp"
    mkdir -p "$STAGE/root" && cp "$ROOT/userland/hello.c" "$STAGE/root/hello.c"
    echo "   $(./tcc -v)${CROSS_I386:+, $(./i386-tcc -v)}, $(file -b tcc | cut -d, -f1-2), staging tree $(du -sh "$STAGE" | cut -f1)"
}

step_guest_make() {
    need_src make
    [ -x "$MUSL_TCC" ] || die "no $MUSL_TCC: run the musl step first"
    M=$B/make; mkdir -p "$M"
    msg "guest make: GNU make $(lock_ref make), static, musl, compiled by the host tcc -> $M/make"
    cd "$M"
    # no NLS, no guile, no dynamic loading of extensions: a static musl binary
    configured config.status "CC=$MUSL_TCC" "srcdir='$SRC/make'" \
        || "$SRC/make/configure" CC="$MUSL_TCC" --disable-nls --without-guile \
        --disable-dependency-tracking --disable-load >"$LOGS/make-configure.log" 2>&1 \
        || fail "$LOGS/make-configure.log" "GNU make configure failed"
    : >"$LOGS/make.log"
    run_progress "make" "$(make_total)" "$LOGS/make.log" make -j"$JOBS" || fail "$LOGS/make.log" "GNU make build failed"
    [ -x make ] || fail "$LOGS/make.log" "no make binary"
    echo "   $(./make --version | head -1), $(file -b make | cut -d, -f1-2), $(ls -l make | awk '{print $5}') bytes"
}

# kernel_source_list: the files of src/linux that the self-hosting image needs, one per line,
# relative to src/linux: every file the kernel build read (the dependency lists Kbuild saved
# as .*.cmd in the build tree), the Makefile, Kbuild and helper files of every directory
# Kbuild descended into (the build tree mirrors them; helpers = scripts, syscall tables,
# linker scripts, *_shipped sources, the console font map and keymap, awk generators: inputs of
# generated files, which no dependency list records), every Kconfig file (Kconfig sources them
# all), Kbuild's own scripts (without the tools this build never runs) and all Makefiles under
# scripts/ (Kbuild descends into scripts/dtc even when it builds nothing), the generic and the
# uapi headers, the arch's headers, boot code, syscall tables and tools, and the configs.
# scripts/selfhost-test.sh host proves the list: it builds the kernel from the tarball.
kernel_source_list() (
    cd "$LINUX"
    {
        find "$KB" -name '.*.cmd' -print0 | xargs -0 cat | grep -oE "$LINUX/[^ ]+" | sed "s|^$LINUX/||"
        git ls-files | grep -E '(^|/)Kconfig[^/]*$'
        (cd "$KB" && find . -type d) | sed 's|^\./||' | while IFS= read -r d; do
            [ -d "$d" ] || continue      # include/config, include/generated: generated only
            for f in "$d"/Makefile* "$d"/Kbuild* "$d"/*.sh "$d"/*.tbl "$d"/*.bc "$d"/*.lds "$d"/*.lds.S "$d"/*.ld "$d"/*.pl \
                     "$d"/*_shipped "$d"/*.uni "$d"/*.map "$d"/*.awk; do
                [ ! -f "$f" ] || echo "$f"      # (no "&&": a failing test would end the subshell under set -e)
            done
        done
        git ls-files Kbuild Makefile COPYING scripts include/linux include/asm-generic include/uapi \
            include/vdso include/generated arch/x86/include arch/x86/boot arch/x86/realmode arch/x86/entry \
            arch/x86/tools arch/x86/configs arch/x86/lib arch/x86/kernel/cpu/mkcapflags.sh kernel/configs \
            kernel/time/timeconst.bc init usr tools/include tools/arch/x86/include arch/um/scripts/Makefile.rules \
            | grep -vE '^scripts/(dtc|coccinelle|gdb|sbom|gendwarfksyms|gcc-plugins|clang-tools|selinux|package|tracing|genksyms|ksymoops|kconfig/tests|rust|atomic|dummy-tools|kunit|ipe|include|lib)/'
        # Kbuild descends into scripts/dtc (and others) even when they build nothing: their Makefiles
        git ls-files scripts | grep -E '/(Makefile[^/]*|Kbuild[^/]*)$'
    } | sed -E 's|//+|/|g; :a; s|(^\|/)\./|\1|; s|[^/]+/\.\./||; ta' | sort -u | while IFS= read -r f; do [ ! -f "$f" ] || echo "$f"; done
)

step_selfhost() {
    need_src linux tinycc
    KB=$B/linux
    [ -f "$KB/vmlinux" ] && [ -f "$KB/$KIMG" ] || die "no complete kernel build in $KB: run the kernel step first (its dependency lists define the pruned source tree)"
    STAGE_TCC=$B/tcc-guest/stage
    [ -x "$STAGE_TCC/usr/bin/tcc" ] && { [ -z "$CROSS_I386" ] || [ -x "$STAGE_TCC/usr/bin/i386-tcc" ]; } || die "no guest tcc${CROSS_I386:+ + i386-tcc} in $STAGE_TCC: run the guest-tcc step first"
    [ -x "$B/busybox/busybox" ] || die "no busybox: run the busybox step first"
    [ -x "$B/make/make" ] || step_guest_make
    [ -x "$MUSL_TCC" ] || die "no $MUSL_TCC: run the musl step first"
    SH=$B/selfhost; STAGE=$SH/stage
    msg "self-hosting image: pruned kernel source, tcc/tld/make sources, guest toolchain -> $O/initramfs-selfhost.cpio"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/usr/src" "$STAGE/usr/bin" "$STAGE/usr/share/tinycc-os-kernel/kconfig" "$SH"
    # 1. the pruned kernel tree (a tar.gz, unpacked to a tmpfs by build-kernel)
    kernel_source_list > "$SH/linux-files.txt"
    kver=$(make -s -C "$LINUX" kernelversion 2>/dev/null || echo unknown)
    # (--hard-dereference: busybox tar unpacks it; no hard links, which GNU tar would make of
    # a file listed twice)
    tar czf "$STAGE/usr/src/linux-$kver.tar.gz" -C "$LINUX" --transform 's,^,linux/,S' \
        --owner=0 --group=0 --numeric-owner --hard-dereference -T "$SH/linux-files.txt"
    echo "   linux-$kver.tar.gz: $(wc -l < "$SH/linux-files.txt") files, $(ls -l "$STAGE/usr/src/linux-$kver.tar.gz" | awk '{print $5}') bytes (list: $SH/linux-files.txt)"
    # 2. tcc (the patched tree, tracked files), tld (this repository), GNU make (the release tarball)
    git -C "$TCCSRC" archive --format=tar.gz --prefix=tinycc/ -o "$STAGE/usr/src/tinycc.tar.gz" HEAD
    tar czf "$STAGE/usr/src/tld.tar.gz" -C "$ROOT/tools" --owner=0 --group=0 --numeric-owner \
        tld/Makefile tld/README.md tld/test.sh $(cd "$ROOT/tools" && ls tld/*.c tld/*.h)
    cp "$SRC/.downloads/make-$(lock_ref make).tar.gz" "$STAGE/usr/src/"
    echo "   $(ls "$STAGE/usr/src" | tr '\n' ' ')"
    # 3. the toolchain: tld (static musl, compiled by the host tcc) with its tool names, GNU
    #    make, the Kbuild wrappers, the flex/bison stand-ins and the build scripts
    "$MUSL_TCC" -std=gnu99 -O2 -o "$STAGE/usr/bin/tld" "$ROOT"/tools/tld/*.c || die "tld (musl) build failed"
    for t in ld nm objcopy readelf ar strip; do ln -sf tld "$STAGE/usr/bin/$t"; done
    cp "$B/make/make" "$STAGE/usr/bin/make"
    cp "$ROOT/kernels/linux/tcc-kcc" "$ROOT/kernels/linux/tcc-hostcc" "$ROOT/kernels/linux/objtool-stub" "$STAGE/usr/bin/"
    cp "$ROOT"/userland/selfhost/build-kernel "$ROOT"/userland/selfhost/build-toolchain "$ROOT"/userland/selfhost/send-file \
       "$ROOT"/userland/selfhost/flex "$ROOT"/userland/selfhost/bison "$STAGE/usr/bin/"
    chmod 755 "$STAGE"/usr/bin/*
    # 4. the Kconfig fragments, the pre-generated Kconfig lexer and parser (flex and bison are
    #    not in the guest), the kernel and tcc patches and the notes
    cp "$ROOT/kernels/linux/minimal.config" "$ROOT/kernels/linux/$ARCH.config" "$ROOT/kernels/linux/tcc.config" "$STAGE/usr/share/tinycc-os-kernel/"
    cp "$KB/scripts/kconfig/lexer.lex.c" "$KB/scripts/kconfig/parser.tab.c" "$KB/scripts/kconfig/parser.tab.h" \
        "$STAGE/usr/share/tinycc-os-kernel/kconfig/" || die "no pre-generated Kconfig lexer/parser in $KB/scripts/kconfig"
    cp -r "$ROOT/patches" "$STAGE/usr/share/tinycc-os-kernel/patches"
    cp "$ROOT/userland/selfhost/README" "$STAGE/usr/share/tinycc-os-kernel/README"
    # the Linux UAPI headers next to musl's (as a musl distribution installs them): the
    # kernel's host programs include <asm/types.h> & co. through tools/include
    [ -f "$KHDR/include/linux/types.h" ] || die "no Linux UAPI headers in $KHDR: run the musl step first"
    mkdir -p "$STAGE/usr/include" && cp -a "$KHDR/include/." "$STAGE/usr/include/"
    # 5. the image: the busybox image's manifest + the guest tcc tree + this tree
    TOOLS=$B/tools; mkdir -p "$TOOLS"
    [ -x "$TOOLS/gen_init_cpio" ] || gcc -O2 -o "$TOOLS/gen_init_cpio" "$LINUX/usr/gen_init_cpio.c"
    export INIT_SH=$ROOT/userland/init.sh BUSYBOX=$B/busybox/busybox
    { cat "$ROOT/userland/initramfs.list"
      "$ROOT/userland/dir2list.sh" "$STAGE_TCC"
      "$ROOT/userland/dir2list.sh" "$STAGE"
    } > "$SH/initramfs-selfhost.gen.list"
    "$TOOLS/gen_init_cpio" "$SH/initramfs-selfhost.gen.list" > "$O/initramfs-selfhost.cpio"
    echo "   toolchain: $(ls "$STAGE/usr/bin" | tr '\n' ' ')"
    echo "   $(ls -l "$O/initramfs-selfhost.cpio" | awk '{print $5}') bytes: $O/initramfs-selfhost.cpio (staging tree $(du -sh "$STAGE" | cut -f1))"
    echo "   boot it with ./run.sh --selfhost (1 GB RAM, every CPU); inside: build-kernel, then send-file"
}

step_initramfs() {
    need_src linux
    [ -x "$TCC" ] || die "no host tcc: run the host-tcc step first"
    msg "initramfs images -> $O"
    TOOLS=$B/tools; mkdir -p "$TOOLS"
    [ -x "$TOOLS/gen_init_cpio" ] || gcc -O2 -o "$TOOLS/gen_init_cpio" "$LINUX/usr/gen_init_cpio.c"
    "$TCC_TARGET" -B"$TCCSRC" -I"$TCCSRC/include" -nostdlib -static -o "$B/init" "$ROOT/userland/init.c"
    export INIT=$B/init INIT_SH=$ROOT/userland/init.sh BUSYBOX=$B/busybox/busybox
    "$TOOLS/gen_init_cpio" "$ROOT/userland/initramfs-nolibc.list" > "$O/initramfs-nolibc.cpio"
    if [ -x "$BUSYBOX" ]; then
        { cat "$ROOT/userland/initramfs.list"
          if [ -x "$B/tcc-guest/stage/usr/bin/tcc" ]; then "$ROOT/userland/dir2list.sh" "$B/tcc-guest/stage"
          else echo "   no guest tcc (guest-tcc step): initramfs without /usr/bin/tcc" >&2; fi
        } > "$B/initramfs.gen.list"
        "$TOOLS/gen_init_cpio" "$B/initramfs.gen.list" > "$O/initramfs.cpio"
    else
        echo "   no $BUSYBOX (busybox step): only the libc-free image was made" >&2
    fi
    ls -l "$O"/*.cpio | awk '{print "   " $5 "\t" $9}'
}

step_iso() {
    msg "ISO -> $O"
    ARCH=$ARCH OUT=$OUT BUILD=$BUILD "$ROOT/boot/mkiso.sh"
}

# rel PATH: PATH relative to the repository when it lies inside it (for the messages)
rel() { case $1 in "$ROOT"/*) echo "${1#"$ROOT"/}";; *) echo "$1";; esac; }
# remove_trees LABEL PATH...: list the PATHs that exist with their sizes, then remove them
# (DRY_RUN=1: list only)
remove_trees() {
    label=$1; shift
    for p; do shift; [ ! -e "$p" ] || set -- "$@" "$p"; done     # keep the ones that exist
    [ $# -gt 0 ] || { msg "$label: nothing to remove"; return 0; }
    msg "$label"
    du -sh "$@" | while IFS='	' read -r size p; do printf '   %6s  %s\n' "$size" "$(rel "$p")"; done
    total=$(du -shc "$@" | tail -n 1 | cut -f1)
    if [ "${DRY_RUN:-0}" != 0 ]; then echo "   $total would be freed; nothing was removed"; return 0; fi
    rm -rf "$@"
    echo "   $total freed"
}

step_clean() {
    remove_trees "clean $ARCH: build trees, logs and results (src/ and the host tcc in src/tinycc stay)" "$B" "$O"
    rmdir "$BUILD" "$OUT" 2>/dev/null || true      # the parents, when this was the last architecture
}

step_clean_all() {
    remove_trees "clean-all: build trees, logs and results of every architecture (src/ stays)" "$BUILD" "$OUT"
}

step_clean_src() {
    set --
    for n in $(lock_names); do
        d=$SRC/$n
        [ -e "$d" ] || continue
        if [ -d "$d/.git" ]; then
            [ -z "$(git -C "$d" status --porcelain)" ] \
                || die "src/$n has uncommitted changes (git -C src/$n status): commit and export them, or discard them, first"
            [ -z "$(git -C "$d" stash list)" ] || die "src/$n has stashed changes (git -C src/$n stash list): apply or drop them first"
            if git -C "$d" rev-parse -q --verify base >/dev/null; then
                commits=$(git -C "$d" rev-list --count --branches ^base)
                series=0; [ ! -f "$ROOT/patches/$n/series" ] || series=$(series_files "$ROOT/patches/$n/series" | wc -l)
                [ "$commits" -le "$series" ] \
                    || die "src/$n has $commits commits on top of 'base' but patches/$n/series lists $series: run scripts/export-patches.sh first"
            fi
        fi
        set -- "$@" "$d"
    done
    remove_trees "clean-src: the fetched component trees (scripts/fetch.sh + scripts/patch.sh recreate them)" "$@" "$SRC/.downloads"
    rmdir "$SRC" 2>/dev/null || true
}

# options first, then every step name is checked before any step runs (a typo after a clean
# step must not cost a build tree)
for s; do
    shift
    case $s in
        -n|--dry-run) DRY_RUN=1;;
        -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d' | cut -c3-; exit 0;;
        all|host-tcc|tcc-test|tcc-archive|tld|kernel|musl|busybox|guest-tcc|guest-make|initramfs|selfhost|iso|clean|clean-all|clean-src) set -- "$@" "$s";;
        -*) die "unknown option '$s' (see the header of $0)";;
        *) die "unknown step '$s' (see the header of $0)";;
    esac
done
[ $# -gt 0 ] || set -- all
for s; do
    case $s in
        all) prepare; for t in host-tcc tld kernel musl busybox guest-tcc initramfs; do "step_$(echo $t | tr - _)"; done;;
        clean|clean-all|clean-src) "step_$(echo $s | tr - _)";;
        *) prepare; "step_$(echo $s | tr - _)";;
    esac
done
