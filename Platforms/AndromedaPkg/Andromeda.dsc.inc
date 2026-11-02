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

[Components]
  !include $(SHARED_CRYPTO_PATH)/Driver/Bin/CryptoDriver.inc.dsc

[PcdsFeatureFlag.common]
  gArmTokenSpaceGuid.PcdCpuDxeProduceDebugSupport|FALSE

  gEfiMdePkgTokenSpaceGuid.PcdComponentNameDisable|TRUE
  gEfiMdePkgTokenSpaceGuid.PcdDriverDiagnosticsDisable|TRUE
  gEfiMdePkgTokenSpaceGuid.PcdComponentName2Disable|TRUE
  gEfiMdePkgTokenSpaceGuid.PcdDriverDiagnostics2Disable|TRUE

  gEfiMdeModulePkgTokenSpaceGuid.PcdTurnOffUsbLegacySupport|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutGopSupport|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSupportUpdateCapsuleReset|TRUE

  gEmbeddedTokenSpaceGuid.PcdPrePiProduceMemoryTypeInformationHob|TRUE

  gEfiMdeModulePkgTokenSpaceGuid.PcdPciBusHotplugDeviceSupport|FALSE

  gEfiMdeModulePkgTokenSpaceGuid.PcdInstallAcpiSdtProtocol|TRUE

