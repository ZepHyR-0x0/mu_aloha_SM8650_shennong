/** @file
  EarlyArchProbeDxe - is gCpu published when the QC drivers start mapping?

  Same range, same API: SetMemorySpaceAttributes fails early in DXE but returns
  Success when called from BDS. CoreSetMemorySpaceAttributes ends in
  gCpu->SetMemoryAttributes, so the obvious suspect is that the CPU arch protocol
  is not published yet at the early call site.

  This driver has Depex TRUE and sits immediately after DxeMain, so it runs
  before EnvDxe and the whole QC sequence. It only reports presence - no MMIO, no
  writes.

  Copyright (c) 2026, ProjectMu shennong port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>

STATIC
VOID
Say (
  IN CONST CHAR8  *Text
  )
{
  SerialPortInitialize ();
  SerialPortWrite ((UINT8 *)Text, AsciiStrLen (Text));
}

STATIC
VOID
Probe (
  IN CONST CHAR8  *Name,
  IN EFI_GUID     *Guid
  )
{
  EFI_STATUS  Status;
  VOID        *Ifc;
  CHAR8       Buf[128];
  UINTN       Len;

  Ifc    = NULL;
  Status = gBS->LocateProtocol (Guid, NULL, &Ifc);
  Len    = AsciiSPrint (Buf, sizeof (Buf), "[ARCH] %a = %r\n", Name, Status);
  SerialPortWrite ((UINT8 *)Buf, Len);
}

EFI_STATUS
EFIAPI
EarlyArchProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  Say ("[ARCH] probe start (before EnvDxe)\n");
  Probe ("CpuArch     ", &gEfiCpuArchProtocolGuid);
  Probe ("MetronomeArch", &gEfiMetronomeArchProtocolGuid);
  Probe ("TimerArch   ", &gEfiTimerArchProtocolGuid);
  Probe ("SecurityArch", &gEfiSecurityArchProtocolGuid);
  Say ("[ARCH] probe done\n");
  return EFI_SUCCESS;
}
