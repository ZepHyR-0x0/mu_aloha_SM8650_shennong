#!/usr/bin/env bash
#
# 一键编译 Xiaomi 14 Pro (shennong / SM8650) 的 UEFI。
#
#   ./build.sh              # DEBUG（默认，带断言/日志）
#   ./build.sh RELEASE      # 发布构建
#
# 可选环境变量:
#   MU_BASE      基线目录          (默认 ~/mu_aloha_base)
#   MU_BASE_REF  基线分支          (默认 dev/update/mu202502)
#   MU_WORK      编译树目录        (默认 ~/mubuild-shennong，仅当基线在 /mnt/* 时使用)
#   MU_VENV      Python venv 目录  (默认 ~/mu-venv-shennong)
#   FORCE_INIT   1 = 强制重新执行 build_uefi.py --init
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE="xiaomi-shennong"
BUILD_TYPE="${1:-DEBUG}"
case "$BUILD_TYPE" in
  DEBUG|RELEASE) ;;
  *) echo "用法: $0 [DEBUG|RELEASE]" >&2; exit 2 ;;
esac

BASE="${MU_BASE:-$HOME/mu_aloha_base}"
WORK="${MU_WORK:-$HOME/mubuild-shennong}"
VENV="${MU_VENV:-$HOME/mu-venv-shennong}"
REF="${MU_BASE_REF:-dev/update/mu202502}"

if [ "$(uname -s)" != "Linux" ]; then
  echo "必须在 Linux/WSL 下运行（Windows 下双击 build.cmd）" >&2
  exit 1
fi
for tool in git rsync python3; do
  command -v "$tool" >/dev/null || { echo "缺少依赖: $tool" >&2; exit 1; }
done

# 1) 基线 + overlay + 子模块补丁
"$REPO/bootstrap.sh" "$BASE" "$REF"

# 2) 源码在 /mnt/*（9p）上编译极慢，统一搬到 ext4 编译树
case "$BASE" in
  /mnt/*)
    mkdir -p "$WORK"
    echo "==> 同步编译树 $BASE -> $WORK"
    rsync -a --delete --exclude=Build --exclude=.git --exclude=.venv "$BASE/" "$WORK/"
    SRC="$WORK"
    ;;
  *)
    SRC="$BASE"
    ;;
esac

# 3) Python 环境
if [ ! -x "$VENV/bin/python" ]; then
  echo "==> 创建 venv: $VENV"
  python3 -m venv "$VENV"
  "$VENV/bin/python" -m pip install --upgrade pip >/dev/null
  "$VENV/bin/python" -m pip install -r "$SRC/pip-requirements.txt"
fi
# shellcheck disable=SC1091
source "$VENV/bin/activate"

# 4) 首次构建需要 stuart --setup/--update
cd "$SRC"
INIT_FLAG="$SRC/Build/.mu-init-done"
if [ "${FORCE_INIT:-0}" = "1" ] || [ ! -f "$INIT_FLAG" ]; then
  echo "==> 初始化构建环境（stuart --setup/--update）"
  ./build_uefi.py --init
  mkdir -p "$SRC/Build"
  touch "$INIT_FLAG"
fi

echo "==> 编译 $DEVICE ($BUILD_TYPE, nproc=$(nproc))"
set -o pipefail
./build_uefi.py -d "$DEVICE" -t "$BUILD_TYPE" 2>&1 | tee "$SRC/build.log"

# 5) build_uefi.py 失败也可能返回 0，必须查日志和镜像
LOG="$SRC/Build/BUILDLOG_ShennongPkg.txt"
if grep -qa "CRITICAL - Build failed\|^ERROR - Error" "$LOG" 2>/dev/null; then
  echo "!! 编译失败，见 $LOG" >&2
  grep -a "error:" "$LOG" | head -10 | cut -c1-200 >&2 || true
  exit 1
fi

IMG="$SRC/Build/ShennongPkg/$DEVICE.img"
[ -s "$IMG" ] || { echo "!! 未生成镜像: $IMG" >&2; exit 1; }
SIZE=$(stat -c%s "$IMG")
[ "$SIZE" -gt 1048576 ] || { echo "!! 镜像过小: $SIZE B" >&2; exit 1; }

python3 "$SRC/scripts/verify_usb_image.py" "$IMG" --stack stock \
  --fv "$SRC/Build/ShennongPkg/${BUILD_TYPE}_CLANGPDB/FV/FVMAIN.Fv" \
  --fd "$SRC/Build/ShennongPkg/${BUILD_TYPE}_CLANGPDB/FV/SM8650_EFI.fd" \
  --stock-root "$SRC/Platforms/ShennongPkg/Device/xiaomi-shennong/Binaries/QcomPkg/Drivers"

# 6) 产物回拷到仓库
mkdir -p "$REPO/Build/ShennongPkg"
cp -f "$IMG" "$REPO/Build/ShennongPkg/$DEVICE.img"
echo "==> 完成: Build/ShennongPkg/$DEVICE.img ($SIZE B)"
