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
| `--baud` | 波特率，默认 `115200` |
| `--timeout` | 单条命令超时（秒），默认 `120` |
| `--open-timeout` | 打开端口超时（秒），默认 `15` |
| `--command`, `-c` | 执行单条命令后退出 |
| `--output`, `-o` | 日志、抓屏或 Shell 输出保存路径 |
| `--shell` | 原始 UEFI Shell 终端 |
| `--local-echo` | 终端模式下回显本地按键 |

## 注意事项

- 命令必须是非空、单行 ASCII，长度不超过 127 字节。
- 一条命令的响应正文超过约 1 KiB 时，不要走普通命令模式去读 Shell，请用 `--shell`。
- `reboot bootloader`、`reboot recovery` 会改变设备状态，自动化脚本里要谨慎。
- `--shell` 不能和 `--command` 同时使用。

## 实测记录（2026-11-02）

- `python cdc-console.py --list`：正常列出 `COM20`，识别为 `VID:PID=18D1:4EE8`。
- 两个入口的 `--help` 均正常输出全部参数。
- CDC 依赖导入正常，`cdc-deps/pyserial 3.5` 可加载。
- 实测 `python cdc-control.py -c help` 时 COM20 返回 `PermissionError(13)`，原因是该端口正被其他进程占用，关闭占用方后即可重试。