[PcdsFixedAtBuild.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"DuoWoA authors"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"2412.74"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareRevision|0x01241274
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemId|"QCOM  "
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x324B44454D4F4351
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultCreatorId|0x4D4F4351
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultCreatorRevision|0x00000001
  gEfiMdePkgTokenSpaceGuid.PcdMaximumUnicodeStringLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdMaximumAsciiStringLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdMaximumLinkedListLength|1000000
  gEfiMdePkgTokenSpaceGuid.PcdSpinLockTimeout|10000000
  gEfiMdePkgTokenSpaceGuid.PcdUefiLibMaxPrintBufferSize|320
  gAdvLoggerPkgTokenSpaceGuid.PcdAdvancedLoggerPreMemPages|3
  gEfiMdePkgTokenSpaceGuid.PcdEnforceSecureRngAlgorithms|FALSE

!if $(TARGET) == DEBUG
  gEfiMdePkgTokenSpaceGuid.PcdDebugClearMemoryValue|0xAF
  #
  # 0x0f has ASSERT_BREAKPOINT_ENABLED, so every ASSERT halts the machine. A DEBUG
  # build is wanted for the extra DEBUG() output, not for new stopping points, so the
  # breakpoint bit is cleared: asserts still print file/line/expression and execution
  # continues. 0x07 = DEBUG_PRINT | ASSERT | DEBUG_CODE.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x07
  #
  # ERROR and WARN only. At 0x800fee0f the DXE dispatcher's per-driver
  # "Loading driver at ..." lines alone exceed the 64 KB log ring, so the ring wraps
  # and the early boot is lost before the QR viewer can read it. Every failure message
  # this bring-up needs is ERROR level, so narrowing the mask keeps the whole timeline
  # instead of discarding most of it.
  #
  #
  # ERROR | WARN | INFO. Narrowing this to ERROR|WARN kept the log inside the ring,
  # but it also silenced VerifiedBootDxe completely - the driver that was enabled
  # specifically to report whether the devinfo partition is readable - because its
  # messages are INFO. The DXE dispatcher's per-driver lines come back with INFO, but
  # the performance library is off now, so the volume stays inside 60 KB.
  #
  #
  # ERROR | WARN, deliberately without INFO.
  #
  # Enabling INFO reset the machine at "Loading DxeCore" - before any driver ran. The
  # only thing INFO changes that early is volume: every SerialPortWrite goes through
  # FrameBufferInMemorySerialPortLib, which touches the display and the log ring on
  # each write, so a few hundred extra INFO lines is a very different workload from a
  # few dozen ERROR lines. ERROR and WARN carry everything this bring-up needs to see
  # fail; INFO is not worth losing the boot over.
  #
  # VerifiedBootDxe reports its devinfo failures at ERROR, so its diagnostics survive
  # this setting - the messages we want are not the ones being filtered.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000003
  #
  # Performance measurement off. In DEBUG this is enabled, and DxeCorePerformanceLib
  # keeps a linked list of records as drivers load. Two symptoms in the boot log come
  # from it, one right after the other:
  #
  #   Failed to get Module Info from Handle! Status = Invalid Parameter
  #   ASSERT [DxeCore] LinkedList.c(267): InternalBaseLibIsListValid (ListHead)
  #
  # LinkedList.c(267) is InsertTailList, so the record list is what is failing
  # validation. None of this measures anything useful during bring-up, and turning it
  # off removes the list, the records and the assert together.
  #
  gEfiMdePkgTokenSpaceGuid.PcdPerformanceLibraryPropertyMask|0
  gEfiMdePkgTokenSpaceGuid.PcdPostCodePropertyMask|0
  gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|0x07
!else
  gEfiMdePkgTokenSpaceGuid.PcdDebugClearMemoryValue|0x00
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x00
  # DEBUG-TRACE: enable ERROR|INFO so the DXE dispatcher prints "Loading driver <GUID>"
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000040
  gEfiMdePkgTokenSpaceGuid.PcdPerformanceLibraryPropertyMask|0
  gEfiMdePkgTokenSpaceGuid.PcdPostCodePropertyMask|0
  gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|0x00
!endif

  gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange|FALSE

  #
  # Make VariableRuntimeDxe work at emulated non-volatile variable mode.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE

  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiACPIReclaimMemory|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiACPIMemoryNVS|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiReservedMemoryType|0
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesData|600
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiRuntimeServicesCode|400
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiBootServicesCode|1500
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiBootServicesData|800
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiLoaderCode|10
  gEmbeddedTokenSpaceGuid.PcdMemoryTypeEfiLoaderData|0

  ## Default Terminal Type
  ## 0-PCANSI, 1-VT100, 2-VT00+, 3-UTF8, 4-TTYTERM
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|4

  # Boot Manager
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|0

  # PCI Express
  gEfiMdeModulePkgTokenSpaceGuid.PcdSrIovSupport|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdAriSupport|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdMrIovSupport|FALSE
  gArmTokenSpaceGuid.PcdPciBusMin|0x0
  gArmTokenSpaceGuid.PcdPciBusMax|0xff

  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxAuthVariableSize|0x2800
  gEfiSecurityPkgTokenSpaceGuid.PcdUserPhysicalPresence|TRUE

  # a PCD that controls the enumeration and connection of ConIn's. When true, ConIn is only connected once a console input is requests
  gEfiMdeModulePkgTokenSpaceGuid.PcdConInConnectOnDemand|FALSE

  # Enable SHELL to build instead of just taking the binary
  gEfiMdePkgTokenSpaceGuid.PcdUefiLibMaxPrintBufferSize|16000
  gEfiShellPkgTokenSpaceGuid.PcdShellProfileMask|0x1f    # All profiles

  gMsGraphicsPkgTokenSpaceGuid.PcdUiThemeInDxe|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerInBootOrder|TRUE

  # PORT: BDS launches the FFS file named here as its "boot manager menu".
  # Upstream points at FrontPage.inf (4042708A-...). On this port FrontPage
  # starts, draws its logo, then dies with a synchronous exception at
  # 0xD5301224 (DXE heap) followed by "recursive exception while dumping CPU
  # state" - it expects SMM variables, an input device and the Dfci/SWM stack
  # that this platform does not have yet.
  # Point the menu at the UEFI Shell instead: it exercises ConOut/HII/BDS and
  # auto-runs startup.nsh (embedded in the FV) to dump diagnostics.
  # 7C04A583-9E3E-4F1C-AD65-E05268D0B4D1 = gUefiShellFileGuid
  # NOTE: this value is OVERRIDDEN. Shennong.dsc includes
  # AndromedaPkg/Frontpage.dsc.inc *after* this file, and that one sets the same
  # PCD to FrontPage (4042708A-0F2D-4823-AC60-0D77B3111889). So the boot manager
  # menu is FrontPage, not the Shell below - the Shell is only what BDS falls
  # back to when FrontPage fails to launch, which is what used to happen because
  # DisplayEngine never dispatched (SetupBrowserDxe was missing from the FV).
  # Kept as the Shell GUID so the upstream default is still recognisable.
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{ 0x83, 0xA5, 0x04, 0x7C, 0x3E, 0x9E, 0x1C, 0x4F, 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 }

  gOemPkgTokenSpaceGuid.PcdUefiVersionNumber|0x1241274 # 1.24.12.74
  gOemPkgTokenSpaceGuid.PcdUefiBuildDate|0x20241223 # 2024-12-23

  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000

!include AndromedaPkg/SecureBootKeys.dsc.inc
!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses.common]
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf

  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf

  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf

  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf

  #
  # Ramdisk Requirements
  #
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf

  # PCD library
  #
  # DxePcdLib declares [Depex.common.DXE_DRIVER] gEfiPcdProtocolGuid and EDK2 merges
  # a library's DEPEX into every module that links it, so this line used to park 55
  # of the 241 files in the FV behind a protocol nothing published. ArmCpuDxe,
  # ArmGicDxe, ArmTimerDxe, BdsDxe, DisplayEngine, HiiDatabase, AcpiTableDxe and
  # DevicePathDxe were among them - none had ever dispatched.
  #
  # BasePcdLibNull was tried as a fix. It removed the dependency, but it stores
  # nothing: every LibPcdSet* is a no-op and every LibPcdGet* asserts and returns 0.
  # That is harmless for FixedAtBuild PCDs, which the build resolves to literals
  # without calling the library at all. It is not harmless for DynamicEx ones, and
  # the GUI died on exactly that - FrontPage writes a pointer into
  # PcdCurrentPointerState and DisplayEngine reads it back to find the shared buffer
  # between them. With the null library the write vanished, the read returned 0, and
  # FormDisplay.c:2087 asserted.
  #
  # PcdShimDxe now publishes a real PCD protocol backed by an in-memory table, so
  # DxePcdLib works and the dependency is satisfied rather than avoided.
  #
  PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf

  # use the accelerated BaseMemoryLibOptDxe by default, overrides for SEC/PEI below
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf

  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf

  # ARM Architectural Libraries
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  DefaultExceptionHandlerLib|ArmPkg/Library/DefaultExceptionHandlerLib/DefaultExceptionHandlerLib.inf
  CpuExceptionHandlerLib|ArmPkg/Library/ArmExceptionLib/ArmExceptionLib.inf
  MuArmGicExLib|MsCorePkg/Library/MuArmGicExLib/MuArmGicExLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf

  ArmHvcLib|ArmPkg/Library/ArmHvcLib/ArmHvcLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf
  
  IoMmuLib|MdeModulePkg/Library/IoMmuLib/IoMmuLib.inf
  Tpm2HelpLib|SecurityPkg/Library/Tpm2HelpLib/Tpm2HelpLib.inf

