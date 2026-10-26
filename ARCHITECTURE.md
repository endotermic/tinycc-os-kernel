# Architecture

From `install.sh` to the busybox prompt in QEMU, the system is a pipeline of
five stages. Each stage hands one artifact to the next: pinned sources, a
patched tree, a kernel plus initramfs images, a running QEMU, and finally a
shell inside the guest. Nothing in the chain uses gcc or binutils except the
very first step, which builds the host tcc.

```
install.sh
   │  options / menus, host prerequisite check
   ├─► scripts/fetch.sh      sources.lock ──► src/<component>  (branch base)
   ├─► scripts/patch.sh      patches/<component>/series ──► branch patched
   └─► scripts/build.sh      host-tcc → tld → kernel → musl → busybox
                             → guest-tcc → guest-make → initramfs
                             → [selfhost] → [iso]
                                  │
                                  ▼
                   out/<arch>/bzImage, initramfs*.cpio, *.iso
                                  │
run.sh ───────────────────────────┘  QEMU: -kernel/-initrd or -cdrom
   │
   ▼
guest: kernel → /init (#!/usr/bin/tcc -run) → busybox sh
```

## 1. Orchestration: `install.sh`

`install.sh` is the user-facing selector. It asks, or takes as options, what to
build (tcc only, or the distro), the architecture (`x86_64` or `i386`), the
output (initrd, plus an ISO), and whether to add the self-hosting image. It then
checks the host for every tool the chosen build needs (`need_*` helpers in
`scripts/lib.sh`, which print the missing Debian/Ubuntu package names) and calls
three scripts in order: `scripts/fetch.sh`, `scripts/patch.sh`,
`scripts/build.sh`. At the end it prints the `run.sh` lines that boot the result.

## 2. Sources: `scripts/fetch.sh` and `scripts/patch.sh`

`sources.lock` pins five upstreams by commit or tarball hash: tinycc, Linux,
musl, busybox and GNU make. `fetch.sh` puts each into `src/<component>` on a
branch `base`. `patch.sh` applies `patches/<component>/series` with `git am`
onto a branch `patched`: the tcc patches are the compiler fixes (and their
regression tests) that make the kernel compile and run, the kernel patches
adapt Kbuild and a few sources to tcc.

Fixes are made as commits in `src/<component>` and exported back into
`patches/` by `scripts/export-patches.sh`, so the patch series is the canonical
form; `src/` is a gitignored working copy.

## 3. Build: `scripts/build.sh`, one architecture per run

`scripts/build.sh [--arch ARCH] [step...]` runs the steps below in this order,
skipping each whose result already exists. Everything lands in `build/<arch>/`
and `out/<arch>/`.

| Step | What it produces | Compiled by |
|------|------------------|-------------|
| `host-tcc` | tcc and the `i386-tcc` cross compiler, built in-tree from `src/tinycc`; `tcc-test` runs its test suite, `tcc-archive` packs the source for the guest | gcc, the only place it is used |
| `tld` | the project's linker suite in `tools/tld/`, replacing binutils for the kernel: `ld`, `nm`, `objcopy`, `readelf`, `ar`, `strip` | host tcc |
| `kernel` | `out/<arch>/bzImage` from `src/linux` with `tinyconfig` plus the fragments in `kernels/linux/` (`minimal.config`, `drivers.config`: PCI, networking, Wi-Fi, Bluetooth, sound, USB, mouse, KMS video; the per-architecture and the tcc fragment) | tcc through the Kbuild wrappers, see below |
| `musl`, `busybox` | a static musl libc and a static busybox linked against it | host tcc |
| `guest-tcc`, `guest-make` | tcc (and `i386-tcc`) and GNU make as static musl binaries for the guest's `/usr/bin` | host tcc |
| `initramfs` | `out/<arch>/initramfs.cpio` and `initramfs-nolibc.cpio` | `gen_init_cpio` from the kernel tree |
| `selfhost` | `out/<arch>/selfhost/initramfs.cpio`, the busybox image plus everything needed to rebuild the system inside the guest | |
| `iso` | `out/<arch>/*.iso`, a hybrid BIOS/UEFI GRUB image made by `boot/mkiso.sh` | grub-mkrescue, xorriso |

### The kernel build without gcc or binutils

Kbuild is given the compiler through three small wrappers whose sources live in
`kernels/linux/` and are compiled by the host tcc into `build/<arch>/kcc/`:

- `tcc-kcc` as `CC` for every C and assembler file, including the real-mode
  setup code via `-m16` (using `i386-tcc` for the 16-bit and 32-bit parts of an
  x86_64 kernel). It answers the few gcc and `as` queries Kbuild makes
  (`-dumpmachine`, `-Wa,--version`, `-print-file-name`), drops the flags tcc
  does not know, defines the `__GNUC__` macros the kernel headers expect, and
  logs every invocation to `kcc.log`.
- `tcc-hostcc` as `HOSTCC` for Kbuild's own tools (`fixdep`, `kconfig`,
  `gen_init_cpio`, ...), running against the host libc.
- `objtool-stub` as `OBJTOOL`, a no-op.

`LD`, `NM`, `OBJCOPY`, `READELF`, `AR` and `STRIP` all point at `tld`. `flex`
and `bison` are the host's for kconfig here; in the guest, pre-generated
scanner and parser sources are copied in by the `flex` and `bison` C scripts.
The build succeeds only when `kcc.log` records zero gcc invocations.
`KBUILD_BUILD_VERSION` counts generations: `#1` is built on the host, `#2` in
the guest.

### The images

