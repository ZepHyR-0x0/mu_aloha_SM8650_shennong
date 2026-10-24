#!/usr/bin/env bash
#
# Switch the xiaomi-shennong DXE profile.
#
#   full : complete port - 40 Qualcomm prebuilt drivers + UEFI core.
#          Currently stops at EnvDxe (EFI_NOT_FOUND) during DXE dispatch.
#   min  : pure UEFI core + console, zero Qualcomm binaries. Proves the
#          BootShim / FD window / PrePi / DxeCore / memory map / GOP / ConOut
#          chain is sound.
#
# Usage:  scripts/profile.sh full|min
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEV="$HERE/../Platforms/ShennongPkg/Device/xiaomi-shennong"

PROF="${1:-}"
case "$PROF" in
  full|min) ;;
  *) echo "usage: $(basename "$0") full|min" >&2; exit 2 ;;
esac

for f in "APRIORI.$PROF.inc" "DXE.$PROF.inc"; do
  if [ ! -f "$DEV/profiles/$f" ]; then
    echo "missing profile file: $DEV/profiles/$f" >&2
    exit 1
  fi
done

cp "$DEV/profiles/APRIORI.$PROF.inc" "$DEV/APRIORI.inc"
cp "$DEV/profiles/DXE.$PROF.inc"     "$DEV/DXE.inc"

echo "profile  = $PROF"
echo "  APRIORI.inc : $(grep -cE '^[[:space:]]+INF ' "$DEV/APRIORI.inc") INF entries"
echo "  DXE.inc     : $(grep -cE '^INF ' "$DEV/DXE.inc") INF entries"
echo
FDF="$HERE/../Platforms/ShennongPkg/Shennong.fdf"

# ---------------------------------------------------------------------------
# Stray FV applications.
#
# BdsDxe enumerates every APPLICATION-type FFS file in the firmware volume and
# creates a boot option for it, so these get launched even though nothing asks
# for them:
#   ufpdevicefw  - Microsoft UFP (USB Functional Port) device firmware, a
#                  vendor-specific USB device-mode protocol for host-driven
#                  flashing. Prebuilt, and it dies at +0x2224 with FAR=0x8 right
#                  after DisplayInit fails to find a GOP.
#   LinuxLoader  - Qualcomm kernel loader; needs the QC stack (UFS, TzDxe, ...).
# The min profile removes both so the shell is the only boot target.
# ---------------------------------------------------------------------------
if [ -f "$FDF" ]; then
  if [ "$PROF" = "min" ]; then
    sed -i -E 's@^([[:space:]]*)(INF[[:space:]]+(AndromedaPkg/UFP/ufpdevicefw|QcomModulePkg/Application/LinuxLoader/LinuxLoader)\.inf).*$@\1#\2   # min profile: stray FV app@' "$FDF"
  else
    #
    # Only LinuxLoader comes back for the non-min profiles.
    #
    # ufpdevicefw must stay out of EVERY profile: it is the driver that takes
    # the data abort at FAR=0x8 (ufpdevicefw + 0x2224) as soon as BDS starts,
    # because S0UsblInitComboPhy failed with "SSBus is NULL". An earlier version
    # of this script un-commented both here, which silently undid the fix and
    # cost several flash cycles - the source tree looked right but the module
    # was back in the FV every time.
    #
    sed -i -E 's@^([[:space:]]*)#[[:space:]]*(INF[[:space:]]+QcomModulePkg/Application/LinuxLoader/LinuxLoader\.inf).*$@\1\2@' "$FDF"
    sed -i -E 's@^([[:space:]]*)(INF[[:space:]]+AndromedaPkg/UFP/ufpdevicefw\.inf).*$@\1#\2   # crashes at +0x2224 (FAR=0x8) on this platform@' "$FDF"
  fi
  echo "  stray FV apps: $(grep -cE '^[[:space:]]*#?[[:space:]]*INF[[:space:]]+(AndromedaPkg/UFP/ufpdevicefw|QcomModulePkg/Application/LinuxLoader)' "$FDF") entries"
  echo "    ufpdevicefw : $(grep -cE '^[[:space:]]*INF[[:space:]]+AndromedaPkg/UFP/ufpdevicefw' "$FDF") active (must be 0)"
  echo "    LinuxLoader : $(grep -cE '^[[:space:]]*INF[[:space:]]+QcomModulePkg/Application/LinuxLoader' "$FDF") active"
fi

echo "Rebuild with:  scripts/fastbuild.sh"