!if $(USE_PHYSICAL_TIMER) == 1
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
!else
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerVirtCounterLib/ArmGenericTimerVirtCounterLib.inf
!endif

  MemoryInitPeiLib|AndromedaPkg/MemoryInitPei/MemoryInitPeiLib.inf

  # ARM PL031 RTC Driver
  RealTimeClockLib|EmbeddedPkg/Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf

#######################
# Add UART section here.
!if $(USE_SCREEN_FOR_SERIAL_OUTPUT) == 1
  !if $(USE_MEMORY_FOR_SERIAL_OUTPUT) == 1
    SerialPortLib|AndromedaPkg/Library/FrameBufferInMemorySerialPortLib/FrameBufferInMemorySerialPortLib.inf
  !else
    SerialPortLib|AndromedaPkg/Library/FrameBufferSerialPortLib/FrameBufferSerialPortLib.inf
  !endif
!else
  !if $(USE_MEMORY_FOR_SERIAL_OUTPUT) == 1
    SerialPortLib|AndromedaPkg/Library/InMemorySerialPortLib/InMemorySerialPortLib.inf
  !else
    !if $(USE_UART_GENI_FOR_SERIAL_OUTPUT) == 1
      SerialPortLib|AndromedaPkg/Library/QcomGeniSerialPortLib/QcomGeniSerialPortLib.inf
    !else
      !if $(USE_UART_DM_FOR_SERIAL_OUTPUT) == 1
        SerialPortLib|AndromedaPkg/Library/UartDmSerialPortLib/UartDmSerialPortLib.inf
      !else
        SerialPortLib|MdePkg/Library/BaseSerialPortLibNull/BaseSerialPortLibNull.inf
      !endif
    !endif
  !endif
