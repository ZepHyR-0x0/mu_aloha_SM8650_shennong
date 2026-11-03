# mu_aloha_SM8650_shennong

[**中文**](README.md) | **English**

> **When borrowing from or using this project, please credit the source prominently in your own project.**
> See [Project Usage](#project-usage) for details.

**Project Mu** UEFI port for **Xiaomi 14 Pro** (shennong / SM8650): SM8650 overlay sources, one-click build scripts, bootable image, and device status visualization.

- Development snapshot: 2026-11-03; DEBUG / ShennongNoSb.
- Image: `Build/ShennongPkg/xiaomi-shennong.img`, 2,785,280 bytes
  SHA256 `c55947484c7c9635df6791e6ba2728a970531a267bd822934ce7a8824d29593e`
- Baseline: Project Mu Aloha `dev/update/mu202502`

## Screenshots

<table>
<tr>
<td><img src="docs/screenshots/boot-menu.png" width="330" alt="Boot Menu"></td>
<td><img src="docs/screenshots/usb-function.png" width="330" alt="USB Function"></td>
</tr>
<tr>
<td align="center">Boot Menu — auto-enters GUI after 5 s of inactivity; can exit to Device Mode</td>
<td align="center">USB Function — four switchable modes</td>
</tr>
</table>
<img src="docs/screenshots/uefi-shell-osk.jpg" width="330" alt="UEFI Shell with On-Screen Keyboard">

**UEFI Shell + On-Screen Keyboard** — block device list from `alias` output, with the soft keyboard at the bottom.

## Current Status

Display, touch, CDC commands, exact 64 / 128 MiB shared RAM allocations, file upload with CRC readback, NSH execution and a returning ARM64 EFI launch have passed on the device. RAM supports requests from 64 to 8192 MiB; 8 GiB allocation has not been validated on hardware. FAT32 files are limited to 4294967295 bytes.

Vibration and flash light have user confirmation. TEST includes a dedicated haptic page with duration and intensity sliders, hardware queries and device lists. RPMB access is read only, with raw response checks and unchanged counters; MAC authentication is unavailable. ACPI table export, links and checksums have passed, while WinPE platform adaptation and secure application business functions remain incomplete.

USB uses the unchanged stock hardware chain. Shared RAM MSC mounting, host file hashes and timed return to CDC passed. Repeated physical CDC unplug/replug, including interruption of log reception, restored enumeration and preserved a test file; a subsequent switch confirmed lock release. Mid-replacement-upload unplug has not yet been captured on this candidate. WiFi/Bluetooth providers, GPS fixes, live sensor samples and brightness control remain incomplete.

The CDC client exposes `ramdisk create/info/hash/release`, `--upload ... --dest ...`, `shell <command>`, `boot <mapped.efi>`, `usb ramdisk <return-seconds>`, `test`, `haptic` and read only RPMB commands. CDC, local read only BlockIo and MSC share one RAM backing allocation. Resolve FS mappings through `shell map -r`.

The client supports `--upload-dir path/to/media` to preserve a complete boot file tree. A local ARM64 ADK 28000.1 minimal command-prompt PE (344.1 MiB, LZX, OfflineAll SAN policy, automount disabled) has passed WIM integrity and ARM64 loader checks. On the current candidate, all 23 media files, WIM CRC readback and four boot-file listings passed. Boot was accepted, then the phone rebooted to Android. The recovered 156133-byte snapshot has phase `efi-shell-start`; no matching return or specific PE error was captured. The reboot cause remains unresolved.

Firmware and client default to CDC line coding 3,000,000 8N1, overridable with `--baud`. File receives are submitted in batches up to 64 KiB; baud alone does not determine this USB path's throughput. Known-length ACK reads avoid waiting for a 64 KiB receive buffer after each upload window. Normal replies retain the established bulk-read path. Alternating 4 MiB uploads on the same firmware measured 0.49 MiB/s with the old client and 0.68 MiB/s with the new client (40% faster), with CRC and complete log framing verified. The 344.1 MiB WIM took 256.4s (1.34 MiB/s), with CRC readback. During uploads, custom pages service bounded bursts and slice page waits at 1 ms; normal waits resume after commit or cancellation. USB driver binaries and all 30 source-lock entries are unchanged.

Queued and active switches exclude other switches and RAM changes. A switch drains the current control/BOT response for at most five seconds and releases its lock on success, disconnect, timeout or stop failure. Unconfirmed controller halt retains DMA buffers. A physical cable falling edge cancels the session and defers stock Stop/StartEx outside the pump by 200 ms. Ordinary bus reset does not take this path. This USB candidate and its stock hardware chain are held by the source lock.

The repository contains production sources and product documentation. Local logs, diagnostic reports and test harnesses stay in the development workspace.

EFI launch now persists Shell start, return and load-failure checkpoints in the existing restricted logfs store, readable through `logcat-last` after reboot. A 3072-byte ARM64 EFI execution marker, normal return and 154821-byte snapshot CRC after a cold reboot have passed. The current image adds FrontPage launch journaling; upload performance evidence comes from the preceding candidate; the current PE retry also rebooted and its launch checkpoint was recovered. The PE reboot cause remains unresolved.

## One-Click Build

Linux / WSL:

```bash
./build.sh              # DEBUG (default)
./build.sh RELEASE      # release build
```

Windows (auto-delegates to WSL):

```bat
build.cmd
```

First run automatically:

1. Clones Project Mu Aloha baseline (default `~/mu_aloha_base`) and pulls submodules;
2. Lays down this repo's SM8650 overlay and applies patches to four submodules via `patches/`;
3. Creates a build tree on ext4 (default `~/mubuild-shennong`, avoiding slow `/mnt/*` 9p mounts);
4. Creates a Python venv (default `~/mu-venv-shennong`) and installs `pip-requirements.txt`;
5. Builds `xiaomi-shennong`, verifies logs and image, copies results back to `Build/ShennongPkg/`.

Optional environment variables: `MU_BASE`, `MU_BASE_REF`, `MU_WORK`, `MU_VENV`, `FORCE_INIT=1`.
Scripts do not depend on any absolute host path — place the repo anywhere and run directly.

Output:

```
Build/ShennongPkg/xiaomi-shennong.img   # for fastboot boot
```

## Device Tree Visualization

The following shows detailed project status:

**[Open devtree/examples/out/shennong.html](devtree/examples/out/shennong.html)**

<iframe src="devtree/examples/out/shennong.html" width="100%" height="640" style="border:1px solid #d0d7de;border-radius:8px"></iframe>

> Some Markdown previewers (including GitHub) do not render `iframe` for security reasons. If so, click the link above directly, or clone the repo and open `devtree/examples/out/shennong.html` in a browser.

Double-click to open in a local clone; it contains 107 nodes, 204 hard dependencies + 27 soft dependencies, 17 layout levels — searchable, filterable by status/group, clickable nodes with evidence chains, blocking chains, and address conflicts.

Current snapshot (`devtree/examples/shennong.state.yaml`, updated 2026-11-02 / b341):


## QR Log Reader

`tools/qr_camera_reader/` is a desktop GUI that uses a webcam to continuously decode QR-code boot logs scrolling on the firmware screen and stitch them together:

```bash
pip install -r tools/qr_camera_reader/requirements.txt
python tools/qr_camera_reader/qr_camera_reader.py --camera 0
```

Supports manual/auto exposure and metering lock (`--exposure`, `--auto-exposure`, `--no-meter`); decoded results can be copied and saved. Used for reading boot logs when no UART is available.

## Repository Contents

| Path | Contents |
|---|---|
| `Platforms/ShennongPkg/` | SM8650 / Xiaomi 14 Pro platform & device layer (DSC/FDF, DXE list, DTB, device blobs) |
| `Platforms/AndromedaPkg/` | FrontPage, QR log, DIAG, UfsPreDxe, ShmBridge stub and other porting changes |
| `Silicon/QC/Sm8650/` | QcomPkg (PlatformPeiLib shim, PlatformTheme, RFS, etc.) |
| `patches/` | Porting patches for four submodules (mu_basecore / mu_plus / mu_silicon_arm_tiano / surface_duo_binaries) |
| `scripts/` | `fastbuild.sh` (compat entry), `profile.sh`, `verify_usb_image.py`, `package_usb_variant.py`, etc. |
| `build_cfg/sm8650.json` | BootShim window (0xA7100000 / 4 MB) |
| `devtree/` | Device tree visualization: scripts, YAML data, single-file HTML |
| `tools/qr_camera_reader/` | QR code log reader |
| `Build/ShennongPkg/xiaomi-shennong.img` | Current bootable image |

This repo does **not** include the Project Mu common base tree, other device targets, other git branches, or analysis/extraction directories — those are prepared automatically by `build.sh`.

## Goal

Provide a bootable, UFS-readable, QR-log-capable, and **state-visualized** Project Mu base for **UniXNU / Hackios** development on Xiaomi 14 Pro (shennong / SM8650).

## Safety Constraints (from actual bring-up)

- Only `fastboot boot` for in-memory execution — **no partition writes**; Xiaomi 14 Pro has no unauthorized EDL, bricking requires disassembly or official repair.
- No disassembly, no UART — debugging is via USB / screen QR codes only.
- Crash recovery: long-press power button for 10–15 seconds.
- This image is for **Xiaomi 14 Pro / shennong** only; do not flash to other devices.

## Project Usage

**If you borrow from or use this project's code, patches, documentation, screenshots, or device-tree visualization data, please credit the source prominently in your own project.**

"Prominently" means visible when the project is opened, for example:
- A notice section at the top of the README or on the landing page
- A dedicated Credits / Acknowledgements section

Suggested wording:

> The SM8650 UEFI port in this project is based on / references the work of **mu_aloha_SM8650_shennong** (Xiaomi 14 Pro / shennong).

Only attribution is required — you are not obligated to open-source your modifications; please **do not** remove existing source attributions in this repo, and do not represent direct copies of this project's work as original. Commercial use and redistribution are equally subject to these terms.

## License & Disclaimer

Code inherits the licenses of Project Mu Aloha and its submodules (see [LICENSE](LICENSE)); third-party components follow their respective licenses.
This repo is provided "as is" for research and development purposes only, without any warranty. Use at your own risk.

## Acknowledgements

Every layer of this project builds on the work of the open-source community.

**Firmware Framework**
- [mu_aloha_platforms_sm8650](https://github.com/xingguangcuican6666/mu_aloha_platforms_sm8650) — source repository of this project
- [Project Mu](https://github.com/microsoft/mu) & [TianoCore EDK II](https://github.com/tianocore/edk2) — UEFI framework & build system
- [Project Mu Aloha](https://github.com/Project-Aloha/mu_aloha_platforms) — this project derives directly from its platform layer
- Microsoft `mu_basecore`, `mu_plus`, `mu_tiano_plus`, `mu_silicon_arm_tiano`,
  `mu_feature_config`, `mu_feature_dfci` — six base submodules
- [Xiaomi_Kernel_OpenSource-shennong](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) —
  kernel implementations for eUSB2 repeater and DWC3 PHY
- Xiaomi SM8650 devicetree source — pin, regulator, and panel parameters
- [UniXNU](https://github.com/UniXNU) houji project — structural reference for the S3910 touch driver
- UEFI-For-OnePlus13R — also SM8650 / lanai; correct source for the FD window `0xA7100000`

**Toolchain**
- `tools/qr_camera_reader`: OpenCV, zxing-cpp, Pillow
- `devtree/`: Python 3.8+ & PyYAML; Graphviz required for SVG/PNG export

**Agent**
- help from `deepseek 4.1 flash` and  `GPT 6.1 sol`
  
Copyrights of stock binaries belong to Qualcomm and Xiaomi; this repo only extracts and invokes them, and does not redistribute their source code.
