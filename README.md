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
(OpenBSD, FreeBSD, Hurd, ...) and the arm64 target are planned but not started; the i386
target (the 32-bit x86 kernel) works since 2026-10-23, see "Targets" below.

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
ten times faster). Every script checks what it needs when it starts and names the missing
pieces with the `apt install` line, before doing anything.
About 2 GB of disk (the kernel checkout is the bulk) and 10 minutes.

```bash
./install.sh                      # asks what to build (tcc only, or the distro), target and output
./install.sh --build tcc          # only the patched tcc: out/x86_64/tcc-<version>-x86_64-linux.tar.gz
./install.sh --build distro --arch x86_64 --output iso --test   # unattended, with tcc's test suite
./install.sh --build distro --output iso --selfhost             # + the self-hosting image and its ISO entries
./install.sh --arch i386                                        # the 32-bit kernel and userland (out/i386)
./run.sh --arch i386                                            # boots it in qemu-system-i386 (chosen by itself when only i386 is built)
./install.sh --clean --arch i386                                # removes build/i386 and out/i386 (src/ stays)
./install.sh --clean-all --dry-run                              # lists what a clean of every architecture would free
./run.sh                          # boots out/x86_64 in QEMU to a busybox shell; 'exit' ends QEMU
./run.sh --arch i386              # the 32-bit system; without --arch, a menu of what is built (on a terminal)
./run.sh --list                   # what out/ holds per architecture and boot form, with the missing files
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
A build tree survives moving or renaming the repository: the steps re-run `configure` for
musl, the guest tcc and GNU make when their `config.mak`/`config.status` record another path,
and Kbuild recompiles what its dependency files point at.

Cleaning up: `./install.sh --clean [--arch ARCH]` (= `scripts/build.sh clean`) removes the build
trees, logs and results of one architecture, `build/<arch>/` and `out/<arch>/`; `--clean-all`
(`clean-all`) those of every architecture. Both print what goes with its size, and `--dry-run`
(`-n`, or `DRY_RUN=1`; build.sh takes the same spellings) prints that and removes nothing.
build.sh checks its whole command line before it runs a step, so a mistyped step or option
after `clean` aborts with nothing removed. The fetched sources in `src/` stay, so the next
`install.sh` or `scripts/build.sh all` rebuilds without downloading (the host tcc is built
in-tree in `src/tinycc` and stays too). `scripts/build.sh clean-src` removes the source trees
and the downloaded tarball as well, but refuses while a tree has uncommitted changes, a stash,
or more commits on top of `base` than its series lists: fixes are committed in `src/<name>` and
exported with `scripts/export-patches.sh`, and nothing unexported may be lost.

The kernel has two consoles: the serial port (what `./run.sh` shows in the terminal) and
the screen, tty0, with a PS/2 keyboard. `./run.sh --vga` opens QEMU's display window next to
the serial terminal, and `./run.sh --iso` does so by default (`--no-vga` for headless); a
shell runs on both consoles. The screen is a KMS display at the native resolution
(`kernels/linux/drivers.config`, 2026-10-26): on QEMU's standard VGA the bochs-drm driver
sets the mode the card's EDID prefers, which `run.sh` makes `RES` (XGA, 1024x768, by
default; `RES=1280x1024 ./run.sh` for another one), on real hardware simpledrm takes over
the framebuffer the firmware set up (UEFI GOP: the panel's native mode; on BIOS, GRUB's
`gfxpayload=auto` in `boot/mkiso.sh` picks the EDID-preferred mode), and fbcon runs on
top (DRM fbdev emulation). The tcc build leaves the real-mode video mode setting out
(`kernels/linux/tcc.config`), so `vga=` has no effect. The ISO's first menu entry makes
the screen the primary console, the second the serial port.

Device drivers (`kernels/linux/drivers.config`, all built in): PCI; TCP/IP (IPv4, IPv6)
over Ethernet (Intel e1000/e1000e, Realtek RTL8139/RTL8169, virtio-net, USB CDC/RNDIS),
Wi-Fi (cfg80211/mac80211 with Atheros ath9k, which needs no firmware file, and the
mac80211_hwsim virtual radios), Bluetooth (USB dongles, UART controllers, the vhci virtual
controller), sound (ALSA: Intel HD Audio, AC'97, USB audio, with the OSS `/dev/dsp`
emulation so that a raw write plays), USB (xHCI/EHCI/OHCI/UHCI, HID keyboards and mice,
mass storage with FAT and ext4), the PS/2 and USB mouse (`/dev/input/mice`, evdev). `/init`
brings the network up with DHCP (busybox udhcpc, its event script a C program) and the
banner lists what was found. `run.sh` attaches QEMU's counterparts by default (`--no-devices`
leaves them out): an e1000 on the user-mode network (the host is 10.0.2.2), an HD Audio card
(`AUDIO=pa` or `pipewire` plays on the host), an xHCI controller with a USB tablet and a
16 MB USB stick image (`out/<arch>/usbstick.img`), the VGA with the `RES` EDID. There is no
emulated Wi-Fi or Bluetooth hardware in QEMU; the virtual radios and controller exercise the
stacks. `scripts/devices-test.sh` boots the image and checks every device class unattended
(`devices-test` in the guest: DHCP, ICMP and HTTP to the host, IPv6, the Wi-Fi interface
up, a Bluetooth controller registered, a tone written to `/dev/dsp`, USB enumeration, a FAT
filesystem on the stick, mouse motion sent through QMP, the screen at `RES`).

The boot ends with:

```
*** tinycc-os-kernel: Linux 7.3.0+ x86_64, #1 SMP PREEMPT ...
*** every file of this kernel was compiled, assembled and linked by tinycc (tcc, tld)
*** BusyBox v1.38.0 (static, musl -- compiled and linked by tinycc as well)
*** tcc version 0.9.28rc e23dabe+110p (x86_64 Linux) is installed, static, musl libc:  try  tcc -run hello.c
*** file manager: nnn (? lists its keys, q quits, Enter opens a file in vi)
*** web browser: links (text mode, http and https: links https://linux.org/ or http://10.0.2.2:8000/; Esc opens the menu, q quits; links -dump URL prints a page)
*** screen: framebuffer 1024x768 (bochs-drmdrmfb), tty1-tty6
*** network: eth0 10.0.2.15 (e1000), sit0, wlan0 (mac80211_hwsim), wlan1 (mac80211_hwsim); bluetooth: no controller (btusb, hci_uart, vhci)
*** sound: HDA-Intel - HDA Intel (/dev/dsp); usb: 2 devices: QEMU USB Tablet; QEMU USB HARDDRIVE; mouse: /dev/input/mice (QEMU QEMU USB Tablet; ImExPS/2 Generic Explorer Mouse)
*** devices-test checks every device (network, Wi-Fi, Bluetooth, sound, USB, mouse, screen)
```

The kernel has SMP (`run.sh` gives the guest every host CPU, up to 8, one socket each: without
ACPI the CPUs come from SeaBIOS's MP table, which lists one CPU per package), tmpfs, two UARTs
(the second is the self-hosting image's file transfer channel), a gzip-compressed image (the
guest's busybox has no xz compressor) and runs under KVM when `/dev/kvm` is writable (`KVM=0`
for TCG).

## Self-hosting

`scripts/build.sh selfhost` (or `install.sh --selfhost`; `--arch i386` for the 32-bit system) makes
`out/<arch>/initramfs-selfhost.cpio`:
the busybox image plus everything needed to rebuild the system inside the guest, without
gcc, binutils, flex or bison:

| In the guest | What |
|------|------|
| `/usr/src/linux-7.3.0.tar.gz` (16 MB, 8600 files, 92 MB unpacked) | the kernel source pruned to what this configuration builds: every file the host build read (from the `.*.cmd` dependency lists of `build/x86_64/linux`), all Makefile/Kbuild/Kconfig files, Kbuild's scripts, the generic, uapi and x86 headers, arch/x86's boot code and tools, the `*_shipped` sources; with the kernel patches applied |
| `/usr/src/tinycc.tar.gz`, `tld.tar.gz`, `make-4.4.1.tar.gz` | tcc with the 110 patches, the linker suite, GNU make |
| `/usr/bin/tcc`, `i386-tcc` | the compiler and the i386 cross compiler `tcc -m16` execs for the real-mode code (static, musl); the i386 image ships only `tcc`, which is that compiler |
| `/usr/bin/tld` (`ld nm objcopy readelf ar strip`), `/usr/bin/make` | the linker suite and GNU make 4.4.1, static musl binaries compiled by tcc |
| `/usr/bin/tcc-kcc`, `tcc-hostcc`, `objtool-stub`, `flex`, `bison` | the Kbuild wrappers (the sources of `kernels/linux/*.c`, which the host build compiles; here C scripts); `flex`/`bison` copy the Kconfig lexer and parser generated on the host (`/usr/share/tinycc-os-kernel/kconfig/`) |
| `/usr/bin/build-kernel`, `build-toolchain`, `send-file` | the commands (`userland/selfhost/`), see `/usr/share/tinycc-os-kernel/README`. Like `/init`, `flex` and `bison`, they are C scripts: a `#!/usr/bin/tcc -run` line, and tcc compiles and runs them in memory. The image has no shell scripts |

```bash
./run.sh --selfhost          # 1 GB of RAM, every CPU (SMP), ttyS1 -> out/<arch>/selfhost/ttyS1.bin
# in the guest:
build-kernel                 # unpacks the tree to /build (tmpfs), tinyconfig + the fragments, the same
                             # make line as the host's kernel step; result: /build/linux/arch/x86/boot/bzImage
send-file /build/linux/arch/x86/boot/bzImage      # over ttyS1 to the host
build-toolchain              # optional: tcc + i386-tcc and tld rebuilt from source into /usr/local,
                             # which build-kernel then uses (--make: GNU make as well)
exit
# on the host:
scripts/recv-file.sh                               # -> out/<arch>/selfhost/bzImage (md5-checked)
KERNEL=out/<arch>/selfhost/bzImage ./run.sh        # boots the guest-built kernel
```

The build number counts the generations: the host build is `#1`, a kernel built inside it
`#2` (`KBUILD_BUILD_VERSION`; the banner says "generation 2: this kernel was built inside a
tinycc-os-kernel guest (tcc@tinycc-os-guest)"), and the image works the same inside #2.
`scripts/selfhost-test.sh` runs it unattended: `host` builds the pruned tree on the host with
the image's toolchain (49 s on 8 cores, a bzImage of the reference's size), `qemu` boots
`./run.sh --selfhost --auto` (the guest runs `build-kernel` and `send-file` itself and shuts
down: 1 min 34 s for the build on 8 virtual CPUs under KVM, 223 MB of tmpfs, 21 s for the
transfer, 2 minutes in all), `boot` boots the received kernel and checks for generation 2.
`AUTO=all` has the guest run `build-toolchain` first and build the kernel with its own,
second-generation tcc and tld (`./run.sh --selfhost --auto=all`), and the same test run on a
guest-built kernel (`KERNEL=out/<arch>/selfhost/bzImage scripts/selfhost-test.sh all`) builds
generation 3 inside generation 2; both verified on x86_64 and i386 (2026-10-23).
`./run.sh --selfhost --auto=test` rebuilds the toolchain and runs tcc's own test suite on it
inside the guest (`build-toolchain --test`): ALL TESTS PASSED on both architectures
(2026-10-23, after fixes 95 and 96, which only a statically linked musl tcc shows; the
shared-object tests are skipped, the image has no dynamic loader). Sizes and times are in
`docs/PORT-NOTES.md` ("Self-hosting"). On the ISO the image is the fourth and
fifth menu entries (`./run.sh --iso --selfhost` selects them).

## Layout

| Path | What |
|------|------|
| `sources.lock` | the ten upstream components with URL, ref and exact commit or tarball hash |
| `scripts/fetch.sh` | shallow-clones each component at its commit into `src/<name>` (branch `base`) |
| `scripts/patch.sh` | `git am` of `patches/<name>/series` onto branch `patched` (idempotent) |
| `scripts/build.sh` | the build steps (host tcc + i386-tcc, tld, kernel, musl, busybox, ncurses, nnn, libressl, links, guest tcc + i386-tcc, guest make, initramfs, selfhost, ISO) and the clean-up steps (`clean`, `clean-all`, `clean-src`) |
| `scripts/selfhost-test.sh`, `scripts/recv-file.sh` | the self-hosting loop unattended (host check, guest build, boot of the result); files sent by the guest cut out of the ttyS1 capture |
| `scripts/export-patches.sh` | regenerates `patches/<name>/` from the commits of `src/<name>` |
| `install.sh`, `run.sh` | the user-facing selector (tcc only as a tar.gz, or the distro; `--clean`, `--clean-all`) and the QEMU boot |
| `patches/tinycc/` | 110 tcc patches in `bugfixes/`, `compatibility/`, `features/` (see below) |
| `patches/linux/` | 19 kernel patches in `workarounds/` and `upstreamable/` |
| `kernels/linux/` | Kconfig fragments (`minimal.config`, `tcc.config`), `tcc-kcc.c` (CC) and `tcc-hostcc.c` (HOSTCC) wrappers for Kbuild, `objtool-stub.c`; C programs, compiled by `build.sh` into `build/<arch>/kcc/` for the host build and run as `tcc -run` scripts in the guest |
| `tools/tld/` | the linker suite that replaces binutils for the kernel: `ld` (GNU linker scripts, `-r`, `-pie`, `-shared`), `nm`, `objcopy`, `readelf -r`, `ar`; its own test suite |
| `userland/` | the libc-free `init.c` (the smoke-test image), `busybox-init.c` (the `/init` of the busybox image, a C script run by `tcc -run`), cpio manifests, `hello.c`; `selfhost/`: `build-kernel.c`, `build-toolchain.c`, `send-file.c`, the `flex.c`/`bison.c` stand-ins (C scripts, installed under their command names) and the guest README |
| `boot/mkiso.sh` | GRUB hybrid BIOS/UEFI ISO |
| `docs/PORT-NOTES.md` | how the port works, wrapper rules, open items, how to debug |
| `docs/FIX-LOG.md` | the per-fix log (problem, fix, verification) with the fix-to-patch map |
| `src/`, `build/`, `out/` | sources, build trees and results (not committed; `install.sh --clean` removes `build/<arch>` and `out/<arch>`, `scripts/build.sh clean-src` the sources, see [Quick start](#quick-start)) |
| `LICENSE`, `LICENSES/`, `REUSE.toml` | BSD-2-Clause for the original parts, the upstream licenses for the patches; the path-to-license map (see [License](#license)) |

## Patches

Patches are applied in the order of `patches/<component>/series`; the four-digit prefix of
every file is its position in that order, so `ls patches/*/*/` sorts the same way. The
directories only sort them by kind:

* **tinycc/bugfixes** (22): tcc was wrong before: a miscompile (goto chains), broken
  encodings (swapgs, mov imm, movaps), truncated constants and addends, stale symbols,
  broken out-of-tree builds, `$` in `.S` macros, a tentative definition in a named
  section, forward labels in symbol differences, an `.eh_frame` CIE in every assembler file,
  a crash on gotos out of cleanup scopes to a gone local label (and the cleanup of a
  variable run by a goto out of its initializer), and two i386 code generator bugs of this
  project's own earlier patches (the regparm indirect call, the struct zero-fill), and the
  `long` bit-scan builtins treated as 64-bit on i386, the ud1 operand size, REX.W on
  segment-register moves, the 8/16-bit relocations in the x86_64 linker and against local
  labels on i386, and regparm with floating-point arguments (all found by the new tests).
* **tinycc/compatibility** (46): accept what GCC (and GNU as) accept, with GCC's
  semantics: builtins, `__auto_type`, `typeof_unqual`, packed enums, cast to union,
  `transparent_union`, forward aliases, `_Pragma`, `#pragma GCC visibility`, global
  register variables, inline-asm constraints and modifiers, the dead-code behaviour an
  optimizing compiler has (unreferenced statics, constant switches, trivial stubs), which
  freestanding code relies on for link-time assertions, GNU as's directive, macro,
  expression and section semantics for the kernel's `.S` files, constant immediates taken
  in the operand size (`andl $(~0xFF000000)`), named sections aligned like their
  contents (the per-cpu cache-hot variables of the SMP kernel), GNU as binary
  constants parsed independently of the libc (the musl-built tcc of the guest), and the
  32-bit kernel's, musl's and busybox's assembler forms (segment-register moves and
  prefixes, 64-bit register names as clobbers, the x87 `"t"`/`"u"` constraints on i386,
  `call *%gs:16`, an `"m"` operand when no register is left, 8/16-bit relocations in
  tcc's own linker), and from the regression tests the `"X"` constraint on i386, `"i"`
  constants printed in the width of their type, `__auto_type` without qualifiers and an
  alias to a static function declared later.
* **tinycc/features** (32): new capabilities: two-pass assembly (inline and whole files),
  GNU as macros, `--start-group`, `-S` (asm-offsets text), `-m16` with GCC's `-mregparm`
  and runtime `.code16/.code32/.code64` in one x86 assembler, inline constant-size
  memcpy/memset, the `m16-check.sh` verification, instruction-table additions (0F 01
  group, xsave, fsgsbase, three-byte opcode maps, SSSE3/SSE4.1/SHA-NI/SSE2 forms, `.octa`),
  and for the i386 target `-fPIC`/`-fPIE` code generation (GOT-relative through `%ebx`, no
  GOT) with the `@GOTOFF`/`_GLOBAL_OFFSET_TABLE_` assembler forms, inline 64-bit shifts and
  bit scans, and the 32-bit arithmetic folds (wrapping addends, power-of-two division,
  `__builtin_mul_overflow` without a division); and nine patches that add regression tests
  for the fixes to tcc's own suite (`tests/tests2/152`-`160`, `tests/pp/24.S`,
  `tests/asmtest.S`), so that `make test` guards them (see `docs/FIX-LOG.md`, "Tests").