!endif
#
########################

  #
  # Uncomment (and comment out the next line) For RealView Debugger. The Standard IO window
  # in the debugger will show load and unload commands for symbols. You can cut and paste this
  # into the command window to load symbols. We should be able to use a script to do this, but
  # the version of RVD I have does not support scripts accessing system memory.
  #
  #PeCoffExtraActionLib|ArmPkg/Library/RvdPeCoffExtraActionLib/RvdPeCoffExtraActionLib.inf
  #PeCoffExtraActionLib|ArmPkg/Library/DebugPeCoffExtraActionLib/DebugPeCoffExtraActionLib.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf

  DebugAgentLib|MdeModulePkg/Library/DebugAgentLibNull/DebugAgentLibNull.inf
  DebugAgentTimerLib|EmbeddedPkg/Library/DebugAgentTimerLibNull/DebugAgentTimerLibNull.inf

  # Flattened Device Tree (FDT) access library
  FdtLib|EmbeddedPkg/Library/FdtLib/FdtLib.inf

  # USB Libraries
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf

  #
  # OpensslPkg libraries needed by multiple firmware features
  #
  RngLib|MdePkg/Library/DxeRngLib/DxeRngLib.inf
  Hash2CryptoLib|SecurityPkg/Library/BaseHash2CryptoLibNull/BaseHash2CryptoLibNull.inf

  #
  # Secure Boot dependencies
  #
  AuthVariableLib|AndromedaPkg/Library/AuthVariableLib/AuthVariableLib.inf
  SecureBootVariableLib|SecurityPkg/Library/SecureBootVariableLib/SecureBootVariableLib.inf
  SecureBootVariableProvisionLib|SecurityPkg/Library/SecureBootVariableProvisionLib/SecureBootVariableProvisionLib.inf
  # re-use the UserPhysicalPresent() dummy implementation from the ovmf tree
  PlatformSecureLib|AndromedaPkg/Library/PlatformSecureLib/PlatformSecureLib.inf

  # Security Libraries
  LockBoxLib            |MdeModulePkg/Library/LockBoxNullLib/LockBoxNullLib.inf
  PasswordPolicyLib     |OemPkg/Library/PasswordPolicyLib/PasswordPolicyLib.inf
  SecureBootKeyStoreLib |MsCorePkg/Library/BaseSecureBootKeyStoreLib/BaseSecureBootKeyStoreLib.inf
  PlatformPKProtectionLib|SecurityPkg/Library/PlatformPKProtectionLibVarPolicy/PlatformPKProtectionLibVarPolicy.inf
  MuSecureBootKeySelectorLib|MsCorePkg/Library/MuSecureBootKeySelectorLib/MuSecureBootKeySelectorLib.inf

  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf

  # Variable Libraries
  VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf

  # Whea Libraries
  MuTelemetryHelperLib  |MsWheaPkg/Library/MuTelemetryHelperLib/MuTelemetryHelperLib.inf

  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf

  # Base ARM libraries
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  ArmMmuLib|ArmPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf

  ArmPlatformLib|AndromedaPkg/Library/ArmPlatformLib/ArmPlatformLib.inf

  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf

  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  PlatformBootManagerLib|MsCorePkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  FrameBufferBltLib|MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.inf

  MemoryTypeInformationChangeLib|MsCorePkg/Library/MemoryTypeInformationChangeLib/MemoryTypeInformationChangeLib.inf
  DxeMemoryProtectionHobLib|MdeModulePkg/Library/MemoryProtectionHobLib/DxeMemoryProtectionHobLib.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf
  MemoryTypeInfoSecVarCheckLib|AndromedaPkg/Library/MemoryTypeInfoSecVarCheckLib/MemoryTypeInfoSecVarCheckLib.inf
  SecurityLockAuditLib|MdeModulePkg/Library/SecurityLockAuditDebugMessageLib/SecurityLockAuditDebugMessageLib.inf
  ResetUtilityLib|MdeModulePkg/Library/ResetUtilityLib/ResetUtilityLib.inf
  HwResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf
  FltUsedLib|MdePkg/Library/FltUsedLib/FltUsedLib.inf
  ResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf

  # UI and graphics
  FrameBufferMemDrawLib    |MsGraphicsPkg/Library/FrameBufferMemDrawLib/FrameBufferMemDrawLibDxe.inf

  # DFCI / XML / JSON Libraries
  DfciUiSupportLib                  |OemPkg/Library/DfciUiSupportLib/DfciUiSupportLib.inf # Supports DFCI Groups.
  DfciV1SupportLib                  |DfciPkg/Library/DfciV1SupportLibNull/DfciV1SupportLibNull.inf # Backwards compatibility with DFCI V1 functions.
  DfciDeviceIdSupportLib            |OemPkg/Library/DfciDeviceIdSupportLib/DfciDeviceIdSupportLib.inf
  DfciGroupLib                      |OemPkg/Library/DfciGroupLib/DfciGroups.inf
  DfciRecoveryLib                   |DfciPkg/Library/DfciRecoveryLib/DfciRecoveryLib.inf
   # Zero Touch is part of DFCI
  ZeroTouchSettingsLib              |ZeroTouchPkg/Library/ZeroTouchSettings/ZeroTouchSettings.inf
   # Libraries that understands the MsXml Settings Schema and providers helper functions
  DfciXmlIdentitySchemaSupportLib   |DfciPkg/Library/DfciXmlIdentitySchemaSupportLib/DfciXmlIdentitySchemaSupportLib.inf
  DfciXmlDeviceIdSchemaSupportLib   |DfciPkg/Library/DfciXmlDeviceIdSchemaSupportLib/DfciXmlDeviceIdSchemaSupportLib.inf
  DfciXmlSettingSchemaSupportLib    |DfciPkg/Library/DfciXmlSettingSchemaSupportLib/DfciXmlSettingSchemaSupportLib.inf
  DfciXmlPermissionSchemaSupportLib |DfciPkg/Library/DfciXmlPermissionSchemaSupportLib/DfciXmlPermissionSchemaSupportLib.inf
  DfciSettingChangedNotificationLib |DfciPkg/Library/DfciSettingChangedNotificationLib/DfciSettingChangedNotificationLibNull.inf

   # Json parser
  JsonLiteParserLib |MsCorePkg/Library/JsonLiteParser/JsonLiteParser.inf

  # TPM Libraries
  TpmMeasurementLib       |MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf

  AdvancedLoggerAccessLib |AdvLoggerPkg/Library/AdvancedLoggerAccessLib/AdvancedLoggerAccessLib.inf

  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf

  CompilerIntrinsicsLib|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

  CapsulePersistLib|MdeModulePkg/Library/CapsulePersistLibNull/CapsulePersistLibNull.inf      ## MU_CHANGE
  OemHookStatusCodeLib|MdeModulePkg/Library/OemHookStatusCodeLibNull/OemHookStatusCodeLibNull.inf
  PcdDatabaseLoaderLib|MdeModulePkg/Library/PcdDatabaseLoaderLib/Dxe/PcdDatabaseLoaderLibDxe.inf

  DfciSettingsLib|DfciPkg/Library/DfciSettingsLib/DfciSettingsLib.inf

  DpcLib|NetworkPkg/Library/DxeDpcLib/DxeDpcLib.inf
  HttpLib|NetworkPkg/Library/DxeHttpLib/DxeHttpLib.inf
  IpIoLib|NetworkPkg/Library/DxeIpIoLib/DxeIpIoLib.inf
  NetLib|NetworkPkg/Library/DxeNetLib/DxeNetLib.inf
  UdpIoLib|NetworkPkg/Library/DxeUdpIoLib/DxeUdpIoLib.inf

  DtPlatformDtbLoaderLib|EmbeddedPkg/Library/DxeDtPlatformDtbLoaderLibDefault/DxeDtPlatformDtbLoaderLibDefault.inf
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf

  FmpAuthenticationLib|SecurityPkg/Library/FmpAuthenticationLibPkcs7/FmpAuthenticationLibPkcs7.inf

  MemoryMapHelperLib|AndromedaPkg/Library/MemoryMapHelperSecLib/MemoryMapHelperSecLib.inf
  ConfigurationMapHelperLib|AndromedaPkg/Library/ConfigurationMapHelperSecLib/ConfigurationMapHelperSecLib.inf
  ShLib|AndromedaPkg/Library/ShLib/ShLib.inf
  UefiCfgLib|AndromedaPkg/Library/UefiCfgLib/UefiCfgLib.inf

  MsSecureBootModeSettingLib|OemPkg/Library/MsSecureBootModeSettingLib/MsSecureBootModeSettingLib.inf
  UpdateFacsHardwareSignatureLib|OemPkg/Library/UpdateFacsHardwareSignatureLib/UpdateFacsHardwareSignatureLib.inf

  # Services tables/Entry points
  RegisterFilterLib           |MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf

  DeviceSpecificBusInfoLib|MsCorePkg/Library/DeviceSpecificBusInfoLibNull/DeviceSpecificBusInfoLibNull.inf
  ExceptionPersistenceLib|MdeModulePkg/Library/BaseExceptionPersistenceLibNull/BaseExceptionPersistenceLibNull.inf

  HashLib|SecurityPkg/Library/HashLibBaseCryptoRouter/HashLibBaseCryptoRouterDxe.inf
  OemTpm2InitLib|SecurityPkg/Library/OemTpm2InitLibNull/OemTpm2InitLib.inf
  Tcg2PhysicalPresenceLib|SecurityPkg/Library/DxeTcg2PhysicalPresenceLib/DxeTcg2PhysicalPresenceLib.inf
  Tcg2PhysicalPresencePromptLib|SecurityPkg/Library/Tcg2PhysicalPresencePromptLib/Tcg2PhysicalPresencePromptLibConsole.inf
  Tcg2PpVendorLib|SecurityPkg/Library/Tcg2PpVendorLibNull/Tcg2PpVendorLibNull.inf
  Tpm2CommandLib|SecurityPkg/Library/Tpm2CommandLib/Tpm2CommandLib.inf
  Tpm2DeviceLib|SecurityPkg/Library/Tpm2DeviceLibTcg2/Tpm2DeviceLibTcg2.inf
  TpmPlatformHierarchyLib|SecurityPkg/Library/PeiDxeTpmPlatformHierarchyLib/PeiDxeTpmPlatformHierarchyLib.inf

  UnitTestBootLib|UnitTestFrameworkPkg/Library/UnitTestBootLibNull/UnitTestBootLibNull.inf
  UnitTestLib|UnitTestFrameworkPkg/Library/UnitTestLib/UnitTestLib.inf
  UnitTestPersistenceLib|UnitTestFrameworkPkg/Library/UnitTestPersistenceLibNull/UnitTestPersistenceLibNull.inf
  UnitTestResultReportLib|UnitTestFrameworkPkg/Library/UnitTestResultReportLib/UnitTestResultReportLibDebugLib.inf

  UfsInfoLib|AndromedaPkg/Library/UfsInfoLib/UfsInfoLib.inf

  PolicyLib|PolicyServicePkg/Library/DxePolicyLib/DxePolicyLib.inf

  PanicLib|MdePkg/Library/BasePanicLibNull/BasePanicLibNull.inf  # MU_CHANGE
  
  ConsoleMsgLib|PcBdsPkg/Library/ConsoleMsgLibNull/ConsoleMsgLibNull.inf

