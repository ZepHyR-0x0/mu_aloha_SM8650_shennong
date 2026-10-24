# 版本与来源

## 快照
- 源仓库分支：`usb-line`
- HEAD：`c9e127dd` — "Find out which device tree the Qualcomm drivers actually read"
- 日期：2026-10-24
- 相关 tag：`ufs-working-2026-10-24`、`gui-boot-2026-10-24-crypto-shmstub`、`qr-log-working-2026-10-24`

## 公共基线
- 仓库：`https://github.com/Project-Aloha/mu_aloha_platforms`
- 建议分支：`dev/update/mu202502`（mu202502 线）
- 说明：作者本地基线是一个手工 seed 的 ProjectMu 快照；本 overlay 会替换/新增移植相关路径，
  其余公共文件来自上述仓库。

## 子模块（bootstrap.sh 自动处理）

| 子模块 | 公共基线 | 最终提交 | 补丁数 |
|---|---|---|---|
| `MU_BASECORE` | `13e2fc5` | `2ac1fbb` | 10 |
| `Common/MU` | `8b646ce` | `368e7f2` | 3 |
| `Silicon/Arm/MU_TIANO` | `e1f0860` | `9a9e531` | 1 |
| `Binaries` | `ce59dcf` | `c6f5a53` | 1 |
| `Platforms/OpensslPkg/.../openssl` | — | `aea7aaf2` | 0（公共提交） |

## 镜像
- 文件：`Build/ShennongPkg/xiaomi-shennong.img`
- 大小：2,686,976 B
- SHA-256：`02b3004eb95c2648314413002cdfb3b91e3e9f6b43c3ef212459063a77b815c2`
- 构建类型：DEBUG（CLANGPDB）

## 工具
- `tools/qr_selftest.py`：QR 编码器自检（格式/版本/RS/布局 + zxing-cpp 解码 v1–v10）
