# kernels/

Per-kernel build inputs. `scripts/build.sh` reads `kernels/<name>/` for the kernel named by
the component; today only `linux/`:

* `minimal.config`: Kconfig fragment applied on top of `make tinyconfig` (x86_64, serial
  console, VGA/framebuffer console with keyboard, initramfs, the few options the busybox
  shell needs). Together with `drivers.config` the single source of truth
  for the configuration; `build.sh` reports every line that `olddefconfig` dropped.
* `drivers.config`: the device drivers (2026-10-26), merged after `minimal.config`: PCI;
  TCP/IP (IPv4 and IPv6) with the Ethernet drivers e1000/e1000e, RTL8139/RTL8169, virtio-net
  and USB Ethernet (CDC, RNDIS); Wi-Fi (cfg80211/mac80211, ath9k, and the mac80211_hwsim
  virtual radios for testing); Bluetooth (btusb, hci_uart, the vhci virtual controller);
  ALSA with HD Audio, AC'97 and USB audio and the OSS `/dev/dsp` emulation; USB (xHCI,
  EHCI, OHCI, UHCI, HID, mass storage with the SCSI disk layer, FAT and ext4); the PS/2 and
  USB mouse with `/dev/input/mice` and evdev; KMS video (DRM with fbdev emulation:
  simpledrm for the firmware's framebuffer, bochs-drm for QEMU's VGA, virtio-gpu), which
  sets the mode the display's EDID prefers, i.e. the native resolution. Everything built
  in, no modules. `run.sh` attaches QEMU's counterparts by default and
  `scripts/devices-test.sh` checks them all (`userland/devices-test.c` in the guest).
* `x86_64.config` / `i386.config`: the word size and what differs per architecture.
* `tcc.config`: fragment applied only for the tcc build (`SECTION_MISMATCH_WARN_ONLY`,
  because tcc does not inline `__init`-only `static inline` helpers).
* `tcc-kcc.c`: the `CC=` wrapper that presents tcc to Kbuild (GCC identity macros,
  `-U__OPTIMIZE__`, `-Wunsupported` under `-Werror`, the `-dumpmachine`/`-Wa,--version`/
  `-print-file-name` answers; nothing goes to gcc). `tcc-hostcc.c` is HOSTCC, `objtool-stub.c`
  OBJTOOL. All three are C programs: `scripts/build.sh kernel` compiles them with the host tcc
  into `build/<arch>/kcc/`, and the self-hosting image carries the sources as `tcc -run`
  scripts (`/usr/bin/tcc-kcc` ...), the only form of script that system has.

Adding a kernel: create `kernels/<name>/` with its configuration and wrapper, a
`patches/<name>/` series, an entry in `sources.lock`, and a `step_kernel` variant in
`scripts/build.sh` selected by the component.
