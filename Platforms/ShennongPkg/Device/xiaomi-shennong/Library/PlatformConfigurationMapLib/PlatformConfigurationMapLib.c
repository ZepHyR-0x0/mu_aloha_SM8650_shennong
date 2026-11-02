#include <Library/BaseLib.h>
#include <Library/PlatformConfigurationMapLib.h>

static CONFIGURATION_DESCRIPTOR_EX gDeviceConfigurationDescriptorEx[] = {
//    {"AbnormalResetOccurredOffset", 0x24},
//    {"BootConfigRegAddr", 0x00786070},
//    {"BootConfigRegMask", 0x0000003E},
//    {"BootConfigRegOfset", 0x01},
    {"AllowNonPersistentVarsInRetail", 0x1},
//    {"DBIDumpDDRBase", 0x82A00000},
//    {"DDRThroughput", 6000000000},
    {"DDRInfoNotifyFlag", 0x0},
    {"DRAM_CLK_PERIOD_ADDR", 0x240BA050},
    {"DetectRetailUserAttentionHotkey", 0x00},
    {"DetectRetailUserAttentionHotkeyCode", 0x17},
    {"DloadCookieAddr", 0x01FD9000},
    {"DloadCookieValue", 0x10},
    //
    // Stock says 2, and 2 is what hung the scheduler along with the threading
    // switches above. Left at 1 - one core is initialised early, the rest come up
    // later. This is the value the port has always booted with.
    //
    {"EarlyInitCoreCnt", 1},
    {"EnableACPIFallback", 0x0},
    {"EnableDisplayImageFv", 0x1},
    //
    // These four stay at the values this port has always booted with, not stock's.
    //
    // Matching stock here hung the boot dead:
    //
    //   MultiThreading: ON
    //   CPU Cores: 8 (init 2)
    //   Init scheduler v3: 0xA7014C6B8
    //   <halt>
    //
    // The stock firmware can run multithreaded because its scheduler is intact.
    // This port's is not - the gEfiSchedIntfGuid HOB arrives with zero data, which
    // is why SecProtocolFinderLib exists here at all - so turning the threading on
    // walks straight into a scheduler that cannot start.
    //
    // EnableDisplayThread is held back with them because a display thread needs the
    // same scheduler.
    //
    {"EnableLogFsSyncInRetail", 0x0},
    {"EnableDisplayThread", 0},
    {"EnableMultiThreading", 0},
    {"EnableMultiCoreFvDecompression", 0},
    {"EnableOEMSetupAppInRetail", 0x0},
//    {"EnablePXE", 0x0},
    {"EnableSDHCSwitch", 0x1},
//    {"EnableSecondaryUFSforHLOS", 0},
    {"EnableShell", 0x1},
    {"EnableUfsIOC", 1},
    {"EnableUefiSecAppDebugLogDump", 0x0},
    {"EnableVariablePolicyEngine", 0},
    {"LoadAutoImageInPILFlag", 0x1},
//    {"GccCe1ClkCntlReg", 0x00152004},
//    {"GccCe1ClkCntlVal", 0x00000038},
    {"MaxCoreCnt", 8},
    {"MaxLogFileSize", 0x400000},
//    {"MemoryCaptureModeOffset", 0x1C},
    {"NumActiveCores", 8},
    {"NumCpus", 8},
//    {"NumCpusFuseAddr", 0x5C04C},
    {"PilSubsysDbgCookieAddr", 0x146806DC},
    {"PilSubsysDbgCookieVal", 0x53444247},
//    {"PSHoldOffset", 0xC000},
//    {"PSHoldSHFT", 0x0},
    {"PrimaryLogoIndex", 0x3},
    {"SecondaryLogoIndex", 0x2},
    {"PwrBtnShutdownFlag", 0x0},
    {"Sdc1GpioConfigOff", 0xA00},
    {"Sdc1GpioConfigOn", 0x1E92},
    {"Sdc2GpioConfigOff", 0xA00},
    {"Sdc2GpioConfigOn", 0x1E92},
    {"SecurityFlag", 0xC4},
    {"SecPagePoolCount", 0x800},
//    {"SerialPortBufferSize", 0x8000},
    {"SharedIMEMBaseAddr", 0x14680000},
    {"ShmBridgememSize", 0xA00000},
    //
    // SmcInvokeConfig has to be here, not only in uefiplat.cfg.
    //
    // This table is what LocateConfigurationMapUINT32ByName actually walks -
    // ConfigurationMapHelperLib resolves to ConfigurationMapHelperSecLib, which
    // returns this array and stops at the 0xFFFFFFFF terminator. The cfg text
    // packed into the FV is a separate thing that nothing on the DXE side parses.
    //
    // So adding the key to uefiplat.cfg alone changed nothing for drivers. Only
    // ScmDxeCompat ever saw it, because PlatformPeiLib's shim answers that one key
    // by hand. The probe confirmed the gap: SmcInvokeConfig read back as
    // EFI_NOT_FOUND from the DXE side while the shim reported Success.
    //
    // 0x20040100 is three 0x100 fields - bits[31:21], bits[20:10], bits[9:0] - the
    // same value the shim returns, so both paths now agree.
    //
    {"SmcInvokeConfig", 0x20040100},
    //
    // These two are in the stock [ConfigParameters] and were missing here.
    // SkipDBISetup = 0 means DBI setup runs; ConfigParameterCount is the value the
    // stock file declares, kept as-is rather than recomputed.
    //
    {"ConfigParameterCount", 0x40},
    {"SkipDBISetup", 0x0},
//    {"TzAppsRegnAddr", 0x87900000},
//    {"TzAppsRegnSize", 0x02200000},
//    {"TzDiagOffset", 0x720},
//    {"TzDiagSize", 0x2000},
    {"UfsSmmuConfigForOtherBootDev", 1},
    {"UsbFnIoRevNum", 0x00010001},
    {"UefiMemUseThreshold", 0xE1},
    {"USBHS1_Config", 0x0},
    /* Terminator */
    {"Terminator", 0xFFFFFFFF}};

CONFIGURATION_DESCRIPTOR_EX *GetPlatformConfigurationMap()
{
  return gDeviceConfigurationDescriptorEx;
}