/** @file
  Observe the UFS host controller just before UFSDxe runs, and log what it looks like.

  This started as a reset driver: clear HCE so UFSDxe could not mistake ABL's leftovers
  for its own initialisation. That experiment has been run and it disproved the idea.

    [UFSPRE] pre  HCS=0x0000010F dp=1 utrl=1 utmrl=1 HCE=0x00000001 IS=0x00000400
    [UFSPRE] HCE cleared after 0 us
    [UFSPRE] post HCS=0x00000008 dp=0 utrl=0 utmrl=0 HCE=0x00000000 IS=0x00000000

  and the dump taken after DXE dispatch was byte for byte identical to the "post" line:
  UFSDxe did not touch a single register. It is not skipping its init because the
  controller looked started - it exits before it reaches hardware at all.

  So the write is gone. Clearing HCE only degraded a working controller, and the
  registers now stay exactly as ABL left them. What remains is the observation, which is
  still worth having: comparing this line with the dump FrontPage prints after dispatch
  proves, on every boot, whether anything in between touched the controller.

  Copyright (c) 2024, Project Mu contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#define UFS_MEM_BASE  0x1D84000ULL

#define UFSHCI_CAP    0x00
#define UFSHCI_VER    0x08
#define UFSHCI_IS     0x20
#define UFSHCI_HCS    0x30
#define UFSHCI_HCE    0x34

#define HCS_DP        BIT0
#define HCS_UTRLRDY   BIT1
#define HCS_UTMRLRDY  BIT2

STATIC
VOID
UfsLog (
  IN CONST CHAR8  *Format,
  ...
  );

// ShmBridge is supplied only by the native ShmBridgeDxeLA.

STATIC
VOID
UfsLog (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR8    Buffer[192];
  UINTN    Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  if (Length > 0) {
    SerialPortWrite ((UINT8 *)Buffer, MIN (Length, sizeof (Buffer) - 1));
  }
}

/**
  Driver entry point: log the controller state and change nothing.
**/
EFI_STATUS
EFIAPI
UfsPreDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Hcs;
  UINT32  Hce;
  UINT32  Is;

  SerialPortInitialize ();

  //
  // Three prints, not one, so the video shows exactly where it stops: entry reached,
  // install called, install returned. The earlier version only printed after
  // InstallMultipleProtocolInterfaces returned, which cannot distinguish "never got
  // here" from "the install call itself hung".
  //
  UfsLog ("[UFSPRE] entry reached\r\n");


  Hcs = MmioRead32 (UFS_MEM_BASE + UFSHCI_HCS);
  Hce = MmioRead32 (UFS_MEM_BASE + UFSHCI_HCE);
  Is  = MmioRead32 (UFS_MEM_BASE + UFSHCI_IS);

  UfsLog (
    "[UFSPRE] CAP=0x%08x VER=0x%08x HCS=0x%08x dp=%u utrl=%u utmrl=%u HCE=0x%08x IS=0x%08x\r\n",
    MmioRead32 (UFS_MEM_BASE + UFSHCI_CAP),
    MmioRead32 (UFS_MEM_BASE + UFSHCI_VER),
    Hcs,
    (Hcs & HCS_DP) ? 1 : 0,
    (Hcs & HCS_UTRLRDY) ? 1 : 0,
    (Hcs & HCS_UTMRLRDY) ? 1 : 0,
    Hce,
    Is
    );

  UfsLog ("[UFSPRE] observation only, no write performed\r\n");

  return EFI_SUCCESS;
}
