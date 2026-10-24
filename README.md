# mu_aloha_SM8650_shennong

Xiaomi 14（shennong / SM8650）Project Mu UEFI 移植的**核心源码 overlay + 可启动镜像**。

- 快照：分支 `usb-line`，HEAD `c9e127dd`（2026-10-24）
- 镜像：`Build/ShennongPkg/xiaomi-shennong.img`（2,686,976 B）
- 基线：Project Mu Aloha（`Project-Aloha/mu_aloha_platforms`，mu202502 线）
- 状态：**能进 FrontPage GUI；UFS 已打通（6 LUN / 123 分区）**；TZ/SCM/RPMB 与 USB（I2C）仍未完成

详细状态见 [STATUS.md](STATUS.md)，构建见 [BUILDING.md](BUILDING.md)，再分发限制见 [NOTICE.md](NOTICE.md)。

## 本仓库包含什么

| 路径 | 内容 |
|---|---|
| `Platforms/ShennongPkg/` | SM8650 / Xiaomi 14 平台与设备层（DSC/FDF、DXE 列表、DTB、设备 blob） |
| `Platforms/AndromedaPkg/` | FrontPage、QR 日志、DIAG、UfsPreDxe、ShmBridge stub 等移植改动 |
| `Silicon/QC/Sm8650/` | PlatformPeiLib shim、PlatformTheme、RFS 等 |
| `scripts/` | `fastbuild.sh`、`profile.sh`、`reorder_apriori.py` |
| `build_cfg/sm8650.json` | BootShim 窗口（0xA7100000 / 4 MB） |
| `patches/` | 四个子模块的移植补丁（mu_basecore / mu_plus / mu_silicon_arm_tiano / SurfaceDuoBinaries） |
| `tools/qr_selftest.py` | QR 编码器自检（v1–v10 + zxing 解码） |
| `Build/ShennongPkg/xiaomi-shennong.img` | 当前可启动镜像 |

**不包含**：Project Mu 公共基础树、其他平台、其他 git 分支、分析/提取目录。

## 快速开始

```bash
# 1. 取公共基线
git clone https://github.com/Project-Aloha/mu_aloha_platforms base
cd base
git checkout dev/update/mu202502

# 2. 应用本 overlay 与子模块补丁
/path/to/mu_aloha_SM8650_shennong/bootstrap.sh "$PWD"

# 3. 构建
./build_setup.sh
pip install --upgrade -r pip-requirements.txt
./build_uefi.py --init
./build_uefi.py -d xiaomi-shennong -t DEBUG
```

产物：`Build/ShennongPkg/xiaomi-shennong.img`。

## 安全约束（来自实际 bring-up）

- 只用 `fastboot boot` 全程内存运行，**不写任何分区**；设备无 EDL，写坏难恢复。
- 不拆机、不接 UART，调试只通过 USB / 屏幕二维码。
- 崩溃恢复：长按电源 10–15 秒。
- 该镜像面向 **Xiaomi 14 / shennong**；不要刷到其他机型。
- 仓库内的 Qualcomm/Xiaomi 二进制 blob 仅用于本机研究，公开再分发前请阅读 [NOTICE.md](NOTICE.md)。
