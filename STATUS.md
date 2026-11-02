# 状态（2026-11-02，`pre-touch` @ `a00ac7d7` / b341）

> 本文只记版本坐标与未完成项。**功能是否可用请看 [README 的「打通情况」](README.md)**，
> 逐节点的证据链看 [设备树可视化](devtree/examples/out/shennong.html)。

## 当前镜像

```
Build/ShennongPkg/xiaomi-shennong.img
2,711,552 字节
SHA256 b6493bc626f7cde22f0381ab8211b94720d4598541bd47e8e8fc64f56832f501
```

## 版本线（b326 → b341）

| 版本 | 内容 |
|---|---|
| b326 | 标准 USB 枚举实机打通（18D1:D00D） |
| b328 | CDC Shell、RAM 盘、静态 TZ/PCIe/WLAN 结论 |
| b329 | BootMenu 改为主/更多两级，修正拖拽滚动，CDC 输出分块加大 |
| b338 | 七项修复，音量键上屏啰嗦模式 |
| b339 | USB 切换与启动优化 |
| b340 | Shell RAM 输入与 Python 控制台 |
| b341 | CDC 完成事件过滤修复、5 秒自动进 GUI、进表单前默认 CDC、`logcat-last`/`logsync`/`shell`/`reboot` 远程命令 |

## 未完成

- **fastboot**：只有枚举和厂商接口外观，没有命令解释器与下载缓冲区，不能刷写。
- **TZ 安全服务**：普通 SMC 可用，但 feature TA 签名校验失败（`ret=12`），Keymaster 仅到加载器。
- **ADSP / PIL**：MDT 文件结构可达，认证与远端启动无证据。
- **充电控制**：只能读状态与接口电压，没有充电策略、没有电量百分比。
- **音频**：LPASS/TDM1 与功放链路未实现。
- **重启目的地**：`reboot bootloader/recovery` 命令已实现，但实机未验证；PMIC 原因码读回仍为 Device Error。
- **相机 / Modem / PCIe / WLAN / NFC**：未实现。

## 安全约束

- 只用 `fastboot boot` 全程内存运行，**不写任何分区**；设备无 EDL。
- 不拆机、不接 UART；日志通过屏幕二维码或 CDC 串口取出。
- 崩溃恢复：长按电源 10–15 秒。
- 镜像只面向 **Xiaomi 14 Pro / shennong**。
