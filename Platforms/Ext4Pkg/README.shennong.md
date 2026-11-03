# Shennong 的只读 ext4 集成

来源和原始文件哈希见 `UPSTREAM.json`。其中哈希记录导入时的上游内容，
不代表本地适配后的文件内容。源码保留上游版权及 BSD-2-Clause-Patent 标识。

平台从 `Ext4Pkg/Ext4Dxe/Ext4Dxe.inf` 构建并嵌入 UEFI_DRIVER，由正常的
DriverBinding / ConnectController 挂载；不直接调用原厂 UFS 私有入口。
独立 Ext4Pkg.dsc 未在本项目单独验收。USB 控制器驱动未修改。

本地改动：

- 将 UTF8 库包含在包内，适配 MU 的包路径和 library class。
- 挂载前要求现有 ReadOnlyStorageDxe 的完整下层保护与 ReadOnly media，
  Open 只接受 READ，Write/SetInfo 保持拒绝写，Flush 为只读空操作。
- 拒绝脏状态、错误状态、孤儿恢复及 journal recovery；加密、inline data、
  未知 incompat 特性由上游拒绝。不会回放日志、解密或修改分区。
- 修正最后部分 block group 的计数，包括仅有一个不完整 group 的小分区。
- 检查 inode/descriptor 大小、GDT 分配上限、磁盘容量及读取边界。
- 修正 UTF8 字节数与 UCS2 字符数比较、255 字节路径段、创建时间字段。
- UTF8 库位移前转为无符号字节，避免 host 上 signed CHAR8 的未定义行为。
- 安装 SFS 失败时释放完整挂载资源。

Linux 验证：

```text
python3 scripts/test_ext4_readonly.py --reference-image /path/to/shennnong_dsp.img
```

测试编译真实驱动 C、真实 UTF8 库和 MU 的红黑树/链表，使用 UEFI I/O mock
与 ASan/UBSan。1 KiB、4 KiB ext4 镜像验证目录、嵌套文件、中文名、255 字节名、
符号链接、稀疏洞、seek/EOF、拒绝写和异常挂载。两个 DSP 样本文件与 debugfs
导出逐字节比较；输入镜像前后哈希相同，底层 write mock 次数为 0。

当前不支持 F2FS、EROFS、文件系统加密、journal recovery。GUI 对不能挂载
的分区只预览起始原始字节。固件构建和 host 测试不代表实机 UFS 枚举、
自动绑定或 GUI 交互已经成功。