[LibraryClasses.common.PEIM, LibraryClasses.common.PEI_CORE, LibraryClasses.common.SEC]
  ArmMmuLib|ArmPkg/Library/ArmMmuLib/ArmMmuPeiLib.inf

[LibraryClasses.common.SEC]
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
  ArmMmuLib|ArmPkg/Library/ArmMmuLib/ArmMmuPeiLib.inf
  MemoryMapHelperLib|AndromedaPkg/Library/MemoryMapHelperSecLib/MemoryMapHelperSecLib.inf
  ConfigurationMapHelperLib|AndromedaPkg/Library/ConfigurationMapHelperSecLib/ConfigurationMapHelperSecLib.inf
  SmemSecLib|AndromedaPkg/Library/SmemSecLib/SmemSecLib.inf
## Patch: A finder lib to get protocol address
  SecProtocolFinderLib|AndromedaPkg/Library/SecProtocolFinderLib/SecProtocolFinderLib.inf
##

[LibraryClasses.common.DXE_CORE]
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  HobLib|MdePkg/Library/DxeCoreHobLib/DxeCoreHobLib.inf
  MemoryAllocationLib|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf
  PerformanceLib|MdeModulePkg/Library/DxeCorePerformanceLib/DxeCorePerformanceLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf

[LibraryClasses.common.DXE_DRIVER]
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf

