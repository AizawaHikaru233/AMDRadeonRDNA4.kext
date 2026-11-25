#!/bin/bash
#
# hdr-evidence.sh — one shot of everything that decides "does macOS consider this display HDR", from the machine
# that has the display. Answers the question the driver cannot: CoreDisplay's verdict is userspace.
#
#   sudo bash hdr-evidence.sh [minutes]        (default 10; how far back the WindowServer log is read)
#
# Output: hdr-evidence-<timestamp>.txt next to this script and on the terminal.
#
# What it reads and why (docs/hdr-userspace-gating.md has the disassembled chain):
#   * "Attempting to get capabilities from plane with no gamma" — the failing gate today. Goal: 0.
#   * "hdr-policy = 0x…" — CoreDisplay prints it only after the plane's capability word is non-zero. Its
#     absence is the same diagnosis from the other side. Goal: the line appears.
#   * "InstallTiming - legacy…" — how the display's timing install went (0xe00002c7 = kIOReturnUnsupported).
#   * the driver's own ring log, which is the only place our hdr:/colour: lines exist.
#   * SLSDisplaySupportsHDRMode / SLSDisplayIsHDRModeEnabled — the verdict itself, but only from inside a
#     logged-in session: over SSH (and with launchctl asuser/bsexec) CGGetOnlineDisplayList comes back empty,
#     so a "no" measured that way is void, not evidence. The script says which case it is.
#
# Read-only: it starts nothing, changes nothing, restarts nothing.
set -u
[ "$(id -u)" -eq 0 ] || { echo "run with sudo (the log and the driver ring need root)"; exit 1; }
MIN=${1:-10}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/hdr-evidence-$(date +%Y%m%d-%H%M%S).txt"
exec > >(tee "$OUT") 2>&1

echo "=== hdr-evidence $(date) ==="
echo "--- system"
sw_vers
sysctl -n kern.osproductversion 2>/dev/null
uptime
echo "panic reports, newest first:"
ls -t /Library/Logs/DiagnosticReports/Kernel-*.panic 2>/dev/null | head -3
echo "boot-args: $(nvram boot-args 2>/dev/null | tr '\t' ' ')"

echo
echo "--- driver"
kextstat -l -b com.hackintosh.AMDRadeonRDNA4 2>/dev/null | tail -1
ioreg -r -d 1 -w0 -l -k 'AMDRadeonRDNA4,Build' 2>/dev/null | grep -E '"AMDRadeonRDNA4,Build"' | head -2
ioreg -r -d 1 -w0 -l -k 'Modes,Count' 2>/dev/null | grep -E '"Modes,Count"' | head -2
echo "driver options: hdr=$(sysctl -n debug.rdna4_hdr 2>/dev/null || echo 'n/a (no such sysctl)')" \
     "rot180=$(sysctl -n debug.rdna4_rot180 2>/dev/null || echo n/a)"
echo "driver ring, hdr/colour/modes lines:"
sysctl -n debug.rdna4_log 2>/dev/null | grep -E 'hdr:|colour:|modes: [0-9]+ mode|modes: id [0-9]+ .*(HDR10|live)' | tail -40

echo
echo "--- WindowServer / CoreDisplay, last ${MIN} min"
WSLOG="$(log show --last "${MIN}m" --info --debug --predicate 'process == "WindowServer"' 2>/dev/null)"
count() { printf '%s\n' "$WSLOG" | grep -c -- "$1"; }
echo "plane with no gamma .......... $(count 'plane with no gamma')     (goal: 0)"
echo "hdr-policy ................... $(count 'hdr-policy')             (goal: >=1; 0 means the capability word is still empty)"
echo "InstallTiming - legacy ....... $(count 'InstallTiming - legacy')  (failures: $(count 'InstallTiming - legacy.*FAILED'))"
echo "0xe00002c7 ................... $(count '0xe00002c7')"
echo "HDR mentions ................. $(count 'HDR\|hdr')"
echo "last matching lines:"
printf '%s\n' "$WSLOG" | grep -E 'plane with no gamma|hdr-policy|InstallTiming|HDR' | tail -12

echo
echo "--- the verdict (needs a logged-in session)"
CONSOLE_OWNER=$(stat -f%Su /dev/console 2>/dev/null)
echo "console owner: ${CONSOLE_OWNER:-unknown}; logged in: $(who | wc -l | tr -d ' ')"
if [ "${CONSOLE_OWNER:-root}" = "root" ]; then
	echo "VOID: no user session. CGGetOnlineDisplayList returns 0 over SSH and at the login window, so a False"
	echo "      measured now says nothing about the display (docs/hdr-30bpp-plan.md, A6/A7)."
else
	/usr/bin/python3 - "$CONSOLE_OWNER" <<'PY'
import ctypes, sys
from ctypes import c_uint32, c_bool, byref
cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
cg.CGMainDisplayID.restype = c_uint32
cg.CGGetOnlineDisplayList.argtypes = [c_uint32, ctypes.POINTER(c_uint32), ctypes.POINTER(c_uint32)]
sl = ctypes.CDLL("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight")
for name in ("SLSDisplaySupportsHDRMode", "SLSDisplayIsHDRModeEnabled"):
    fn = getattr(sl, name, None)
    if fn is None:
        print(f"{name}: not exported by this SkyLight")
        continue
    fn.argtypes = [c_uint32]
    fn.restype = c_bool
ids = (c_uint32 * 8)()
n = c_uint32(0)
cg.CGGetOnlineDisplayList(8, ids, byref(n))
print(f"online displays: {n.value} (main 0x{cg.CGMainDisplayID():x})")
if n.value == 0:
    print("VOID again: this process is not in the session after all; run it from Terminal.app inside the desktop.")
for i in range(n.value):
    d = ids[i]
    sup = sl.SLSDisplaySupportsHDRMode(d) if hasattr(sl, "SLSDisplaySupportsHDRMode") else None
    en = sl.SLSDisplayIsHDRModeEnabled(d) if hasattr(sl, "SLSDisplayIsHDRModeEnabled") else None
    print(f"display 0x{d:x}: supportsHDR={bool(sup)} hdrEnabled={bool(en)}")
PY
fi

echo
echo "written to $OUT"
