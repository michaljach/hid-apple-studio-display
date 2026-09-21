# hid-apple-studio-display

Linux kernel driver that puts the brightness of an **Apple Studio Display**
(2022) or **Studio Display XDR** (2026) under `/sys/class/backlight`, attached
to the DRM connector the display is on — the same layout GPU drivers use for
laptop panels. Desktop environments then treat it as an ordinary backlight:
GNOME (mutter ≥ 48) shows its brightness slider for the monitor, brightness
keys work, and tools like `brightnessctl` or `light` can drive it.

```
$ ls /sys/class/backlight/
apple_studio_display0 -> ../../devices/pci0000:00/.../drm/card1/card1-DP-1/apple_studio_display0
$ brightnessctl -d apple_studio_display0 set 50%
```

Tested on a Studio Display XDR with Linux 7.2 and GNOME 50 (NVIDIA, DisplayPort
for video + the display's USB link to the host). Compiles against every LTS
kernel from 5.10 up (checked in CI).

## Requirements

- The display's **USB/Thunderbolt link connected to the host** — brightness is
  a USB HID control, not DDC/CI. Over Thunderbolt/USB‑C this is automatic; on a
  DisplayPort GPU you need an adapter that also passes USB data (the display's
  speakers, camera and hub come with it).
- Linux ≥ 5.10 and its headers.
- For the GNOME slider: mutter ≥ 48. Anything else that talks to
  `/sys/class/backlight` works regardless of desktop.

## Install

### Any distribution

```sh
git clone https://github.com/michaljach/hid-apple-studio-display
cd hid-apple-studio-display
sudo ./install.sh
```

`install.sh` installs the kernel headers and DKMS with your package manager
(apt, dnf/yum, zypper, pacman, xbps, apk), registers the module with DKMS so it
is rebuilt on every kernel update, and loads it. Where DKMS does not exist
(Alpine, Gentoo, …) it builds and installs the module for the running kernel
instead. `sudo ./uninstall.sh` reverses all of it.

Without the script: `make dkms-install` (DKMS) or `make && sudo make install`
(this kernel only).

### Distribution packages

| Distribution | |
|---|---|
| Arch / Manjaro / EndeavourOS | `packaging/arch/PKGBUILD` — `cd packaging/arch && makepkg -si` (AUR: `hid-apple-studio-display-dkms`) |
| Debian / Ubuntu / Mint / Pop!_OS | `cp -r packaging/debian debian && dpkg-buildpackage -us -uc -b`, then `apt install ../hid-apple-studio-display-dkms_*.deb` |
| Fedora / RHEL / Rocky / Alma / openSUSE | `rpmbuild -ba packaging/rpm/hid-apple-studio-display-dkms.spec` (needs `dkms`, from EPEL on RHEL‑likes) |

All three ship the same thing: the source in `/usr/src/` plus a DKMS
registration, so the module follows kernel updates.

### Secure Boot

Out-of-tree modules must be signed when Secure Boot is on. DKMS signs its
builds with a machine-owner key; enroll it once and reboot:

```sh
sudo mokutil --import /var/lib/dkms/mok.pub     # Ubuntu: /var/lib/shim-signed/mok/MOK.der
```

## Verify

```sh
dmesg | grep apple-studio
#   apple-studio-display ...: backlight apple_studio_display0 registered on card1-DP-1 (400..60000, now 30000)
cat /sys/class/backlight/apple_studio_display0/actual_brightness
```

GNOME only looks for backlights when its monitor list changes, so the **first
time** after installing either re-plug the display or re-apply the display
configuration (Settings → Displays). From the next boot on the module is loaded
before the session starts and nothing needs doing.

## Module parameters

| Parameter | Default | |
|---|---|---|
| `connector` | *(auto)* | DRM connector the display is on, e.g. `DP-1`. Auto-detection picks the single connected connector; with several monitors connected you must set this. |
| `fade_ms` | `0` | Transition time the display applies to brightness changes, in ms. |

Set them in `/etc/modprobe.d/hid-apple-studio-display.conf`:

```
options hid-apple-studio-display connector=DP-1 fade_ms=150
```

## How it works

The display's USB device carries several HID interfaces; one of them is a
*Monitor Control* collection (usage page 0x80). Its feature report 1 holds the
brightness as a 32-bit value in 0.01-nit units — logical range 400–60000, i.e.
4–600 nits — followed by a 16-bit transition time in milliseconds. The driver
binds that interface, reads the field layout from the report descriptor rather
than hard-coding it, and registers a `raw` backlight device with the display's
DRM connector as parent, which is what mutter requires before it will attach a
backlight to an external monitor. The display's other interfaces (vendor and
sensor-hub collections) are passed through to the generic HID paths untouched.

`brightness` is in the display's native units. Values below the hardware
minimum (400) are raised to it: the panel can be dimmed but not switched off
through this control. Requests wake the (autosuspended) display first.

## Known limitations

- **No change notifications.** The descriptor advertises an input report for
  brightness changes, but the interrupt endpoint behind it fails on the XDR;
  keeping it open makes usbhid reset the display every few seconds. Changes made
  by other means are only seen when the value is read.
- Brightness is linear in nits, so half of the slider is 300 nits. A
  perceptual curve would feel closer to macOS.
- The 2022 Studio Display (USB ID `05ac:1114`) uses the same report layout but
  has not been tested with this driver.
- Finding the connector walks the device tree (PCI display controller →
  `drm/cardN` → connector). Non-PCI GPUs are not handled; set `connector=`.
- With HDR enabled in GNOME, mutter uses its own reference-white control
  instead of the backlight.

## Contributing

The driver is `hid-apple-studio-display.c`; everything else is packaging. CI
compiles it against each supported LTS kernel plus current stable, runs
checkpatch, and builds the Debian package. Please keep `checkpatch.pl --strict`
clean and bump the version in `dkms.conf`, `MODULE_VERSION`, the PKGBUILD, the
spec and `debian/changelog` together (CI checks they agree).

## License

GPL-2.0-only. See `LICENSE`.
