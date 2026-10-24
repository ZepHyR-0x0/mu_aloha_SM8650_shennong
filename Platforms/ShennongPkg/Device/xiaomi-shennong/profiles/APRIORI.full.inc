APRIORI DXE {
    # First, install the PCD driver and call DxeInit
    INF MdeModulePkg/Core/Dxe/DxeMain.inf
    INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

    INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
    INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
    INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf

    #
    # ResetSystemRuntimeDxe installs gEfiResetArchProtocolGuid. Qualcomm's own
    # ResetRuntimeDxe does not (its PE does not even contain the GUID), so the
    # arch protocol was never published and every driver that DEPEXes on it -
    # SimpleWindowManagerDxe, UsbfnDwc3Dxe, HALIOMMU - stayed in the deferred
    # queue forever. Dispatched early because it has no dependencies.
    #
    INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf

    #
    # The gEfiSecurityArchProtocolGuid chain has to be complete BEFORE any USB
    # driver runs, because UsbDeviceDxe opens EFI_USBFN_IO_PROTOCOL once at init
    # and never retries:
    #
    #   RngDxe -> gEfiRngProtocolGuid
    #   BaseCryptoDriverDxeAARCH64 -> gEdkiiCryptoProtocolGuid
    #   SecurityStubDxe -> gEfiSecurityArchProtocolGuid
    #   UsbfnDwc3Dxe (needs the arch set) -> EFI_USBFN_IO_PROTOCOL
    #   UsbDeviceDxe (needs only PMIC+SMEM) -> opens USBFN_IO
    #
    # RngDxe used to sit far below UsbDeviceDxe and SecurityStubDxe was not in
    # this list at all, so UsbDeviceDxe dispatched first, failed to find USBFN_IO
    # and stayed broken - the device log shows exactly that:
    #
    #   UsbDeviceOpenProtocols: Failed to locate USB FN IO protocol: Not Found
    #
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/RNGDxe/RngDxe.inf
    INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf

    # Next, install cpu protocol and enable the interrupt controller
    INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
    INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf

    # Text console before any driver that might print through ConOut. Without
    # it ConOut stays NULL until ConSplitterDxe and any earlier Print() takes a
    # NULL+8 data abort (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL has Reset at 0 and
    # OutputString at 8). It talks to the framebuffer through SerialPortLib and
    # needs no GOP, no HII font and no Qualcomm plumbing.
    INF AndromedaPkg/Driver/SerialTextConsoleDxe/SerialTextConsoleDxe.inf

    # Core Drivers
    INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
    INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/EnvDxeEnhanced/EnvDxeEnhanced.inf
    # STOCK ORDER (decoded from the uefi_a APRIORI table, GUID fc510ee7):
    #   DxeCore -> EnvDxe -> RscRt -> SCHandler -> Runtime -> Cpu -> Gic ->
    #   Metronome -> Timer -> Smem -> DALSys -> HWIO -> ChipInfo -> ...
    # EnvDxe is the FIRST QC driver and needs nothing but DxeCore.
    INF AndromedaPkg/Driver/RamPartitionDxe/RamPartitionDxe.inf
    # EnvDxe publishes the uefiplat.cfg config database that SmemDxe, DALSYSDxe,
    # HWIODxe and the rest of the QC stack query - keep it first (stock order).
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
    INF AndromedaPkg/Driver/EarlyBdsArchDxe/EarlyBdsArchDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/IPCCDxe/IPCCDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/GLinkDxe/GLinkDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/Smp2pDxe/Smp2pDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SSRDxe/SSRDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
    # ClockDxe DISABLED: it is the last driver printed before the hang.
# It is a PatchedBinary built for another SoC and appears to spin forever
# here (SM8650). ABL already leaves the GCC clocks running, so skipping it
# is acceptable while we chase USB.
INF QcomPkg/PatchedBinaries/ClockDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ICBDxe/ICBDxe.inf
    #
    # Stock uefi_a APRIORI order (decoded from the real image):
    #   0x0190 ClockDxe -> 0x01A0 ICBDxe -> 0x01B0 ShmBridgeDxeLA
    #   -> 0x01C0 ScmDxeCompat
    # ScmDxeCompat has no DEPEX, so this position IS its dispatch time, and it
    # needs ShmBridge up first. Placing it after PlatformInfoDxe was wrong.
    #
#    #INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
# ShmBridgeDxe DISABLED: it needs ScmDxeCompat, which hangs (see below).
# Nothing on the current USB path references ShmBridge.
#
#    FILE DRIVER = 41EBB6A6-2C19-4C02-841D-848CE004A282 {
#            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.efi
#            SECTION UI = "ShmBridgeDxe"
#    }

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf
# ScmDxeCompat DISABLED: its entry point queries the config key "SmcInvokeConfig",
# which this port does not define. The returned fields (bits[31:21]/[20:10]/[9:0])
# size the SMC argument buffer, so with no value it calls AllocatePool(0), gets NULL
# and returns EFI_OUT_OF_RESOURCES -> its own ASSERT_EFI_ERROR dead-loops ("hang").
# Re-enable together with a correct SmcInvokeConfig value when UFS/TZ is on the table.
#
#    FILE DRIVER = D8F91DDC-87C8-42A2-963E-8DD24A9F1FB5 {
#            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.efi
#            SECTION UI = "ScmDxeCompat"
#    }

    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf
    INF QcomPkg/PatchedBinaries/SPMIDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ResetRuntimeDxe/ResetRuntimeDxe.inf
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf
    # PmicDxeLa DISABLED: it prints 'Mi Haptics' and then takes a synchronous
# external abort inside SPMI.dll, because SPMIDxe already failed with
# 'Failed to initialize reset' (it cannot get its GCC reset/clock while
# ClockDxe is disabled). Removing the consumer lets the rest of DXE run
# so we can see how far the USB stack gets.
INF QcomPkg/PatchedBinaries/PmicDxeLa.inf

#    INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
    FILE DRIVER = 6b38f7b4-ad98-40e9-9093-aca2b5a253c4 {
            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.efi
            SECTION UI = "DiskIoDxe"
    }
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
    FILE DRIVER = 1fa1f39e-feff-4aae-bd7b-38a070a3b609 {
            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.efi
            SECTION UI = "PartitionDxe"
    }
    INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
    INF MdeModulePkg/Universal/FvSimpleFileSystemDxe/FvSimpleFileSystemDxe.inf
#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/PatchedBinaries/SdccDxe.inf
    #
    # Stock uefi_a order for this stretch:
    #   0x0210 DiskIoDxe -> 0x0220 PartitionDxe -> 0x0260 UFSDxe
    #   -> 0x0270 Fat -> ... -> 0x02C0 ConfigWordsDxe
    #
    # ConfigWordsDxe used to sit near the top of this list, long before UFSDxe,
    # which is why it reported:
    #   ConfigWords: Error Reading from Partition: Not Found
    # It reads a partition; the block stack has to exist first.
    #
    INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
    INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/IsolatedBinaries/UFSDxe.inf
    INF FatPkg/EnhancedFatDxe/Fat.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ConfigWordsDxe/ConfigWordsDxe.inf
    #INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/TzDxeLA.inf
# TzDxe DISABLED: it needs to load Keymaster from a UFS partition

# TzDxe DISABLED: it needs to load Keymaster from a UFS partition

# TzDxe DISABLED: it needs to load Keymaster from a UFS partition

# TzDxe DISABLED: it needs to load Keymaster from a UFS partition

    INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VariableDxe/VariableDxe.inf
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QcomWDogDxe/QcomWDogDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/FeatureEnablerDxe/FeatureEnablerDxe.inf

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/FeatureEnablerDxe.inf
    FILE DRIVER = 1C04F529-0612-4319-9315-B2E75C16810A {
            SECTION PE32 = QcomPkg/PatchedBinaries/FeatureEnablerDxe.efi
            SECTION UI = "FeatureEnablerDxe"
    }

#    INF AndromedaPkg/Driver/DisplayCallerDxe/DisplayCallerDxe.inf

    INF AndromedaPkg/Driver/SimpleFbDxe/SimpleFbDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/FvUtilsDxe/FvUtilsDxe.inf

    FILE DRIVER = 3CF7F36E-5A97-496A-BB4B-D4F9937FA311 {
            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QRKSDxe/QRKSDxe.efi 
            SECTION UI = "QRKSDxe"
    }

    #INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf
    #INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PILDxe/PILDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SPSSDxe/SPSSDxe.inf
    #INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
    #INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf
# WatchdogTimerDxe DISABLED: it arms a 5 minute timer and calls ResetSystem when
# it expires, which is indistinguishable from a hang during bring-up.
#    INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
#    INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
    INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
    INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
    INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
    INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
    INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/FontDxe/FontDxe.inf
    # Diagnostic probe last, so its output stays at the bottom of the screen.
    # Both drivers were only ever experiments and their sources are not part of
    # this commit, so referencing them makes the FDF fail with
    # "NOT found in DSC file". UsbSerialFnDxe additionally contended with
    # UsbMsdDxe for EFI_USBFN_IO_PROTOCOL, which is what turned "cable plugged
    # in" into an immediate reset (HANDOVER sections 16 and 18).
    # INF AndromedaPkg/Driver/UsbSerialFnDxe/UsbSerialFnDxe.inf
    # INF AndromedaPkg/Driver/UsbProbeDxe/UsbProbeDxe.inf

    INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PmicGlinkDxe/PmicGlinkDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/PatchedBinaries/QcomChargerDxeLA.inf

    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/XhciDxe/XhciDxe.inf
    FILE DRIVER = B7F50E91-A759-412C-ADE4-DCD03E7F7C28 {
            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/XhciDxe/XhciDxe.efi
            SECTION UI = "XhciDxe"
    }

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbBusDxe/UsbBusDxe.inf
    FILE DRIVER = 240612B7-A063-11D4-9A3A-0090273FC14D {
            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbBusDxe/UsbBusDxe.efi
            SECTION UI = "UsbBusDxe"
    }

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbKbDxe/UsbKbDxe.inf
#    FILE DRIVER = 2D2E62CF-9ECF-43B7-8219-94E7FC713DFE {
#            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbKbDxe/UsbKbDxe.efi
#            SECTION UI = "UsbKbDxe"

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbMassStorageDxe/UsbMassStorageDxe.inf
    FILE DRIVER = 9FB4B4A7-42C0-4BCD-8540-9BCC6711F83E {
            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbMassStorageDxe/UsbMassStorageDxe.efi
            SECTION UI = "UsbMassStorageDxe"
    }

    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/PatchedBinaries/UsbConfigDxe.inf
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf  # dup GUID with PatchedBinaries/ButtonsDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf

    #
    # UsbMsdDxe has an empty DEPEX, so it dispatches purely on APRIORI order.
    # It calls UsbMsdStartDevice, which needs EFI_USB_DEVICE_PROTOCOL from
    # UsbDeviceDxe just above - so it must come AFTER it. Previously it was not
    # in the APRIORI list at all, which left the order to FV discovery.
    #
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
    INF AndromedaPkg/Driver/UsbDevStartDxe/UsbDevStartDxe.inf
    INF AndromedaPkg/Driver/GcdProbeDxe/GcdProbeDxe.inf
#    INF AndromedaPkg/Driver/TouchProbeDxe/TouchProbeDxe.inf   # disabled: reading TLMM resets the device
#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/LimitsDxe/LimitsDxe.inf
    FILE DRIVER =  F09AD92E-4E91-490F-AB99-E69939B840B2 {
            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/LimitsDxe/LimitsDxe.efi
            SECTION UI = "LimitsDxe"
    }
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/GpiDxe/GpiDxe.inf

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UCDxe/UCDxe.inf
    FILE DRIVER = 5F0937CD-73F9-445E-B320-BB6FD5D3CBDE {
            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UCDxe/UCDxe.efi
            SECTION UI = "UCDxe"
    }

    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SimpleTextInOutSerialDxe/SimpleTextInOutSerial.inf
    INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
    INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
    INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ASN1X509Dxe/ASN1X509Dxe.inf
    FILE DRIVER = C2F9A4F5-F7B4-43E7-BA99-5EA804CC103A {
        SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ASN1X509Dxe/ASN1X509Dxe.efi
        SECTION UI = "ASN1X509Dxe"
    }


    FILE DRIVER =  32C71E68-83A8-46ED-AED1-094E71B12057 {
        SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SecRSADxe/SecRSADxe.efi
        SECTION UI = "SecRSADxe"
    }

#   FILE DRIVER =  FD975FB5-92C3-40B3-B05C-9C434326AB64 {
#        SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VerifiedBootDxe/VerifiedBootDxe.efi
#        SECTION UI = "VerifiedBootDxe"

    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/RscDxe/RscDxe.inf
}
