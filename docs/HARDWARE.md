# Real hardware: the driver roadmap

`kernels/linux/drivers.config` covers what QEMU emulates and a few common real devices. This list
names what a real PC or laptop needs on top of it, in the order it is planned.  Every group is
built in (no modules) and compiled by tcc, so each one grows the kernel and the self-hosting
build and may turn up new tcc bugs (record them in FIX-LOG.md).  Mark a group done here when it
is in drivers.config and verified.

Status as of 2026-11-17 (the audit of the i386 `.config` of that day).

## Group 1: ACPI, internal storage, real-time clock — DONE on i386 (2026-11-17)

In drivers.config.  i386: built by tcc with no tcc change (2305 tcc / 0 gcc invocations, bzImage
6 574 080 -> 7 360 512 bytes); in QEMU (`-device ahci` + `ide-hd`, `-device nvme`, SMP=4) the
ACPI interpreter starts, all 4 CPUs come up, the AHCI disk is `sda`, the NVMe one `nvme0n1`,
rtc_cmos sets the clock, `/sys/class/dmi/id` is there; devices-test 13/13; selfhost-test all
(generation 2 built in the guest in 3m44s, 2764 tcc / 0 gcc, boots).  The self-hosting run found
that a fresh tinyconfig turns IOSCHED_BFQ on where the incrementally merged host tree had it off
(the pruned guest source then lacked block/bfq-*.c): drivers.config pins it off.  Not tested:
real hardware, SD readers (QEMU's `sdhci-pci` is untested), x86_64.

| Need | Options | Without it |
|---|---|---|
| ACPI | `ACPI`, `ACPI_BUTTON`, `ACPI_AC`, `ACPI_BATTERY`, `ACPI_THERMAL`, `ACPI_FAN`, `ACPI_PROCESSOR` | no CPU enumeration on firmware without MP tables (1 CPU), interrupt routing guessed, no power-off, no battery/AC/lid/power button |
| SATA | `ATA`, `SATA_AHCI`, `ATA_PIIX` (older Intel and QEMU's IDE) | the internal disk is invisible |
| NVMe | `BLK_DEV_NVME` | the internal SSD is invisible |
| SD card readers | `MMC`, `MMC_BLOCK`, `MMC_SDHCI`, `MMC_SDHCI_PCI`, `MISC_RTSX_PCI` + `MMC_REALTEK_PCI` | no SD card |
| Filesystems | `EXFAT_FS`, `ISO9660_FS`, `NLS_UTF8`, `USB_UAS` | SD cards over 32 GB, CDs/ISOs, fast USB 3 disks |
| RTC | `RTC_CLASS`, `RTC_DRV_CMOS`, `RTC_HCTOSYS` | the clock starts in 1970 and TLS certificate checks fail |
| DMI | `DMI`, `DMIID` | the quirk tables of i8042, HDA, touchpads and ACPI do not apply |

## Group 2: Wi-Fi that modern laptops have
- Drivers: `IWLWIFI` + `IWLMVM` (Intel), `RTW88`/`RTW89` (Realtek), `MT7921E` (MediaTek),
  `ATH10K`/`ATH11K` (Qualcomm), `BRCMFMAC` (Broadcom).  Only `ATH9K` is in today.
- Every one of them needs firmware files: a `/lib/firmware` in the image (linux-firmware subset
  per driver; licences!) or `EXTRA_FIRMWARE` built into the kernel.
- Userland: `wpa_supplicant` (and `iw`) are not in the image; without them no WPA network.

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
- i386 only: no `HIGHMEM` (about 896 MB of RAM usable) and no PAE/NX; x86_64 has neither limit.
