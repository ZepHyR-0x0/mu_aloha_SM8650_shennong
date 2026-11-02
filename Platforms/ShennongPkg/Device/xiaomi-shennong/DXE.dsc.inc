[Components.common]
AndromedaPkg/Driver/ReadOnlyStorageDxe/ReadOnlyStorageDxe.inf
AndromedaPkg/Driver/ProjectInfoCacheDxe/ProjectInfoCacheDxe.inf
AndromedaPkg/Driver/UsbStockReplayDxe/UsbStockReplayDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
AndromedaPkg/Driver/UsbDevStartDxe/UsbDevStartDxe.inf
AndromedaPkg/Driver/UsbfnDwc3Ref/UsbfnDwc3Ref.inf
AndromedaPkg/Driver/GcdProbeDxe/GcdProbeDxe.inf
AndromedaPkg/Driver/EarlyBdsArchDxe/EarlyBdsArchDxe.inf
#AndromedaPkg/Driver/TouchProbeDxe/TouchProbeDxe.inf   # disabled: reading TLMM resets the device
# ScmDxeCompat: enabled. The entry point needs uefiplat.cfg's SmcInvokeConfig,
# which is now present (0x20040100). Without it the SMC argument buffer was
# zero-length, AllocatePool(0) returned NULL and the driver's own ASSERT dead-
# looped - the "hang" that got it removed twice before.
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/Smp2pDxe/Smp2pDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ConfigWordsDxe/ConfigWordsDxe.inf
AndromedaPkg/Driver/RamPartitionDxe/RamPartitionDxe.inf
#AndromedaPkg/Driver/ShmBridgeStubDxe/ShmBridgeStubDxe.inf
CryptoPkg/Binaries/edk2-basecrypto-driver-bin_extdep/Driver/Bin/CryptoDriverBin_ALL_Dxe_RELEASE_AARCH64.inf
AndromedaPkg/Driver/UfsPreDxe/UfsPreDxe.inf
AndromedaPkg/Driver/ScmProbeDxe/ScmProbeDxe.inf
AndromedaPkg/Driver/AdspProbeDxe/AdspProbeDxe.inf
AndromedaPkg/Driver/PcdShimDxe/PcdShimDxe.inf

# Synaptics S3910P touch: GENI SPI at 0xA90000 plus the TCM message layer,
# publishing Absolute Pointer. Written here because the image has no SPI driver
# at all - SPIDxe publishes a stub.
AndromedaPkg/Driver/SynapticsTouchDxe/SynapticsTouchDxe.inf
AndromedaPkg/Driver/UfsWakeDxe/UfsWakeDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/EnvDxeEnhanced/EnvDxeEnhanced.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ParserDxe/ParserDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ProjectInfoDxe/ProjectInfoDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/MiPkg/Protocol/MiToken/MiToken.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/IsolatedBinaries/Shell/Shell.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SerialPortDxe/SerialPortDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VerifiedBootDxe/VerifiedBootDxe.inf
# ShmBridgeDxeLA: the real one. It issues three SMCs to register a shared-memory
# pool with TrustZone, which is what smcinvoke needs - the stub's buffers are plain
# pool allocations and TZ rejects them with -6 (0xFFFFFFFA). It failed to install on
# earlier attempts because TZ was not answering; SCM now gets real answers from TZ.
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeLA_Shennong.inf
# TzDxeLA: enabled. It needs Keymaster from a UFS partition, and UFSDxe now
# publishes all six LUNs - keymaster_a / keymaster_b live on LUN 4, and bio4
# exists, so the partition is readable from UEFI. It also needs ScmDxeCompat,
# which is enabled above.
#
# ScmDxeLA_Shennong.inf stays commented: that variant is not among the binaries
# extracted from this device, so there is nothing to point at.
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/TzDxeLA.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PsStateDxe/PsStateDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VariableDxe/VariableDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ResetRuntimeDxe/ResetRuntimeDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/FontDxe/FontDxe.inf
    $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QcomWDogDxe/QcomWDogDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/IPCCDxe/IPCCDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/GLinkDxe/GLinkDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/IsolatedBinaries/UFSDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ICBDxe/ICBDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QcomBds/QcomBds.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Application/QcomChargerApp/QcomChargerApp.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/RscDxe/RscDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SimpleTextInOutSerialDxe/SimpleTextInOutSerial.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/GpiDxe/GpiDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SdccDxe/SdccDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/FeatureEnablerDxe/FeatureEnablerDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QRKSDxe/QRKSDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ChargerExDxe/ChargerExDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PmicGlinkDxe/PmicGlinkDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/LimitsDxe/LimitsDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SSRDxe/SSRDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/XhciDxe/XhciDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbBusDxe/UsbBusDxe.inf
    $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbKbDxe/UsbKbDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbMassStorageDxe/UsbMassStorageDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbInitDxe/UsbInitDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UCDxe/UCDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/RNGDxe/RngDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PILDxe/PILDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SecRSADxe/SecRSADxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ASN1X509Dxe/ASN1X509Dxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SPSSDxe/SPSSDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/FvUtilsDxe/FvUtilsDxe.inf

# Patched Binaries
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/PatchedBinaries/ButtonsDxe.inf
$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/IsolatedBinaries/UFSDxe.inf
QcomPkg/PatchedBinaries/SPMIDxe.inf
AndromedaPkg/Driver/VerboseKeyDxe/VerboseKeyDxe.inf
# Historical GPIO-patched variant is excluded; stock replay uses Binaries above.
QcomPkg/PatchedBinaries/ClockDxe.inf
QcomPkg/PatchedBinaries/PmicDxeLa.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/PatchedBinaries/QcomChargerDxeLA.inf
#QcomPkg/PatchedBinaries/FeatureEnablerDxe.inf
#$(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf  # DEBUG: panel re-init (Xiaomi ABL may tear down display)  # disabled: panel alive via ABL splash; suspected ASSERT source
#AndromedaPkg/Driver/DisplayCallerDxe/DisplayCallerDxe.inf

# PORT: UEFI Shell - BDS boots it as the boot manager menu (see
# PcdBootManagerMenuFile in Andromeda.dsc.inc).
ShellPkg/Application/Shell/Shell.inf

# Console text straight to the framebuffer via SerialPortLib; replaces the
# Qualcomm SimpleTextInOutSerial prebuilt, whose entry point fails here.
AndromedaPkg/Driver/SerialTextConsoleDxe/SerialTextConsoleDxe.inf
