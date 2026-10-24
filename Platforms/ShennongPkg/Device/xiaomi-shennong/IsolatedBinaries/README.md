# IsolatedBinaries — 来源受限的二进制，单独存放

本目录存放**不是来自厂商 `Binaries/` 树**的二进制。单独建目录是为了让来源一目了然，
不把这类文件混进厂商快照里。

---

## UFSDxe.efi

| 项 | 值 |
|---|---|
| 来源 | 用户提供的 `隔离分析/DXE驱动/UFSDxe.efi` |
| 大小 | 143360 |
| 构建 | `BOOT.MXF.2.1-01767.1-LANAI-1_20231219_205220` |
| 构建类型 | `DEBUG_CLANG160LINUX` |
| 厂商树对应文件 | `Binaries/QcomPkg/Drivers/UFSDxe/UFSDxe.efi`（151552，`RELEASE_CLANG160LINUX`，MIUI 构建） |

### 为什么要换

厂商树里的 RELEASE 版只通过 HWIO 协议取 UFS 基址，实测拿到 `0x00000004`：

```
ERROR: 0x00000018 Failed to map UFS_MEM_UFS base address: 0x00000004
```

这份 DEBUG 版除了 HWIO，还会查 PlatformInfo（含
`Error: Failed to locate PlatformInfo protocol.` / `Error: GetPlatformInfo failed.`
两条错误串），因此 HWIO 失败时有回退路径。

### ⚠️ 来源风险

提供该文件的镜像是**用于写 RPMB 解锁小米 14 系列 BL 的工具镜像**。
用户明确要求：**从中提取的文件不得在未提示的情况下直接使用。**

### 使用前做过的检查

| 检查 | 结果 |
|---|---|
| PE 结构 | 合法：AArch64 (`0xAA64`)、PE32+ (`0x20B`)、3 段、subsystem 11（EFI boot service driver）、与原版一致 |
| 段表 | `.text` / `.data` / `.reloc`，结构与原版一致（`.text` 0x1D000 vs 0x1F000，DEBUG/RELEASE 差异） |
| RPMB 相关字符串 | **29 处，与原版完全相同**（`ProvisionRpmbTestKey`、`ConfigRpmbStorPartitions`、`RPMB Listener` 等标准 UFS 功能） |
| `Scm` 相关 | 6 处，与原版相同 |
| `unlock` / `spoof` / `patch` | **0 / 0 / 0**（原版也是 0） |
| `erase` | 1 处，内容是 `Purge event timer creation failed, cannot use non-blocking block erase.`（正常的块擦除提示） |

结论：**能力集与厂商版一致**，没有发现额外的解锁/篡改功能。
但"没找到"不等于"已证明安全" —— 使用前请自行确认来源可信。

### 回退

把 `DXE.full.inc` / `DXE.dsc.inc` / `APRIORI.full.inc` 里的
`IsolatedBinaries/UFSDxe.inf` 改回 `Binaries/QcomPkg/Drivers/UFSDxe/UFSDxe.inf` 即可。
