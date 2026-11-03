#FILE FREEFORM = DDE58710-41CD-4306-DBFB-3FA90BB1D2DD {
#    SECTION UI = "uefiplat.cfg"
#    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/uefiplat.cfg
#}

INF AndromedaPkg/Driver/SimpleFbDxe/SimpleFbDxe.inf
INF AndromedaPkg/Driver/RamPartitionDxe/RamPartitionDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/EnvDxeEnhanced/EnvDxeEnhanced.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ParserDxe/ParserDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SerialPortDxe/SerialPortDxe.inf
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VerifiedBootDxe/VerifiedBootDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PsStateDxe/PsStateDxe.inf
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VariableDxe/VariableDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ResetRuntimeDxe/ResetRuntimeDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/FontDxe/FontDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QcomWDogDxe/QcomWDogDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/IPCCDxe/IPCCDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/GLinkDxe/GLinkDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
#
# ProjectInfoDxe. Not referenced anywhere before this - not commented out, simply
# absent from both lists. It reports the project/board identity, which is the one field
# the log currently leaves blank:
#
#   Chip Name:
#
# Placed right after PlatformInfoDxe since it is the same kind of information.
#
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ProjectInfoDxe/ProjectInfoDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/MiPkg/Protocol/MiToken/MiToken.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/IsolatedBinaries/Shell/Shell.inf
#
# ScmDxeCompat has no DEPEX, so APRIORI order alone decides when it runs. It
# queries the config key "SmcInvokeConfig", which lives in the DTB
# (/soc/qcom_smcinvoke, compatible "qcom,smcinvoke"). Running it before
# PlatformInfoDxe made that lookup fail, AllocatePool(0) return NULL, and
# ScmArmV8.c dereference it - the crash seen at scmarmv8.c +143.
#
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
#
# ScmDxeCompat DISABLED AGAIN (was re-enabled to chase USB).
#
# With the shim answering SmcInvokeConfig the lookup now succeeds:
#   [SH] cfg-u32 SmcInvokeConfig -> Success
# and then the boot hangs inside SmcInvokeInit - because SCM talks to TrustZone
# over SMC, and TZ is not loaded:
#   SPSSDxe: MountFatPartition(spunvm) failed
#   pil-SPSS: Failed to load metadata
# and SPSS itself cannot load because UFS is dead:
#   ERROR: Failed to map UFS_MEM_UFS base address : 0x32B84239
#
# UFS -> SPSS/TZ -> SCM -> QMI -> glink -> NPA -> USB. The bottom link is UFS.
# Chasing the config value cannot fix a hang that waits on a TZ that never runs.
#
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ICBDxe/ICBDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QcomBds/QcomBds.inf
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Application/QcomChargerApp/QcomChargerApp.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/RscDxe/RscDxe.inf

