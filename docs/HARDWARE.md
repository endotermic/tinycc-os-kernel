# Real hardware: the driver roadmap

`kernels/linux/drivers.config` covers what QEMU emulates and a few common real devices. This list
names what a real PC or laptop needs on top of it, in the order it is planned.  Every group is
built in (no modules) and compiled by tcc, so each one grows the kernel and the self-hosting
build and may turn up new tcc bugs (record them in FIX-LOG.md).  Mark a group done here when it
is in drivers.config and verified.

Status as of 2026-11-17 (the audit of the i386 `.config` of that day).

## Group 1: ACPI, internal storage, real-time clock — DONE (2026-11-17)

x86_64: clean full build (3212 tcc / 0 gcc, bzImage 8 328 704 bytes), the same QEMU check
(ACPI, 4 CPUs, sda, nvme0n1, rtc_cmos sets the clock), devices-test 13/13, selfhost-test all
(generation 2 built in the guest in 4m19s, boots).

In drivers.config.  i386: built by tcc with no tcc change (2305 tcc / 0 gcc invocations, bzImage
6 574 080 -> 7 360 512 bytes); in QEMU (`-device ahci` + `ide-hd`, `-device nvme`, SMP=4) the
ACPI interpreter starts, all 4 CPUs come up, the AHCI disk is `sda`, the NVMe one `nvme0n1`,
rtc_cmos sets the clock, `/sys/class/dmi/id` is there; devices-test 13/13; selfhost-test all
(generation 2 built in the guest in 3m44s, 2764 tcc / 0 gcc, boots).  The self-hosting run found
that a fresh tinyconfig turns IOSCHED_BFQ on where the incrementally merged host tree had it off
(the pruned guest source then lacked block/bfq-*.c): drivers.config pins it off.  Not tested:
real hardware, SD readers (QEMU's `sdhci-pci` is untested).

| Need | Options | Without it |
|---|---|---|
| ACPI | `ACPI`, `ACPI_BUTTON`, `ACPI_AC`, `ACPI_BATTERY`, `ACPI_THERMAL`, `ACPI_FAN`, `ACPI_PROCESSOR` | no CPU enumeration on firmware without MP tables (1 CPU), interrupt routing guessed, no power-off, no battery/AC/lid/power button |
| SATA | `ATA`, `SATA_AHCI`, `ATA_PIIX` (older Intel and QEMU's IDE) | the internal disk is invisible |
| NVMe | `BLK_DEV_NVME` | the internal SSD is invisible |
| SD card readers | `MMC`, `MMC_BLOCK`, `MMC_SDHCI`, `MMC_SDHCI_PCI`, `MISC_RTSX_PCI` + `MMC_REALTEK_PCI` | no SD card |
| Filesystems | `EXFAT_FS`, `ISO9660_FS`, `NLS_UTF8`, `USB_UAS` | SD cards over 32 GB, CDs/ISOs, fast USB 3 disks |
| RTC | `RTC_CLASS`, `RTC_DRV_CMOS`, `RTC_HCTOSYS` | the clock starts in 1970 and TLS certificate checks fail |
| DMI | `DMI`, `DMIID` | the quirk tables of i8042, HDA, touchpads and ACPI do not apply |

## Group 2: Wi-Fi that modern laptops have — DONE (2026-11-17)

- Drivers (drivers.config): Intel `IWLWIFI` with `IWLDVM`, `IWLMVM`, `IWLMLD`; Realtek
  `RTW88` (8821CE, 8822BE/CE, 8723DE) and `RTW89` (8851BE, 8852AE/BE/BTE/CE, 8922AE); MediaTek
  `MT7921E` (MT7921/MT7922) and `MT7925E`; Qualcomm `ATH10K_PCI`, `ATH11K_PCI`, `ATH12K`;
  Broadcom `BRCMFMAC_PCIE`; plus `HWMON` (ath12k needs it to link).  bzImage x86_64
  8.3 -> 10.8 MB, i386 7.4 -> 9.7 MB.
- tcc fixes 134 (an array initialized from a parenthesized string literal: rtw88) and 135
  (unreferenced static const data is not emitted: iwlwifi's `mei_ops`), docs/FIX-LOG.md.
- Firmware: `scripts/build.sh firmware` (scripts/firmware-select.py) copies what the drivers
  request (`modules.builtin.modinfo`) from linux-firmware (a partial clone, `#sparse` lines of
  sources.lock) and wireless-regdb into `out/<arch>/firmware.cpio.gz`: 216 files, 130 MB
  unpacked, 58 MB packed, with the licences.  WHENCE's links resolve (duplicates become
  symlinks), an older API version stands in for a versioned name that is not out yet, and an
  iwlwifi name brings the other steppings of its family (MODULE_FIRMWARE lists only one).
  Left out: access-point chips (QCA988X, QCA9887, QCN9074, QCN9274); 43 PCIe names are not in
  linux-firmware at all (chips announced before their firmware: Intel dr/sc2, MT7935, a few
  Broadcom parts).  The ISO's busybox entries load it as a second initrd (`run.sh --iso` then
  gives 768 MB); `run.sh --firmware` appends it for the direct boot.
- Userland (`scripts/build.sh libnl wifi`): libnl 3.12 (core + genl), wpa_supplicant and
  hostapd 2.12 (nl80211, WPA2/WPA3 SAE/OWE, 802.11r, EAP with LibreSSL), iw 6.17, all built
  by tcc unpatched; the guest's `wifi` command (userland/wifi/wifi.c: status, scan, connect
  NAME [PASSWORD] with DHCP, off).