* **linux/workarounds** (5): guarded by `__TINYC__` or `__SIZEOF_INT128__`; the kernel
  needs a compiler that inlines or that has `__int128` for these spots.
* **linux/upstreamable** (10): semantics-preserving rewrites that stop depending on
  constant propagation through a local variable, a `!THP` stub for `vma_thp_gfp_mask()`,
  and three toolchain-capability options: `OBJTOOL=` for an external objtool, the
  `CONFIG_AS_AVX2`/`AS_AVX512` probes, and the `X86_BOOT_VIDEO_MODES` option for the
  real-mode setup; from the i386 port the 64-bit division helpers for 32-bit targets whose
  compiler calls them (`GENERIC_LIB_DIVDI3`, with `CC_IS_TCC`), the swiotlb wrappers made
  dead code without `CONFIG_SWIOTLB`, and x86's `pmdp/pudp_test_and_clear_young()`
  declared only where they are defined; candidates for mainline.

Every tcc patch compiles on its own (the series was built commit by commit) and the tip
passes `make test`, on the glibc host and inside both musl guests, including the project's
own regression tests; asm encodings were byte-compared with GNU as and semantics with gcc
as the oracle. The kernel patches are checkpatch-clean except for the deliberately missing
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
| x86_64 | works: every file of the kernel is compiled or assembled by tcc (2085 invocations including Kconfig probes with the drivers of 2026-10-26, 0 gcc) and linked by `tools/tld`; the real-mode code by `i386-tcc -m16`; the host tools by tcc; objtool is a stub (see `docs/PORT-NOTES.md`); SMP; the device drivers (`kernels/linux/drivers.config`: networking, Wi-Fi, Bluetooth, sound, USB, mouse, KMS video; `scripts/devices-test.sh` passes its 13 checks); self-hosting (the kernel rebuilds itself inside the guest) |
| i386 | works (2026-10-23): the 32-bit kernel (`ARCH=i386`, `kernels/linux/i386.config`) compiled by the `i386-tcc` cross compiler (2099 tcc invocations with the drivers, 0 gcc), linked by `tools/tld`, with the decompressor and the 32-bit vDSO as position-independent code (`-fPIE`/`-fpic`, tcc fix 74); musl, busybox and tcc built for i386 by the same compiler; boots in `qemu-system-i386` to the busybox shell; the device drivers too (`scripts/devices-test.sh --arch i386`; tcc fixes 112-114 came from this build); self-hosting as well (`scripts/selfhost-test.sh --arch i386 all`: generation 2 built inside the 32-bit guest in 1 min 32 s boots). Details in `docs/PORT-NOTES.md`, "The i386 target" |
| arm64 | not available: tcc has an arm64 code generator but its assembler cannot handle the kernel's inline asm; nothing attempted |