# Patched Binaries
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/PatchedBinaries/ButtonsDxe.inf
INF QcomPkg/PatchedBinaries/SPMIDxe.inf
INF AndromedaPkg/Driver/VerboseKeyDxe/VerboseKeyDxe.inf
#
# Runs immediately before UFSDxe. Clears the controller's enable bit so the storage
# driver cannot mistake ABL's leftovers for its own initialisation, and logs the state
# either side of the write. FV order is the only ordering control here - the INF is
# DEPEX TRUE and APRIORI is inert on this platform.
#
#
# Publishes the ShmBridge protocol without going through SMC.
#
# RpmbListener.c +160 asserts on LocateProtocol(9C1EB71F-DD6C-4ED5-9F6A-5CC0CA789F16),
# which is the ShmBridge protocol, not SCM. The real ShmBridgeDxeLA issues three SMCs
# and never installs the protocol on this port, so the lookup fails and UFSDxe stops
# before it ever touches the controller. The stub installs the same interface (Version,
# Allocate at +8, Free at +0x10) using plain pool allocations - no SMC, no RPMB access.
#
# FV order is the ordering control: this must precede UFSDxe.
#
#INF AndromedaPkg/Driver/ShmBridgeStubDxe/ShmBridgeStubDxe.inf
# ^ never dispatched (no [SHMSTUB] line ever appeared). The install now happens
# inside UfsPreDxe, which demonstrably runs. Kept here, disabled, so the failure is
# recorded rather than lost.
#
# SecurityStubDxe's DxeCryptLib constructor locates the crypto protocol at load
# time and asserts when it is missing:
#
#   Locate gEfiCryptoProtocol failed: Not Found
#   ASSERT [SecurityStubDxe] DxeCryptLib.c(62)/(63)
#
# The provider used to sit here, ahead of RngDxe, and that was the bug. Its
# BaseCryptoDriverDxe DEPEXes on gEfiRngProtocolGuid, so DxeCore skips it on the
# first pass - but SecurityStubDxe, later in the same pass, does not wait, and its
# constructor then finds nothing. It now sits directly after RngDxe; see there.
#
#
# The real ShmBridge, ahead of UfsPreDxe on purpose.
#
# smcinvoke needs its buffers registered with TrustZone. ShmBridgeDxeLA does that
# with three SMCs; the stub UfsPreDxe installs only hands out pool memory, and TZ
# rejects an unregistered buffer with -6 (0xFFFFFFFA), which is exactly what
# ScmDxeCompat reported:
#
#   Scm Invoke call failed ret 0xFFFFFFFA
#   IClientEnv_registerWithCredentials failed -92
#
# This was tried before and the real driver never installed - it issues its SMCs
# and gives up when they fail. At that time TZ was not answering at all. It is now:
# SCM gets real error codes back rather than a fault, so the calls have somewhere
# to land.
#
# Both drivers publish the same GUID and whichever installs first is what every
# later LocateProtocol returns, so UfsPreDxe now checks before installing its stub
# and stands down if this one got there. If this driver still fails to install, the
# stub takes over and the log says so - no worse than before, and visible.
#
#
# This line puts the module IN THE FV. The INF in APRIORI.inc only puts it in the
# dispatch-order table - it does not pack the file. Removing this line while adding
# the APRIORI entry silently deleted the driver from the image: FVMAIN.Fv.txt went
# from one occurrence to zero. Both lines are needed, which is how ScmProbeDxe is
# set up too.
#
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
INF AndromedaPkg/Driver/UfsPreDxe/UfsPreDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/IsolatedBinaries/UFSDxe.inf
#
# After UFSDxe, not before: this reads UTRLBA and rides the transfer list UFSDxe built.
# If UFSDxe never gets that far, UTRLBA reads zero and this returns without doing
# anything, which is the correct behaviour.
#
INF AndromedaPkg/Driver/UfsWakeDxe/UfsWakeDxe.inf
#
# VerifiedBootDxe enabled as a diagnostic, not for its verdict.
#
# It reads the device state from the devinfo partition through plain block I/O, so it
# is the one component here that exercises UFS partitions without needing TZ or SCM -
# which is exactly the question we cannot answer at the moment. UFSDxe runs and prints
# nothing at all, so either it initialises and publishes nothing, or it returns early.
# VB will say which: a readable devinfo means UFS partitions work, a reported failure
# pins the problem lower down.
#
# Placed after UFSDxe for that reason. APRIORI is inert here and this INF declares no
# DEPEX, so FV order is the only ordering control.
#
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/VerifiedBootDxe/VerifiedBootDxe.inf
#
# TzDxeLA re-enabled, on its own, now that the UFS mapping is fixed.
#
# It was disabled because it asserts at TzDxeLBox.c +1119 (!EFI_ERROR) loading
# Keymaster from a UFS partition while UFS was dead:
#   Failed to map UFS_MEM_UFS base address: 0x00000004
# The map now covers [0x1D80000, 0x1DB0000) with no holes, so the open question is
# whether that partition is readable yet.
#
# Deliberately one module, not the whole TZ chain. ScmDxeCompat and ShmBridgeDxeLA
# stay out: without TZ they hang in SmcInvokeInit, and a hang costs more to diagnose
# than an assert. TzDxeLA either initialises or reports a clean failure.
#
# Position matters. APRIORI is inert here and none of these INFs declares a DEPEX,
# so FV order is the only control - TzDxeLA must follow UFSDxe or it is back to
# loading Keymaster from a disk that is not there.
#
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/TzDxeLA.inf
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf
#
# ShmBridgeDxeLA and ScmDxeCompat, enabled together on purpose.
#
# They are a pair: ScmDxeCompat publishes gQcomScmProtocolGuid and there is no other
# producer in this FV, which is why QRKSDxe reports "gQcomScmProtocolGuid - Fail- 14"
# and why UFSDxe's RPMB provisioning cannot find ScmProtocol. ShmBridgeDxeLA sets up
# the shared-memory pool that the SCM clients allocate from, so it goes first.
#
# Both are DEPEX TRUE and APRIORI is inert here, so FV order is the ordering control.
# They follow TzDxeLA, which must itself follow UFSDxe - TzDxeLA loads Keymaster from
# a UFS partition.
#
# This is the one gamble in the chain: UFS controller init was just repaired, and if
# it now gets far enough, TZ can read the partition, SCM can come up, and the RPMB
# listener stops being the blocker. If TZ still cannot read UFS, SmcInvokeInit will
# block instead of asserting - which is why this was held back until the UFS map was
# known to be hole-free.
#
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf
# Replay uses the extracted stock module, not the previous GPIO-patched variant.
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
# ClockDxe: ENABLED, and it works.
#
# The note that used to sit here said "DISABLED: it is the last driver printed
# before the hang, a PatchedBinary built for another SoC". None of that holds
# any more. The line below is not commented out, the ClockDxe in the FV is the
# stock shennong one (PE32 229,380 bytes against 229,376 for
# Binaries/QcomPkg/Drivers/ClockDxe/ClockDxe.efi), and it appears in the boot
# log as [DRV] A6DEB54D02531A4D8A82677A683B0D29.
#
# It also cannot be skipped: decoding I2CDxe.depex shows the second GUID in it
# is 241AFAE6-885F-4F6C-A7EA-C28EAB79C3E5, which is gEfiClockProtocolGuid, so
# I2CDxe would never dispatch without the clock driver publishing that protocol
# - and I2CDxe does dispatch. The clock is therefore not what breaks I2C.
INF QcomPkg/PatchedBinaries/ClockDxe.inf
#INF AndromedaPkg/Driver/DisplayCallerDxe/DisplayCallerDxe.inf
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf  # DEBUG: panel re-init (Xiaomi ABL may tear down display)  # disabled: panel alive via ABL splash; suspected ASSERT source
# PmicDxeLa DISABLED: it prints 'Mi Haptics' and then takes a synchronous
# external abort inside SPMI.dll, because SPMIDxe already failed with
# 'Failed to initialize reset' (it cannot get its GCC reset/clock while
# ClockDxe is disabled). Removing the consumer lets the rest of DXE run
# so we can see how far the USB stack gets.
INF QcomPkg/PatchedBinaries/PmicDxeLa.inf
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/PatchedBinaries/QcomChargerDxeLA.inf

