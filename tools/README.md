# Project Mu CDC 工具

这套工具用于连接 Project Mu / UEFI 固件通过 USB 暴露的 CDC 串口，执行命令、读取日志、抓屏，并进入 UEFI Shell。核心实现是 `cdc_client.py`，两个入口脚本只是它的两种调用方式。

## 文件结构

- `cdc_client.py`：串口传输、CDC 命令协议解析（`OK/ERR`、`BEGIN/END`、base64 BMP 解码）和 CLI 主体。
- `cdc-console.py`：交互式控制台。不带参数启动后循环等待用户输入命令，适合手动排查。
- `cdc-control.py`：单条命令模式。默认执行 `help`，适合脚本和自动化。
- `cdc-deps/`：随目录内置的 `pyserial 3.5`，CDC 用法默认从这里加载，不需要手动装串口库。
- `requirements.txt`：只给 `qr_camera_reader.py`（摄像头二维码录入）用，和 CDC 无关。
- `qr_camera_reader.py`：独立的摄像头二维码工具，不在本 README 展开。

## 背景与工作流

- 设备开机进入 GUI / BootMenu 后会自动进入 CDC 模式，此时才会枚举出 USB COM 口。
- CDC 模式下如果 3 分钟没有任何操作，设备会自动重启进入 Android 系统，连接会随之断开。
- `qr_camera_reader.py`（QRScanner）是在 USB 不通的情况下，供人工把日志/数据以屏幕二维码形式回传给 PC 的备用通道。

## 依赖

CDC 部分只依赖 Python 内置库和已随目录提供的 `pyserial 3.5`，通常无需额外安装。实测环境为 Windows + Python 3.14。

如果确实要装串口库：

```bat
python -m pip install pyserial==3.5
```

## 连接

设备默认通过 USB VID:PID `18D1:4EE8` 识别。先确认端口：

```bat
cd /d L:\Desktop\ProjectMu\tools
python cdc-console.py --list
```

只有一个匹配设备时会自动选择端口；端口不明确时用 `--port` 指定：

```bat
python cdc-console.py --port COM20
```

注意串口是独占的。如果另一个进程（控制台、miniterm、串口助手）已经占用了同一 COM 口，连接会报 `PermissionError`，需要先释放该端口。

## 使用

### 交互式控制台

```bat
python cdc-console.py
```

支持的命令：

```text
help, logcat, logcat-last, logsync, scrcat,
press up / down / power / esc,
shell <command>,
reboot system / bootloader / recovery
```

输入 `quit` 或 `exit` 退出。

### 单条命令（自动化）

```bat
python cdc-control.py -c help
python cdc-control.py -c logcat -o boot_log.txt
python cdc-control.py -c logcat-last -o previous_log.txt
python cdc-control.py -c scrcat -o screen.bmp
python cdc-control.py -c "shell echo hello"
python cdc-control.py -c "press up"
```

`-o` 不传时，会按时间戳在当前目录生成文件：日志保存为 `.txt`，抓屏保存为解码后的 `.bmp`。

### UEFI Shell 终端

```bat
python cdc-console.py --shell
python cdc-console.py --shell --local-echo
```

`--shell` 进入原始终端模式，方向键、退格会转发到设备，按 `Ctrl-Z` 退出。

## 常用参数

| 参数 | 说明 |
| --- | --- |
| `--port`, `-p` | 指定 COM 口，如 `COM20` |
| `--list` | 只列出串口，不打开 |
| `--baud` | CDC line coding，默认 `3000000`，可覆盖 |
| `--timeout` | 单条命令超时（秒），默认 `120` |
| `--open-timeout` | 打开端口超时（秒），默认 `15` |
| `--command`, `-c` | 执行单条命令后退出 |
| `--output`, `-o` | 日志、抓屏或 Shell 输出保存路径 |
| `--shell` | 原始 UEFI Shell 终端 |
| `--upload-dir` | 逐文件上传目录，保留相对路径，`--dest` 可指定目录前缀 |
| `--local-echo` | 终端模式下回显本地按键 |

## 注意事项

