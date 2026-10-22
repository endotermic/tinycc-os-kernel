# tinycc-os-kernel

Build an operating system kernel with the [Tiny C Compiler](https://repo.or.cz/tinycc.git)
instead of GCC, and boot it with a userland that tcc compiled as well.

Today that is the Linux kernel on x86_64: a tinyconfig-based kernel compiled, assembled
and linked without gcc or binutils (tcc for every C and `.S` file including the 16-bit
boot code, `tools/tld` for the links) boots in QEMU into a static busybox shell (musl,
also built by tcc) with tcc itself installed in the guest, so `tcc -run hello.c` works
inside the VM. The system is self-hosting: the self-hosting image carries the pruned kernel
source, the tcc, tld and GNU make sources and the toolchain (tcc, i386-tcc, tld, make, all
static musl binaries built by tcc), and `build-kernel` inside the guest rebuilds the kernel
with them; the result boots as "generation 2" (see "Self-hosting" below). Other kernels
(OpenBSD, FreeBSD, Hurd, ...) and the i386 and arm64 targets are planned but not started;
see "Targets" below.

This repository contains all of our work and only our work: the pinned source versions,
the compiler patches, the kernel patches, the build recipes and the documentation. The
compiler work and the OS work live together because every change is part of one effort. The kernel, tcc, musl and busybox sources are
downloaded from their git repositories at exact commits and patched locally, so the
scripts and patches never break when upstream moves.

## Quick start

Requirements: git, GNU make, gcc (only to build tcc itself on the host; the kernel, its
host tools and the links use no gcc and no binutils), binutils' `ar` for musl and busybox,
flex, bison, bc, cpio, rsync, QEMU (`qemu-system-x86_64`); for the ISO also grub-pc-bin,
grub-efi-amd64-bin, xorriso and mtools; for `--efi` OVMF; for the self-hosting image curl or
wget (GNU make's tarball) and 1 GB of RAM for the guest (KVM access makes the guest build
ten times faster).
About 2 GB of disk (the kernel checkout is the bulk) and 10 minutes.

```bash
./install.sh                      # asks what to build (tcc only, or the distro), target and output
./install.sh --build tcc          # only the patched tcc: out/x86_64/tcc-<version>-x86_64-linux.tar.gz
./install.sh --build distro --arch x86_64 --output iso --test   # unattended, with tcc's test suite
./install.sh --build distro --output iso --selfhost             # + the self-hosting image and its ISO entries
./run.sh                          # boots out/x86_64 in QEMU to a busybox shell; 'exit' ends QEMU
./run.sh --nolibc                 # the libc-free smoke test: banner, reboot, QEMU exits
./run.sh --iso                    # the ISO (GRUB menu) with QEMU's display window; --efi: on OVMF
./run.sh --vga                    # direct boot with the display window too (--no-vga: headless)
./run.sh --selfhost               # the self-hosting image: 1 GB, every CPU; inside: build-kernel, send-file
scripts/selfhost-test.sh all      # the whole loop unattended: build the kernel in the guest, boot it (#2)
```

Downloads and builds draw progress bars on a terminal (git's transfer percentages; one
step per compiled file for the builds). The totals for the kernel and busybox bars are
estimates on the first run and are corrected from the real count afterwards (`make -n`
cannot tell for those two). Without a terminal, milestone lines are printed instead;
`PROGRESS=0` turns the bars off. Full build output is in `build/<arch>/logs/`.

The kernel has two consoles: the serial port (what `./run.sh` shows in the terminal) and
the screen, tty0, with a PS/2 keyboard. `./run.sh --vga` opens QEMU's display window next to
the serial terminal, and `./run.sh --iso` does so by default (`--no-vga` for headless); a
shell runs on both consoles. On the ISO, GRUB sets a graphics mode before
booting and hands its linear framebuffer to the kernel (simplefb + fbcon), on BIOS via VESA
and on UEFI via GOP; booted directly with `-kernel`, the screen is the VGA text console
(the tcc build leaves the real-mode video mode setting out, see `kernels/linux/tcc.config`,
so `vga=` has no effect there).
`gfxpayload=auto` in `boot/mkiso.sh` takes the monitor's preferred mode from EDID when the
firmware reports one, so on real hardware the native resolution is used. QEMU's virtual
display has no monitor to ask, so `run.sh` sets the resolution itself: XGA (1024x768) by
default, `RES=1280x1024 ./run.sh` for another one, passed to the ISO's GRUB as an SMBIOS
OEM string. The first menu entry makes the screen the primary console, the second the
serial port.

The boot ends with:

```
*** tinycc-os-kernel: Linux 7.3.0+ x86_64, #1 SMP PREEMPT ...
*** every file of this kernel was compiled, assembled and linked by tinycc (tcc, tld)
*** BusyBox v1.38.0 (static, musl -- compiled and linked by tinycc as well)
*** tcc version 0.9.28rc e23dabe+71p (x86_64 Linux) is installed, static, musl libc:  try  tcc -run hello.c
*** screen: VGA text console, tty1-tty6
```

The kernel has SMP (`run.sh` gives the guest every host CPU, up to 8, one socket each: without
ACPI the CPUs come from SeaBIOS's MP table, which lists one CPU per package), tmpfs, two UARTs
(the second is the self-hosting image's file transfer channel), a gzip-compressed image (the
guest's busybox has no xz compressor) and runs under KVM when `/dev/kvm` is writable (`KVM=0`
for TCG).

## Self-hosting

`scripts/build.sh selfhost` (or `install.sh --selfhost`) makes `out/x86_64/initramfs-selfhost.cpio`:
the busybox image plus everything needed to rebuild the system inside the guest, without
gcc, binutils, flex or bison:

| In the guest | What |
|------|------|
| `/usr/src/linux-7.3.0.tar.gz` (16 MB, 8600 files, 92 MB unpacked) | the kernel source pruned to what this configuration builds: every file the host build read (from the `.*.cmd` dependency lists of `build/x86_64/linux`), all Makefile/Kbuild/Kconfig files, Kbuild's scripts, the generic, uapi and x86 headers, arch/x86's boot code and tools, the `*_shipped` sources; with the 12 kernel patches applied |
| `/usr/src/tinycc.tar.gz`, `tld.tar.gz`, `make-4.4.1.tar.gz` | tcc with the 72 patches, the linker suite, GNU make |
| `/usr/bin/tcc`, `i386-tcc` | the compiler and the i386 cross compiler `tcc -m16` execs for the real-mode code (static, musl) |
| `/usr/bin/tld` (`ld nm objcopy readelf ar strip`), `/usr/bin/make` | the linker suite and GNU make 4.4.1, static musl binaries compiled by tcc |
| `/usr/bin/tcc-kcc`, `tcc-hostcc`, `objtool-stub`, `flex`, `bison` | the Kbuild wrappers; `flex`/`bison` copy the Kconfig lexer and parser generated on the host (`/usr/share/tinycc-os-kernel/kconfig/`) |
| `/usr/bin/build-kernel`, `build-toolchain`, `send-file` | the commands (`userland/selfhost/`), see `/usr/share/tinycc-os-kernel/README` |

```bash
./run.sh --selfhost          # 1 GB of RAM, every CPU (SMP), ttyS1 -> out/x86_64/selfhost/ttyS1.bin
# in the guest:
build-kernel                 # unpacks the tree to /build (tmpfs), tinyconfig + the fragments, the same
                             # make line as the host's kernel step; result: /build/linux/arch/x86/boot/bzImage
send-file /build/linux/arch/x86/boot/bzImage      # over ttyS1 to the host
build-toolchain              # optional: tcc + i386-tcc and tld rebuilt from source into /usr/local,
                             # which build-kernel then uses (--make: GNU make as well)
exit
# on the host:
scripts/recv-file.sh                               # -> out/x86_64/selfhost/bzImage (md5-checked)
KERNEL=out/x86_64/selfhost/bzImage ./run.sh        # boots the guest-built kernel
```

The build number counts the generations: the host build is `#1`, a kernel built inside it
`#2` (`KBUILD_BUILD_VERSION`; the banner says "generation 2: this kernel was built inside a
tinycc-os-kernel guest (tcc@tinycc-os-guest)"), and the image works the same inside #2.
`scripts/selfhost-test.sh` runs it unattended: `host` builds the pruned tree on the host with
the image's toolchain (49 s on 8 cores, a bzImage of the reference's size), `qemu` boots
`./run.sh --selfhost --auto` (the guest runs `build-kernel` and `send-file` itself and shuts
down: 1 min 34 s for the build on 8 virtual CPUs under KVM, 223 MB of tmpfs, 21 s for the
transfer, 2 minutes in all), `boot` boots the received kernel and checks for generation 2.
Sizes and times are in `docs/PORT-NOTES.md` ("Self-hosting"). On the ISO the image is the
fourth and fifth menu entries (`./run.sh --iso --selfhost` selects them).

## Layout

| Path | What |
|------|------|
| `sources.lock` | the four upstream components with URL, ref and exact commit |
| `scripts/fetch.sh` | shallow-clones each component at its commit into `src/<name>` (branch `base`) |
| `scripts/patch.sh` | `git am` of `patches/<name>/series` onto branch `patched` (idempotent) |
| `scripts/build.sh` | the build steps (host tcc + i386-tcc, tld, kernel, musl, busybox, guest tcc + i386-tcc, guest make, initramfs, selfhost, ISO) |
| `scripts/selfhost-test.sh`, `scripts/recv-file.sh` | the self-hosting loop unattended (host check, guest build, boot of the result); files sent by the guest cut out of the ttyS1 capture |
| `scripts/export-patches.sh` | regenerates `patches/<name>/` from the commits of `src/<name>` |
| `install.sh`, `run.sh` | the user-facing selector (tcc only as a tar.gz, or the distro) and the QEMU boot |
| `patches/tinycc/` | 72 tcc patches in `bugfixes/`, `compatibility/`, `features/` (see below) |
| `patches/linux/` | 12 kernel patches in `workarounds/` and `upstreamable/` |
| `kernels/linux/` | Kconfig fragments (`minimal.config`, `tcc.config`), `tcc-kcc` (CC) and `tcc-hostcc` (HOSTCC) wrappers for Kbuild, `objtool-stub` |
| `tools/tld/` | the linker suite that replaces binutils for the kernel: `ld` (GNU linker scripts, `-r`, `-pie`, `-shared`), `nm`, `objcopy`, `readelf -r`, `ar`; its own test suite |
| `userland/` | the libc-free `init.c`, the busybox `/init` script, cpio manifests, `hello.c`; `selfhost/`: `build-kernel`, `build-toolchain`, `send-file`, the `flex`/`bison` stand-ins and the guest README |
| `boot/mkiso.sh` | GRUB hybrid BIOS/UEFI ISO |
| `docs/PORT-NOTES.md` | how the port works, wrapper rules, open items, how to debug |
| `docs/FIX-LOG.md` | the per-fix log (problem, fix, verification) with the fix-to-patch map |
| `src/`, `build/`, `out/` | sources, build trees and results (not committed) |
| `LICENSE`, `LICENSES/`, `REUSE.toml` | BSD-2-Clause for the original parts, the upstream licenses for the patches; the path-to-license map (see [License](#license)) |

## Patches

Patches are applied in the order of `patches/<component>/series`; the four-digit prefix of
every file is its position in that order, so `ls patches/*/*/` sorts the same way. The
directories only sort them by kind:

* **tinycc/bugfixes** (14): tcc was wrong before: a miscompile (goto chains), broken
  encodings (swapgs, mov imm, movaps), truncated constants and addends, stale symbols,
  broken out-of-tree builds, `$` in `.S` macros, a tentative definition in a named
  section, forward labels in symbol differences, an `.eh_frame` CIE in every assembler file.
* **tinycc/compatibility** (42): accept what GCC (and GNU as) accept, with GCC's
  semantics: builtins, `__auto_type`, `typeof_unqual`, packed enums, cast to union,
  `transparent_union`, forward aliases, `_Pragma`, `#pragma GCC visibility`, global
  register variables, inline-asm constraints and modifiers, the dead-code behaviour an
  optimizing compiler has (unreferenced statics, constant switches, trivial stubs), which
  freestanding code relies on for link-time assertions, GNU as's directive, macro,
  expression and section semantics for the kernel's `.S` files, constant immediates taken
  in the operand size (`andl $(~0xFF000000)`), named sections aligned like their
  contents (the per-cpu cache-hot variables of the SMP kernel), and GNU as binary
  constants parsed independently of the libc (the musl-built tcc of the guest).
* **tinycc/features** (16): new capabilities: two-pass assembly (inline and whole files),
  GNU as macros, `--start-group`, `-S` (asm-offsets text), `-m16` with GCC's `-mregparm`
  and runtime `.code16/.code32/.code64` in one x86 assembler, inline constant-size
  memcpy/memset, the `m16-check.sh` verification, and instruction-table additions (0F 01
  group, xsave, fsgsbase, three-byte opcode maps, SSSE3/SSE4.1/SHA-NI/SSE2 forms, `.octa`).
* **linux/workarounds** (5): guarded by `__TINYC__` or `__SIZEOF_INT128__`; the kernel
  needs a compiler that inlines or that has `__int128` for these spots.
* **linux/upstreamable** (7): semantics-preserving rewrites that stop depending on
  constant propagation through a local variable, a `!THP` stub for `vma_thp_gfp_mask()`,
  and three toolchain-capability options: `OBJTOOL=` for an external objtool, the
  `CONFIG_AS_AVX2`/`AS_AVX512` probes, and the `X86_BOOT_VIDEO_MODES` option for the
  real-mode setup; candidates for mainline.

Every tcc patch compiles on its own (the series was built commit by commit) and the tip
passes `make test`; asm encodings were byte-compared with GNU as and semantics with gcc as
the oracle. The kernel patches are checkpatch-clean except for the deliberately missing
`Signed-off-by` (the human submitter adds it; they carry `Assisted-by: LLM` as
Documentation/process/coding-assistants.rst asks). Series developed with AI assistance
(Claude Code); the details of every fix are in `docs/FIX-LOG.md`.

### Changing something

```bash
scripts/fetch.sh && scripts/patch.sh        # src/tinycc on branch 'patched'
cd src/tinycc && $EDITOR tccgen.c && make && make test && git commit -a
cd ../.. && scripts/export-patches.sh --category bugfixes tinycc   # rewrites patches/tinycc/
scripts/build.sh kernel                       # rebuilds with the new compiler
```

`export-patches.sh` keeps the category of existing patches by position and puts new
commits in `--category`. To move to a newer upstream commit, change `sources.lock`,
`scripts/fetch.sh`, `git -C src/tinycc rebase --onto base ...` and export again.

## Targets

| Arch | Status |
|------|--------|
| x86_64 | works: every file of the kernel is compiled or assembled by tcc (1546 invocations including Kconfig probes, 0 gcc) and linked by `tools/tld`; the real-mode code by `i386-tcc -m16`; the host tools by tcc; objtool is a stub (see `docs/PORT-NOTES.md`); SMP; self-hosting (the kernel rebuilds itself inside the guest) |
| i386 | not implemented: `i386-tcc` now exists and assembles the kernel's 16/32-bit code (for the x86_64 boot path); a 32-bit kernel fragment and the 32-bit kernel's own constructs are untried |
| arm64 | not available: tcc has an arm64 code generator but its assembler cannot handle the kernel's inline asm; nothing attempted |

`install.sh --arch` accepts all three and explains what is missing for the two that do not
work. The scripts take `ARCH` everywhere so that adding a target is wiring, not
restructuring. Other kernels would get `kernels/<name>/` and `patches/<name>/`.

## Releases

* **v0.1 "Protoktistos"** (2026-10-22): the first bootable state. Linux x86_64 built entirely
  by tcc (no gcc, no binutils: tcc assembles, `tools/tld` links), booting into a tcc-built
  musl/busybox userland with tcc in the guest, self-hosting (the guest rebuilds its kernel),
  as a kernel+initramfs pair or a BIOS/UEFI ISO.

## Pinned versions

| Component | Version | Commit |
|-----------|---------|--------|
| tinycc | mob, 2026-10-16 (0.9.28rc) | e23dabed |
| linux | master, 2026-10-19 (7.3.0+, after the 7.4 merge window opened); fetched from kernel.org, GitHub as fallback | f47d2236 |
| musl | 1.2.6 | 9fa28ece |
| busybox | 1.38.0 | fc71374d |
| make | 4.4.1 (release tarball, sha256 in `sources.lock`; the guest's make for self-hosting) | dd16fb1d... |

## License

The original parts of this repository (the scripts, `tools/tld`, `kernels/`, `userland/`,
`boot/`, the documentation, the `series` files) are under the
[BSD-2-Clause](LICENSE) license, so that they can be used by GPL and by BSD projects
alike. The patches are derivative works of the component they modify and carry its
license: `patches/linux/` is GPL-2.0-only, `patches/tinycc/` is LGPL-2.1-or-later (texts in
`LICENSES/`, notes in `patches/<component>/LICENSE`). A future component gets the same
treatment: `patches/freebsd/` would be BSD-2-Clause, `patches/openbsd/` ISC, `patches/hurd/`
GPL-2.0-or-later.

Every source file carries an `SPDX-License-Identifier` line; `REUSE.toml` maps the rest
(following the [REUSE](https://reuse.software) specification, `reuse lint` checks it). The
built images contain GPL software (kernel, busybox); `sources.lock` plus `patches/` is
their corresponding source.
