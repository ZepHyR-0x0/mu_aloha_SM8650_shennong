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
    #
    # The crypto provider, and this line is the one that was missing.
    #
    # The chain above was written out correctly, but only two of its three links
    # were actually placed here - RngDxe and SecurityStubDxe. CryptoDriverBin_ALL,
    # which carries BaseCryptoDriverDxe and is the only publisher of
    # gEdkiiCryptoProtocolGuid, was left to the ordinary FV walk. So SecurityStubDxe
    # dispatched in this early pass with no crypto provider behind it, and its
    # constructor said so every boot:
    #
    #   [DxeCryptLib] Failed to locate Crypto Protocol. Status = Not Found
    #   ASSERT [SecurityStubDxe] DxeCryptLib.c(63): mCryptoProtocol != ((void *) 0)
    #
    # gEfiSecurityArchProtocolGuid is then incomplete, which is what UsbfnDwc3Dxe
    # DEPEXes on - so UsbFunctionIo was never published, UsbDeviceDxe opened it once
    # at init and got Not Found, and the USB device controller never started.
    #
    # It sits between RngDxe and SecurityStubDxe because that is the order the chain
    # needs: the provider must come after the module that satisfies its DEPEX and
    # before its consumer.
    #
    INF CryptoPkg/Binaries/edk2-basecrypto-driver-bin_extdep/Driver/Bin/CryptoDriverBin_ALL_Dxe_RELEASE_AARCH64.inf
    INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf

    # Next, install cpu protocol and enable the interrupt controller
    INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
    INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
    #
    # DtPlatformDxe, ahead of everything that reads GPIO or I2C configuration.
    #
    # It was in the FV (Shennong.fdf:117) but not in this list, so it dispatched in
    # ordinary FV order - which put it after TLMMDxe at entry 166 and after
    # UsbConfigDxe at 75. TLMMDxe builds its pin configuration from the device tree,
    # and with no DT published yet it comes up with nothing to configure against.
    # That is the most likely reason UsbConfigDxe's GPIO enable fails with 0x7 while
    # the GPIO protocol itself is present:
    #
    #   Failed to enable GPIO status : 0x7
    #   usb_shared_wcd_usb_switch_reset, core : 0x0
    #   usb_shared_i2c_write, not open 0x%x : 0x0
    #
    # The FV has no DT blob of its own - "[DIAG] fdt FDRes: no FDT magic" - so the
    # blob has to come from the XBL DT in the HOB, which is what DtPlatformDxe
    # publishes. Nothing else in this list can supply it.
    #
    INF EmbeddedPkg/Drivers/DtPlatformDxe/DtPlatformDxe.inf

    # Text console before any driver that might print through ConOut. Without
    # it ConOut stays NULL until ConSplitterDxe and any earlier Print() takes a
    # NULL+8 data abort (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL has Reset at 0 and
    # OutputString at 8). It talks to the framebuffer through SerialPortLib and
    # needs no GOP, no HII font and no Qualcomm plumbing.
    INF AndromedaPkg/Driver/SerialTextConsoleDxe/SerialTextConsoleDxe.inf

    # Core Drivers
    INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
    INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

    #
    # The PCD protocol, first of all. Everything that links DxePcdLib has a
    # DEPEX on gEfiPcdProtocolGuid merged in, so this must dispatch before any
    # of them can start. It depends on nothing but DxeCore.
    #
    INF AndromedaPkg/Driver/PcdShimDxe/PcdShimDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/EnvDxeEnhanced/EnvDxeEnhanced.inf
    # STOCK ORDER (decoded from the uefi_a APRIORI table, GUID fc510ee7):
    #   DxeCore -> EnvDxe -> RscRt -> SCHandler -> Runtime -> Cpu -> Gic ->
    #   Metronome -> Timer -> Smem -> DALSys -> HWIO -> ChipInfo -> ...
    # EnvDxe is the FIRST QC driver and needs nothing but DxeCore.
    #
    # The real ShmBridge, ahead of RamPartitionDxe's stub.
    #
    # The dependency runs the other way round from what it looks like.
    # ScmDxeCompat's LocateProtocol at .data 0xB0A8 uses GUID 9C1EB71F, which is
    # ShmBridge - not SCM. ScmArmV8Init asserts on that lookup, so it dead-loops
    # without it, and SmcInvokeInit's IClientEnv_open then goes through whatever
    # ShmBridge implementation it was handed.
    #
    # RamPartitionDxe publishes a stub at entry 59. The stub satisfies the lookup,
    # which is why the boot proceeds at all, but it does not register any memory
    # with TrustZone - so every smcinvoke call that has to move a structure across
    # comes back refused. That is the current state.
    #
    # Placing the real driver here gives it first refusal. If its three SMCs work,
    # ScmDxeCompat gets the real one and smcinvoke has a chance. If they fail, the
    # stub at 59 still publishes and nothing is worse than it is now.
    #
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
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

    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf
    #
    # ScmDxeCompat is enabled now that uefiplat.cfg carries SmcInvokeConfig.
    # Previously it was commented out because its entry point allocated a
    # zero-length SMC argument buffer - AllocatePool(0) returned NULL and its own
    # ASSERT_EFI_ERROR dead-looped, which looked like a hang. The three bitfields
    # in that key (bits[31:21]/[20:10]/[9:0]) are the buffer sizes, so a value with
    # any zero field puts it straight back there; 0x20040100 is three 0x100s.
    #
    # No DEPEX, so this position is its dispatch time. The stock image has it
    # after ICBDxe, which is where it sits.
    #
    # The FILE DRIVER form that used to be here carried GUID D8F91DDC-..., which
    # is not this module. ScmDxeCompat.inf declares
    # 4B4973EE-401B-4F36-A6A9-533DFCCDFC33, which matches the module GUID in
    # reverse/src/ScmDxeCompat.c, so the INF is used instead.
    #

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

