# mu_aloha_SM8650_shennong

**中文** | [**English**](README_EN.md)

> **借鉴或使用本项目时，请在项目的显眼位置注明本项目和上游来源。**
> 具体要求见 [项目使用](#项目使用)。

**Xiaomi 14 Pro**（shennong / SM8650）的 Project Mu UEFI 移植：SM8650 overlay 源码、一键编译脚本、可启动镜像 及设备树。项目旨在为 Xiaomi 14 pro（shennong） 机型的相关 Windows on arm 及其他系统的移植项目（如UniXNU）提供力所能及的帮助。

- 开发快照：2026-11-03，DEBUG / ShennongNoSb
- 镜像：`Build/ShennongPkg/xiaomi-shennong.img`，2,789,376 字节
  SHA256 `0f6d90402184ea33bf5c28a362d453d3bc9ab3c4202a21e54a4b9fe4d6c9fb2e`
- 基线：Project Mu Aloha `dev/update/mu202502`

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

当前镜像的实机记录见本地交付目录。

**日志** — 可用

- 支持自动写入 logfs 持久化日志和QR/USB-大容量存储设备/USB-CDC三种传输方式；
- 引导镜像时长按音量上键开启日志屏显。

**屏幕显示** — 可用

- 1440×3200 面板点亮，帧缓冲控制台可读（无 UART 时的唯一输出通道）
- 图形界面（FrontPage）可进入：主菜单、文件浏览、电源状态、USB、退出页
- 开机先显示 Boot Menu，5 秒无操作自动进 GUI；按键或触摸可取消倒计时

| 能力 | 当前结果 |
|---|---|

| 能力 | 当前结果 |
|---|---|
| 屏幕 / 触摸 / OSK | 可用：1440×3200 帧缓冲、GUI 和屏幕键盘 |
| CDC / cdcshell | 可用：日志、硬件测试、Shell、文件上传和 EFI 启动命令 |
| RAM disk | 64..8192 MiB，分配失败会明确报错；64 / 128 MiB 实机精确分配通过，8 GiB 实机分配未验收 |
| CDC 上传 / Shell | 默认 1 MiB 确认窗口；同候选 4 MiB 交替上传约 0.68 → 1.78 MiB/s，四次 CRC 一致；NSH 与 3072 字节 EFI 执行/返回通过 |
| USB MSC | 共享 RAM 已在 Windows 挂载，文件读取校验和定时回 CDC 后回读通过。 |
| USB 热拔插 | 多次真实 CDC 拔插后自动恢复，含日志接收中断；原文件 CRC 保留与再次切换通过。大文件替换上传中拔线的实机验收尚未捕获。 |
| UFS | 原厂 LUN/GPT 读取、FAT 浏览；非加密 ext4 只读驱动已集成，保持普通存储写/擦除保护 |
| RPMB | 原始响应只读成功；类型、结果、地址、nonce、前后计数器核对，MAC 未认证；无写/密钥/解锁接口 |
| 震动 | 用户确认明显震动；TEST 独立页面，时长与强度滑动条，DIRECT_PLAY gain 映射 |
| 闪光灯 | 用户确认发光；双通道短脉冲测试、关闭与配置恢复 |
| ACPI | 实机 15 张表、8 个 80 字节 GICC、独立 GICR、GTDT 29/30/27/26 与 Always On 核对；bootcheck 可见，Windows 平台适配未完成 |
| ARM64 WinPE | 本轮完整目录上传、WIM CRC 和四个启动文件列表通过；执行启动后用户观察到鼠标保留、GUI 背景黑屏，随后重启，主机枚举为 Android ADB。PE 启动失败，具体重启原因未确定。 重启后读回171750字节旧快照，ReadyToBoot与watchdog关闭可见，最后保存点为efi-shell-start，尚无EBS/内核错误记录。 |
| SMC / TZ | 普通 SMC、QSEE 版本、PAS 能力及 Keymaster 句柄查询；安全应用业务与子系统启动未完成 |
| WiFi / 蓝牙 | WCN7850 电源/时钟/PHY/PCIe 链路及 17cb:1107 识别通过、资源恢复成功；真实 AP/BLE 扫描后端仍缺失 |
| GPS / 陀螺仪 / 加速度 | SMEM 中 ADSP/MODEM/CDSP 描述符与 16 KiB TX 可读，RX 未建立；没有定位或实时采样数据 |
| 亮度 | 可查询 DCS provider；当前 provider 缺失，调节未完成 |
| 电池 / 充电 | 部分 getter 可用，电量、温度与充电控制未完成 |
| 相机 / 麦克风 / 扬声器 / GPU | 业务驱动未完成 |

USB 只走 stock 硬件链，原厂 UsbInitDxe、UsbfnDwc3Dxe、UsbDeviceDxe 未修改；旧非 stock 路径已移除。构建保留源码锁和镜像来源校验。

## CDC 与共享 RAM

CDC、USB Function 和本地文件系统共用同一块 RAM，不复制盘面。上传时独占 RAM，禁止切换或释放；MSC 接管时撤销本地挂载，归还后恢复本地只读访问。同容量 create 复用原盘，改变容量需要先 release。

**TZ / SMC** — 部分可用

- 普通 SMC 调用有正常回应，ShmBridge 已建立
- 安全服务没全通：feature TA 签名校验失败（`ret=12`），Keymaster 只到加载器

固件与客户端默认 CDC line coding 为 3,000,000 8N1，baud 本身不限制 USB 吞吐量。默认客户端使用 `put2` 协商 1 MiB 确认窗口，每次 DMA 仍不超过 64 KiB；可用 `--upload-window 65536` 选择旧窗口。旧固件只有明确返回未知命令时才回退 `put`。CRC32 改用固定 nibble 表，末窗保留最多 1023 字节补零、流式 CRC 和断线取消未提交替换。相同候选交替上传 4 MiB，64 KiB 窗口为 0.67–0.68 MiB/s，1 MiB 窗口为 1.78 MiB/s，约 2.6 倍，四次 CRC 一致。 本轮 344.1 MiB WIM 上传 186.2 秒（1.85 MiB/s），CRC 回读通过。 性能对照来自 1fa1536c 候选；发布候选的 USB 模块二进制一致，平台与 Windows 检查在发布候选复测。

正常停止后使用原厂 Stop/StartEx 流程，仅在 halt 未确认时尝试原厂初始化；停止失败保留 DMA 缓冲和 RAM。同模式 CDC 停止/重启与共享文件 CRC 回读已通过。

切换从排队开始禁止其他切换、RAM 大小修改和释放。TEST 显示 switching 状态；驱动对重入返回 Already Started。回切先等待当前控制/BOT 事务完成，最多 5 秒；成功、拔线、超时和停止失败均完成收尾并释放切换锁。停止失败时保留 DMA 缓冲，后续重连或再次请求可重试。CDC 检测实际线缆下降沿，取消会话，并在 pump 外经过 200 ms 后走原厂 Stop/StartEx；普通总线 reset 不触发这条重建。多次真实 CDC 拔插后自动恢复，含日志接收中断；原文件 CRC 保留与再次切换通过。大文件替换上传中拔线的实机验收尚未捕获。

USB 原厂硬件二进制保持不变；本轮授权扩展后的源码锁已更新。默认 HS；`usb speed ss` 是易失实验选项，也可从 TEST → USB host 进入速度页面。SS 无法配置时 15 秒回 HS CDC，启动失败也走现有 DMA 安全恢复路径；切换与上传共用互斥。实机 SS 请求和切回 HS 成功，但本次实际仍为 HS / 512 字节包，USB 3.0 链路尚未验收。重启恢复 HS 默认。

远程 EFI 启动会先触发一次 ReadyToBoot，记录前后保存点并关闭 UEFI watchdog，再由真实 Shell 控制台启动。测试 EFI 执行标记、返回及 `[WINLOAD] ReadyToBoot` 日志已验证。RAM backing 使用 EfiReservedMemoryType，设备路径保留原所有权标记并追加标准 RAM disk 节点；仍是同一块共享内存和本地只读 BlockIo。Windows 修补器纠正 PE SizeOfImage 偏移（PE+0x50），限制镜像查找和修补边界，并清理代码缓存。本机 EL1 下 ACTLR_EL1 只读成功，关闭继承的 ACTLR 删除补丁；尚不能认定它是此前重启的根因。

```text
ramdisk create 64
ramdisk info
ramdisk hash EFI/BOOT/BOOTAA64.EFI
shell map -r
shell FSx:\path\script.nsh
boot FSx:\EFI\BOOT\BOOTAA64.EFI
usb ramdisk 60
usb cdc
ramdisk release
rpmb-info
rpmb-read 0 1
test platform
test devices
test wireless-scan
test bluetooth
test sensors
test brightness-status
test bootcheck
test glink
test mhi
test transports
test gyro
test gps
usb speed
usb speed ss
usb speed hs
haptic level 500 75 direct
```

```bash
python tools/cdc-control.py --port COM20 --upload path/to/file --dest EFI/BOOT/BOOTAA64.EFI
python tools/cdc-control.py --port COM20 --upload-dir path/to/pe/media
python tools/cdc-console.py --port COM20
```

`FSx` 从 `shell map -r` 查找，不能固定盘符。`boot` 只接受单个映射 `.efi` 路径，ACK 后启动并使用真实控制台；客户端保持串口等待返回，断连或未观察到返回会明确报告。NSH 使用 `shell` 命令执行。FAT32 单文件最多 4294967295 字节；大盘不等于可存放大于 4 GiB 的单文件。8 GiB 上限按实际可用内存分配，不自动缩容。

已使用本地 ADK 28000.1 构建 344.1 MiB ARM64 命令行 PE，LZX，SAN OfflineAll、关闭自动挂载。本轮完整目录上传、WIM CRC 和四个启动文件列表通过；执行启动后用户观察到鼠标保留、GUI 背景黑屏，随后重启，主机枚举为 Android ADB。PE 启动失败，具体重启原因未确定。 重启后读回171750字节旧快照，ReadyToBoot与watchdog关闭可见，最后保存点为efi-shell-start，尚无EBS/内核错误记录。


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

离线交互设备树含 122 个节点，可搜索、过滤状态并查看依赖。状态更新于 2026-11-03，明确区分实机成功、部分可用与待实现；列表页面存在不代表无线或传感器已驱动。

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
| `Platforms/AndromedaPkg/` | FrontPage、QR 日志、DIAG、UfsPreDxe、共享 RAM、平台与硬件测试源码 |
| `Silicon/QC/Sm8650/` | QcomPkg（PlatformPeiLib shim、PlatformTheme、RFS 等） |
| `patches/` | 四个子模块的移植补丁（mu_basecore / mu_plus / mu_silicon_arm_tiano / surface_duo_binaries） |
| `scripts/` | 构建入口、源码锁清单与构建时镜像校验；本地检测脚本不随仓库分发 |
| `build_cfg/sm8650.json` | BootShim 窗口（0xA7100000 / 4 MB） |
| `devtree/` | 设备树可视化：脚本、YAML 数据、单文件 HTML |
| `tools/qr_camera_reader/` | 二维码日志识别器 |
| `Build/ShennongPkg/xiaomi-shennong.img` | 当前可启动镜像 |

本仓库**不包含** Project Mu 公共基础树、其他机型、其他 git 分支和分析/提取目录，由 `build.sh` 自动准备。



## 安全约束（来自实际 bring-up）

- 只用 `fastboot boot` 全程内存运行，**不写任何分区**；xiaomi 14 pro 设备无免授权 EDL，写坏需拆机或移交官方修复。
- 不拆机、不接 UART，调试只通过 USB / 屏幕二维码。
- 崩溃恢复：长按电源 10–15 秒。
- 该镜像面向 **Xiaomi 14 Pro / shennong**；不要刷到其他机型。

## 项目使用

**如果你借鉴或使用本项目的代码、补丁、文档、截图或设备树可视化数据，请在你自己项目的显眼位置注明来源。**
若条件允许，请一并注明并致谢上游项目。

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

**智能体**
- deepseek v4.1 flash 和 GPT 6.1 sol 的协助指导与疑难排查。

原厂二进制的版权属于 Qualcomm 与小米，本仓库只做提取与调用，不重新分发其源码。
