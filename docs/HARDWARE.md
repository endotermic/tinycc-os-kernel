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

## Group 3: GPU and display
- `DRM_I915` (Intel), `DRM_AMDGPU` (+ firmware) / `DRM_RADEON`, `DRM_NOUVEAU`.
- `BACKLIGHT_CLASS_DEVICE`, `ACPI_VIDEO` (brightness keys), `MTRR` + `X86_PAT` (write-combined
  framebuffer).
- Today: simpledrm on the firmware framebuffer only (no mode change, no external monitor).

## Group 4: input
- `HID_MULTITOUCH`, `I2C_HID_ACPI`, `I2C_DESIGNWARE_PLATFORM`, `INTEL_LPSS(_PCI)`, `PINCTRL` +
  the Intel/AMD pin controllers: I2C precision touchpads (almost every laptop since ~2016).
- `MOUSE_PS2_ELANTECH`, `INPUT_TOUCHSCREEN`.

## Group 5: Ethernet and USB
- `IGB`, `IGC` (Intel I225/I226), `TIGON3`, `ALX`, `ATL1C`, `USB_RTL8152` (most USB-C dongles).
- `PCIEPORTBUS`, `HOTPLUG_PCI`, `USB4`/`THUNDERBOLT`.

## Group 6: sound
- HDA codecs: `SND_HDA_CODEC_HDMI`, `CIRRUS`, `SIGMATEL`, `ANALOG`, `VIA`, `CA0132`.
- `SND_SOC` + SOF/SoundWire for Intel laptops with digital microphones (ACPI + firmware).

## Group 7: power and the rest
- `PM`, `SUSPEND`, `THERMAL`, `X86_MCE`, `EFI` + `EFIVAR_FS`, `INTEL_IOMMU`, `NTFS3_FS`.
- i386 only: `HIGHMEM4G` is on since 2026-11-17 (the self-hosting guest's final link of the
  kernel with the Wi-Fi drivers ran out of the 896 MB of low memory); still no PAE/NX, so at
  most 4 GB; x86_64 has neither limit.
