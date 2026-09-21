#!/bin/sh
# Removes hid-apple-studio-display: unloads the module, unregisters every
# version from DKMS (or deletes a plain "make install" copy) and cleans
# /usr/src.
set -eu

MOD=hid-apple-studio-display
KVER=$(uname -r)
have() { command -v "$1" >/dev/null 2>&1; }

if [ "$(id -u)" -ne 0 ]; then
	if have sudo; then exec sudo -- "$0" "$@"
	elif have doas; then exec doas -- "$0" "$@"
	else echo "run this as root" >&2; exit 1
	fi
fi

modprobe -r "$MOD" 2>/dev/null || true

if have dkms; then
	dkms status -m "$MOD" 2>/dev/null | sed -n 's|^\([^,:]*/[^,:]*\).*|\1|p' | sort -u | while read -r mv; do
		echo "==> dkms remove $mv"
		dkms remove "$mv" --all || true
	done
fi
rm -rf "/usr/src/$MOD-"*

# plain "make install" copies
rm -f /lib/modules/*/extra/"$MOD".ko*
depmod -a "$KVER"

echo "==> $MOD removed"
