APRIORI DXE {
    #
    # MINIMAL PROFILE - pure UEFI core + console, ZERO Qualcomm binaries.
    #
    # Why this profile exists
    # -----------------------
    # The full profile carries 40 Qualcomm prebuilt drivers. The very first of
    # them (EnvDxe) builds the platform configuration database from
    # uefiplat.cfg / the Info Blk HOB, and everything after it queries that
    # database. EnvDxe returns EFI_NOT_FOUND on this port, so the whole QC
    # stack never gets off the ground.
    #
    # This profile proves the parts that do NOT depend on Qualcomm plumbing:
    #   BootShim -> FD window 0xA7100000 -> PrePi -> DxeCore -> memory map ->
    #   ARM architectural drivers -> GOP (SimpleFbDxe) -> ConOut -> BDS
    #
    # Switch with: scripts/profile.sh full|min
    #

    # --- UEFI core -------------------------------------------------------
    INF MdeModulePkg/Core/Dxe/DxeMain.inf
    INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf
    INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
    INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
    INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf

    # --- ARM architectural ------------------------------------------------
    INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
    INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf

    #
    # --- Console, as early as possible ------------------------------------
    #
    # gST->ConOut stays NULL until ConSplitterDxe runs. Any driver that prints
    # through ConOut (Print(), ConOut->OutputString, a ConOut-based DebugLib)
    # before that takes a NULL+8 data abort: EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL has
    # Reset at offset 0 and OutputString at offset 8, which is exactly the
    # FAR = 0x8 / EC=0x25 / ISS=6 (translation fault, level 2) seen right after
    # AcpiPlatformDxe was loaded. Bring the console up first so no later driver
    # can trip over a missing ConOut.
    #
    #
    # Serial-backed text console FIRST: no GOP, no HII font, no QC config DB.
    # BDS and the shell talk through ConOut; without a working ConOut device
    # they run but print nothing, which is exactly the "silent hang" seen
    # after [BDS] entry.
    #
    # Written from scratch instead of the Qualcomm prebuilt: its entry point
    # returns an error here and trips ASSERT_EFI_ERROR in its own AutoGen.c.
    INF AndromedaPkg/Driver/SerialTextConsoleDxe/SerialTextConsoleDxe.inf

    INF AndromedaPkg/Driver/SimpleFbDxe/SimpleFbDxe.inf
    INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
    INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
    INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
    INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
    INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
    INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf

    # --- Remaining architectural -----------------------------------------
    INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
    INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

    # --- Platform services + BDS -----------------------------------------
    INF AndromedaPkg/Driver/RamPartitionDxe/RamPartitionDxe.inf
    INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
    INF MdeModulePkg/Universal/FvSimpleFileSystemDxe/FvSimpleFileSystemDxe.inf
    INF FatPkg/EnhancedFatDxe/Fat.inf
    INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
    INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
    INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
    INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
    INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
}