# DXE without depex
##INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf

# Diagnostics + console (added for the USB bring-up test)
INF AndromedaPkg/Driver/SerialTextConsoleDxe/SerialTextConsoleDxe.inf
# Both drivers were only ever experiments and their sources are not part of this
# commit, so referencing them makes the FDF fail with "NOT found in DSC file".
# INF AndromedaPkg/Driver/UsbSerialFnDxe/UsbSerialFnDxe.inf
# INF AndromedaPkg/Driver/UsbProbeDxe/UsbProbeDxe.inf

# ScmDxeCompat DISABLED: queries config key "SmcInvokeConfig" (undefined here) ->
# AllocatePool(0) -> NULL -> EFI_OUT_OF_RESOURCES -> its ASSERT_EFI_ERROR dead-loops.
#
#FILE DRIVER = D8F91DDC-87C8-42A2-963E-8DD24A9F1FB5 {
#    SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.efi
#    SECTION UI = "ScmDxeCompat"
#}

#
# TZ / SCM chain DISABLED for good.
#
# TzDxeLA asserts the moment it is enabled:
#   ASSERT TzDxeLBox.c +1119: !EFI_ERROR (Status)   Status = Not Found
# because it loads Keymaster from a UFS partition and UFS still does not come up
# (Failed to map UFS_MEM_UFS base address: 0x00000004, and ConfigWords reports
# 'Error Reading from Partition'). With TzDxeLA out, SCM has no secure world to
# talk to and SmcInvokeInit blocks, so ShmBridge and ScmDxeCompat go with it.
#
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/TzDxeLA.inf
# TzDxe DISABLED: it needs to load Keymaster from a UFS partition

# TzDxe DISABLED: it needs to load Keymaster from a UFS partition

# TzDxe DISABLED: it needs to load Keymaster from a UFS partition

# TzDxe DISABLED: it needs to load Keymaster from a UFS partition


##INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
# ShmBridgeDxe DISABLED together with ScmDxeCompat (it needs SCM).
#FILE DRIVER = 41EBB6A6-2C19-4C02-841D-848CE004A282 {
#    SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.efi
#    SECTION UI = "ShmBridgeDxe"
#}

#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QRKSDxe/QRKSDxe.inf
FILE DRIVER = 3CF7F36E-5A97-496A-BB4B-D4F9937FA311 {
    SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QRKSDxe/QRKSDxe.efi
    SECTION UI = "QRKSDxe"
}

FILE DRIVER = 6b38f7b4-ad98-40e9-9093-aca2b5a253c4 {
    SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.efi
    SECTION UI = "DiskIoDxe"
}

FILE DRIVER = 1fa1f39e-feff-4aae-bd7b-38a070a3b609 {
    SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.efi
    SECTION UI = "PartitionDxe"
}

#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/LimitsDxe/LimitsDxe.inf
FILE DRIVER =  F09AD92E-4E91-490F-AB99-E69939B840B2 {
    SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/LimitsDxe/LimitsDxe.efi
    SECTION UI = "LimitsDxe"
}

#
# RngDxe is required: BaseCryptoDriverDxeAARCH64 DEPEXes on gEfiRngProtocolGuid
# and is the only provider of gEdkiiCryptoProtocolGuid, which SecurityStubDxe
# needs to publish gEfiSecurityArchProtocolGuid. Without the whole chain,
# SimpleWindowManagerDxe (and UsbfnDwc3Dxe) never dispatch.
#
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/RNGDxe/RngDxe.inf

