/** @file
  EarlyBdsArchDxe - publish gEfiBdsArchProtocolGuid before BDS actually runs.

  Why
  ---
  HALIOMMUDxe DEPEXes on twelve architectural protocols, one of which is
  gEfiBdsArchProtocolGuid (665e3ff6-46cc-11d4-9a38-0090273fc14d). The only
  publisher is BdsDxe, and BdsDxe cannot be moved earlier without also starting
  BDS early - its entry installs the protocol and then immediately runs
  BdsEntry, which would begin booting before half of DXE has dispatched.

  So HALIOMMU can never dispatch until position ~59, and every driver before it
  that needs a device mapped reports:

    DALLOC Device [0x2000070]: HWIO mapping failed
    Failed to map PhysicalAddr:0x..., size:0x..., SetMemorySpaceAttributes

  Installing the protocol early from here satisfies the DEPEX without starting
  BDS. BdsDxe still installs its own instance later and still runs BdsEntry
  itself; this stub exists purely so the dependency graph can be satisfied in
  time.

  The Entry below does nothing. If anything ever calls it expecting a real BDS,
  the boot will stop here instead of continuing - that is the cost of this
  approach and the reason the driver prints when it is called.

  Copyright (c) 2026, ProjectMu shennong port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/BaseLib.h>
#include <Library/SerialPortLib.h>
#include <Protocol/Bds.h>

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
EFIAPI
EarlyBdsEntry (
  IN EFI_BDS_ARCH_PROTOCOL  *This
  )
{
  //
  // Deliberately not starting BDS: BdsDxe will do that when it dispatches.
  // Reaching this line means something asked for BDS earlier than expected.
  //
  Say ("[BDS] early stub Entry called - real BDS not available yet\n");
}

STATIC EFI_BDS_ARCH_PROTOCOL  mBdsArch = { EarlyBdsEntry };

EFI_STATUS
EFIAPI
EarlyBdsArchEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gEfiBdsArchProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mBdsArch
                  );

  if (EFI_ERROR (Status)) {
    Say ("[BDS] early BdsArch install failed\n");
  } else {
    Say ("[BDS] early BdsArch installed\n");
  }

  return EFI_SUCCESS;
}