`install.sh --arch` accepts all three and explains what is missing for arm64. The scripts
take `ARCH` everywhere (`scripts/build.sh`, `run.sh`, `boot/mkiso.sh`: build trees in
`build/<arch>/`, results in `out/<arch>/`), so adding a target is wiring, not restructuring:
i386 cost one case in `build.sh` (the kernel's `ARCH=i386`, `i386-tcc` as the target
compiler, `i386-libtcc1.a`, tcc's `--cpu=i386`), the `kernels/linux/i386.config` fragment
and a `qemu-system-i386` line in `run.sh`; the rest was compiler and linker work. Other
kernels would get `kernels/<name>/` and `patches/<name>/`.

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
| ncurses | 6.5 (release tarball, sha256 in `sources.lock`; wide-char static library for nnn, the consoles' terminfo compiled in; 1 patch: smcup/rmcup for the Linux console, whose alternate screen the kernel implements) | 136d91bc... |
| nnn | v5.3 (the guest's file manager, static, musl, built by tcc against that ncurses) | 5f1c78b2 |
| musl-fts | v1.2.7 (only its `fts.h`: nnn includes the header, which musl lacks) | 0bde52df |
| libressl | 4.4.0 (release tarball, sha256 in `sources.lock`; libcrypto, libssl and libtls, static, musl, built by tcc unpatched without assembly: the TLS library of links; its `cert.pem`, Mozilla's root store, is the guest's `/etc/ssl/cert.pem`) | 7d02522a... |
| links | 2.30 (release tarball, sha256 in `sources.lock`; the guest's web browser: text mode, static, musl, built by tcc unpatched, with TLS from that LibreSSL, so http:// and https://) | 7f0d54f4... |

## License

The original parts of this repository (the scripts, `tools/tld`, `kernels/`, `userland/`,
`boot/`, the documentation, the `series` files) are under the
[BSD-2-Clause](LICENSE) license, so that they can be used by GPL and by BSD projects
alike. The patches are derivative works of the component they modify and carry its
license: `patches/linux/` is GPL-2.0-only, `patches/tinycc/` is LGPL-2.1-or-later,
`patches/ncurses/` is X11 (texts in `LICENSES/`, notes in `patches/<component>/LICENSE`). A future component gets the same
treatment: `patches/freebsd/` would be BSD-2-Clause, `patches/openbsd/` ISC, `patches/hurd/`
GPL-2.0-or-later.

Every source file carries an `SPDX-License-Identifier` line; `REUSE.toml` maps the rest
(following the [REUSE](https://reuse.software) specification, `reuse lint` checks it). The
built images contain GPL software (kernel, busybox); `sources.lock` plus `patches/` is
their corresponding source.