[LibraryClasses.common.UEFI_APPLICATION]
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiTianoCustomDecompressLib.inf

[LibraryClasses.common.UEFI_DRIVER]
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiTianoCustomDecompressLib.inf
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  ArmHvcLib|ArmPkg/Library/ArmHvcLib/ArmHvcLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLibRuntimeDxe.inf

[Components.common]
  AdvLoggerPkg/AdvancedFileLogger/AdvancedFileLogger.inf

  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf

  # Processor Core Services
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf {
    <LibraryClasses>
    NULL|MsCorePkg/Library/MemoryProtectionExceptionHandlerLib/MemoryProtectionExceptionHandlerLib.inf
!ifdef SHENNONG_USB_FAULT_TRACE
!if $(SHENNONG_USB_FAULT_TRACE) == 1
    # The memory-protection library owns DefaultExceptionHandler. Call a unique
    # bounded dump from that implementation rather than competing for its name.
    NULL|AndromedaPkg/Library/StockExceptionHandlerLib/StockExceptionHandlerLib.inf
!endif
!endif
  }

  ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  # DT
  EmbeddedPkg/Drivers/DtPlatformDxe/DtPlatformDxe.inf
  EmbeddedPkg/Drivers/VirtualKeyboardDxe/VirtualKeyboardDxe.inf

  EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf {
    <LibraryClasses>
	  RealTimeClockLib|AndromedaPkg/Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.inf
  }

  EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf

  FatPkg/EnhancedFatDxe/Fat.inf

  # HID Support
  HidPkg/HidKeyboardDxe/HidKeyboardDxe.inf
  HidPkg/HidMouseAbsolutePointerDxe/HidMouseAbsolutePointerDxe.inf

  MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressDxe.inf

  # Pci
  MdeModulePkg/Bus/Pci/PciBusDxe/PciBusDxe.inf

  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf

  # USB
  MdeModulePkg/Bus/Usb/UsbMouseAbsolutePointerDxe/UsbMouseAbsolutePointerDxe.inf

  # DXE
  MdeModulePkg/Core/Dxe/DxeMain.inf {
    <LibraryClasses>
    NULL|MdeModulePkg/Library/DxeCrc32GuidedSectionExtractLib/DxeCrc32GuidedSectionExtractLib.inf
    NULL|MdeModulePkg/Library/DxeCrc32GuidedSectionExtractLib/DxeCrc32GuidedSectionExtractLib.inf
    NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }

  # Runtime Services
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf

  MdeModulePkg/Universal/Acpi/AcpiPlatformDxe/AcpiPlatformDxe.inf
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  MdeModulePkg/Universal/Acpi/BootGraphicsResourceTableDxe/BootGraphicsResourceTableDxe.inf
  MdeModulePkg/Universal/Acpi/FirmwarePerformanceDataTableDxe/FirmwarePerformanceDxe.inf

  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf {
    <LibraryClasses>
    NULL|AndromedaPkg/Library/KernelErrataPatcherLib/KernelErrataPatcherLib.inf
  }

  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf

  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf

  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf

  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf

  # Disk IO
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf

  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  MdeModulePkg/Universal/DriverHealthManagerDxe/DriverHealthManagerDxe.inf
  MdeModulePkg/Universal/EsrtFmpDxe/EsrtFmpDxe.inf
  MdeModulePkg/Universal/FvSimpleFileSystemDxe/FvSimpleFileSystemDxe.inf
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf

  MdeModulePkg/Universal/MemoryTest/NullMemoryTestDxe/NullMemoryTestDxe.inf

  # PCD Database
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf {
    <LibraryClasses>
    PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  }

  # BDS
  MdeModulePkg/Universal/PrintDxe/PrintDxe.inf

  MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf

  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf

  # Security Stub
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf {
    <LibraryClasses>
      NULL|SecurityPkg/Library/DxeImageVerificationLib/DxeImageVerificationLib.inf
  }

  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
  MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf

  MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf

  # Fake Variable Services
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      BaseCryptLib|OpensslPkg/Library/BaseCryptLib/RuntimeCryptLib.inf
      TlsLib|OpensslPkg/Library/TlsLib/TlsLib.inf
      IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