#
# The crypto provider, directly after RngDxe and nowhere else.
#
# CryptoDriverBin_ALL carries BaseCryptoDriverDxe, which DEPEXes on
# gEfiRngProtocolGuid and is the only publisher of gEdkiiCryptoProtocolGuid.
# SecurityStubDxe's DxeCryptLib constructor needs that protocol and has DEPEX TRUE,
# so it does not wait for anything - if the provider has not dispatched by the time
# DxeCore reaches SecurityStubDxe, the constructor fails and the assert fires.
#
# DxeCore walks the FV in order and defers drivers whose DEPEX is unsatisfied
# without coming back to them until the next pass. So the provider has to appear
# after the module that satisfies its DEPEX, and before its consumer. That is
# exactly here: RngDxe above, SecurityStubDxe at Shennong.fdf:165 below.
#
# It used to sit near the top of the list, ahead of RngDxe, which put it in the
# deferred bucket on the pass that SecurityStubDxe ran in.
#
INF CryptoPkg/Binaries/edk2-basecrypto-driver-bin_extdep/Driver/Bin/CryptoDriverBin_ALL_Dxe_RELEASE_AARCH64.inf

#
# Qualcomm's ResetRuntimeDxe does NOT install gEfiResetArchProtocolGuid (verified:
# the GUID is absent from its PE), so nothing provided that arch protocol and
# SimpleWindowManagerDxe could not dispatch. EDK2's own driver does install it.
#
INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf

# Raw Files
FILE FREEFORM = A91D838E-A5FA-4138-825D-455E23030794 {
    SECTION UI = "logo1.bmp"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/logo1.bmp
}


FILE FREEFORM = 12BDE69E-CC7D-4CA9-9766-B4D9EE40053B {
    SECTION UI = "Panel_N11u_42_02_0a_amoled_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N11u_42_02_0a_amoled_dsc_cmd.xml
}

FILE FREEFORM = 6AC84C5B-017E-4FE0-919B-ABCABA5C6023 {
    SECTION UI = "Panel_N18_38_0c_0a_prim_ltpo_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N18_38_0c_0a_prim_ltpo_dsc_cmd.xml
}

FILE FREEFORM = 3F01BC81-09F2-4685-884F-AAF932927BDB {
    SECTION UI = "Panel_N18_42_02_0a_sec_ltpo_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N18_42_02_0a_sec_ltpo_dsc_cmd.xml
}

FILE FREEFORM = ABF776B7-81A6-4B52-A053-5A8C94CAFAB3 {
    SECTION UI = "Panel_N1_42_02_0a_ltpo_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N1_42_02_0a_ltpo_dsc_cmd.xml
}

FILE FREEFORM = E8CF7EF8-1271-4371-82C2-706BC386E820 {
    SECTION UI = "Panel_N2_42_02_0a_P11_ltpo_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N2_42_02_0a_P11_ltpo_dsc_cmd.xml
}

FILE FREEFORM = C9815EF0-B16F-4242-B701-23FF4BB22E3D {
    SECTION UI = "Panel_N2_42_02_0a_P2_ltpo_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N2_42_02_0a_P2_ltpo_dsc_cmd.xml
}

FILE FREEFORM = 39900925-CB7E-413A-BF07-E3727AB41B75 {
    SECTION UI = "Panel_N2_42_02_0a_ltpo_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N2_42_02_0a_ltpo_dsc_cmd.xml
}

FILE FREEFORM = 3DE713BB-9651-49BB-A6D0-BF421A6FA918 {
    SECTION UI = "Panel_N3_42_0d_0a_ltpo_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N3_42_0d_0a_ltpo_dsc_cmd.xml
}

FILE FREEFORM = F25C8B61-5864-4E57-839F-591613CD869E {
    SECTION UI = "Panel_N801_ext_bridge_prim_ivi_video.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N801_ext_bridge_prim_ivi_video.xml
}

FILE FREEFORM = F240DCB8-F0BD-4795-8A1D-417AF1F26E41 {
    SECTION UI = "Panel_N801_ext_bridge_sec_hud_video.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N801_ext_bridge_sec_hud_video.xml
}

FILE FREEFORM = E87C6E94-27D6-4245-9667-2222A847D7ED {
    SECTION UI = "Panel_N8_42_02_0a_prim_ltpo_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N8_42_02_0a_prim_ltpo_dsc_cmd.xml
}

FILE FREEFORM = 0D4D6FC6-ECB2-40BB-A1A0-B4015CE017A4 {
    SECTION UI = "Panel_N8_42_02_0a_sec_P11_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N8_42_02_0a_sec_P11_dsc_cmd.xml
}

FILE FREEFORM = 13360C68-F11F-4CF6-A03C-C80858DED953 {
    SECTION UI = "Panel_N8_42_02_0a_sec_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N8_42_02_0a_sec_dsc_cmd.xml
}

FILE FREEFORM = 21032958-BA78-499A-8ADA-094DECE23CBD {
    SECTION UI = "Panel_N8_42_02_0b_prim_ltpo_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_N8_42_02_0b_prim_ltpo_dsc_cmd.xml
}

FILE FREEFORM = CAAB3E2D-3009-49ED-B869-1917522632E2 {
    SECTION UI = "Panel_O11_42_02_0a_amoled_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_O11_42_02_0a_amoled_dsc_cmd.xml
}

