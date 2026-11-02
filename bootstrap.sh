#!/usr/bin/env bash
#
# 把本仓库（Xiaomi 14 Pro / shennong / SM8650 的 overlay）铺到 Project Mu Aloha 基线上。
#
# 为避免上游分支漂移，所有子模块都钉死到本快照验证过的提交；补丁涉及的四个子模块
# 会先回到补丁基线，再应用 patches/ 下的移植补丁。
#
# 用法:
#   ./bootstrap.sh [基线目录] [基线分支]
#   MU_BASE=... MU_BASE_REF=... ./bootstrap.sh
#
# 可重复执行。
#
set -euo pipefail

BASE=${1:-${MU_BASE:-$HOME/mu_aloha_base}}
REF=${2:-${MU_BASE_REF:-main}}
OVERLAY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BASE_REPO=${BASE_REPO:-https://github.com/Project-Aloha/mu_aloha_platforms}

if [ ! -d "$BASE/.git" ]; then
  echo "==> 克隆基线 $BASE_REPO ($REF) -> $BASE"
  mkdir -p "$(dirname "$BASE")"
  git clone --depth 1 -b "$REF" "$BASE_REPO" "$BASE"
else
  git -C "$BASE" fetch --depth 1 origin "$REF"
  git -C "$BASE" checkout -q -B "$REF" FETCH_HEAD
  git -C "$BASE" reset -q --hard FETCH_HEAD
fi

fetch_commit() {
  local dir=$1 sha=$2
  if ! git -C "$BASE/$dir" cat-file -e "$sha^{commit}" 2>/dev/null; then
    git -C "$BASE/$dir" fetch --depth 1 origin "$sha"
  fi
}

# 上游分支可能删掉某些子模块（例如 Common/MU_TIANO），缺了就补回 .gitmodules
ensure_submodule() {
  local dir=$1 url=$2
  if ! git -C "$BASE" config -f "$BASE/.gitmodules" --get-regexp '\.path$' |
       awk -v p="$dir" '$2 == p { found = 1 } END { exit !found }'; then
    echo "==> 补登记子模块 $dir"
    git -C "$BASE" config -f "$BASE/.gitmodules" "submodule.$dir.path" "$dir"
    git -C "$BASE" config -f "$BASE/.gitmodules" "submodule.$dir.url" "$url"
  fi
  git -C "$BASE" config "submodule.$dir.url" "$url"
}

# 把子模块钉到指定提交（不再跟随上游 .gitmodules 里的分支）
init_submodule() {
  local dir=$1 sha=$2 url=$3
  echo "==> $dir @ $sha"
  ensure_submodule "$dir" "$url"
  git -C "$BASE" update-index --add --cacheinfo "160000,$sha,$dir"
  git -C "$BASE" submodule update --init --depth 1 --force -- "$dir"
  fetch_commit "$dir" "$sha"
  git -C "$BASE/$dir" checkout -q -f "$sha"
  git -C "$BASE/$dir" reset -q --hard "$sha"
  git -C "$BASE/$dir" clean -qfd
}

apply_patches() {
  local dir=$1 base=$2 series=$3 url=$4
  init_submodule "$dir" "$base" "$url"
  for p in "$OVERLAY/patches/$series"/*.patch; do
    echo "    $(basename "$p")"
    # EDK2 子模块源码在仓库里是 CRLF（各自 .gitattributes 为 `* -text`），
    # 而本仓库 .gitattributes 的 `*.patch text eol=lf` 会把补丁规范化成 LF，
    # 直接用 git apply 会因行尾不匹配而全部失败。--ignore-whitespace 让上下文
    # 匹配忽略 CR，补丁才能正常应用。
    git -C "$BASE/$dir" apply --ignore-whitespace --whitespace=nowarn "$p"
  done
}

# 未打补丁的子模块：钉到快照验证过的提交
init_submodule Common/MU_OEM_SAMPLE  e798945fbe64baad73e9c4ec7cfb26efaa95d4e3 https://github.com/microsoft/mu_oem_sample.git
init_submodule Common/MU_TIANO        2dd1beb9cd7631030dfc8e74cb4840a89b5d008d https://github.com/microsoft/mu_tiano_plus.git
init_submodule Features/CONFIG        645210d06a6c05644dbb7e6ef16d6d38d90e6273 https://github.com/microsoft/mu_feature_config.git
init_submodule Features/DFCI          8f5f6a4f41e81312cafc7411e9491445001d72cf https://github.com/microsoft/mu_feature_dfci
init_submodule Platforms/CranePkg     2d9ab7412377aa6401174edc2cd6227a171aa6b7 https://github.com/Project-Aloha/CrDK
init_submodule Platforms/SurfaceDuoACPI d742758fe575c08b5278d84fec1b3b313551a587 https://github.com/Project-Aloha/acpi_oem_third_party_common
init_submodule Platforms/OpensslPkg/Library/OpensslLib/openssl aea7aaf2abb04789f5868cbabec406ea43aa84bf https://github.com/openssl/openssl

# 打补丁的子模块：补丁基线 + patches/
apply_patches MU_BASECORE           13e2fc5a51365ea4587b4777496417d0b084ab74 mu_basecore           https://github.com/microsoft/mu_basecore.git
apply_patches Common/MU             8b646ce3484d3536c65244a3afd7b60739dc6983 mu_plus               https://github.com/microsoft/mu_plus.git
apply_patches Silicon/Arm/MU_TIANO  e1f0860555bf578e330db599463458347ddaedca mu_silicon_arm_tiano  https://github.com/microsoft/mu_silicon_arm_tiano.git
apply_patches Binaries              ce59dcf54753e112d7b847dd997d3ae934094e0d surface_duo_binaries  https://github.com/Project-Aloha/SurfaceDuoBinaries-fork

echo "==> 铺设 overlay"
rm -rf "$BASE/Platforms/ShennongPkg" "$BASE/Platforms/AndromedaPkg" "$BASE/Silicon/QC/Sm8650"
mkdir -p "$BASE/Platforms" "$BASE/Silicon/QC" "$BASE/build_cfg" "$BASE/scripts"
cp -a "$OVERLAY/Platforms/ShennongPkg"  "$BASE/Platforms/"
cp -a "$OVERLAY/Platforms/AndromedaPkg" "$BASE/Platforms/"
cp -a "$OVERLAY/Silicon/QC/Sm8650"      "$BASE/Silicon/QC/"
cp -a "$OVERLAY/build_cfg/sm8650.json"  "$BASE/build_cfg/"
rsync -a "$OVERLAY/scripts/" "$BASE/scripts/"
cp -f "$OVERLAY/pip-requirements.txt"   "$BASE/pip-requirements.txt"

echo "==> 完成，基线已就绪: $BASE"