- Verified in QEMU, both arches: devices-test 14/14, its new `wpa` check (hostapd on one
  mac80211_hwsim radio, `wifi connect` from the other: association COMPLETED with SAE);
  the firmware image unpacks into `/lib/firmware` and cfg80211 loads `regulatory.db` from it;
  selfhost-test all.  Not tested: any real card.

## Group 3: GPU and display — DONE (2026-11-17)

- Drivers (drivers.config): Intel `DRM_I915` and `DRM_XE` (with `X86_PLATFORM_DEVICES`), AMD
  `DRM_AMDGPU` with SI and CIK support and the DC display engine, `DRM_RADEON`, NVIDIA
  `DRM_NOUVEAU`; `BACKLIGHT_CLASS_DEVICE`, `ACPI_VIDEO`, `MTRR` and `X86_PAT` (the boot log
  shows PAT's write-combining entry).  simpledrm keeps the screen until one of them probes.
  x86_64 bzImage 10.8 -> 18.3 MB (5 196 compiler runs from clean).
- i386 leaves out amdgpu and xe (i386.config): their GPUs postdate 32-bit-only PCs, and
  amdgpu's DC converts doubles to 64-bit integers, which tcc's i386 back end does through
  libtcc1's __fixdfdi/__fixsfdi (3 700 references, not linked into a kernel).  i386
  bzImage 9.7 -> 12.3 MB.  scripts/build.sh's fragment check now lets a later fragment
  override an earlier one.
- tcc fixes 136 (`__VA_OPT__`: xe), 137 (GNU `, ## args` pastes the argument unexpanded:
  nouveau), 138 (`__builtin_constant_p` in a constant expression: nouveau) and 139 (u64 <->
  double inline on x86: amdgpu's DC), docs/FIX-LOG.md.
- Firmware: the firmware image takes what i915, xe, amdgpu, radeon and nouveau request from
  linux-firmware's i915/, xe/, amdgpu/, radeon/ and nvidia/ (without NVIDIA's GSP firmware,
  145 MB, which sources.lock does not check out: nouveau drives the GPUs before Ada without
  it; Ada and later stay on simpledrm or the iGPU), without AMD's datacenter accelerators
  (MI100/MI200/MI300/MI350).  The image grew to 1 517 files, 257 MB unpacked (amdgpu 100 MB,
  i915 11 MB, radeon 6 MB, xe 5.5 MB, nvidia 3.3 MB), 104 MB packed; `run.sh --firmware`
  and `--iso` give 1 GB.  Missing: firmware of unreleased AMD parts (GC 11.5.4/11.5.6/12.1,
  PSP 15) and the GSP files.
- Self-hosting: the pruned tree now carries the Makefiles that others include through
  `$(src)/` (i915's gvt/, nouveau's Kbuild chain) and xe's `*.rules` (the input of its generated
  xe_wa_oob.h).  The tree and the objects grew to 1.8 GB (x86_64) and 1.3 GB (i386), which no
  longer fit a tmpfs in the guest's RAM (the i386 link was OOM-killed at 1.5 GB): the guest
  builds on a disk image that `run.sh --selfhost` attaches (8 GB sparse, marked
  "TCC-BUILD-DISK"; userland/selfhost/build-disk formats it ext2 and mounts it on /build).
