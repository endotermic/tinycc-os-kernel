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

## Group 6: sound
- HDA codecs: `SND_HDA_CODEC_HDMI`, `CIRRUS`, `SIGMATEL`, `ANALOG`, `VIA`, `CA0132`.
- `SND_SOC` + SOF/SoundWire for Intel laptops with digital microphones (ACPI + firmware).

## Group 7: power and the rest
- `PM`, `SUSPEND`, `THERMAL`, `X86_MCE`, `EFI` + `EFIVAR_FS`, `INTEL_IOMMU`, `NTFS3_FS`.
- i386 only: `HIGHMEM4G` is on since 2026-11-17 (the self-hosting guest's final link of the
  kernel with the Wi-Fi drivers ran out of the 896 MB of low memory); still no PAE/NX, so at
  most 4 GB; x86_64 has neither limit.