#
# DiskIoDxe and PartitionDxe used to be added here as prebuilt FILE DRIVER
# blocks, and again further down as INF lines. Both copies were live, so the FV
# carried two files with each FILE_GUID and GenFv wrote that GUID into the APRIORI
# file twice.
#
# That is not cosmetic. Dispatcher.c:1483 walks the APRIORI file and schedules
# every GUID it finds without checking whether the driver is already on
# mScheduledQueue, so a duplicated GUID inserts the same ScheduledLink twice,
# rewrites the queue's links, and the next insert trips
# LinkedList.c(267) InternalBaseLibIsListValid (ListHead).
#
# The INF form further down is the one kept: it sits immediately before UFSDxe,
# which is the order the comment there documents as the stock one.
#
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
    #
    # The SCM / partition probe, and both of its constraints come from APRIORI.
    #
    # APRIORI-listed drivers go on mScheduledQueue before the dispatcher walks the
    # FV (Dispatcher.c:1483), so their order here is their order at runtime. A
    # module that is only in DXE.inc runs after all of them, however early its FV
    # offset looks - which is why this driver printed nothing on two boots when it
    # sat only in DXE.inc.
    #
    # It has to be after UFSDxe, because it reports BlockIo and PartitionInfo
    # counts and reads the Keymaster partition's first bytes; placed at entry 35 it
    # correctly reported "BlockIo: none" simply because UFS was not up yet.
    #
    # It has to be before TzDxeLA, which asserts at TzDxeLBox.c(246) and dead-loops.
    #
    INF AndromedaPkg/Driver/ScmProbeDxe/ScmProbeDxe.inf
    INF FatPkg/EnhancedFatDxe/Fat.inf
    # Connect only after the FAT driver's binding protocol has been installed.
    INF AndromedaPkg/Driver/AdspProbeDxe/AdspProbeDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ConfigWordsDxe/ConfigWordsDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/TzDxeLA.inf
    #
    # TzDxeLA is enabled, and the reason it was held back no longer holds.
    #
    # It was disabled because it loads Keymaster from a UFS partition and UFS
    # published nothing. UFSDxe now enumerates all six LUNs - 129 BlockIo
    # handles, 123 non-empty partitions, matching the stock GPT one for one -
    # and keymaster_a / keymaster_b are on LUN 4. bio4 is published, so the
    # partition is reachable from UEFI even though Android userspace does not
    # expose that LUN as a normal block device.
    #
    # The other prerequisite is ScmDxeCompat, enabled above. reverse/src/TzDxeLA.c
    # shows every QSEECOM method forwarding to gScmProtocol->ScmSendCommand, so
    # without SCM the protocol installs and then does nothing.
    #
    # Position is the stock one: after ConfigWordsDxe, which is after UFSDxe and
    # Fat. TzDxe reads a partition during init, so the block stack has to exist.
    #
    # ShmBridgeStubDxe is still a stub. If QSEECom reaches the point of sharing
    # memory and fails there, replacing the stub with the real ShmBridgeDxeLA is
    # the first thing to try.
    #

    INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VariableDxe/VariableDxe.inf
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QcomWDogDxe/QcomWDogDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
#
# FeatureEnablerDxe: the patched binary below is the one kept. The plain INF that
# used to sit here pointed at the unpatched copy, so both were packed and the
# GUID appeared twice in the APRIORI file - the same double-scheduling problem
# described above for DiskIoDxe.
#
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
    #
    # SPSSDxe moved up to just after UFSDxe, before TzDxeLA.
    #
    # It used to run at entry 55, behind TzDxeLA at 48. TzDxeLA is the driver that
    # loads the TZ apps, and it fails with LOAD_ERROR from TrustZone; SPSSDxe is the
    # one that loads the secure processor firmware. If the latter is a prerequisite
    # for the former, the old order guaranteed it never arrived in time.
    #
    # This is an ordering experiment, not a conclusion - if TzDxeLA behaves the same
    # way after the move, the ordering is not the cause.
    #
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

    #
    # UsbDeviceDxe and UsbDevStartDxe are deliberately NOT here.
    #
    # Both DEPEX on EFI_USBFN_IO_PROTOCOL, and UsbfnDwc3Dxe is the only publisher.
    # As APRIORI entries they were dispatched in this pass regardless - UsbDevice
    # existed, UsbFunctionIo did not, and the probe said so:/n    #
    #   [USBH] UsbDevice     [0/1] handle=0xD123FE18
    #   [USBH] UsbFunctionIo none
    #
    # Left in the ordinary FV list they are dispatched by the normal path, which
    # does honour the DEPEX, so they wait for the function driver instead of
    # racing it.
    #