## Patch: Fix memset redefine issue while linking, only implement while screen serial is enabled.
!if $(USE_SCREEN_FOR_SERIAL_OUTPUT) == 1
      CompilerIntrinsicsLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
!endif
## End patch
      OpensslLib|OpensslPkg/Library/OpensslLib/OpensslLib.inf # Contains openSSL library used by BaseCryptoLib
      #
      # DxeRngLib's constructor probes the RNG protocol with
      #   GetInfo (RngProtocol, &Size, NULL)
      # and expects EFI_BUFFER_TOO_SMALL. Qualcomm's RngDxe answers
      # EFI_INVALID_PARAMETER, which the library treats as fatal, so the
      # constructor returns an error and this driver loses its RNG entirely:
      #
      #   ASSERT [VariableRuntimeDxe] AutoGen.c(549)  (DxeRngLibConstructor)
      #
      # A timer-seeded RNG is fine here - this port does not do authenticated
      # variables - and it removes the dependency on the non-conforming protocol.
      # Scoped to this module rather than changed globally.
      #
      RngLib|MdePkg/Library/BaseRngLibTimerLib/BaseRngLibTimerLib.inf
  }

  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf

  MsCorePkg/CheckHardwareConnected/CheckHardwareConnected.inf
  MsCorePkg/MuVarPolicyFoundationDxe/MuVarPolicyFoundationDxe.inf {
    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugClearMemoryValue|0x00
      gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x00
      gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x00000000
      gEfiMdePkgTokenSpaceGuid.PcdPerformanceLibraryPropertyMask|0
      gEfiMdePkgTokenSpaceGuid.PcdPostCodePropertyMask|0
      gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|0x00
  }

  NetworkPkg/ArpDxe/ArpDxe.inf
  NetworkPkg/Dhcp4Dxe/Dhcp4Dxe.inf
  NetworkPkg/Dhcp6Dxe/Dhcp6Dxe.inf
  NetworkPkg/DnsDxe/DnsDxe.inf
  NetworkPkg/DpcDxe/DpcDxe.inf
  NetworkPkg/HttpDxe/HttpDxe.inf
  NetworkPkg/HttpUtilitiesDxe/HttpUtilitiesDxe.inf
  NetworkPkg/Ip4Dxe/Ip4Dxe.inf
  NetworkPkg/Ip6Dxe/Ip6Dxe.inf
  NetworkPkg/MnpDxe/MnpDxe.inf
  NetworkPkg/Mtftp4Dxe/Mtftp4Dxe.inf
  NetworkPkg/Mtftp6Dxe/Mtftp6Dxe.inf
  NetworkPkg/SnpDxe/SnpDxe.inf
  NetworkPkg/TcpDxe/TcpDxe.inf
  NetworkPkg/TlsDxe/TlsDxe.inf
  NetworkPkg/Udp4Dxe/Udp4Dxe.inf
  NetworkPkg/Udp6Dxe/Udp6Dxe.inf
  NetworkPkg/UefiPxeBcDxe/UefiPxeBcDxe.inf
  NetworkPkg/VlanConfigDxe/VlanConfigDxe.inf

  SecurityPkg/Tcg/Tcg2Dxe/Tcg2Dxe.inf {
    <LibraryClasses>
      Tpm2DeviceLib|SecurityPkg/Library/Tpm2DeviceLibRouter/Tpm2DeviceLibRouterDxe.inf
      HashLib|SecurityPkg/Library/HashLibBaseCryptoRouter/HashLibBaseCryptoRouterDxe.inf
      NULL|SecurityPkg/Library/HashInstanceLibSha1/HashInstanceLibSha1.inf
      NULL|SecurityPkg/Library/HashInstanceLibSha256/HashInstanceLibSha256.inf
      NULL|SecurityPkg/Library/HashInstanceLibSha384/HashInstanceLibSha384.inf
      NULL|SecurityPkg/Library/HashInstanceLibSha512/HashInstanceLibSha512.inf
      NULL|SecurityPkg/Library/HashInstanceLibSm3/HashInstanceLibSm3.inf
      OemTpm2InitLib|SecurityPkg/Library/OemTpm2InitLibNull/OemTpm2InitLib.inf
      NULL|SecurityPkg/Library/OemTpm2InitLibNull/OemTpm2InitLibVendorNull.inf
  }

  SecurityPkg/VariableAuthenticated/SecureBootConfigDxe/SecureBootConfigDxe.inf
  AndromedaPkg/Driver/SecureBootProvisioningDxe/SecureBootProvisioningDxe.inf

  # ACPI and SMBIOS
  # We use pre-built ACPI tables, so only core components are loaded
  AndromedaPkg/Driver/SmBiosTableDxe/SmBiosTableDxe.inf

  # PrePI
  AndromedaPkg/PrePi/PeiUniCore.inf {
		<LibraryClasses>
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }

  AndromedaPkg/UFP/ufpdevicefw.inf

	QcomModulePkg/Application/LinuxLoader/LinuxLoader.inf {
		<LibraryClasses>
			DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
			UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
			UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
			CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
			Zlib|QcomModulePkg/Library/zlib/zlib.inf
			ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
			BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
			DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
			DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
			HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
			PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
			DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf

			FdtLib|EmbeddedPkg/Library/FdtLib/FdtLib.inf
			LibUfdt|QcomModulePkg/Library/LibUfdt/LibUfdt.inf
			ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
			BootLib|QcomModulePkg/Library/BootLib/BootLib.inf
			StackCanary|QcomModulePkg/Library/StackCanary/StackCanary.inf
			FastbootLib|QcomModulePkg/Library/FastbootLib/FastbootLib.inf
			AvbLib|QcomModulePkg/Library/avb/AvbLib.inf
			UbsanLib|QcomModulePkg/Library/UbsanLib/UbsanLib.inf
	}

