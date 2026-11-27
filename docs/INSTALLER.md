# Hard disk installer: the plan

Status: planned (2026-11-27), nothing built yet.  Mark a stage done here when it is built and
verified, the way docs/HARDWARE.md tracks the driver groups.

The goal: boot the ISO (or a USB stick made by `install.sh`), choose "Install to a hard disk",
and afterwards the machine boots Autarkeia OS from its internal disk (SATA, NVMe, eMMC/SD, USB
disk) with no medium attached, on BIOS and UEFI machines, x86_64 and i386.  The installed system
keeps its root file system, its accounts and `/home` across reboots, can rebuild itself (the
self-hosting tree is installed), and can sync its sources with the upstream repositories (git).
The same disk can also be written from a Linux host: `install.sh --mode disk`.

## Decisions (2026-11-27)

1. **The published default passwords are replaced during the install, always.**  The image's
   root password and the demo accounts (ada, grace, dennis) are public; an installed system never
   keeps them.  The installer asks for a new root password and for the user's own account (name
   and password); the demo accounts are not installed.  There is no "keep" option, and the
   automatic test mode takes its passwords from a config file on the marked test disk, never from
   the image or the kernel command line.
2. **ext4 is the root file system from the first release of the installer.**  e2fsprogs
   (`mke2fs`, `e2fsck`, `tune2fs`) is built with tcc before the installer (stage 1), the root is
   formatted ext4 with its journal, and `/init` runs `e2fsck -p` before mounting it read-write.
   No ext2 interim.
3. **The self-hosting tree is installed by default**, together with **git**, built with tcc, so
   the installed system can fetch, pull and push the project repository and the pinned upstream
   sources.  It can be left out with an option, not the other way round.
4. **The host's `install.sh` gets `--mode disk`**: the same layout, file systems, system and boot
   loader written from a Linux host onto a drive or an image file.

## What exists today, and what it means for the design

| Fact (as of 2026-11-27) | Consequence |
|---|---|
| The whole system runs from RAM: GRUB loads `initramfs.cpio.gz` (51 MB x86_64, 48 MB i386), `rootfstype=ramfs`, `/init` is `userland/busybox-init.c` run by `tcc -run` | an installed system is that same tree unpacked onto a disk partition; nothing on it needs a different build |
| Guest scripts must be C run with `tcc -run` (memory: guest scripts in C) | the installer is `userland/installer.c` and the sync tool `userland/autarkeia-sync.c`, C scripts, not shell scripts |
| busybox has `fdisk` (MBR only, no `FDISK_SUPPORT_LARGE_DISKS`, GPT read off), `mke2fs` (ext2 only), `mkfs.vfat`, `mkswap`/`swapon`, `blkid`, `cpio`, `xz`, `switch_root`, `chpasswd`; no `e2fsck`, no `sfdisk`, no GPT writer | the installer writes the GPT itself (in C); ext4 needs e2fsprogs built with tcc (decision 2) |
| Kernel: `EFI_PARTITION`, `EXT4_FS`, `VFAT_FS`, `NLS_CODEPAGE_437`, `NLS_ISO8859_1`, `EFIVAR_FS`, NVMe, AHCI, ATA_PIIX, USB storage, SDHCI, all built in; no `EFI_STUB` | the kernel mounts a GPT ext4 root with no initramfs (`root=PARTUUID=...`); the firmware cannot start it directly, so GRUB stays the boot loader |
| GRUB is not built by tcc; the host's `grub-mkrescue` makes the ISO (`boot/mkiso.sh`); the host has `/usr/lib/grub/{i386-pc,x86_64-efi,i386-efi}` | the GRUB pieces a disk needs are made on the host at ISO build time and shipped on the ISO; the guest only copies and patches them |
| Firmware lives xz-compressed in the ISO's `/firmware`, found by `userland/firmware-medium.c` through `tcc.firmware=ID` and `/firmware/.id` | the installer finds its payload the same way and copies `/firmware` to `/lib/firmware` on the disk (the loader takes `NAME.xz`) |
| zlib, LibreSSL, curl and expat are already built with tcc (for links/NetSurf/fontconfig); git's own Makefile needs no autotools | git needs no new libraries (stage 1) |
| `out/<arch>/initramfs-selfhost.cpio` (step `selfhost`) holds the pruned sources and the toolchain; `userland/selfhost/build-disk.c` formats only a disk marked with a magic string | the installed self-hosting tree comes from that image; the automatic install test uses the same marked-disk idea |

