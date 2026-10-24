# 许可证与再分发提示

## 代码
本仓库的移植代码基于 Project Mu Aloha（`Project-Aloha/mu_aloha_platforms`）及其子模块，
沿用其许可证，见根目录 `LICENSE`。第三方子模块（MU_BASECORE、mu_plus、mu_silicon_arm_tiano、
OpenSSL、SurfaceDuoBinaries 等）分别遵循各自仓库的许可证。

## 二进制 blob（重要）

以下内容是从 **作者自己的 Xiaomi 14（shennong）固件/解锁镜像中提取**的，**不属于**本仓库代码许可证：

- `Platforms/ShennongPkg/Device/xiaomi-shennong/Binaries/`
- `Platforms/ShennongPkg/Device/xiaomi-shennong/IsolatedBinaries/`
- `Platforms/ShennongPkg/Device/xiaomi-shennong/PatchedBinaries/`
- `Platforms/ShennongPkg/Device/xiaomi-shennong/DeviceTreeBlob/`（设备树）
- `Silicon/QC/Sm8650/QcomPkg/PatchedBinaries/`
- 构建出的 `Build/ShennongPkg/xiaomi-shennong.img` 内含上述模块

这些文件的权利归 Qualcomm / Xiaomi 等原权利人所有。**公开再分发可能侵犯版权**。
发布到 GitHub 前建议：

1. 将仓库设为 **private**；或
2. 删除上述目录，改为提供 `extract_blobs.sh`，让使用者从自己的设备/固件中提取；或
3. 仅在符合当地法律和权利人许可的前提下发布。

## 免责

- 本项目仅用于个人设备研究与 bring-up，**不提供任何担保**。
- 只通过 `fastboot boot` 内存运行；作者未验证任何分区写入。
- 设备无 EDL；错误操作可能导致无法启动，恢复需长按电源 10–15 秒或重新 `fastboot boot`。
- 不要在非 Xiaomi 14 / shennong 设备上使用。
