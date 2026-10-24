/** @file

  Copyright (c) 2011-2014, ARM Limited. All rights reserved.
  Copyright (c) 2014, Linaro Limited. All rights reserved.
  Copyright (c) 2023, DuoWoA Authors. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent


  NOTICE:
    Some Addresses, Protocols, Functions and Guids are got from decompile tools.

**/

#include <PiPei.h>

#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/SecProtocolFinderLib.h>
#include "PlatformPeiLibInternal.h"
#include <Library/PrintLib.h>

// PORT-TRACE: the QC DXE drivers call into this shim through the ShLib HOB.
// DEBUG() is compiled out (MDEPKG_NDEBUG), so log with SerialPortWrite, which
// reaches the framebuffer console (FrameBufferSerialPortLib).
STATIC
VOID
ShTrace (
  IN CONST CHAR8 *Label,
  IN CONST CHAR8 *Name,
  IN EFI_STATUS  Status
  )
{
  CHAR8 Buffer[160];
  UINTN Length;

  Length = AsciiSPrint (
             Buffer, sizeof (Buffer),
             "[SH] %a %a -> %r\n\r",
             Label, (Name != NULL) ? Name : "(null)", Status);
  SerialPortWrite ((UINT8 *)Buffer, Length);
}


STATIC
EFI_STATUS
CfgGetMemInfoByName(
    CHAR8 *RegionName, ARM_MEMORY_REGION_DESCRIPTOR_EX *MemRegions)
{
  EFI_STATUS Status;
  CHAR8      Buffer[128];
  UINTN      Length;

  Status = LocateMemoryMapAreaByName(RegionName, MemRegions);

  //
  // Print what we actually hand back. UFSDxe reports
  //   ERROR: 0x00000018 Failed to map UFS_MEM_UFS base address: 0x00000004
  // even though the config database in the FV clearly carries
  //   UFS_MEM_UFS  0x1D84000  0x3000
  // so this line shows whether the descriptor is filled in at all.
  //
  Length = AsciiSPrint (
             Buffer, sizeof (Buffer),
             "[SH] mem-by-name %a -> %r base=0x%lx size=0x%lx\n\r",
             RegionName, Status,
             (MemRegions != NULL) ? MemRegions->Address : 0,
             (MemRegions != NULL) ? MemRegions->Length : 0);
  SerialPortWrite ((UINT8 *)Buffer, Length);

  return Status;
}

//
// Value handed to ScmDxeCompat for "SmcInvokeConfig", which it forwards to
// SmcInvokeInit. 0x20040100 is the value observed on this platform; keep it as
// a named constant so it is easy to try another if SmcInvokeInit rejects it.
//
#define SMC_INVOKE_CONFIG_VALUE  0x20040100

STATIC
EFI_STATUS
CfgGetMemInfoByAddress(
    UINT64 RegionBaseAddress, ARM_MEMORY_REGION_DESCRIPTOR_EX *MemRegions)
{
  EFI_STATUS Status = LocateMemoryMapAreaByAddress(RegionBaseAddress, MemRegions);
  CHAR8      Buffer[96];
  UINTN      Length;

  Length = AsciiSPrint (
             Buffer, sizeof (Buffer),
             "[SH] mem-by-addr 0x%lx -> %r\n\r",
             RegionBaseAddress, Status);
  SerialPortWrite ((UINT8 *)Buffer, Length);
  return Status;
}

