#!/bin/sh
# Installs hid-apple-studio-display on any distribution:
#   1. installs the kernel headers for the running kernel and DKMS with the
#      distribution's package manager (apt, dnf, yum, zypper, pacman, xbps, apk)
#   2. registers the module with DKMS so it is rebuilt on every kernel update,
#      or, where DKMS is unavailable, builds and installs it for this kernel
#   3. loads the module
#   4. installs the udev rule and helpers that make iio-sensor-proxy use the
#      display's front light sensor and give the orientation interface to this
#      driver, plus the (experimental, opt-in) asd-autorotate helper
#
# Usage: sudo ./install.sh            (or just ./install.sh; it re-runs itself
#                                      through sudo/doas when needed)
set -eu

cd "$(dirname "$0")"
MOD=hid-apple-studio-display
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
KVER=$(uname -r)

log()  { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m==> %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31m==> %s\033[0m\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

if [ "$(id -u)" -ne 0 ]; then
	if have sudo; then exec sudo -- "$0" "$@"
	elif have doas; then exec doas -- "$0" "$@"
	else die "run this as root"
	fi
fi

headers_present() { [ -e "/lib/modules/$KVER/build/Makefile" ]; }

# --- 1. build dependencies -------------------------------------------------

if ! headers_present || ! have dkms; then
	log "Installing kernel headers for $KVER and DKMS"
	if have apt-get; then
		apt-get install -y "linux-headers-$KVER" dkms build-essential
	elif have dnf; then
		dnf install -y "kernel-devel-uname-r == $KVER" gcc make \
			|| dnf install -y kernel-devel gcc make
		dnf install -y dkms \
			|| warn "dkms is not available (on RHEL/Rocky/Alma enable EPEL); continuing without it"
	elif have yum; then
		yum install -y "kernel-devel-uname-r == $KVER" gcc make || yum install -y kernel-devel gcc make
		yum install -y dkms || warn "dkms is not available (enable EPEL); continuing without it"
	elif have zypper; then
		flavor=${KVER##*-}
		zypper --non-interactive install "kernel-$flavor-devel" dkms gcc make
	elif have pacman; then
		case "$KVER" in
			*-rt-lts*)   hdr=linux-rt-lts-headers ;;
			*-rt*)       hdr=linux-rt-headers ;;
			*-lts*)      hdr=linux-lts-headers ;;
			*-zen*)      hdr=linux-zen-headers ;;
			*-hardened*) hdr=linux-hardened-headers ;;
			*)           hdr=linux-headers ;;
		esac
		pacman -S --needed --noconfirm "$hdr" dkms
	elif have xbps-install; then
		xbps-install -Sy dkms "linux$(echo "$KVER" | cut -d. -f1,2)-headers"
	elif have apk; then
		apk add build-base linux-lts-dev || apk add build-base linux-virt-dev
	elif have emerge; then
		warn "Gentoo: expecting a configured kernel tree at /usr/src/linux; install sys-kernel/dkms for automatic rebuilds"
	else
		warn "Unknown distribution: install the headers for kernel $KVER (and optionally dkms) yourself"
	fi
fi

headers_present || die "kernel headers for $KVER not found at /lib/modules/$KVER/build"

# --- 2. install ------------------------------------------------------------

if have dkms; then
	log "Registering $MOD $VER with DKMS"
	# drop any version previously registered so an upgrade or reinstall is clean
	dkms status -m "$MOD" 2>/dev/null | sed -n 's|^\([^,:]*/[^,:]*\).*|\1|p' | sort -u | while read -r mv; do
		dkms remove "$mv" --all >/dev/null 2>&1 || true
	done
	rm -rf "/usr/src/$MOD-"*
	mkdir -p "/usr/src/$MOD-$VER"
	cp "$MOD.c" Makefile dkms.conf "/usr/src/$MOD-$VER/"
	dkms install "$MOD/$VER" || true
	dkms status -m "$MOD" 2>/dev/null | grep -q "$MOD/$VER, $KVER.*installed" \
		|| die "DKMS build failed; see /var/lib/dkms/$MOD/$VER/build/make.log"
else
	warn "DKMS not available: building for $KVER only (rerun this script after kernel updates)"
	make KVER="$KVER"
	make KVER="$KVER" install
fi

# --- 3. load ---------------------------------------------------------------

modprobe -r "$MOD" 2>/dev/null || true
if ! modprobe "$MOD"; then
	if [ -d /sys/firmware/efi/efivars ] && have mokutil && mokutil --sb-state 2>/dev/null | grep -q enabled; then
		die "loading failed and Secure Boot is enabled: the module must be signed with an enrolled key.
    DKMS signs modules with /var/lib/dkms/mok.pub; enroll it once with
        mokutil --import /var/lib/dkms/mok.pub
    then reboot, choose 'Enroll MOK' and run this script again."
	fi
	die "modprobe $MOD failed; see 'dmesg | tail'"
fi

# --- 4. desktop integration ------------------------------------------------

install -Dm755 contrib/asd-als-role /usr/lib/udev/asd-als-role
install -Dm755 contrib/asd-bind-orientation /usr/lib/udev/asd-bind-orientation
install -Dm644 contrib/90-hid-apple-studio-display.rules /etc/udev/rules.d/90-hid-apple-studio-display.rules
install -Dm755 contrib/asd-autorotate /usr/local/bin/asd-autorotate
install -Dm644 contrib/asd-autorotate.service /usr/local/lib/systemd/user/asd-autorotate.service
/usr/lib/udev/asd-bind-orientation
if have udevadm; then
	udevadm control --reload
	udevadm trigger --subsystem-match=iio --action=change 2>/dev/null || true
fi
have systemctl && systemctl try-restart iio-sensor-proxy.service 2>/dev/null || true

log "Installed. The module now loads automatically whenever the display is plugged in."
sleep 1
bl=
for d in /sys/class/backlight/apple_studio_display*; do
	[ -e "$d" ] && bl=$d && break
done
if [ -n "$bl" ]; then
	printf '    backlight: %s\n    connector: %s\n    brightness: %s / %s\n' \
		"$bl" "$(basename "$(dirname "$(readlink -f "$bl")")")" \
		"$(cat "$bl/brightness")" "$(cat "$bl/max_brightness")"
	printf '    GNOME picks up a new backlight on the next monitor change:\n    re-plug the display, or re-apply the display settings once.\n'
	for d in /sys/bus/iio/devices/iio:device*; do
		[ -e "$d/name" ] && [ "$(cat "$d/name")" = apple_studio_display_orientation ] \
			&& printf '    orientation sensor: %s (x=%s y=%s z=%s)\n' "$d" \
				"$(cat "$d/in_incli_x_raw")" "$(cat "$d/in_incli_y_raw")" "$(cat "$d/in_incli_z_raw")"
	done
	printf '    Automatic brightness: gsd-power does not re-claim the light sensor after the\n    iio-sensor-proxy restart above; toggle Settings > Power > Automatic Screen\n    Brightness off and on once (or log out and in).\n'
	printf '    Auto-rotate (experimental): systemctl --user enable --now asd-autorotate\n'
else
	printf '    No display found yet: plug the Studio Display'"'"'s USB/Thunderbolt link in and check\n    /sys/class/backlight/ or "dmesg | grep apple-studio".\n'
fi
