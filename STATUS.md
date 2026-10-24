# 状态（2026-10-24，`usb-line` @ `c9e127dd`）

## 已完成

### 启动 / UI
- PrePi → DxeCore → BDS → FrontPage GUI 全链路可启动（DEBUG / CLANGPDB）
- 帧缓冲串口控制台、字库；GOP + SWM + UIToolKit
- 开机启动菜单：GUI / 动态 BootOrder / Shell / 重启；音量上下选择、电源键确认、10 秒超时
- 音量键双击映射左右键（DisplayEngine）
- 64 KiB 环形启动日志；`QR LOG` 分页 + `LOGAUTO` 自动播放（QR v10，271 B/页）
- DIAG 页、按键日志、ELF 分区扫描、BlockIo/GPT 探测

### UFS（本版最大成果）
- memory map 修复：`UFS = 0x1D80000 + 0x30000`，无缝覆盖 UFSDxe 请求
- PrePi cfg shim：`OverRideSharedImemBootCfg = 0x8`（UFS boot device）
- ShmBridge protocol 由 `UfsPreDxe` / `RamPartitionDxe` 发布，RPMB listener 不再断言
- **6 个 LUN / 123 个分区，与原厂 GPT 逐一吻合**；tag `ufs-working-2026-10-24`
- `UfsWakeDxe`、UFSHCI 寄存器/ICE 诊断

### 构建 / 稳定性
- DEBUG 默认构建；`PcdDebugPropertyMask` 允许断言打印不halt
- RNG 构造函数（DxeRngLib/RngDxe 兼容）修复
- 性能测量链表断言修复；GIC 伪中断风暴修复
- `scripts/profile.sh` 的 profile 复制机制；`reorder_apriori.py`

## 未完成 / 待排查

### 1. USB 设备模式（当前主线）
- I2C master 协议数 = **0**；`usb_shared_i2c_write, not open`
- `DTBExtnProtocol` 已由 EnvDxe 从 HOB 发布，SFDUMP 确认 TE 扫描正常
- I2CDxe 拿到协议但静默发布 0 个 master；下一步对比 XBL DTB / FV DTB 的 `/soc/i2c`
- DWC3 `RunStop=0`；`FpTryStartUsb` 目前主动跳过 Start（历史上会 hang）
- USB redriver / GPIO 使能失败（`Failed to enable GPIO status : 0x7`）

### 2. TZ / SCM / RPMB
- `usb-line` 上已撤回 `TzDxeLA` + `ScmDxeCompat`，保证 UFS 可用
- `TzDxeLA` 在 `TzDxeLBox.c +1136` ELF 校验失败；ELFSCAN 发现 12 个 ELF 分区但 `shnum=0`
- SCM 缺失导致 RPMB 测试密钥 / QSEECom / Keymaster 链不可用
- `ShmBridge` 已能发布；SCM 仍未起来

### 3. 其它
- 电池/充电：`BatteryVoltage: 0`、`core_dbc_threshold`、QcomCharger 未启用
- 变量存储：依赖存储/认证链，仍有 `Memory Type Information` 警告
- DSDT/ACPI 未完成（README 标 WIP）
- 显示面板 / 触摸 / 音频 / 相机 / Modem / PCIe / GPU / NPU 未实现
- 部分 QC 模块未进 FV（DisplayDxe、QcomBds、QcomCharger、ProjectInfo、MiToken 等，见 `未启用驱动清单` 的后续版本）
- USB Shell / `UsbKbDxe` 目前无输入设备可用

## 版本 / 标签（源仓库）
- `ufs-working-2026-10-24`（UFS 打通）
- `gui-boot-2026-10-24-crypto-shmstub`（GUI + ShmBridge stub）
- `qr-log-working-2026-10-24`（二维码日志链路）
- 当前分支 `usb-line`（从 `qr-log` 切出，摘掉 TZ 链）

## 测试方式
- 屏幕：FrontPage；`QR LOG` / `LOGAUTO` 用手机扫码读取完整启动日志（含 `[DIAG]`、`[ELFSCAN]`、`[SFDUMP]`）
- 不使用 UART；异常时看屏幕 ASSERT（`TzDxeLBox.c +NNN` 等）与 QR 日志尾部