STATIC
EFI_STATUS
CfgGetCfgInfoString(CHAR8 *Key, CHAR8 *Value, UINTN *ValBuffSize)
{
  if (AsciiStriCmp(Key, "OsTypeString") == 0) {
    AsciiStrCpyS(Value, *ValBuffSize, "WP");
    ShTrace ("cfg-str", Key, EFI_SUCCESS);
    return EFI_SUCCESS;
  }

  ShTrace ("cfg-str", Key, EFI_NOT_FOUND);
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
CfgGetCfgInfoVal(CHAR8 *Key, UINT32 *Value)
{
  EFI_STATUS Status;

  //
  // PORT: answer the keys the platform expects but no configuration map here
  // provides.
  //
  // ScmDxeCompat queries "SmcInvokeConfig" and feeds it to SmcInvokeInit. The
  // lookup falls through to this shim, which has no entry for it, so it returns
  // EFI_NOT_FOUND and the driver stops right there:
  //
  //   [SH] load-lib UEFI Config Lib -> Success
  //   [SH] cfg-u32 SmcInvokeConfig -> Not Found
  //
  // Without SCM nothing downstream works - no QMI, no PMIC glink, no NPA client
  // for UsbConfigLib. Treat it as enabled.
  //
  if (AsciiStriCmp (Key, "SmcInvokeConfig") == 0) {
    *Value = SMC_INVOKE_CONFIG_VALUE;
    ShTrace ("cfg-u32", Key, EFI_SUCCESS);
    return EFI_SUCCESS;
  }

  //
  // QcomLib's GetSharedImemBootDeviceType() reads "SharedIMEMBaseAddr", validates
  // shared IMEM + 0x50, and then uses OverRideSharedImemBootCfg as the boot device
  // type itself when this lookup succeeds with a value <= 0xB.
  //
  // UFSDxe's entry only takes the full UFS initialisation path (link startup, ICE,
  // LUN scan, BlockIo publishing) when that type compares equal to 8:
  //
  //   cmp w8, #8 ; cset w0, eq
  //   type != 8 -> SMMU config only, return EFI_SUCCESS, publish nothing
  //
  // 0x8 is the value carried by the reference configuration maps for this family,
  // so answer it explicitly. 0 made UFSDxe return success without publishing any
  // block device, which is the "UFS is silent" symptom.
  //
  // The +160 assert seen with 0x8 is NOT the ProdMode HOB: it is the RPMB
  // listener's LocateProtocol(9C1EB71F = ShmBridge) failing. ProdMode is only read
  // afterwards by DebugAssert (0x6FF4 calls the getter at 0x16014) when the assert
  // handler decides how to stop, which is why "Prodmode HOB not found" appears next
  // to the assert. ShmBridgeStubDxe publishes that protocol; the HOB would only
  // change how an assert halts.
  //
  // Editing uefiplat.cfg does not help: these lookups do not read the cfg text, the
  // key table is compiled into the config library.
  //
  if (AsciiStriCmp (Key, "OverRideSharedImemBootCfg") == 0) {
    *Value = 0x8;
    ShTrace ("cfg-u32", Key, EFI_SUCCESS);
    return EFI_SUCCESS;
  }

  //
  // LoadAutoImageInPILFlag is absent from this platform's configuration map but is
  // present as 0x1 in the public ports referenced above. It gates automatic firmware
  // image loading, and this port still reports "fwload - ERR - fail!" from GpiDxe -
  // the GPIO firmware never loads. Worth supplying rather than leaving the lookup to
  // fail.
  //
  if (AsciiStriCmp (Key, "LoadAutoImageInPILFlag") == 0) {
    *Value = 0x1;
    ShTrace ("cfg-u32", Key, EFI_SUCCESS);
    return EFI_SUCCESS;
  }

  //
  // MaxCoreCnt is absent from this platform's map but is 8 in public SM8650 ports, and
  // SecProtocolFinderLib looks it up by name while bringing up secondary cores. A
  // failed lookup there leaves the core count to a default rather than to the platform.
  //
  if (AsciiStriCmp (Key, "MaxCoreCnt") == 0) {
    *Value = 8;
    ShTrace ("cfg-u32", Key, EFI_SUCCESS);
    return EFI_SUCCESS;
  }

  Status = LocateConfigurationMapUINT32ByName(Key, Value);

  ShTrace ("cfg-u32", Key, Status);
  return Status;
}

STATIC
EFI_STATUS
CfgGetCfgInfoVal64(CHAR8 *Key, UINT64 *Value)
{
  EFI_STATUS Status = LocateConfigurationMapUINT64ByName(Key, Value);

  ShTrace ("cfg-u64", Key, Status);
  return Status;
}

STATIC
UINTN
SFlush(VOID) { return EFI_SUCCESS; }

STATIC
UINTN
SControl(IN UINTN Arg, IN UINTN Param) { return EFI_SUCCESS; }

STATIC
BOOLEAN
SPoll(VOID) { return TRUE; }

STATIC
UINTN
SDrain(VOID) { return EFI_SUCCESS; }

STATIC
EFI_STATUS
ShInstallLib(IN CHAR8 *LibName, IN UINT32 LibVersion, IN VOID *LibIntf)
{
  return EFI_SUCCESS;
}

UefiCfgLibType ConfigLib = {0x00010002,          CfgGetMemInfoByName,
                            CfgGetCfgInfoString, CfgGetCfgInfoVal,
                            CfgGetCfgInfoVal64,  CfgGetMemInfoByAddress};

SioPortLibType SioLib = {
    0x00010001, SerialPortRead, SerialPortWrite, SPoll,
    SDrain,     SFlush,         SControl,        SerialPortSetAttributes,
};

STATIC
EFI_STATUS
ShLoadLib(CHAR8 *LibName, UINT32 LibVersion, VOID **LibIntf)
{
  if (LibIntf == NULL)
    return EFI_NOT_FOUND;

  if (AsciiStriCmp(LibName, "UEFI Config Lib") == 0) {
    *LibIntf = &ConfigLib;
    ShTrace ("load-lib", LibName, EFI_SUCCESS);
    return EFI_SUCCESS;
  }

  if (AsciiStriCmp(LibName, "SerialPort Lib") == 0) {
    *LibIntf = &SioLib;
    ShTrace ("load-lib", LibName, EFI_SUCCESS);
    return EFI_SUCCESS;
  }

  ShTrace ("load-lib", LibName, EFI_NOT_FOUND);
  return EFI_NOT_FOUND;
}

ShLibLoaderType ShLib = {0x00010001, ShInstallLib, ShLoadLib};

STATIC
VOID BuildMemHobForFv(IN UINT16 Type)
{
  EFI_PEI_HOB_POINTERS      HobPtr;
  EFI_HOB_FIRMWARE_VOLUME2 *Hob = NULL;

  HobPtr.Raw = GetHobList();
  while ((HobPtr.Raw = GetNextHob(Type, HobPtr.Raw)) != NULL) {
    if (Type == EFI_HOB_TYPE_FV2) {
      Hob = HobPtr.FirmwareVolume2;
      /* Build memory allocation HOB to mark it as BootServicesData */
      BuildMemoryAllocationHob(
          Hob->BaseAddress, EFI_SIZE_TO_PAGES(Hob->Length) * EFI_PAGE_SIZE,
          EfiBootServicesData);
    }
    HobPtr.Raw = GET_NEXT_HOB(HobPtr);
  }
}

STATIC GUID gEfiInfoBlkHobGuid   = EFI_INFORMATION_BLOCK_GUID;
STATIC GUID gFvDecompressHobGuid = EFI_FV_DECOMPRESS_GUID;
STATIC GUID gEfiShLibHobGuid     = EFI_SHIM_LIBRARY_GUID;

VOID InstallPlatformHob()
{
  static int initialized = 0;

  if (!initialized) {
    ARM_MEMORY_REGION_DESCRIPTOR_EX InfoBlk;
    EFI_STATUS InfoBlkStatus;

    InfoBlkStatus = LocateMemoryMapAreaByName ("Info Blk", &InfoBlk);
    {
      // PORT-TRACE: EnvDxe rejects the platform when GetInfoBlkPtr() fails, so
      // show both the resolved address and the first words XBL left there.
      CHAR8  T[192];
      UINTN  TL;
      UINT32 *W = (UINT32 *)(UINTN)InfoBlk.Address;

      if (EFI_ERROR (InfoBlkStatus)) {
        TL = AsciiSPrint (T, sizeof (T),
               "[SH] InfoBlk lookup FAILED -> %r\n\r", InfoBlkStatus);
      } else {
        TL = AsciiSPrint (T, sizeof (T),
               "[SH] InfoBlk @0x%lx len 0x%lx : %08x %08x %08x %08x %08x %08x %08x %08x\n\r",
               (UINT64)InfoBlk.Address, (UINT64)InfoBlk.Length,
               W[0], W[1], W[2], W[3], W[4], W[5], W[6], W[7]);
      }
      SerialPortWrite ((UINT8 *)T, TL);
    }
    UINTN Data3 = 0xA7100000; // FV2 Address (SM8650 window, per OnePlus 13R port)
    UINTN InfoBlkAddress = InfoBlk.Address;
    UINTN ShLibAddress   = (UINTN)&ShLib;
    UINTN SchedIntf=0;
    UINTN DTBExtnProtocol=0;

    InitProtocolFinder(&SchedIntf, &DTBExtnProtocol);
    {
      // Preserve the SecProtocolFinderLib trace: the "UEFI Log" ring is only
      // 64 KiB and these lines are the first thing written into it, so by the
      // time DXE can show them they have been overwritten by the boot spew.
      UINT8 *SfDiag   = NULL;
      UINTN  SfDiagLen = 0;

      SecFinderGetDiag (&SfDiag, &SfDiagLen);
      if ((SfDiag != NULL) && (SfDiagLen != 0)) {
        BuildGuidDataHob (&gAndromedaSfDiagHobGuid, SfDiag, SfDiagLen);
      }
    }
    BuildMemHobForFv(EFI_HOB_TYPE_FV2);
    BuildGuidDataHob(
        &gEfiInfoBlkHobGuid, &InfoBlkAddress, sizeof(InfoBlkAddress));
    BuildGuidDataHob(&gEfiShLibHobGuid, &ShLibAddress, sizeof(ShLibAddress));
    BuildGuidDataHob(&gFvDecompressHobGuid, &Data3, sizeof(Data3));
    BuildGuidDataHob(&gEfiSchedIntfGuid, &SchedIntf, sizeof(SchedIntf));
    BuildGuidDataHob(&gEfiSecDtbGuid, &DTBExtnProtocol, sizeof(DTBExtnProtocol));

    initialized = 1;
  }
}

EFI_STATUS
EFIAPI
PlatformPeim(VOID)
{

  BuildFvHob(PcdGet64(PcdFvBaseAddress), PcdGet32(PcdFvSize));

  InstallPlatformHob();

  return EFI_SUCCESS;
}