#     INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf # b198: stock USB function driver removed
#     INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf # b198: direct USB2 controller owner

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/XhciDxe/XhciDxe.inf
# b198 direct USB2 test: disable competing owner XhciDxe
#     FILE DRIVER = B7F50E91-A759-412C-ADE4-DCD03E7F7C28 {
#             SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/XhciDxe/XhciDxe.efi
#             SECTION UI = "XhciDxe"
#     }

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbBusDxe/UsbBusDxe.inf
# b198 direct USB2 test: disable competing owner UsbBusDxe
#     FILE DRIVER = 240612B7-A063-11D4-9A3A-0090273FC14D {
#             SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbBusDxe/UsbBusDxe.efi
#             SECTION UI = "UsbBusDxe"
#     }

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbKbDxe/UsbKbDxe.inf
#    FILE DRIVER = 2D2E62CF-9ECF-43B7-8219-94E7FC713DFE {
#            SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbKbDxe/UsbKbDxe.efi
#            SECTION UI = "UsbKbDxe"

#   INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbMassStorageDxe/UsbMassStorageDxe.inf
# b198 direct USB2 test: disable competing owner UsbMassStorageDxe
#     FILE DRIVER = 9FB4B4A7-42C0-4BCD-8540-9BCC6711F83E {
#             SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbMassStorageDxe/UsbMassStorageDxe.efi
#             SECTION UI = "UsbMassStorageDxe"
#     }

#     INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/PatchedBinaries/UsbConfigDxe.inf # b198: direct USB2 controller owner
#    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf  # dup GUID with PatchedBinaries/ButtonsDxe.inf
    INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
    #INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf

    #
    # UsbMsdDxe has an empty DEPEX, so it dispatches purely on APRIORI order.
    # It calls UsbMsdStartDevice, which needs EFI_USB_DEVICE_PROTOCOL from
    # UsbDeviceDxe just above - so it must come AFTER it. Previously it was not
    # in the APRIORI list at all, which left the order to FV discovery.
    #
#     INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf # b198: direct USB2 controller owner
    #INF AndromedaPkg/Driver/UsbDevStartDxe/UsbDevStartDxe.inf
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
# b198 direct USB2 test: disable competing owner UCDxe
#     FILE DRIVER = 5F0937CD-73F9-445E-B320-BB6FD5D3CBDE {
#             SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UCDxe/UCDxe.efi
#             SECTION UI = "UCDxe"
#     }

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