FILE FREEFORM = 949E308D-1A8B-4E17-8B49-444C155DB8BD {
    SECTION UI = "Panel_O11_42_02_0a_p01_amoled_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_O11_42_02_0a_p01_amoled_dsc_cmd.xml
}

FILE FREEFORM = 9B65D65E-1C2D-4A1C-9B2E-2C2F5D6D3F21 {
    SECTION UI = "Panel_O11_42_02_0b_amoled_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_O11_42_02_0b_amoled_dsc_cmd.xml
}

FILE FREEFORM = D7170685-5965-4B44-9305-37CDB199B9BE {
    SECTION UI = "Panel_boe_amoled_fhd_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_boe_amoled_fhd_dsc_cmd.xml
}

FILE FREEFORM = 88CA5ABF-D0AC-4093-A68C-0CFAE1EF9635 {
    SECTION UI = "Panel_boe_amoled_wqhd_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_boe_amoled_wqhd_dsc_cmd.xml
}

FILE FREEFORM = 6523A5C9-91CB-45C0-A2DC-25997CF23369 {
    SECTION UI = "Panel_boe_amoled_wqhd_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_boe_amoled_wqhd_dsc_vid.xml
}

FILE FREEFORM = 8F276EE4-E75E-4C3B-8BCC-C37771E26478 {
    SECTION UI = "Panel_nt36860_wqhd_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_nt36860_wqhd_dsc_cmd.xml
}

FILE FREEFORM = 6F2D4884-D8A0-11E9-AF7A-D77B1FE990EC {
    SECTION UI = "Panel_nt36860_wqhd_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_nt36860_wqhd_dsc_vid.xml
}

FILE FREEFORM = F434E630-5D5D-49AB-BD05-D0C8DA17A84A {
    SECTION UI = "Panel_primary_sim_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_primary_sim_vid.xml
}

FILE FREEFORM = 8297C793-9861-422C-9F67-D447AF1EA812 {
    SECTION UI = "Panel_r66451_120hz_fhd_plus_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_r66451_120hz_fhd_plus_dsc_cmd.xml
}

FILE FREEFORM = F4A245C8-940E-4523-A53B-82E3C8BC6970 {
    SECTION UI = "Panel_r66451_120hz_fhd_plus_dsc_cmd_demura.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_r66451_120hz_fhd_plus_dsc_cmd_demura.xml
}

FILE FREEFORM = 61156380-EB8A-4618-940F-AB9BDEA1EEC2 {
    SECTION UI = "Panel_r66451_120hz_fhd_plus_dsc_cmd_hdk.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_r66451_120hz_fhd_plus_dsc_cmd_hdk.xml
}

FILE FREEFORM = 44B09226-C2B4-45ED-AA6F-6686DB103978 {
    SECTION UI = "Panel_r66451_120hz_fhd_plus_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_r66451_120hz_fhd_plus_dsc_vid.xml
}

FILE FREEFORM = 9874B60A-6B80-4D76-8EEB-7862C780EC03 {
    SECTION UI = "Panel_r66451_144hz_fhd_plus_dsc_cmd_cphy.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_r66451_144hz_fhd_plus_dsc_cmd_cphy.xml
}

FILE FREEFORM = BC28E12E-C7E0-47F9-BBF9-0B45ED6F4B18 {
    SECTION UI = "Panel_r66451_144hz_fhd_plus_dsc_vid_cphy.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_r66451_144hz_fhd_plus_dsc_vid_cphy.xml
}

FILE FREEFORM = 81CE7295-491D-4E3F-AAA7-AB43FEF2F615 {
    SECTION UI = "Panel_r66451_60hz_fhd_plus_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_r66451_60hz_fhd_plus_dsc_cmd.xml
}

FILE FREEFORM = 637AD94D-7D9B-4D5F-B420-770B65BECDA8 {
    SECTION UI = "Panel_r66451_60hz_fhd_plus_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_r66451_60hz_fhd_plus_dsc_vid.xml
}

FILE FREEFORM = 95FA1AB2-A4C8-4891-ABD3-3DE4BAB07680 {
    SECTION UI = "Panel_rumi_cwb_vga_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_rumi_cwb_vga_dsc_vid.xml
}

FILE FREEFORM = 294C3CF4-D6B6-48D3-8C59-12326B83B682 {
    SECTION UI = "Panel_sec_vtdr6130_144hz_fhd_plus_dsc_cmd_rfi.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_sec_vtdr6130_144hz_fhd_plus_dsc_cmd_rfi.xml
}

FILE FREEFORM = 3D03BAB4-E61B-47B3-A562-18206869B5D2 {
    SECTION UI = "Panel_sec_vtdr6130_144hz_fhd_plus_dsc_vid_rfi.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_sec_vtdr6130_144hz_fhd_plus_dsc_vid_rfi.xml
}

FILE FREEFORM = 9A2A35AB-C69B-2729-A529-4C2A6BECAA74 {
    SECTION UI = "Panel_secondary_sim_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_secondary_sim_vid.xml
}