###################################################################################################
#
# BuildOptions Section - Define the module specific tool chain flags that should be used as
#                        the default flags for a module. These flags are appended to any
#                        standard flags that are defined by the build process. They can be
#                        applied for any modules or only those modules with the specific
#                        module style (EDK or EDKII) specified in [Components] section.
#
###################################################################################################
[BuildOptions]
  *_CLANGPDB_AARCH64_CC_FLAGS = -Wno-unused-const-variable -Wno-sometimes-uninitialized -Wno-unaligned-access -D CLANG -Wno-ignored-pragma-optimize
  *_*_*_CC_FLAGS = -DSECURE_BOOT=$(SECURE_BOOT) -DUSE_MEMORY_FOR_SERIAL_OUTPUT=$(USE_MEMORY_FOR_SERIAL_OUTPUT) -DUSE_SCREEN_FOR_SERIAL_OUTPUT=$(USE_SCREEN_FOR_SERIAL_OUTPUT) -DPLATFORM_HAS_ACTLR_EL1_UNIMPLEMENTED_ERRATA=$(PLATFORM_HAS_ACTLR_EL1_UNIMPLEMENTED_ERRATA) -DPLATFORM_HAS_AMCNTENSET0_EL0_UNIMPLEMENTED_ERRATA=$(PLATFORM_HAS_AMCNTENSET0_EL0_UNIMPLEMENTED_ERRATA) -DPLATFORM_HAS_GIC_V3_WITHOUT_IRM_FLAG_SUPPORT_ERRATA=$(PLATFORM_HAS_GIC_V3_WITHOUT_IRM_FLAG_SUPPORT_ERRATA) -DPLATFORM_HAS_PSCI_MEMPROTECT_FAILING_ERRATA=$(PLATFORM_HAS_PSCI_MEMPROTECT_FAILING_ERRATA) -D __DEPRECATED_AARCH64_4K_RUNTIME_GRANULARITY
  *_*_*_CC_FLAGS = -DZ_SOLO -DPRODUCT_NAME=\"Andromeda\" -DINIT_BIN=\"/init\" -DENABLE_UPDATE_PARTITIONS_CMDS -DENABLE_BOOT_CMD -DENABLE_DEVICE_CRITICAL_LOCK_UNLOCK_CMDS -DUSERDATA_FS_TYPE=\"ext4\"

[BuildOptions.common.EDKII.SEC,BuildOptions.common.EDKII.MM_CORE_STANDALONE]
  GCC:*_GCC*_*_DLINK_FLAGS = -z common-page-size=0x1000
  GCC:*_CLANGPDB_*_DLINK_FLAGS = /ALIGN:0x1000 /FILEALIGN:0x1000

[BuildOptions.common.EDKII.DXE_CORE,BuildOptions.common.EDKII.DXE_DRIVER,BuildOptions.common.EDKII.UEFI_DRIVER,BuildOptions.common.EDKII.UEFI_APPLICATION,BuildOptions.common.EDKII.MM_CORE_STANDALONE,BuildOptions.common.EDKII.MM_STANDALONE]
  GCC:*_GCC*_*_DLINK_FLAGS = -z common-page-size=0x1000
  GCC:*_CLANGPDB_*_DLINK_FLAGS = /ALIGN:0x1000

[BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER]
  GCC:*_GCC*_ARM_DLINK_FLAGS = -z common-page-size=0x1000
  GCC:*_GCC*_AARCH64_DLINK_FLAGS = -z common-page-size=0x10000
  GCC:*_CLANGPDB_AARCH64_DLINK_FLAGS = /ALIGN:0x10000
  RVCT:*_*_ARM_DLINK_FLAGS = --scatter $(EDK_TOOLS_PATH)/Scripts/Rvct-Align4K.sct

[BuildOptions.AARCH64.EDKII.MM_CORE_STANDALONE,BuildOptions.AARCH64.EDKII.MM_STANDALONE]
  GCC:*_*_*_CC_FLAGS = -mstrict-align -march=armv8-a