- Verified in QEMU: both kernels boot, devices-test 14/14 (the screen still on bochs-drm),
  selfhost-test host and all on both arches with the default 1 GB (generation 2 built in
  12 min 7 s on x86_64, 7 min 52 s on i386, and boots).  Not tested: any real GPU.

## Group 4: touchpads and touchscreens — DONE (2026-11-18)

- I2C HID (drivers.config): `I2C_HID_ACPI` and `HID_MULTITOUCH` (precision touchpads and HID
  touchscreens, ACPI PNP0C50), on the DesignWare I2C controllers of the chipset
  (`I2C_DESIGNWARE_PLATFORM`/`_PCI`/`_BAYTRAIL`, Intel LPSS through `X86_INTEL_LPSS` and
  `MFD_INTEL_LPSS_ACPI`/`_PCI`, AMD through `X86_AMD_PLATFORM_DEVICE`, and `I2C_AMD_MP2` for
  AMD's sensor-hub bridge), with the pin controllers their interrupt GPIOs come from: Intel
  Bay Trail, Cherry View, Lynx Point, Sunrise Point, Broxton, Gemini Lake, Cannon Lake, Ice
  Lake, Jasper Lake, Elkhart Lake, Tiger Lake, Alder Lake, Meteor Lake, Meteor Point and
  the ACPI-described Intel platform controller; `PINCTRL_AMD`.
- PS/2 and SMBus: `MOUSE_PS2_ELANTECH` (+ SMBus), Synaptics InterTouch through RMI4 over SMBus
  (`RMI4_SMB`, F03/F11/F12/F30, `HID_RMI`), Elan I2C/SMBus touchpads (`MOUSE_ELAN_I2C`), the
  SMBus controllers they sit on (`I2C_I801`, `I2C_PIIX4`).
- Touchscreens: `INPUT_TOUCHSCREEN` with `TOUCHSCREEN_GOODIX` (tablets, 2-in-1s); the HID
  ones go through hid-multitouch.
- X: the touchpads reach TinyX through /dev/input/mice (mousedev's touchpad mode: relative
  motion from the absolute position, a tap shorter than 200 ms is a left click; the physical
  button clicks too); no gestures, no two-finger scroll.
- tcc fix 140 (`__builtin_constant_p` of an address: the Intel pinctrl tables),
  docs/FIX-LOG.md.  Both arches take the whole group.
- Verified in QEMU (which has no I2C HID device): devices-test's new `touchpad` check makes a
  precision touchpad through `/dev/uhid` (`UHID`), as an I2C HID device on the HID bus;
  hid-multitouch binds it and a finger sliding right comes out of /dev/input/mice as motion
  to the right.  15/15 on both arches; i801 probes QEMU's SMBus.  selfhost-test all on
  both (i386 8 min 10 s at 1 GB; x86_64 13 min at 1.5 GB: at 1 GB the guest's `ld` was
  OOM-killed linking vmlinux, so `run.sh --selfhost` now gives 1536M).  Not tested: a real
  touchpad, the LPSS/AMD I2C controllers and the pin controllers.

## Group 5: Ethernet, PCIe ports, USB4 — DONE (2026-11-18)

- Wired Ethernet (drivers.config), next to e1000/e1000e/r8169/8139 and virtio: Intel `IGB`
  (82575/82576, I210/I211/I350) and `IGC` (I225/I226 2.5G), Broadcom `TIGON3`, Qualcomm
  Atheros `ALX`, `ATL1C`, `ATL1E`, Marvell `SKY2`, JMicron `JME`, Aquantia `AQTION`.
- USB Ethernet: Realtek `USB_RTL8152` (RTL8152/8153/8156: most USB-C dongles and docks),
  `USB_LAN78XX`, `USB_NET_SMSC95XX`, `USB_NET_AQC111`, next to CDC ECM/NCM, RNDIS and AX88179.
- PCIe: `PCIEPORTBUS` with `PCIEAER`, `HOTPLUG_PCI` (`_PCIE` native and `_ACPI`).
- USB4/Thunderbolt 3+: `USB4` (the connection manager: PCIe and DisplayPort tunnels of docks)
  and `USB4_NET`; USB-C port management through ACPI: `TYPEC`, `TYPEC_UCSI`, `UCSI_ACPI`.
- Firmware: linux-firmware's `tigon/` (tg3) is now checked out; `rtl_nic/` was already there
  (r8169) and now also gives r8152 its RTL8153/8156 files.  rtl8157/8159 are not in
  linux-firmware yet.
- No tcc fix was needed (2 784 compiler runs for the step).
- Verified in QEMU: devices-test's new `igb` check (QEMU's 82576 on a user network of its own:
  /init's DHCP lease on eth1 and an ICMP echo through it), 16/16 on both arches; on q35 with
  the igb behind a `pcie-root-port` the port driver binds with AER, the NIC works through it
  and acpiphp registers the slots (QEMU's firmware keeps native hotplug, so pciehp is not
  exercised).  selfhost-test all on both (i386 9 min 10 s, x86_64 15 min 14 s at 1.5 GB).
  bzImage: x86_64 18.6 -> 19.3 MB, i386 12.6 -> 13.3 MB.  Not tested: any real NIC,
  USB4/Thunderbolt, UCSI.

## Group 6: sound and HDMI audio — DONE (2026-11-18)

- HD Audio codecs (drivers.config), next to Realtek, Conexant and the generic one: Analog
  Devices, SigmaTel/IDT, VIA, Senary, Creative CA0110 and CA0132 (with its DSP), C-Media,
  Cirrus Logic (`SND_HDA_CODEC_CIRRUS`: CS420x/CS421x, CS8409).
- HDMI and DisplayPort audio: `SND_HDA_CODEC_HDMI` with the Intel, AMD/ATI and NVIDIA
  variants and the generic one; the GPU drivers of group 3 hand it the monitors' ELD.
- The speaker amplifiers behind the codec of recent laptops: Cirrus CS35L41/CS35L56, TI
  TAS2781 (I2C and SPI), Awinic AW88399; `SPI_PXA2XX` and `SPI_AMD` for the SPI ones.  They
  read their calibration from EFI variables, so the kernel now has `EFI` (UEFI runtime
  services; GRUB still boots it through the BIOS protocol, no EFI stub): from group 7.
- Digital microphones and DSP audio: Intel SOF (`SND_SOC_SOF_*`, Skylake .. Panther Lake,
  HDA link + codec, SoundWire) with the common machine drivers (HDA + DMIC, SoundWire, RT5682,
  NAU8825, DA7219, ES8336, CS42L42, SSP amplifiers); AMD ACP for Renoir, Yellow Carp (ACP6.x)
  and Pink Sardine (ACP6.3/7.x) DMICs.  snd-intel-dsp-config chooses SOF only on machines
  whose ACPI announces a DMIC or SoundWire.
- Firmware: SOF's firmware and topologies are not in linux-firmware: sources.lock pins
  sof-bin 2026.09.2 (17.5 MB tarball), and the firmware step copies its `sof*` directories to
  `/lib/firmware/intel` (611 files, 58 MB); linux-firmware's `cirrus/` (the amplifiers' tuning
  per laptop model, 1 388 files, 9 MB), `ti/audio/tas2781` (all 93 files and WHENCE's 124
  links to them, through firmware-select.py's new `--all`: the driver builds the names from the
  laptop's subsystem id), `ctefx.bin` (CA0132) are checked out now.  The firmware image: x86_64 123 MB packed (+18 MB), i386 85 MB.
- tcc fix 141 (a small static function with an `if` of constant condition is inlined, as
  gcc: efi_is_mixed() without CONFIG_EFI_MIXED), docs/FIX-LOG.md.  bzImage x86_64 19.3 ->
  20.7 MB, i386 13.3 -> 14.4 MB.  Both arches take the whole group.
- `SERIAL_MULTI_INSTANTIATE` is pinned on (the amplifiers' ACPI node names both of a pair;
  the drivers only imply it, so the host tree kept it off while the guest's fresh
  configuration turned it on, and the pruned self-hosting tree lacked its source).
- The ISOs now carry GRUB's 32-bit UEFI loader too (BOOTIA32.EFI next to BOOTX64.EFI) when the
  host has grub-efi-ia32-bin (installed 2026-11-18): the i386 ISO for 32-bit UEFI tablets.
- Verified in QEMU: devices-test 16/16 on both arches (sound on intel-hda + hda-duplex, the
  generic codec); the x86_64 ISO on OVMF (EFI runtime services set up, /init at 1 GB with the
  bigger firmware image); the i386 ISO on the 64-bit OVMF through qemu-system-x86_64 (boots,
  "No EFI runtime due to 32/64-bit mismatch", as any 32-bit kernel there; no 32-bit OVMF
  package in this Ubuntu, so BOOTIA32.EFI itself is untested); selfhost-test all on both
  (i386 10 min 50 s, x86_64 16 min at 1.5 GB).
  QEMU emulates no HDMI codec, DSP, SoundWire or amplifier: not tested on real hardware.

## Group 7: power, the CPU's health, IOMMU, NTFS, laptop keys — DONE (2026-11-19)

- Power (drivers.config): `PM` (the devices' runtime power management) and `SUSPEND`: suspend
  to RAM (ACPI S3) and to idle (s2idle, the "modern standby" of recent laptops) through
  `/sys/power/state` or busybox `rtcwake`; no hibernation (no swap).  `INTEL_IDLE` (the
  CPUs' deep C-states); cpufreq with the Intel and AMD P-state drivers was on already.
- The CPU's health: `X86_MCE` (Intel, AMD: memory and cache errors are reported), the core
  temperatures (`SENSORS_CORETEMP`, `SENSORS_K10TEMP`), Intel's DPTF devices
  (`INT340X_THERMAL`); ACPI thermal zones and fans were on already.
- `IOMMU_SUPPORT` with `INTEL_IOMMU` (VT-d), `AMD_IOMMU` and `IRQ_REMAP`: DMA from
  Thunderbolt/USB4 devices is confined, x2APIC machines get interrupt remapping.
- `NTFS3_FS` (read and write, with the LZX/Xpress decompression of Windows' compact files).
- The laptops' keys and switches: Intel HID events and virtual buttons (the tablet switch of
  2-in-1s), `THINKPAD_ACPI`, `IDEAPAD_LAPTOP`, `ASUS_NB_WMI`, `HP_WMI`, `DELL_WMI`, `ACER_WMI`.
- `EFI_VARS`/`EFIVAR_FS`: on since `EFI` came with group 6.
- i386: `X86_PAE` (i386.config): PAE page tables give the 32-bit kernel the NX bit;
  `HIGHMEM64G` is gone from the kernel, so a 32-bit kernel still uses at most 4 GB.  AMD's
  IOMMU, interrupt remapping and int340x are 64-bit only.
- tcc fixes 142 (an inlined stub's unused parameter given an address: no store, so the
  structure it names can go: the Dell drivers) and 143 (`.p2align 4,,7`: the i386 resume
  code), docs/FIX-LOG.md.  bzImage x86_64 20.7 -> 21.5 MB, i386 14.4 -> 15.1 MB.
- Verified in QEMU: devices-test's new `suspend` (busybox rtcwake -m mem -s 3: S3 entered,
  the CMOS alarm wakes the machine, the network answers afterwards) and `ntfs` (a disk made
  by the host's mkntfs + ntfscp: the file read, another written and read back after a
  remount) checks, 18/18 on both arches (NX active on both, i386 through PAE); the device checks on q35 with QEMU's Intel IOMMU
  (translated DMA, interrupt remapping in x2APIC mode): network, sound, USB, storage and
  suspend work through it; the x86_64 ISO on OVMF; selfhost-test all on both (i386 11 min,
  x86_64 15 min 46 s).  Not tested: real laptops' S3/s2idle
  (firmware and drivers decide), the platform drivers, MCE reports.

## Firmware from the boot medium (2026-11-25)

- The ISO's busybox entries loaded the firmware image (`out/<arch>/firmware.cpio.gz`, 123 MB
  packed on x86_64) as a second initrd.  GRUB reads initrds through the BIOS or UEFI disk
  calls: in QEMU (SeaBIOS, IDE CD) 190 MB took 12.6 s of black screen between the menu and
  the kernel's first line, the kernel's own decompression 0.5 s of it; on OVMF more than two
  minutes.  The kernel then unpacked 320 MB into the ramfs (1 GB of RAM needed).
- Now the ISO has a `/firmware` directory instead (boot/mkiso.sh: the image unpacked, every
  file xz-compressed with CRC32, links to files renamed NAME.xz -> TARGET.xz; 105 MB, made
  once per image in `build/<arch>/firmware-iso`), and every entry's command line names it by
  `tcc.firmware=ID` (the image's checksum, also in `/firmware/.id`).  The kernel gets
  `BLK_DEV_SR` (CDs, QEMU's IDE CD) and `FW_LOADER_COMPRESS_XZ`.  `/init` runs
  `/usr/sbin/firmware-medium` (userland/firmware-medium.c) before the network: it mounts
  every `sr*`/`sd*` as iso9660 on `/run/boot-medium` until one has the ID (up to 20 s: a USB
  stick enumerates after `/init` starts), writes the directory to
  `/sys/module/firmware_class/parameters/path`, and probes again the devices that missed
  their files, since the drivers are built in and probe before `/init`: every unbound PCI
  device and USB interface, and every device that logged a warning about firmware
  (`/dev/kmsg` records with a `DEVICE=`) is unbound and probed again; not storage or USB
  controllers, bridges, hubs or USB disks, and not a display driver that drives the screen.
  i915's files (4.5 MB packed) and `regulatory.db` stay in the initramfs for that reason
  (`build/<arch>/firmware/early`, the firmware step).
- Measured in QEMU, x86_64: menu to the kernel's first line 12.6 s -> 5.2 s (SeaBIOS) and
  more than 120 s -> 49 s (OVMF), `/init` at 21.6 s -> 10.9 s after power-on, the ISO
  207 -> 199 MB, RAM for the ISO back to 640 MB (543 MB total, 388 MB available at the
  prompt).  Verified: the CD found as `/dev/sr0`; the ISO as a USB stick found as `/dev/sda`
  0.9 s after `/init`; a firmware file loaded from the medium through the xz decoder (`iw reg
  reload` with `/lib/firmware/regulatory.db` removed: works, fails with the path cleared);
  the kmsg scan.  Not tested: the probing again with a real card (QEMU emulates none whose
  driver needs firmware; its ati-vga with radeon dies in radeon's bandwidth code, a divide
  error, before /init).

## Virtual machines: VirtualBox and VMware displays (2026-11-26)

- `drivers.config` gets `DRM_VMWGFX` (VMware's SVGA II adapter; VirtualBox's VMSVGA, the
  default graphics controller of its Linux guests) and `DRM_VBOXVIDEO` (VirtualBox's VBoxVGA
  and VBoxSVGA), with `HYPERVISOR_GUEST`, which vmwgfx depends on (the hypervisor detection
  of `arch/x86/kernel/cpu/`, including VMware's hypercall in `vmware.c`; `PARAVIRT` stays
  off).  Before, these machines had simpledrm at the mode the firmware or GRUB set.  Both
  arches; one tcc fix (144: `(%dx)` as the port operand of `in`/`out`, and `vmmcall`, both in
  VMware's hypercall).  bzImage x86_64 21.5 -> 21.6 MB, i386 15.1 -> 15.3 MB.
- Verified in VirtualBox 7.2 (BIOS, the ISO on an IDE CD, a throwaway VM), both arches:
  VMSVGA -> `fb0: vmwgfxdrmfb` (Screen Target display unit, command buffers) and the xdm login
  on it at 1280x1024; VBoxVGA -> `fb0: vboxvideodrmfb` and the login at 1024x768.  vmwgfx logs
  "running on an unsupported hypervisor" in VirtualBox, as with any distribution's kernel.
  VBoxSVGA (meant for Windows guests): vboxvideo binds and the login shows, but it logs
  `vbva_enable failed` and `hgsmi_get_mode_hints failed: -5`; not looked into, VirtualBox
  recommends VMSVGA for Linux.  QEMU's `-vga vmware`: vmwgfx refuses it ("Hardware has no
  pitchlock", probe error -38), an upstream limit of the driver on QEMU's SVGA II; use
  QEMU's standard VGA (bochs-drm) or virtio there.  Not tested: VMware Workstation/ESXi
  themselves, resizing the VM window (TinyX does not follow a mode change), mouse
  integration (needs vmmouse/vboxguest and an X driver; the USB tablet works).
- Regressions: devices-test 18/18 and selfhost-test all on both arches (the guest's tcc
  builds the kernel with both drivers; x86_64 17 min, i386 11 min), tcc `make test`.

## Real hardware reports

- 2026-11-19, Acer Aspire laptop, x86_64 ISO (commit 37af92b): boots; the desktop (xdm
  login, JWM, mlterm, the other X programs) works.  links -g is very slow to draw, but
  resizing its window no longer hangs.  Cause: Xfbdev drew in the frame buffer itself, video
  memory that is very slow to read (scrolling, blending, compositing read it); since
  patches/tinyx/features/0007 it draws in a shadow in RAM when it measures slow reads
  (FIX-LOG C30; /tmp/Xfbdev.log names the choice).  To re-test on the laptop.  Not yet checked there: Wi-Fi, sound, suspend,
  the touchpad driver in use, the internal disk.