## Disk layout

GPT on every disk (works past 2 TiB, and UEFI firmware expects it), with a protective MBR that
carries GRUB's BIOS boot sector, so one disk boots on both firmwares:

| # | Size | Type GUID | File system | Content |
|---|---|---|---|---|
| 1 | 1 MiB at LBA 2048 | BIOS boot `21686148-6449-6E6F-744E-656564454649` | none | GRUB's i386-pc `core.img` |
| 2 | 256 MiB | EFI System `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` | FAT32, label `AUTARKEIA` | `EFI/BOOT/BOOTX64.EFI`, `EFI/BOOT/BOOTIA32.EFI`, `boot/grub/grub.cfg`, `boot/bzImage`, `boot/grub/<platform>/` modules |
| 3 | optional, default min(RAM, 2 GiB) | Linux swap `0657FD6D-A4AB-43C4-84E5-0933C84B4F4F` | swap | — |
| 4 | the rest, at least 8 GiB | Linux root x86-64 `4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709` / x86 `44479540-F297-41B2-9AF7-D131D5F0458A` | ext4, label `autarkeia` | the system, the self-hosting tree, `/home` |

The minimum disk is 10 GiB: the self-hosting kernel build alone takes 1.3 GB (i386) to 1.7 GB
(x86_64) of source and objects, and git checkouts of the components need room on top.

The kernel and GRUB's config sit on the FAT partition, so GRUB needs only `part_gpt` and `fat`
and the kernel can be replaced from any OS.  The root partition's PARTUUID is random per install
and is written into `grub.cfg` as `root=PARTUUID=<uuid>`; the disk's own name (`sda`, `nvme0n1`)
is never used, since it changes between machines and boot orders.

Whole-disk install only in stages 1-5; installing next to another OS is stage 6.

## Boot chain of the installed system

1. BIOS: the MBR boot code (GRUB `boot.img`, its core-image LBA patched to 2048) loads `core.img`
   from partition 1.  UEFI: the firmware starts `EFI/BOOT/BOOTX64.EFI` (or `BOOTIA32.EFI` on 32-bit
   UEFI) from the ESP's removable-media path; no NVRAM entry is needed (stage 5 adds one).
