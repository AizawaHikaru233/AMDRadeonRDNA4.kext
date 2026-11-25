#!/bin/bash
#
# install-kext-esp.sh — put a built kext into the OpenCore ESP and prove it landed.
#
#   sudo bash install-kext-esp.sh [path/to/AMDRadeonRDNA4.kext]
#
# Default kext: <repo>/Build/AMDRadeonRDNA4-0.4.0-RELEASE/AMDRadeonRDNA4.kext next to this script's tree.
#
# Why this exists: the earlier one-liner mounted every EFI volume with `diskutil mount` and searched
# for OpenCore.efi. After a reboot the disk identifiers change (disk0s2 <-> disk1s2), and
# `diskutil mount` sometimes refuses a healthy FAT32 ESP ("Volume on disk1s2 failed to mount") while a
# plain `mount -t msdos` succeeds. Both cost a round trip each time a kext has to go in. This script
# tries both, in order, and only accepts a volume that really is the OpenCore one.
#
# It never edits config.plist: Kernel -> Add has to be enabled already (it was, at install time).
# It unmounts what it mounted and prints the md5 of both sides; a MISMATCH exits non-zero.
set -u
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
KEXT="${1:-$REPO/Build/AMDRadeonRDNA4-0.4.0-RELEASE/AMDRadeonRDNA4.kext}"
[ -d "$KEXT" ] || { echo "no kext at $KEXT"; exit 1; }
BUNDLE="$(basename "$KEXT")"
BIN="$KEXT/Contents/MacOS/${BUNDLE%.kext}"

MOUNT=/tmp/esp-install
mkdir -p "$MOUNT"

mounted_by_us=""
find_esp() {
	for id in $(diskutil list | awk '/EFI /{print $NF}'); do
		# 1) the normal way
		if diskutil mount "$id" >/dev/null 2>&1; then
			mp=$(diskutil info -plist "$id" 2>/dev/null | plutil -p - 2>/dev/null | awk -F'"' '/"MountPoint"/{print $4}')
			if [ -n "${mp:-}" ] && [ -f "$mp/EFI/OC/OpenCore.efi" ]; then
				mounted_by_us="$id"
				echo "$mp"
				return 0
			fi
			diskutil unmount "$id" >/dev/null 2>&1
		fi
		# 2) the fallback that works when diskutil refuses a sound volume
		mount -t msdos "/dev/$id" "$MOUNT" 2>/dev/null || continue
		if [ -f "$MOUNT/EFI/OC/OpenCore.efi" ]; then
			mounted_by_us="$MOUNT"
			echo "$MOUNT"
			return 0
		fi
		umount "$MOUNT" 2>/dev/null
	done
	return 1
}

ESP=$(find_esp) || { echo "no EFI volume carries EFI/OC/OpenCore.efi"; exit 1; }
echo "ESP: $ESP"
ls "$ESP/EFI/OC/Kexts" | tr '\n' ' '; echo

rm -rf "$ESP/EFI/OC/Kexts/$BUNDLE"
cp -aR "$KEXT" "$ESP/EFI/OC/Kexts/" || { echo "copy failed"; exit 1; }
sync

a=$(md5 -q "$ESP/EFI/OC/Kexts/$BUNDLE/Contents/MacOS/${BUNDLE%.kext}" 2>/dev/null)
b=$(md5 -q "$BIN" 2>/dev/null)
echo "esp=$a"; echo "build=$b"
ok=1
[ -n "$a" ] && [ "$a" = "$b" ] && echo MATCH || { echo MISMATCH; ok=0; }

if ! plutil -p "$ESP/EFI/OC/config.plist" 2>/dev/null | grep -A3 "\"$BUNDLE\"" | grep -q '"Enabled" => true'; then
	echo "WARNING: config.plist does not show $BUNDLE with Enabled = true"
	ok=0
fi

case "$mounted_by_us" in
/tmp/esp-install|"$MOUNT") umount "$MOUNT" 2>/dev/null || true ;;
*) diskutil unmount "$mounted_by_us" >/dev/null 2>&1 || true ;;
esac
echo "reboot to load it: sudo shutdown -r now"
[ "$ok" -eq 1 ]
