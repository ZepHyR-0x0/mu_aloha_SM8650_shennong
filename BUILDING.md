# 构建

本仓库是 **overlay**：需要 Project Mu Aloha 公共基线，再覆盖本目录并给子模块打补丁。

## 环境

- Ubuntu 22.04 / 24.04（WSL2 也可；源码放在 ext4，**不要放在 `/mnt`**）
- Python 3.12 + venv、mono-devel、git、build-essential
- LLVM/clang（CLANGPDB）、`aarch64-linux-gnu-` 工具链
- 参考基线 README 的 `build_setup.sh` / `pip-requirements.txt`

## 步骤

```bash
# 1) 公共基线（mu202502 线）
git clone https://github.com/Project-Aloha/mu_aloha_platforms base
cd base
git checkout dev/update/mu202502

# 2) 覆盖本 overlay + 子模块补丁
/path/to/mu_aloha_SM8650_shennong/bootstrap.sh "$PWD"

# 3) 环境
./build_setup.sh
pip install --upgrade -r pip-requirements.txt
./build_uefi.py --init

# 4) 构建（DEBUG 默认，带断言输出）
./build_uefi.py -d xiaomi-shennong -t DEBUG
```

产物：`Build/ShennongPkg/xiaomi-shennong.img`。

> `scripts/fastbuild.sh` 是作者本机用的加速脚本，里面硬编码了 `/mnt/l/...` 和
> `/root/mubuild`，使用前改成你的路径；否则直接用上面的 `build_uefi.py`。

## 子模块补丁（bootstrap.sh 会自动做）

| 子模块 | 公共基线 | 补丁 |
|---|---|---|
| `MU_BASECORE` | `13e2fc5` | `patches/mu_basecore/*.patch`（10 个 PORT 提交） |
| `Common/MU` | `8b646ce` | `patches/mu_plus/*.patch`（异常转储、进度码、双击左右键） |
| `Silicon/Arm/MU_TIANO` | `e1f0860` | `patches/mu_silicon_arm_tiano/*.patch`（GIC 中断） |
| `Binaries` | `ce59dcf` | `patches/surface_duo_binaries/*.patch`（logo 头） |
| `Platforms/OpensslPkg/.../openssl` | — | 切到 `aea7aaf2`（公共提交） |

## 烧写（务必只 boot，不写分区）

```bash
fastboot boot Build/ShennongPkg/xiaomi-shennong.img
```

设备无 EDL；写坏分区难以恢复。崩溃时长按电源 10–15 秒。

## 注意

- 修改 `profiles/*.inc` 后必须运行 `bash scripts/profile.sh full`，否则 FDF 读的
  `DXE.inc` 不会更新，新模块会静默丢失。
- 构建类型默认 DEBUG；RELEASE 下 `DEBUG()`/`ASSERT()` 会被编译掉。
