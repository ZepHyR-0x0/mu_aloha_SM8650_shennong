## @file
#
#  Copyright (c) 2011-2015, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2016, Intel Corporation. All rights reserved.
#  Copyright (c) 2018 - 2019, Bingxing Wang. All rights reserved.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##
# NOTE (SM8650/Xiaomi 14 port, 2026-10):
#  - SoC: SM8650 (Snapdragon 8 Gen 3, Qualcomm codename "pineapple"), Xiaomi 14 = shennong/houji.
#  - CONFIRMED from live DTB dump (/sys/firmware/fdt of shennong, 2026-10-21):
#    * Debug UART = 0x89C000 (DT node qcom,qup_uart@89c000, compatible "qcom,geni-debug-uart").
#      NOTE: MTP reference value is 0xA9C000 - do NOT use it on shennong hardware.
#    * GIC: dist 0x17100000 / redist 0x17180000 / its 0x17140000 (DT interrupt-controller@17100000).
#    * SMEM = 0x81D00000 (+0x200000); PDC = 0xB220000; TLMM pinctrl = 0xF000000.
#    * Arch timer = 19.2MHz. msm-id 0x22d(557 PINEAPPLE)/0x241(577 PINEAPPLEP), board-id 0x08.
#  - TODO(verify with abl_a dump): PcdCpuVectorBaseAddress/GIC-PrePiStackBase are carried over
#    from Sm8550 (kalama); UEFI FD window 0xC7CC0000 needs RamPartitionDxe/uefiplat.cfg confirmation.
#  - -march stays armv9-a for GCC 9.4 toolchain compatibility.
##
[BuildOptions.common]
  *_*_*_CC_FLAGS = -march=armv9-a+crypto+rcpc

[PcdsFixedAtBuild.common]
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000         # Starting address, 2GB Base
  gAndromedaPkgTokenSpaceGuid.PcdCpuVectorBaseAddress|0xa7600000
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x17100000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x17180000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0x17140000

  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x00008650
  gEmbeddedTokenSpaceGuid.PcdPrePiStackBase|0xa760d000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackSize|0x00040000      # 256K stack
  # TODO: Find the PcdInterruptBaseAddress
  gEmbeddedTokenSpaceGuid.PcdInterruptBaseAddress|0x09bc0000
  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|44

  gAndromedaPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Snapdragon (TM) 8 Gen 3 @ 3.30 GHz"
  gAndromedaPkgTokenSpaceGuid.PcdSmbiosProcessorRetailModel|"SM8650"

  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|3

  # UART - debug UART from shennong DTB: qcom,qup_uart@89c000 (geni-debug-uart)
  gAndromedaPkgTokenSpaceGuid.PcdUartSerialBase|0x89C000

[LibraryClasses.common]
  # Different Device has different Memory Map.
  # So the define of this Lib is moved to KailuaPkg/Kailua.dsc
  #PlatformMemoryMapLib|QcomPkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf

  # Different Device has different Configuration Map.
  # So the define of this Lib is moved to KailuaPkg/Kailua.dsc
  #PlatformConfigurationMapLib|QcomPkg/Library/PlatformConfigurationMapLib/PlatformConfigurationMapLib.inf

  PlatformPeiLib|QcomPkg/PlatformPei/PlatformPeiLib.inf
  PlatformPrePiLib|QcomPkg/Library/PlatformPrePiLib/PlatformPrePiLib.inf
  AslUpdateLib|AndromedaPkg/Library/DxeAslUpdateLib/DxeAslUpdateLib.inf
  RFSProtectionLib|QcomPkg/Library/RFSProtectionLib/RFSProtectionLib.inf

  AcpiPlatformUpdateLib|QcomPkg/Library/AcpiPlatformUpdateLib/AcpiPlatformUpdateLib.inf
  PlatformThemeLib|QcomPkg/Library/PlatformThemeLib/PlatformThemeLib.inf