- 命令必须是非空、单行 ASCII，长度不超过 127 字节。
- Shell 命令输出按 BEGIN/END 长度读取，最多 256 KiB；交互式程序使用 `--shell`。
- `reboot bootloader`、`reboot recovery` 会改变设备状态，自动化脚本里要谨慎。
- `--shell` 不能和 `--command` 同时使用。

## RAM 文件上传与引导


CDC、USB Function 和本地文件系统共用同一块 RAM，不复制盘面。上传时独占 RAM，禁止切换或释放；MSC 接管时撤销本地挂载，归还后恢复本地只读访问。同容量 create 复用原盘，改变容量需要先 release。

固件与客户端默认 CDC line coding 为 3,000,000 8N1，客户端可用 `--baud 115200` 覆盖。baud 配置本身不限制这条 USB 数据路径；文件接收按剩余窗口批量提交，单次最大 64 KiB，末窗补零最多 1023 字节，仍校验 CRC 并在断线时取消未提交替换。客户端按已知长度读取每个 64 KiB 窗口的 ACK，避免等待 64 KiB 接收缓冲填满；普通命令和长正文沿用原接收路径。相同固件交替上传 4 MiB，旧客户端约 0.49 MiB/s，新客户端约 0.68 MiB/s，提升 40%，CRC 与完整日志帧通过。344.1 MiB WIM 实测 256.4 秒，平均 1.34 MiB/s，CRC 回读通过。页面上传期间使用最多 16 次 / 4 ms 的应用事件服务，并在页面等待中每 1 ms 服务；上传结束或取消后恢复正常等待。USB 驱动二进制与 30 项源码锁保持不变。

正常停止后使用原厂 Stop/StartEx 流程，仅在 halt 未确认时尝试原厂初始化；停止失败保留 DMA 缓冲和 RAM。同模式 CDC 停止/重启与共享文件 CRC 回读已通过。

切换从排队开始禁止其他切换、RAM 大小修改和释放。TEST 显示 switching 状态；驱动对重入返回 Already Started。回切先等待当前控制/BOT 事务完成，最多 5 秒；成功、拔线、超时和停止失败均完成收尾并释放切换锁。停止失败时保留 DMA 缓冲，后续重连或再次请求可重试。CDC 检测实际线缆下降沿，取消会话，并在 pump 外经过 200 ms 后走原厂 Stop/StartEx；普通总线 reset 不触发这条重建。多次真实 CDC 拔插后自动恢复，含日志接收中断；原文件 CRC 保留与再次切换通过。大文件替换上传中拔线的实机验收尚未捕获。

当前 USB 上层候选与 stock 硬件链按本页镜像及源码锁固定，后续平台推进沿用这个版本。

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
haptic level 500 75 direct
```

```bash
python tools/cdc-control.py --port COM20 --upload path/to/file --dest EFI/BOOT/BOOTAA64.EFI
python tools/cdc-control.py --port COM20 --upload-dir path/to/pe/media
python tools/cdc-console.py --port COM20
```

`FSx` 从 `shell map -r` 查找，不能固定盘符。`boot` 只接受单个映射 `.efi` 路径，ACK 后启动并使用真实控制台；客户端保持串口等待返回，断连或未观察到返回会明确报告。NSH 使用 `shell` 命令执行。FAT32 单文件最多 4294967295 字节；大盘不等于可存放大于 4 GiB 的单文件。8 GiB 上限按实际可用内存分配，不自动缩容。

已使用本地 ADK 28000.1 构建 344.1 MiB 的 ARM64 命令行 PE，普通 LZX 压缩；未添加桌面、PowerShell、.NET 或第三方驱动。SAN 默认 OfflineAll，关闭自动挂载。精简 ARM64 PE 完整目录上传与 WIM CRC 已通过；两次 boot 指令接受后 CDC 断开，重试时用户看到黑屏，电脑端随后重新枚举为 Android ADB。PE 启动未成功，重启原因待定位。

目录上传前检查全部文件路径与 FAT32 大小限制；每个文件独立校验 CRC 并提交，失败保留已完成文件。上传 PE 目录前先用 CDC 创建至少 512 MiB RAM，随后上传 media/ 内容，查询 FS 映射并执行 boot。