FILE FREEFORM = D3F8580C-CCB8-4804-BB03-776E7B542FC2 {
    SECTION UI = "Panel_secondary_truly_1080p_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_secondary_truly_1080p_cmd.xml
}

FILE FREEFORM = D192FEE9-6A04-4BE8-ABFE-5534E2B457EC {
    SECTION UI = "Panel_secondary_truly_1080p_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_secondary_truly_1080p_vid.xml
}

FILE FREEFORM = 23EB5E2D-4A4D-4B35-BB7B-BE39D7EE440D {
    SECTION UI = "Panel_secondary_vtdr6130_144hz_fhd_plus_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_secondary_vtdr6130_144hz_fhd_plus_dsc_cmd.xml
}

FILE FREEFORM = A413F4E1-50FE-4848-82F1-EFB814608221 {
    SECTION UI = "Panel_secondary_vtdr6130_144hz_fhd_plus_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_secondary_vtdr6130_144hz_fhd_plus_dsc_vid.xml
}

FILE FREEFORM = 0AE323D6-2EBE-11E8-8349-9BA636A0D80C {
    SECTION UI = "Panel_sharp_1080p_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_sharp_1080p_cmd.xml
}

FILE FREEFORM = 0A60AF27-D18D-452F-BB8B-1A886F7F6CCC {
    SECTION UI = "Panel_sharp_120hz_qhd_plus_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_sharp_120hz_qhd_plus_dsc_cmd.xml
}

FILE FREEFORM = 81466938-BB9E-43DC-A208-A41BA82A5861 {
    SECTION UI = "Panel_sharp_120hz_qhd_plus_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_sharp_120hz_qhd_plus_dsc_vid.xml
}

FILE FREEFORM = 81D00687-181B-4B23-A1CC-85FE6B9E25E6 {
    SECTION UI = "Panel_sharp_120hz_qhdpipe_plus_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_sharp_120hz_qhdpipe_plus_dsc_vid.xml
}

FILE FREEFORM = 8BC595B7-391A-4A9A-8256-5B9983B30392 {
    SECTION UI = "Panel_sharp_4k_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_sharp_4k_dsc_cmd.xml
}

FILE FREEFORM = 19B58ACD-B1F8-4078-B9A5-A33584B680E2 {
    SECTION UI = "Panel_sharp_4k_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_sharp_4k_dsc_vid.xml
}

FILE FREEFORM = 87273B42-311D-11E8-9324-DFB2DADFC3D1 {
    SECTION UI = "Panel_truly_1080p_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_truly_1080p_cmd.xml
}

FILE FREEFORM = BAFD3200-311D-11E8-8651-0FB3CACF38FB {
    SECTION UI = "Panel_truly_1080p_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_truly_1080p_vid.xml
}

FILE FREEFORM = EB0A97F6-39DB-41D4-9B3C-CA3BADC144B4 {
    SECTION UI = "Panel_truly_wqxga_cmd_splitlink.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_truly_wqxga_cmd_splitlink.xml
}

FILE FREEFORM = 8CAE2C55-FC5A-410B-812B-8CA22F572F36 {
    SECTION UI = "Panel_truly_wqxga_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_truly_wqxga_dsc_cmd.xml
}

FILE FREEFORM = 13AC9DC9-0092-4374-AE1A-06FF35950FD0 {
    SECTION UI = "Panel_truly_wqxga_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_truly_wqxga_dsc_vid.xml
}

FILE FREEFORM = FC6315F9-F8FE-4222-A1E3-226DF55D7592 {
    SECTION UI = "Panel_truly_wqxga_dual_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_truly_wqxga_dual_cmd.xml
}

FILE FREEFORM = 95E7B040-26A2-4511-9ABB-1D95D6DA7082 {
    SECTION UI = "Panel_truly_wqxga_dual_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_truly_wqxga_dual_vid.xml
}

FILE FREEFORM = 138816EF-44B3-4FA1-9F8C-CA23B0AC72CD {
    SECTION UI = "Panel_vtdr6130_120hz_fhd_plus_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_vtdr6130_120hz_fhd_plus_dsc_cmd.xml
}

FILE FREEFORM = 54603FD1-6CD3-469E-A2FB-D0759525C423 {
    SECTION UI = "Panel_vtdr6130_120hz_fhd_plus_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_vtdr6130_120hz_fhd_plus_dsc_vid.xml
}

FILE FREEFORM = AE3F9548-010C-4AEC-A9D2-4AE3E16D2F8B {
    SECTION UI = "Panel_vtdr6130_144hz_fhd_plus_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_vtdr6130_144hz_fhd_plus_dsc_cmd.xml
}

FILE FREEFORM = 3DE03011-41E8-4353-A4E5-02721E71A0F4 {
    SECTION UI = "Panel_vtdr6130_144hz_fhd_plus_dsc_cmd_demura.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_vtdr6130_144hz_fhd_plus_dsc_cmd_demura.xml
}

