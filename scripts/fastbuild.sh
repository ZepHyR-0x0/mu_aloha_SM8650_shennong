#!/bin/bash
#
# Fast build: sources live on the Windows drive (L:) for editing/git, but the
# build tree lives on WSL's ext4 (/root/mubuild) because /mnt/l is 9p and
# dominates build time (24 cores sat at load 1.26).
#
# Usage:  wsl -d Ubuntu-20.04 -- bash /mnt/l/Desktop/ProjectMu/mu_aloha_platforms_sm8650-main/scripts/fastbuild.sh [extra args]
#
set -e

SRC="/mnt/l/Desktop/ProjectMu/mu_aloha_platforms_sm8650-main"
DST="/root/mubuild"
VENV="$SRC/.venv"

mkdir -p "$DST"

echo "==> syncing sources (incremental) $SRC -> $DST"
# NOTE: no --delete-excluded here - it would wipe /root/mubuild/Build and force
# a full rebuild every time. Excluded paths are simply left untouched.
rsync -a --exclude=Build --exclude=.git --exclude=.venv "$SRC/" "$DST/"

cd "$DST"
# shellcheck disable=SC1091
source "$VENV/bin/activate"

echo "==> building (nproc=$(nproc))"
set -o pipefail
#
# Build type. DEBUG by default: the bring-up is at the point where missing DEBUG()
# output costs more than the extra image size, and PcdDebugPropertyMask is 0x07 in
# this tree so asserts print without halting. Pass "-t RELEASE" explicitly to override.
#
BUILD_TYPE="${BUILD_TYPE:-DEBUG}"
case " $* " in
  *" -t "*) EXTRA_ARGS="" ;;
  *)        EXTRA_ARGS="-t $BUILD_TYPE" ;;
esac
echo "==> build type: ${BUILD_TYPE} (override with BUILD_TYPE=... or -t ...)"
./build_uefi.py -d xiaomi-shennong $EXTRA_ARGS "$@" 2>&1 | tee /root/mubuild/build.log

# build_uefi.py returns 0 even when the EDK2 build fails, so check the log.
if grep -qa "CRITICAL - Build failed\|^ERROR - Error" "$DST/Build/BUILDLOG_ShennongPkg.txt" 2>/dev/null; then
  echo "!! BUILD FAILED - see $DST/Build/BUILDLOG_ShennongPkg.txt"
  grep -a "error:" "$DST/Build/BUILDLOG_ShennongPkg.txt" | head -10 | cut -c1-200
  exit 1
fi

echo "==> copying artifact back to the Windows drive"
mkdir -p "$SRC/Build/ShennongPkg"
cp -f "$DST/Build/ShennongPkg/xiaomi-shennong.img" "$SRC/Build/ShennongPkg/xiaomi-shennong.img"
ls -la "$SRC/Build/ShennongPkg/xiaomi-shennong.img"
