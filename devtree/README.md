# devtree_viz — 设备树 / 打通状态可视化

一个 Python 脚本，吃两个文件：

1. **设备树**（YAML/JSON）：节点、基址、大小、IRQ、驱动、DT 路径、依赖关系；
2. **状态文件**（YAML/JSON）：每个节点当前是已打通、部分可用、失败还是未开始。

输出一个**单文件、离线可用的交互式 HTML**（也支持 DOT / SVG / PNG / Mermaid / JSON）。
用途是把“这个 SoC 上哪些块已经点亮、卡在谁身上、地址有没有打架”从散落的
Markdown/日志变成一张能点、能搜、能追依赖链的图。

[打开当前设备能力与共享 RAM 状态图](examples/out/shennong.html)

```
tree.yaml ─┐
           ├─► 解析/校验 ─► 分层布局 ─► HTML / DOT / SVG / PNG / Mermaid / JSON
state.yaml ┘        │
                    └─► 阻塞链、状态矛盾、地址重叠、进度统计
```

## 快速开始

```bash
# 依赖：Python 3.8+、PyYAML；Graphviz 的 dot 仅 svg/png 输出需要
python devtree/devtree_viz.py \
  devtree/examples/shennong.tree.yaml \
  devtree/examples/shennong.state.yaml \
  -o devtree/examples/out/shennong.html

# 只校验，不生成文件（适合 CI / 提交前检查）
python devtree/devtree_viz.py tree.yaml state.yaml --check --strict
```

当前数据更新于 2026-11-03，119 个节点：CDC 共享 RAM 上传与 Shell 执行、震动、闪光灯、ACPI/SMC/TZ 与 RPMB 只读；未完成的无线、传感器与 USB 恢复项单独标记。HTML 自包含，状态数据不附带本地日志或检测报告。

> 示例里 **工作状态** 与 **补丁状态** 分开记录：
> 例如 UFS 是“外部 DEBUG 二进制 + 可用”，ClockDxe 是“4 字节指令补丁 + 可用”，
> 而 QcomChargerDxeLA 是“无补丁 + 启动失败”。

## 设备树文件

```yaml
meta:
  title: Shennong (SM8650) UEFI 打通设备树
  subtitle: 小米 shennong · Project Mu
  updated: "2026-10-28"

groups:                       # 可选；不写则按节点里出现的 group 自动建组
  - {id: usb, label: USB, color: "#bc8cff"}

nodes:
  - id: usb_dwc3              # 唯一 id，状态文件靠它关联
    name: DWC3 USB 控制器
    group: usb
    base: 0xA600000           # 支持 0x 十六进制或十进制；无 MMIO 可省略
    size: 0x100000
    irq: [0x82, 0xE, 0xF, 0x11]
    driver: UsbDwc3Dxe
    dt_path: /soc/ssusb@a600000
    compatible: qcom,dwc-usb3-msm
    deps: [usb_hsphy, usb_ssphy, gcc]      # 硬依赖：缺失/未通会阻塞本节点
    soft_deps: [usb_eud]                   # 软依赖：缺失只降级，不阻塞
    tags: [usb, dwc3]
    no_overlap: false         # true 则跳过地址重叠检测（共享内存/保留区）
    patch:                    # 可选：与“工作状态”分开记录
      kind: binary            # none|stock|binary|source|config|disabled|unknown
      ref: Silicon/QC/Sm8650/QcomPkg/PatchedBinaries/ClockDxe.efi
      delta: 4 bytes @0xA810  # 与 stock 的差异（可选）
      enabled: true
      note: 与 stock 差 4 字节；DXE.inc 中已启用
    notes: 人看的补充说明
```

`state` / `status` 也可以直接写在树节点里，但**会被状态文件覆盖**。

`patch.kind` 的含义：

| kind | 含义 |
|---|---|
| `binary` | 二进制替换 / 指令级补丁 |
| `source` | 本仓源码驱动替换 stock |
| `config` | 顺序、映射、配置修补 |
| `disabled` | 补丁存在但未启用 |
| `stock` | 明确使用原厂二进制，无补丁 |
| `none` | 没有补丁（可用来写“补丁文件不存在”这类结论） |
| `unknown` | 补丁状态未核实 |

补丁状态只描述“改了什么”，不代表“能工作”；后者仍由状态文件的
`ok/partial/fail/...` 表示。

## 状态文件

```yaml
meta:
  updated: "2026-10-28"
  source: DXE-STATUS.md

states:
  usb_dwc3:
    state: patched
    evidence: "隔离构建树修复完成；宿主测试通过"
    note: 实机枚举仍以串口日志为准
    updated: "2026-10-27"

  # 简写
  tlmm: ok
```

状态值与项目 `DXE-STATUS.md` 的判定口径一致：