`userland/initramfs.list` is the manifest of the busybox image. It holds
busybox with `/bin/sh`, the guest tcc tree (headers, `libtcc1.a`, musl's
`crt*.o` and `libc.a`, listed by `userland/dir2list.sh`), `hello.c`,
`/init`, which is `userland/busybox-init.c` installed verbatim, udhcpc's
event script (`userland/udhcpc-script.c`) and the device test
(`userland/devices-test.c`), C scripts as well. The smoke-test
image `initramfs-nolibc.cpio` holds only `userland/init.c`, a libc-free static
binary that prints a banner and reboots.

The self-hosting image adds the pruned kernel source as a tarball, the tcc,
tld and make sources, the guest toolchain, and the commands in
`userland/selfhost/`: `build-kernel`, `build-toolchain`, `send-file`, `flex`,
`bison`, plus the three Kbuild wrapper sources. All of them are C sources
installed under their command names.

## 4. Boot: `run.sh` and QEMU

`run.sh [--arch ARCH] [--iso|--selfhost|--nolibc] [--auto=MODE]` picks an image
and starts QEMU:

- Direct boot passes the kernel with `-kernel`, the cpio with `-initrd`, and
  `console=tty0 console=ttyS0` (plus `APPEND`), so the serial console lands on
  the terminal through `-nographic -serial mon:stdio`. It gives the guest as
  many virtual CPUs as the host has, up to eight, and uses KVM with the host
  CPU model when `/dev/kvm` is writable.
- The emulated devices for `kernels/linux/drivers.config` come along unless
  `--no-devices`: an e1000 on QEMU's user-mode network, an HD Audio card with a
  codec, an xHCI controller with a USB tablet and a USB stick image, and the
  standard VGA with an EDID whose preferred mode is `RES`, so that the kernel's
  bochs-drm driver sets that resolution. `scripts/devices-test.sh` boots this
  way with `APPEND=tcc.devtest`, serves HTTP to the guest and moves the mouse
  through QMP while the guest's `devices-test` checks every device.
- `--iso` attaches the image as a CD-ROM and tells GRUB through an SMBIOS OEM
  string which menu entry and console order to use. `--efi` boots it through
  OVMF instead of SeaBIOS.
- `--selfhost` gives the guest 1 GB and captures the second serial port to a
  file; `send-file` in the guest writes results there and
  `scripts/recv-file.sh` cuts them out by their headers and checks the MD5.
- `--auto=MODE` puts `tcc.selfhost=MODE` on the kernel command line, which
  `/init` reads to run a build unattended.

`-no-reboot` makes QEMU exit when the guest reboots, so leaving the shell ends
the run.

## 5. Inside the guest

SeaBIOS or GRUB loads the bzImage. Its real-mode setup was compiled by
`i386-tcc` with `-m16`, the decompressor and the kernel proper by tcc, all
linked by tld. The kernel unpacks the initramfs and executes `/init`.

That file starts with `#!/usr/bin/tcc -run`. The kernel's script loader
(`CONFIG_BINFMT_SCRIPT`) therefore starts the guest's static musl tcc, which
compiles `busybox-init.c` in memory and runs its `main`. PID 1 is tcc running a
C program. It:

1. mounts `devtmpfs`, `proc` and `sysfs` with `mount(2)`;
2. runs `busybox --install -s` to create the applet links and sets `PATH`;
   brings the loopback up and runs udhcpc on every Ethernet interface (its
   event script is the C program `/usr/share/udhcpc/default.script`);
3. prints the banner: kernel version, busybox and tcc builds, the generation
   number from `/proc/version`, CPUs and memory, the screen (framebuffer size
   and driver), the network interfaces with their addresses and drivers, the
   Bluetooth controllers, the sound card, the USB devices and the mice;
4. starts a shell on `/dev/tty1` when a screen is present and the serial port
   is the primary console;
5. runs the `tcc.selfhost` mode if one was given (`auto` builds a kernel and
   sends it, `all` rebuilds the toolchain, `test` runs the tcc test suite and
   sends the logs), or `devices-test` when `tcc.devtest` was given, and shuts
   down afterwards;
6. runs `cttyhack sh` on the console, which is the busybox prompt you see;
7. when that shell exits, drains the console, syncs, and calls
   `reboot(RB_AUTOBOOT)`, so QEMU exits.

In the self-hosting image the same mechanism goes one level further.
`build-kernel`, `build-toolchain`, `send-file`, `flex` and `bison` are C scripts
run by tcc. Inside the guest's kernel build the Kbuild wrappers are the same
`tcc-kcc.c`, `tcc-hostcc.c` and `objtool-stub.c` sources as on the host, this
time run as `tcc -run` scripts instead of compiled binaries. The guest rebuilds
its own kernel, and its own tcc and tld, with no shell script of the project's
in the loop: every script of the built system is a C program.

## Where things live

| Path | Role |
|------|------|
| `install.sh`, `run.sh` | entry points |
| `scripts/` | `fetch.sh`, `patch.sh`, `build.sh`, `lib.sh` (shared helpers, host check), `export-patches.sh`, `recv-file.sh`, `selfhost-test.sh`, `devices-test.sh` |
| `sources.lock`, `patches/` | pinned upstreams and the patch series |
| `kernels/linux/` | kernel config fragments and the Kbuild wrapper sources |
| `tools/tld/` | the linker suite |
| `userland/` | `init.c`, `busybox-init.c`, `udhcpc-script.c`, `devices-test.c`, `initramfs.list`, `dir2list.sh`, `selfhost/` C scripts |
| `boot/` | `mkiso.sh`, which also writes the GRUB menu into the ISO |
| `docs/` | `PORT-NOTES.md` (how tcc was made to build the kernel), `FIX-LOG.md` (every tcc fix) |
| `src/`, `build/`, `out/` | gitignored: patched sources, build trees, results |
