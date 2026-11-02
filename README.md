# mu_aloha_SM8650_shennong

**中文** | [**English**](README_EN.md)

> **借鉴或使用本项目时，请在项目的显眼位置注明来源。**
> 具体要求见 [项目使用](#项目使用)。

**Xiaomi 14 Pro**（shennong / SM8650）的 Project Mu UEFI 移植：SM8650 overlay 源码、一键编译脚本、可启动镜像 及 测试套件。

- 快照：分支 `pre-touch`，HEAD `a00ac7d7`（b341，2026-11-02）
- 镜像：`Build/ShennongPkg/xiaomi-shennong.img`，2,711,552 字节
  SHA256 `b6493bc626f7cde22f0381ab8211b94720d4598541bd47e8e8fc64f56832f501`
- 基线：Project Mu Aloha `main`

## 界面

<table>
<tr>
<td><img src="docs/screenshots/boot-menu.png" width="330" alt="Boot Menu"></td>
<td><img src="docs/screenshots/usb-function.png" width="330" alt="USB Function"></td>
</tr>
<tr>
<td align="center">Boot Menu —— 5 秒无操作自动进入 GUI，可退出至设备模式</td>
<td align="center">USB Function —— 四种模式可切换</td>
</tr>
</table>
<img src="docs/screenshots/uefi-shell-osk.jpg" width="330" alt="UEFI Shell 与屏幕键盘">

**UEFI Shell + 屏幕键盘** —— `alias` 输出的块设备列表，以及底部的软键盘。

## 当前情况

**屏幕显示** — 可用
- 1440×3200 面板点亮，帧缓冲控制台可读（无 UART 时的唯一输出通道）
- 图形界面（FrontPage）可进入：主菜单、文件浏览、电源状态、USB、退出页
- 开机先显示 Boot Menu，5 秒无操作自动进 GUI；按键或触摸可取消倒计时

**触摸** — 可用
- 整屏触摸输入，GUI 点击、列表双向滑动、屏幕键盘（OSK）都正常
- 根因曾是 L12/IOVDD 在 UEFI 里没投电，投上后芯片立即应答

**USB 设备模式（支持USB-CDC/大容量存储设备模式）** — 可用
- 电脑识别为 `18D1:D00D` / Android Bootloader Interface，标准枚举完整
- 可切换四种形态：只读 logfs 快照（U 盘）、CDC 串口（Python 控制台）、
  512 MiB RAM 盘、关闭
- 实机：COM20 可交互，Shell 命令、日志读取（174 KB / 1.5 秒）、整屏截图（13 秒）都通
- **不是** fastboot：没有命令解释器，不能刷写

**存储（UFS）** — 可用
- 6 个 LUN、123 个分区，与原厂 GPT 逐一吻合
- 只读：能浏览 FAT 分区、读文件；写/擦除一律拒绝

**TZ / SMC** — 部分可用
- 普通 SMC 调用有正常回应，ShmBridge 已建立
- 安全服务没全通：feature TA 签名校验失败（`ret=12`），Keymaster 只到加载器

**充电 / 电池** — 部分可用
- 能读到充电器连接状态、电池存在、接口报告的电压
- **不能**充电控制，也**不显示**电量百分比和温度（数据源不可信，不做假值）

**GPU** — 未实现
- UEFI 阶段不做 3D，显示走 2D 帧缓冲，不需要 GPU 驱动

**音频** — 未实现
- 扬声器需要 LPASS/TDM1 + 功放链路，UEFI 侧没有播放栈

**相机 / Modem / PCIe / WLAN / NFC** — 未实现



## 一键编译

Linux / WSL：

```bash
./build.sh              # DEBUG（默认）
./build.sh RELEASE      # 发布构建
```

Windows（自动转交 WSL）：

```bat
build.cmd
```

首次运行会自动：

1. 克隆 Project Mu Aloha 基线（默认 `~/mu_aloha_base`）并拉取子模块；
2. 铺入本仓库的 SM8650 overlay，按 `patches/` 给四个子模块打补丁；
3. 在 ext4 上建编译树（默认 `~/mubuild-shennong`，避开 `/mnt/*` 的 9p 慢盘）；
4. 创建 Python venv（默认 `~/mu-venv-shennong`）并安装 `pip-requirements.txt`；
5. 编译 `xiaomi-shennong`，校验日志与镜像，回拷到 `Build/ShennongPkg/`。

可选环境变量：`MU_BASE`、`MU_BASE_REF`、`MU_WORK`、`MU_VENV`、`FORCE_INIT=1`。
脚本不依赖任何本机绝对路径，仓库放在哪都能直接执行。

产物：

```
Build/ShennongPkg/xiaomi-shennong.img   # fastboot boot 用
```

## 可视化设备树

以下展示了项目的详细情况：

**[打开 devtree/examples/out/shennong.html](devtree/examples/out/shennong.html)**

<iframe src="devtree/examples/out/shennong.html" width="100%" height="640" style="border:1px solid #d0d7de;border-radius:8px"></iframe>

> 部分 Markdown 预览器（包括 GitHub）出于安全策略不渲染 `iframe`。遇到这种情况请直接点上面的链接，或把仓库克隆到本地后用浏览器打开 `devtree/examples/out/shennong.html`。

在本地克隆中双击即可打开；它包含 107 个节点、204 条硬依赖 + 27 条软依赖、17 层布局，可以搜索、按状态/分组过滤、点击节点查看证据链、看阻塞链和地址冲突。

当前快照（`devtree/examples/shennong.state.yaml`，更新于 2026-11-02 / b341）：


## QR 识别器

`tools/qr_camera_reader/` 是一个桌面 GUI，用摄像头连续识别固件屏幕上滚动的二维码日志并拼接：

```bash
pip install -r tools/qr_camera_reader/requirements.txt
python tools/qr_camera_reader/qr_camera_reader.py --camera 0
```

支持手动/自动曝光与测光锁定（`--exposure`、`--auto-exposure`、`--no-meter`），识别结果可复制保存，用于读取无 UART 情况下的启动日志。

## 仓库内容

| 路径 | 内容 |
|---|---|
| `Platforms/ShennongPkg/` | SM8650 / Xiaomi 14 Pro 平台与设备层（DSC/FDF、DXE 列表、DTB、设备 blob） |
| `Platforms/AndromedaPkg/` | FrontPage、QR 日志、DIAG、UfsPreDxe、ShmBridge stub 等移植改动 |
| `Silicon/QC/Sm8650/` | QcomPkg（PlatformPeiLib shim、PlatformTheme、RFS 等） |
| `patches/` | 四个子模块的移植补丁（mu_basecore / mu_plus / mu_silicon_arm_tiano / surface_duo_binaries） |
| `scripts/` | `fastbuild.sh`（兼容入口）、`profile.sh`、`verify_usb_image.py`、`package_usb_variant.py` 等 |
| `build_cfg/sm8650.json` | BootShim 窗口（0xA7100000 / 4 MB） |
| `devtree/` | 设备树可视化：脚本、YAML 数据、单文件 HTML |
| `tools/qr_camera_reader/` | 二维码日志识别器 |
| `Build/ShennongPkg/xiaomi-shennong.img` | 当前可启动镜像 |

本仓库**不包含** Project Mu 公共基础树、其他机型、其他 git 分支和分析/提取目录，由 `build.sh` 自动准备。

## 目标

为 Xiaomi 14 Pro（shennong / SM8650）的 **UniXNU / Hackios** 开发提供可启动、可读 UFS、能通过二维码取日志、并且**状态可视化**的 Project Mu 基座。

## 安全约束（来自实际 bring-up）

- 只用 `fastboot boot` 全程内存运行，**不写任何分区**；xiaomi 14 pro 设备无免授权 EDL，写坏需拆机或移交官方修复。
- 不拆机、不接 UART，调试只通过 USB / 屏幕二维码。
- 崩溃恢复：长按电源 10–15 秒。
- 该镜像面向 **Xiaomi 14 Pro / shennong**；不要刷到其他机型。

## 项目使用

**如果你借鉴或使用本项目的代码、补丁、文档、截图或设备树可视化数据，请在你自己项目的显眼位置注明来源。**

"显眼位置"指打开项目就能看到的地方，例如：
- README 首屏或开头的说明段
- 独立的致谢 / Credits 章节

推荐的表述：

> 本项目的 SM8650 UEFI 部分移植基于/参考了 **mu_aloha_SM8650_shennong**（Xiaomi 14 Pro / shennong）的工作。

只需注明来源，不要求开源你的修改；**请不要删掉本仓库已有的出处说明**，也不要把直接复制本项目的工作表述为原创。商业使用与二次分发同样适用。

## 许可与免责

代码沿用 Project Mu Aloha 及其子模块的许可证（见 [LICENSE](LICENSE)），第三方组件遵循各自许可证。
本仓库按“现状”提供，仅用于研究与开发，不提供任何担保，使用风险自负。

## 致谢

本项目的每一层都离不开开源社区的工作，在此致谢。

**固件框架**
- [mu_aloha_platforms_sm8650](https://github.com/xingguangcuican6666/mu_aloha_platforms_sm8650) —— 本项目的源码仓库
- [Project Mu](https://github.com/microsoft/mu) 与 [TianoCore EDK II](https://github.com/tianocore/edk2) —— UEFI 框架与构建体系
- [Project Mu Aloha](https://github.com/Project-Aloha/mu_aloha_platforms) —— 本项目直接派生自它的平台层
- Microsoft `mu_basecore`、`mu_plus`、`mu_tiano_plus`、`mu_silicon_arm_tiano`、
  `mu_feature_config`、`mu_feature_dfci` —— 六个基础子模块
- [Xiaomi_Kernel_OpenSource-shennong](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) ——
  eUSB2 repeater 与 DWC3 PHY 的内核实现
- Xiaomi SM8650 devicetree 源码 —— 引脚、regulator、面板参数
- [UniXNU](https://github.com/UniXNU) 的 houji 项目 —— 触摸 S3910 驱动的结构参考
- UEFI-For-OnePlus13R —— 同为 SM8650 / lanai，FD 窗口 `0xA7100000` 的正确取值来源

**工具链**
- `tools/qr_camera_reader`：OpenCV、zxing-cpp、Pillow
- `devtree/`：Python 3.8+ 与 PyYAML，导出 SVG/PNG 时另需 Graphviz

原厂二进制的版权属于 Qualcomm 与小米，本仓库只做提取与调用，不重新分发其源码。