| 值 | 图标 | 含义 | 计入“已打通” | 默认阻塞下游 |
|---|---|---|---|---|
| `ok` | ✅ | 有实机现象支撑 | 是 | 否 |
| `patched` | 🔧 | 打补丁后才工作 | 是 | 否 |
| `partial` | ⚠️ | 部分成功，有已知缺陷 | 否 | 否（`--block-on-partial` 可开） |
| `fail` | ⛔ | 实测失败，有日志证据 | 否 | 是 |
| `todo` | ⏳ | 未开始 | 否 | 是 |
| `unknown` | ❓ | 无任何实机证据 | 否 | 是 |
| `unused` | 🗑️ | 本硬件/本阶段不需要 | 否 | 否 |

别名（`done` / `wip` / `blocked` / `??` / emoji 等）会自动归一化。
状态文件可以只写一部分节点，未写的默认为 `unknown`。
键可以是节点 `id`，也可以直接写 `dt_path`。

## 语义

- **分层**：按硬+软依赖的最长路径分层，列内用重心排序减少连线交叉；
  单列超过 12 个节点自动折成相邻子列。
- **阻塞链**：`blockers` 是沿硬依赖向上传递的全部未通节点；
  `direct_blockers` 是直接未通的依赖。
- **状态矛盾**：节点标记为可用（ok/patched），但上游仍有未通节点时，
  HTML 里用红色虚线和“⛓”标出，并单独统计。这能抓住“下层没通却把上层记成好了”的错误。
- **地址重叠**：区间相交会被列出，区分“包含（contains）”和“部分重叠（partial）”。
  共享内存/保留区可用 `no_overlap: true` 排除。
  （示例里 AOSS 包含 SPMI_PMIC_ARB，对应的就是 GCD 拒绝外层区间那个坑。）
- **校验**：重复 id、未知依赖、依赖环、未知状态、未知 patch.kind、
  状态文件里的孤儿节点都会报错；有错误时不生成输出。
- **补丁与状态分离**：侧栏会显示补丁类型、来源文件、字节差异和是否启用；
  工具栏的“⚙ 有补丁记录”可按补丁过滤。

## 交互式 HTML

- 滚轮缩放、拖拽平移、左下小地图（点击跳转）；
- 搜索 名称/id/基址/驱动/DT 路径；按状态、分组、补丁记录过滤；
- 单击节点：基址、大小、区间、IRQ、驱动、DT 路径、依赖/被依赖、
  阻塞链、证据、备注；
- 选中节点时高亮它的上下游路径，其余淡出；
- 侧栏“需要关注”列出失败/未开始/受阻项，“地址重叠”可点击定位；
- 快捷键：`/` 搜索、`0` 全览、`+/-` 缩放、`Esc` 返回总览、双击节点聚焦。

## 其它输出

```bash
-f html      自包含交互页（默认）
-f dot       Graphviz DOT
-f svg/png   调用本机 dot 渲染
-f mermaid   Mermaid flowchart（贴文档用）
-f json      计算后的完整数据（节点、边、阻塞、冲突、统计）
```

## 从真实 DTS 生成骨架

```bash
# 先把 dtb 转成文本：dtc -I dtb -O dts -o fdt.dts fdt.dtb
# 或直接吃 fdtdump 输出（本项目 extract/shennong_fdt.dts 就是）
python devtree/devtree_viz.py --import-dts extract/shennong_fdt.dts \
  -o devtree/examples/out/imported.tree.yaml \
  --under /soc --depth 1
```

导入器会：

- 按父节点 `#address-cells/#size-cells` 解析 `reg` 的基址/大小；
- 用 `__symbols__` 的标签给节点起 id；
- 按 phandle 引用（clocks/resets/interconnects/power-domains/`*-supply` 等）
  自动连依赖，并丢弃成环的回边（会打印丢弃数量）；
- 把 `status = "okay"` 只记进 notes —— **DT 的 okay 不等于 UEFI 已打通**；
- 对 sharedmem/sram 类节点自动设置 `no_overlap: true`。

导入结果只是骨架：`driver`、`irq` 需要手工补，依赖也需要人工按驱动初始化
顺序筛选。可以用 `--include` / `--exclude` 控制范围（默认排除 coresight）。

## 限制

- 状态必须人工标注，工具不会从 DT 或日志里猜“打通”；
- 依赖关系是**逻辑依赖**，不是编译期 DEPEX，也不自动等于驱动加载顺序；
- 大型导入树（几百节点）全览时只能看结构，定位要靠搜索/分组/小地图；
- 一个节点只表示一段主 MMIO 区间，多段 `reg` 只取第一段并写进 notes。

## 目录

```
devtree/
├── devtree_viz.py              # 主脚本（单文件，仅依赖 PyYAML）
├── README.md
└── examples/
    ├── shennong.tree.yaml      # 56 节点示例树
    ├── shennong.state.yaml     # 对应状态（DXE-STATUS 口径）
    ├── preview.png             # 交互页截图（PMIC GLINK 探针详情）
    └── out/shennong.html       # 生成好的交互页
```