FILE FREEFORM = 0EF4D970-0B59-4AF4-A8F7-F6A239EED792 {
    SECTION UI = "Panel_vtdr6130_144hz_fhd_plus_dsc_cmd_rfi.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_vtdr6130_144hz_fhd_plus_dsc_cmd_rfi.xml
}

FILE FREEFORM = 12372575-2A07-4C4E-BE92-6093A55FED33 {
    SECTION UI = "Panel_vtdr6130_144hz_fhd_plus_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_vtdr6130_144hz_fhd_plus_dsc_vid.xml
}

FILE FREEFORM = FFE173B7-8204-4353-BF62-70F8CE770916 {
    SECTION UI = "Panel_vtdr6130_144hz_fhd_plus_dsc_vid_demura.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_vtdr6130_144hz_fhd_plus_dsc_vid_demura.xml
}

FILE FREEFORM = 36630E03-0236-4983-ABDE-F982B23419FE {
    SECTION UI = "Panel_vtdr6130_60hz_fhd_plus_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_vtdr6130_60hz_fhd_plus_dsc_cmd.xml
}

FILE FREEFORM = 573FA171-1D5C-4C12-BDC8-19DCEFBBD483 {
    SECTION UI = "Panel_vtdr6130_60hz_fhd_plus_dsc_vid.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/Panel_vtdr6130_60hz_fhd_plus_dsc_vid.xml
}

FILE FREEFORM = FFB5253F-77AE-4103-B2A1-F1A74C63BD04 {
    SECTION UI = "sprconfig_n11u_42_02_0a_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/sprconfig_n11u_42_02_0a_dsc_cmd.xml
}

FILE FREEFORM = 4E40B970-5921-11EE-96EA-1FC465478E3A {
    SECTION UI = "sprconfig_n3_42_0d_0a_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/sprconfig_n3_42_0d_0a_dsc_cmd.xml
}

FILE FREEFORM = BC0FD621-43BE-4AB1-86D0-160FB616A6CB {
    SECTION UI = "sprconfig_o11_42_02_0a_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/sprconfig_o11_42_02_0a_dsc_cmd.xml
}

FILE FREEFORM = B2E00564-1B7F-453E-9C83-177585CD6F98 {
    SECTION UI = "sprconfig_o11_42_02_0b_dsc_cmd.xml"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/sprconfig_o11_42_02_0b_dsc_cmd.xml
}

FILE FREEFORM = 03E9256F-E621-48D8-8576-7E3B4FCF2D41 {
    SECTION UI = "PanelSimConfig.csv"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/PanelSimConfig.csv
}


FILE FREEFORM = 21E9BDD9-6C3F-4F10-84A5-BBEC322741F1 {
    SECTION UI = "uefipil.cfg"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/uefipil.cfg
}


FILE FREEFORM = 45FE4B7C-150C-45DA-A021-4BEB2048EC6F {
    SECTION UI = "QcomChargerCfg.cfg"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/QcomChargerCfg.cfg
}

###### FV2 ######
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SimpleTextInOutSerialDxe/SimpleTextInOutSerial.inf
INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/GpiDxe/GpiDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SdccDxe/SdccDxe.inf
# Charging drivers: ENABLED.
#
# Both were commented out, so the image carried no charger driver at all - only
# PmicDxe and PmicGlinkDxe, and the latter reaches the PMIC over GLINK to the
# ADSP, which is not running here. That left the Power Control USB protocol
# answering for port 0 but with nothing behind it.
#
# Both .depex files are two bytes (TRUE), so neither has a dependency and both
# will dispatch unconditionally.
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ChargerExDxe/ChargerExDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PmicGlinkDxe/PmicGlinkDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
#INF (already provided by the FILE DRIVER block below, same GUID 1C04F529)
# Stock USB function driver is the sole controller owner.
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
# INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf # b198: direct USB2 controller owner
# INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/XhciDxe/XhciDxe.inf # b198: direct USB2 controller owner
# INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbBusDxe/UsbBusDxe.inf # b198: direct USB2 controller owner
# INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbKbDxe/UsbKbDxe.inf # b198: direct USB2 controller owner
# INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbMassStorageDxe/UsbMassStorageDxe.inf # b198: direct USB2 controller owner
# INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf # b198: direct USB2 controller owner
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
INF AndromedaPkg/Driver/UsbStockReplayDxe/UsbStockReplayDxe.inf
INF AndromedaPkg/Driver/GcdProbeDxe/GcdProbeDxe.inf
INF AndromedaPkg/Driver/EarlyBdsArchDxe/EarlyBdsArchDxe.inf
#INF AndromedaPkg/Driver/TouchProbeDxe/TouchProbeDxe.inf   # disabled: reading TLMM resets the device
# INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf # b198: direct USB2 controller owner
# INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UsbInitDxe/UsbInitDxe.inf # b198: direct USB2 controller owner

