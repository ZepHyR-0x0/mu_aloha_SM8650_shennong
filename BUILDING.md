# 构建

本仓库是 **overlay**：公共 Project Mu Aloha 基线由脚本自动准备，SM8650 平台代码、子模块补丁和构建脚本都在本仓库里。

## 一键编译

```bash
./build.sh              # DEBUG（默认）
./build.sh RELEASE      # 发布构建
```

Windows 下双击 `build.cmd`（转交 WSL）。

首次运行会：克隆基线 → 拉子模块 → 铺 overlay → 打补丁 → 在 ext4 建编译树 → 建 venv → 编译。之后是增量构建。

可覆盖的环境变量：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `MU_BASE` | `~/mu_aloha_base` | 基线仓库目录 |
| `MU_BASE_REF` | `main` | 基线分支 |
| `MU_WORK` | `~/mubuild-shennong` | ext4 编译树（基线在 `/mnt/*` 时才用） |
| `MU_VENV` | `~/mu-venv-shennong` | Python venv |
| `FORCE_INIT` | `0` | 置 1 强制重跑 `build_uefi.py --init` |

产物：`Build/ShennongPkg/xiaomi-shennong.img`。

## 手动步骤（与 build.sh 等价）

```bash
./bootstrap.sh ~/mu_aloha_base          # 基线 + overlay + 子模块补丁
cd ~/mu_aloha_base
python3 -m venv ~/mu-venv-shennong && source ~/mu-venv-shennong/bin/activate
pip install -r pip-requirements.txt
./build_uefi.py --init
./build_uefi.py -d xiaomi-shennong -t DEBUG
```

> 源码放在 `/mnt/c`、`/mnt/d` 等 9p 盘上编译会非常慢；`build.sh` 会自动同步到 `MU_WORK` 再编。

## 环境依赖

- Ubuntu 22.04 / 24.04（WSL2 也可）
- Python 3.12 + venv
- clang/LLVM（CLANGPDB）、lld、`aarch64-linux-gnu-` 工具链
- mono-devel、git、rsync、build-essential、iasl、nasm
- 首次构建需要网络（克隆基线 / 子模块 / pip）

## 子模块补丁（`bootstrap.sh` 自动应用）

| 子模块 | 补丁基线 | 补丁数 |
|---|---|---|
| `MU_BASECORE` | `13e2fc5` | 11 |
| `Common/MU` | `8b646ce` | 9 |
| `Silicon/Arm/MU_TIANO` | `e1f0860` | 1 |
| `Binaries` | `ce59dcf` | 1 |
| `Platforms/OpensslPkg/.../openssl` | — | 固定到 `aea7aaf2` |

`bootstrap.sh` 可重复执行；补丁涉及的子模块会先强制回到基线提交再重新打补丁。

## 烧写（务必只 boot，不写分区）

```bash
fastboot boot Build/ShennongPkg/xiaomi-shennong.img
```

设备无 EDL；写坏分区难以恢复。崩溃时长按电源 10–15 秒。

## 注意

- 修改 `profiles/*.inc` 后必须运行 `bash scripts/profile.sh full`，否则 FDF 读的 `DXE.inc` 不会更新，新模块会静默丢失。
- 构建类型默认 DEBUG；RELEASE 下 `DEBUG()`/`ASSERT()` 会被编译掉。
