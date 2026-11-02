# 版本与来源

## 快照
- 源仓库分支：`pre-touch`
- HEAD：`efef05a4` — "b326: complete device repeater and WCD setup at native USB init"
- 日期：2026-10-31
- 机型：Xiaomi 14 Pro（shennong / SM8650）

## 公共基线
- 仓库：`https://github.com/Project-Aloha/mu_aloha_platforms`
- 分支：`dev/update/mu202502`（mu202502 线）
- 说明：`bootstrap.sh` 自动克隆基线；本仓库只带 SM8650 overlay 与子模块补丁。

## 子模块（bootstrap.sh 自动处理）

| 子模块 | 公共基线 | 补丁数 |
|---|---|---|
| `MU_BASECORE` | `13e2fc5` | 11 |
| `Common/MU` | `8b646ce` | 9 |
| `Silicon/Arm/MU_TIANO` | `e1f0860` | 1 |
| `Binaries` | `ce59dcf` | 1 |
| `Platforms/OpensslPkg/.../openssl` | — | 0（固定 `aea7aaf2`） |

## 镜像
- 文件：`Build/ShennongPkg/xiaomi-shennong.img`
- 由 `./build.sh` 生成并回拷。

## 工具
- `tools/qr_camera_reader/`：摄像头二维码日志识别器（opencv + zxing-cpp + Pillow）
- `devtree/devtree_viz.py`：设备树/打通状态可视化（HTML / DOT / SVG / PNG / Mermaid / JSON）
