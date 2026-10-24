#!/usr/bin/env bash
#
# Apply this overlay (and the port patches) onto a Project Mu Aloha baseline.
#
# Usage:
#   ./bootstrap.sh /path/to/base
#   BASE_REPO=... BASE_REF=... ./bootstrap.sh /path/to/base
#
set -euo pipefail

BASE_DIR=${1:?usage: bootstrap.sh <base-dir>}
OVERLAY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

BASE_REPO=${BASE_REPO:-https://github.com/Project-Aloha/mu_aloha_platforms}
BASE_REF=${BASE_REF:-dev/update/mu202502}

if [ ! -d "$BASE_DIR/.git" ]; then
  echo "==> cloning $BASE_REPO ($BASE_REF)"
  mkdir -p "$(dirname "$BASE_DIR")"
  git clone --filter=blob:none "$BASE_REPO" "$BASE_DIR"
fi

git -C "$BASE_DIR" checkout "$BASE_REF"

echo "==> copying overlay"
mkdir -p "$BASE_DIR/Platforms" "$BASE_DIR/Silicon/QC" "$BASE_DIR/scripts" \
         "$BASE_DIR/build_cfg" "$BASE_DIR/Build/ShennongPkg"
rm -rf "$BASE_DIR/Platforms/ShennongPkg" "$BASE_DIR/Platforms/AndromedaPkg"
cp -a "$OVERLAY/Platforms/ShennongPkg"   "$BASE_DIR/Platforms/"
cp -a "$OVERLAY/Platforms/AndromedaPkg"  "$BASE_DIR/Platforms/"
cp -a "$OVERLAY/Silicon/QC/Sm8650"       "$BASE_DIR/Silicon/QC/"
cp -a "$OVERLAY/scripts/."               "$BASE_DIR/scripts/"
cp -a "$OVERLAY/build_cfg/."             "$BASE_DIR/build_cfg/"
cp -a "$OVERLAY/Build/ShennongPkg/."     "$BASE_DIR/Build/ShennongPkg/"

echo "==> syncing submodules"
git -C "$BASE_DIR" submodule update --init \
  MU_BASECORE Common/MU Silicon/Arm/MU_TIANO Binaries

fetch_commit() {
  local dir=$1 sha=$2
  if ! git -C "$BASE_DIR/$dir" cat-file -e "$sha^{commit}" 2>/dev/null; then
    git -C "$BASE_DIR/$dir" fetch --depth 1 origin "$sha" || true
  fi
}

apply_patches() {
  local dir=$1 base=$2 series=$3
  echo "==> $dir @ $base"
  fetch_commit "$dir" "$base"
  git -C "$BASE_DIR/$dir" checkout -f "$base"
  for p in "$OVERLAY/patches/$series"/*.patch; do
    echo "    $(basename "$p")"
    git -C "$BASE_DIR/$dir" apply --whitespace=nowarn "$p"
  done
}

apply_patches MU_BASECORE                13e2fc5 mu_basecore
apply_patches Common/MU                  8b646ce mu_plus
apply_patches Silicon/Arm/MU_TIANO       e1f0860 mu_silicon_arm_tiano
apply_patches Binaries                   ce59dcf surface_duo_binaries

# OpenSSL is a public upstream commit; no patch.
OSSL="$BASE_DIR/Platforms/OpensslPkg/Library/OpensslLib/openssl"
if [ -d "$OSSL/.git" ]; then
  fetch_commit "Platforms/OpensslPkg/Library/OpensslLib/openssl" aea7aaf2
  git -C "$OSSL" checkout -f aea7aaf2
fi

cat <<EOF

==> done
cd "$BASE_DIR"
./build_setup.sh
pip install --upgrade -r pip-requirements.txt
./build_uefi.py --init
./build_uefi.py -d xiaomi-shennong -t DEBUG

Output: Build/ShennongPkg/xiaomi-shennong.img
EOF