#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UCDxe/UCDxe.inf
# b198 direct USB2 test: disable competing owner UCDxe
# FILE DRIVER = 5F0937CD-73F9-445E-B320-BB6FD5D3CBDE {
#     SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/UCDxe/UCDxe.efi
#     SECTION UI = "UCDxe"
# }
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PILDxe/PILDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SPSSDxe/SPSSDxe.inf
#
# ScmDxeCompat and TzDxeLA, here and not earlier.
#
# This is the whole point of the position. The chain is
#   UFS -> SPSS/TZ -> SCM -> QMI -> glink -> NPA
# and SCM sits above TZ, not beside it: SmcInvokeInit issues SMC calls that only
# answer once the secure processor is running. When ScmDxeCompat was enabled up
# near ICBDxe, the SmcInvokeConfig lookup succeeded and the boot then hung inside
# SmcInvokeInit - because SPSS had not loaded:
#   SPSSDxe: MountFatPartition(spunvm) failed
#   pil-SPSS: Failed to load metadata
# That was when UFS was dead. UFS now publishes all six LUNs and spunvm is on
# LUN 4 (sde66), so SPSSDxe has something to mount and the order below is the one
# that can actually work.
#
# TzDxeLA comes after ScmDxeCompat because every QSEECOM method forwards to
# gScmProtocol->ScmSendCommand (reverse/src/TzDxeLA.c); with SCM absent the
# protocol installs and then does nothing.
#
# Neither declares a DEPEX and APRIORI is inert in this port, so FV order is the
# only ordering control - this position is the whole mechanism.
#
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.inf
#
# The probe, between SCM and TzDxe on purpose: it reports the state ScmDxeCompat
# left behind, and it has to run before TzDxeLA reaches the code that crashes.
#
INF AndromedaPkg/Driver/PcdShimDxe/PcdShimDxe.inf
# Touch: GENI SPI at 0xA90000 + Synaptics TCM, publishes Absolute Pointer.
# The driver prints each stage and never fails the boot, so a partial chain still
# lets the UI come up.
INF AndromedaPkg/Driver/SynapticsTouchDxe/SynapticsTouchDxe.inf
INF AndromedaPkg/Driver/ReadOnlyStorageDxe/ReadOnlyStorageDxe.inf
INF Ext4Pkg/Ext4Dxe/Ext4Dxe.inf
INF AndromedaPkg/Driver/ProjectInfoCacheDxe/ProjectInfoCacheDxe.inf
INF AndromedaPkg/Driver/ScmProbeDxe/ScmProbeDxe.inf
INF AndromedaPkg/Driver/AdspProbeDxe/AdspProbeDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/TzDxe/TzDxeLA.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/FvUtilsDxe/FvUtilsDxe.inf

# Dxe without depex 
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ASN1X509Dxe/ASN1X509Dxe.inf
FILE DRIVER = C2F9A4F5-F7B4-43E7-BA99-5EA804CC103A {
    SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ASN1X509Dxe/ASN1X509Dxe.efi
    SECTION UI = "ASN1X509Dxe"
}

#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SecRSADxe/SecRSADxe.inf
FILE DRIVER = 32C71E68-83A8-46ED-AED1-094E71B12057 {
    SECTION UI = "SecRSADxe"
    SECTION PE32 = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SecRSADxe/SecRSADxe.efi
}

# Patched Binaries
#INF QcomPkg/PatchedBinaries/FeatureEnablerDxe.inf
FILE DRIVER = 1C04F529-0612-4319-9315-B2E75C16810A {
    SECTION PE32 = QcomPkg/PatchedBinaries/FeatureEnablerDxe.efi
    SECTION UI = "FeatureEnablerDxe"
}
# --- New SM8650 drivers (from uefi_a dump); enable as needed for bring-up ---
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/Smp2pDxe/Smp2pDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/SSRDxe/SSRDxe.inf
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/RmVmDxe/RmVmDxe.inf
INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/ConfigWordsDxe/ConfigWordsDxe.inf
#INF $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/QcomPkg/Drivers/HSUARTDxe/HSUART.inf

#
# SecParti.cfg - the RPMB/GPT partition configuration.
#
# Without it the boot reports:
#
#   Failed to load SecParti.cfg Status = (0xE)
#   Failed to initialize Partition config. Status 0x0000000E
#
# 0xE is EFI_NOT_FOUND, and it is genuine absence: the only reference to this
# file was in the older Binaries/DXE.inc, and the file itself was not in
# Binaries/RawFiles/ at all, so nothing was ever packed.
#
# The content is the copy recovered from this device's own firmware
# (extract/uefi_extract/SecParti.cfg.raw), not a reconstruction - the file starts
# by stating that its version must match the partition table version in TZ, so a
# guessed one would be worse than none.
#
FILE FREEFORM = A1E235DE-E825-4591-9623-C43175811826 {
    SECTION UI = "SecParti.cfg"
    SECTION RAW = $(PACKAGE_NAME)/Device/$(TARGET_DEVICE)/Binaries/RawFiles/SecParti.cfg
}