2. Both GRUB images carry an embedded early config made on the host:
   `search --no-floppy --file --set=root /autarkeia/installed` then
   `set prefix=($root)/boot/grub` and `configfile`.  The marker file is written by the installer.
   (A second installed disk in the same machine would be ambiguous; GRUB picks the first one it
   finds.  Accepted; documented in the installer's help.)
3. `grub.cfg` (written by the installer; entries: graphical login, graphical mode without a
   login, text console, serial first):
   `linux /boot/bzImage root=PARTUUID=<uuid> rootfstype=ext4 ro rootwait init=/init console=... tcc.xdm`
   with no `initrd` line: the kernel mounts the disk itself, read-only, and boot skips GRUB's slow
   51 MB initrd read (7 s of the ISO boot's black screen were initrd reading).
4. `/init` on the disk is the same `busybox-init.c`; it checks the root with `e2fsck -p` and
   remounts it read-write (stage 3).

## The GRUB pieces, made on the host (`boot/mkiso.sh`)

A new directory `/install/grub/` on the ISO (and on the `install.sh --mode files` stick):

- `boot.img` — `/usr/lib/grub/i386-pc/boot.img` as it is.
- `core.img` — `grub-mkimage -O i386-pc -c early.cfg -p /boot/grub biosdisk part_gpt fat search_fs_file configfile normal ...`.
  Its first sector (`diskboot.img`) holds a block list that `grub-bios-setup` normally fills.
  To get it exactly right, mkiso.sh runs `grub-bios-setup` once against a template disk image with
  the layout above (sparse, built with `sfdisk` on the host) and ships the resulting sectors
  0-439 of the MBR and the 1 MiB of partition 1.  The installer writes those bytes and keeps its
  own partition table (MBR bytes 440-511).  Because partition 1 is always at LBA 2048, the
  shipped blob is valid for every disk; the installer still checks the two patched fields
  (boot.img offset 0x5c = 2048, diskboot's first block-list entry = 2049) before writing.
- `BOOTX64.EFI` — `grub-mkimage -O x86_64-efi -c early.cfg -p /boot/grub part_gpt fat search_fs_file configfile normal linux all_video gfxterm ...`
- `BOOTIA32.EFI` — the same for `i386-efi`, when `/usr/lib/grub/i386-efi` exists.
- `modules/<platform>/` — the `.mod` files `normal` loads later (`linux`, `all_video`, `smbios`,
  `regexp`, `serial`, ...), copied to the ESP's `boot/grub/<platform>/`.
- `SHA256SUMS` over all of them; mkiso.sh fails when a GRUB platform it needs is missing
  (`need_dir`, like install.sh's `need_files_tools`).

`scripts/lib.sh` gains `need_installer_tools` (grub-mkimage, grub-bios-setup, sfdisk) used by
mkiso.sh.  `install.sh --mode disk` uses the same blobs, so the host and the guest installer
write identical boot code.

## New packages built with tcc (stage 1)

- **e2fsprogs** (pinned release tarball in `sources.lock`, `patches/e2fsprogs/`): `mke2fs`
  (`mkfs.ext4`), `e2fsck` (`fsck.ext4`), `tune2fs`, `blkid` is busybox's.  Static, musl, its
  shipped `configure` run as the other components' are (the host has no autotools; the
  configure script itself needs none).  In the busybox image (the installer needs them on the
  live system) and on the disk.
- **git** (pinned release tarball, `patches/git/`): built with its own Makefile and
  `NO_PERL NO_PYTHON NO_TCLTK NO_GETTEXT NO_INSTALL_HARDLINKS`, `CURLDIR`/`OPENSSLDIR` pointing at
  the tcc-built curl and LibreSSL (smart HTTPS transport: `git-remote-https`), zlib, expat
  (`http-push`).  The few git commands that are still shell scripts run under busybox `sh`;
  that is git's own internals, not a guest script of ours.  The CA bundle curl already uses is
  git's `http.sslCAInfo`.  On the disk, and in the self-hosting image (so the live self-hosting
  boot has it too).
- Both are new tcc targets: every tcc bug they turn up is fixed in `src/tinycc`, exported, and
  recorded in FIX-LOG.md and the tcc fix log memory; their test suites (`make check` of e2fsprogs,
  a subset of git's `t/` that runs without Perl) go into the regression notes.

## The installer: `userland/installer.c` (`/usr/sbin/autarkeia-install`)

A C script in the busybox image (added to `userland/initramfs.list`), run as root on a console.
Steps, each printed as it runs:

1. **Find the payload.** Reuse firmware-medium's search (factor the medium search out of
   `firmware-medium.c` into a shared header, or call `firmware-medium --mount-only`): the mounted
   boot medium at `/run/boot-medium` with `boot/bzImage`, `boot/initramfs.cpio.gz`,
   `boot/initramfs-selfhost.cpio`, `/firmware`, `/install/grub/` and `/install/repo.bundle`.
   Extend the search to FAT (the `install.sh --mode files` stick) as well as iso9660.  No medium
   (a direct `run.sh` boot): stop with a clear message.
2. **Choose the disk.** List `/sys/block/*` that are disks (not `loop`, `ram`, `sr`, `zram`), with
   size, model (`device/model`), removable flag and existing partitions/labels (`blkid`).  Refuse
   the disk the medium is on, any disk with a mounted partition, and disks under 10 GiB (under
   4 GiB with `--no-selfhost`).
3. **Confirm.** Show the disk, its size and what is on it, then require typing the disk's name
   (`sda`) — not just "y".  `--yes` skips this only together with `--disk`, and the automatic mode
   (below) only accepts a disk marked `TCC-INSTALL-DISK` in its first bytes.
4. **Accounts (mandatory, decision 1).** A new root password (asked twice, at least 8
   characters, not one of the image's published passwords), the user's login name (checked
   against existing system names) and password.  Stored only as hashes (musl `crypt()` with
   SHA-512, `$6$`); no plain text is written anywhere.
5. **Options** (defaults in brackets): swap partition [min(RAM, 2 GiB), 0 = none]; host name
   [autarkeia]; the default boot entry [graphical login]; copy the firmware [yes, ~120 MB];
   the self-hosting tree and git [yes; `--no-selfhost` leaves them out].
6. **Partition.** Write GPT (primary header at LBA 1, entries at LBA 2-33, backup at the end,
   CRC32 over both) and the protective MBR, in C — ~200 lines, no new tool.  Random disk GUID and
   PARTUUIDs from `/dev/urandom`.  `BLKRRPART` ioctl, then wait for the partition nodes
   (`nvme0n1p4` vs `sda4` naming).
7. **Format.** `mkfs.vfat -F 32 -n AUTARKEIA`, `mkswap`, `mkfs.ext4 -L autarkeia` (e2fsprogs).
8. **Copy the system.** Mount root on `/mnt/target`; `gzip -dc boot/initramfs.cpio.gz | cpio -idm`
   from the medium (the pristine image, not the running RAM tree, which holds runtime state); the
   medium's `/firmware` → `/lib/firmware` (`cp -a`, xz files as they are); the self-hosting cpio on
   top (toolchain and sources); git and e2fsprogs if the base image lacks them.
9. **The self-hosting tree as git repositories.** `/usr/src/autarkeia` is a clone of
   `/install/repo.bundle` (the project repository with its history, made by mkiso.sh with
   `git bundle create --all`), with `origin` set to https://github.com/endotermic/tinycc-os-kernel
   and the checkout at the commit the ISO was built from.  The pruned component sources of the
   self-hosting image become `/usr/src/autarkeia/src/<component>`, each committed as a local git
   repository on branch `patched` with its pinned upstream URL from `sources.lock` as `upstream`,
   so `autarkeia-sync` (stage 4) can fetch the full history later.
10. **System files.** `/etc/fstab` (root by PARTUUID with `errors=remount-ro`, ESP on `/boot/efi`,
    swap), `/etc/hostname`, `/etc/autarkeia-installed` (date, version from `/etc/os-release`, disk
    GUID, the project commit); `/etc/passwd`, `/etc/shadow`, `/etc/group` rewritten: root with the
    new hash, the user's account (home `/home/<name>`, groups for audio, video, input), the demo
    accounts removed; the xdm greeter text (which lists the live image's accounts and passwords)
    replaced by a plain login prompt.
11. **Boot loader.** Mount the ESP; copy `bzImage` to `boot/`, the EFI images to `EFI/BOOT/`, the
    modules to `boot/grub/<platform>/`, write `boot/grub/grub.cfg` and `/autarkeia/installed`; write
    MBR bytes 0-439 and partition 1 from `/install/grub/` (checksums checked first).
12. **Verify and finish.** `sync`; read back and compare the kernel, `grub.cfg` and the GRUB
    blobs; `e2fsck -n` the root after unmounting; print "remove the medium and reboot"; offer to
    reboot.

Entry points:

- a new ISO menu entry "Install Autarkeia OS to a hard disk": `tcc.install` on the command line
  makes `/init` run the installer on the screen console after the banner (text mode, no X), and
  a root shell after it;
- `autarkeia-install` from any root shell, and a JWM menu entry that opens it in mlterm (stage 5);
- automatic: `tcc.install=auto` (and `run.sh --install`, via the SMBIOS OEM string like
  `menu=selfhost`) installs on the one marked disk, then powers off — for tests.  The marked
  disk's first sector holds `TCC-INSTALL-DISK` and a small key=value block (root hash, user name,
  user hash, options) written by the test script; the installer reads it before wiping the disk.

Options: `--disk NAME`, `--yes`, `--swap SIZE`, `--hostname NAME`, `--user NAME`,
`--no-selfhost`, `--no-firmware`, `--auto`, `--list`, `-h`.  Passwords are only ever asked on
the terminal or read as hashes from the marked disk; there is no password option.

## The host side: `install.sh --mode disk`

A third mode next to `raw` and `files`, for a drive attached to the host (USB-SATA adapter,
external SSD, an SD card for a small machine) or, with `--image FILE --size SIZE`, a disk image
for a virtual machine:

- the same GPT layout (host `sfdisk`), `mkfs.vfat`, `mkswap`, `mkfs.ext4` (host e2fsprogs,
  checked with `need_tool mkfs.ext4:e2fsprogs`), the same payload taken from the chosen ISO
  (`xorriso -osirrox` extracts `boot/`, `/firmware`, `/install/`), the same repo bundle and
  component repositories (host `git`), the same boot blobs written with `dd`;
- the same mandatory accounts: it asks for the root password and the user's account, hashes
  them with `openssl passwd -6` (or `mkpasswd`), and writes the same `/etc/passwd`, `/etc/shadow`,
  `/etc/group`, `/etc/fstab`, `grub.cfg` as the guest installer — the file contents come from
  one description (a shared template file under `boot/install/`, read by both `installer.c` and
  install.sh), so the two cannot drift;
- the existing safety rules apply: the menu offers only removable/USB drives, a disk holding a
  mounted system file system is refused, `--device` must name a whole drive, final confirmation
  unless `--yes`; `INSTALL_ALLOW_LOOP=1` for loop devices;
- `--verify` reads back the boot files like `files` mode, and runs `e2fsck -fn` on the root.

## Changes to `/init` (`userland/busybox-init.c`) for a disk root

- Detect the root: `statfs("/")` gives `RAMFS_MAGIC` (live) or `EXT4_SUPER_MAGIC` (installed).
- Installed: `e2fsck -p` on the root device (still read-only); exit code 1 → go on, 2 → reboot,
  4 or more → a root shell on the console with the message to run `e2fsck` by hand.  Then remount
  `/` rw, mount tmpfs on `/tmp` and `/run`, `swapon -a`, `mount -a` (the ESP on `/boot/efi`), set
  the host name; skip `busybox --install -s` when the links already exist (they persist now),
  and skip `firmware-medium` (the files are in `/lib/firmware`).
- On shutdown/reboot: `sync`, `swapoff -a`, unmount `/boot/efi`, remount `/` read-only before
  `reboot(2)`.
- The banner says "installed on <disk> (PARTUUID ...)" instead of "running from RAM".
- Anything that writes into `/etc` at boot must not grow files on every boot (check
  `udhcpc-script.c`, resolv.conf, the xdm and startx scripts).

## Stages

1. **Packages and boot pieces** — e2fsprogs and git built with tcc on both arches (new steps in
   `scripts/build.sh`, in `all`); mkiso.sh makes `/install/grub/` for i386-pc, x86_64-efi and
   i386-efi and `/install/repo.bundle`.  A host-only test writes the boot pieces onto a sparse
   image together with a hand-made ext4 root (host `sfdisk`, `mkfs`) and boots it in QEMU on
   SeaBIOS and OVMF to the banner.  This proves the boot chain before any guest code exists.
2. **`install.sh --mode disk`** — the host writer, built on the stage-1 test code and the shared
   templates; its `--image` output is the first real installed system and the test bed for
   stage 3.
3. **`/init` on a disk root** — the busybox-init.c changes above, including `e2fsck -p` and the
   read-only remount at shutdown; tested on the stage-2 image (also after killing QEMU mid-write:
   the next boot must replay the journal and come up).
4. **The guest installer, text mode** — `installer.c`, the ISO entry, `tcc.install=auto`,
   `run.sh --install`, `tests/installer/run.sh` (below); and `autarkeia-sync` (a C script):
   `git -C /usr/src/autarkeia pull`, then for each component fetch the pinned commit from
   `upstream` when `sources.lock` changed and re-apply the patch series — the guest-side
   equivalent of `scripts/fetch.sh` + `scripts/patch.sh`.  Both arches, BIOS and UEFI.
5. **Comfort** — the JWM menu entry and mlterm front end; an NVRAM boot entry through efivarfs
   (`Boot####` + `BootOrder`, so firmware that ignores the removable path still finds it);
   `--upgrade`: replace the kernel, the GRUB files and the system files from a newer medium (or
   from a kernel built by the self-hosting tree) while keeping `/etc/passwd`, `/etc/shadow`,
   `/etc/group`, `/etc/hostname`, `/home`, `/root` and `/usr/src/autarkeia`.
6. **Next to another OS** — install into free space or a chosen partition on a disk that already
   has GPT, reuse an existing ESP (add `EFI/autarkeia/grubx64.efi` + an NVRAM entry instead of
   overwriting `EFI/BOOT/`), never touch BIOS boot code there.  Only after stages 1-5 have been
   used on real hardware.

## Tests

- `tests/installer/run.sh [--arch ARCH] [--efi]`: a fresh sparse 12 GiB image marked
  `TCC-INSTALL-DISK` with test passwords (random per run, hashed into the marker block); QEMU 1
  boots the ISO with `tcc.install=auto` and the image attached and must power off with
  "install: done" on serial; QEMU 2 boots only from the image (no CD), must reach the banner with
  "installed on", log in as the new user with the test password, and fail to log in with every
  published image password (root, ada, grace, dennis); QEMU 3 boots it again after writing a file
  in `/home/<user>` from QEMU 2 and checks the file is there.  Matrix: x86_64 SeaBIOS, x86_64 OVMF
  (`OVMF_CODE_4M.fd`), i386 SeaBIOS, i386 on 32-bit OVMF (q35, as `run.sh --efi`).  Disks on
  AHCI, NVMe and IDE in turn, so the `sda`/`nvme0n1p` naming is covered.
- Self-hosting on the installed disk: `build-kernel` runs on the ext4 root (no separate build
  disk needed), the new kernel boots; `git -C /usr/src/autarkeia fetch` and `git log` work over
  HTTPS when the test grants network (QEMU user networking), `git status` is clean after install.
- `install.sh --mode disk --image` (host): the same QEMU 2/3 checks on its output; and the two
  installers' system files compared (`/etc/fstab`, `/etc/passwd` shape, `grub.cfg` minus UUIDs).
- Every QEMU run inside a `systemd-run --user --scope -p MemoryMax=` with a timeout and capped
  output (memory: careful test runs; the host has 6 GB).
- Refusal tests: the installer must refuse an unmarked disk in auto mode, the medium's own disk,
  a disk with a mounted partition, a too-small disk, an empty or published password, and a
  marker block without passwords.
- devices-test and selfhost-test stay green (the initramfs changes are small; the self-hosting
  image grows by git and e2fsprogs).
- Real hardware last: the Acer Aspire that booted the x86_64 ISO on 2026-11-19, on a spare disk.

## GRUB built by tcc (a separate track)

The installer uses GRUB images made by the host's grub-mkimage (stage 1). On 2026-11-27 a trial
build of GRUB 2.14 with tcc (compiler) and tld (linker) was made in a scratch directory to see
how far that is: the x86_64-efi target builds completely (kernel.img and 293 modules), and after
tcc fix 147 (`__attribute__((ms_abi))`, FIX-LOG.md) the image runs on OVMF: it reads grub.cfg
from a FAT partition, lists disks and files, prints a file, halts. GRUB's host tools were still
built by gcc, and the trial needed workarounds for these open gaps:

| Gap | Where | Effect on GRUB |
|---|---|---|
| R_X86_64_PC16 left for 16-bit branches inside a section (GNU as resolves them) | tcc assembler | the `linux` module (relocator16.S) does not load: no Linux boot yet |
| multi-pass assembly does not reset `.code16/32/64` between passes | tcc assembler | relocator32.S fails; `.code64` at the top works around it |
| `(%rsi, 1)` (base with scale, no index) rejected | tcc assembler | relocator sources |
| no 16-bit addressing modes (`(%si)`, `(%di)`, `6(%bp)`) | tcc assembler | i386-pc boot code, relocator16.S |
| forward jumps not relaxed to the short form | tcc assembler | i386-pc boot.S exceeds 512 bytes |
| no `-S` (assembly output) | tcc | i386-pc `rs_decoder.h` |
| `-mrtd` ignored silently | tcc | i386-pc ABI (harmless in GRUB 2.14's asm) |
| `-mcmodel=large` ignored | tcc | cross-module PC32/PLT32, rejected by GRUB's module verifier (the trial let them through; fine below 4 GB) |
| extern data through GOTPCRELX | tcc | rejected by the verifier; `#pragma GCC visibility push(hidden)` avoids it |
| `_Pragma` in `tcc -E`; no `__GNUC__`, `__INT_FAST32_TYPE__`, `__INT_LEAST*`; `.stab` from `-g` | tcc | configure and build steps |
| no `-Ttext`, no `__bss_start`/`_edata`/`_end`; strip without `-K`/`-o`; objcopy without `--add-section`; nm without `-P` | tld | link and module packaging |
| `asorti` | host awk (no gawk) | genmoddep.awk |

## Open questions

- git's release and e2fsprogs' release to pin (the latest at the time stage 1 starts).
- Should the image's demo accounts stay in the live ISO at all, now that an installed system
  never has them?  (They are what the live xdm greeter shows; the plan keeps them live-only.)
- Full component history for `autarkeia-sync`: fetch shallow at the pinned commit (as fetch.sh
  does; the kernel is the big one) or deepen on request?  The plan: shallow by default,
  `--deepen` on request.
