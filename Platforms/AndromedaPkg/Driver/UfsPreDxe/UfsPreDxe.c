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

//
// ShmBridge protocol, 9C1EB71F-DD6C-4ED5-9F6A-5CC0CA789F16. UFSDxe's RPMB listener
// does LocateProtocol on this and asserts at RpmbListener.c +160 when it fails, before
// it ever looks at RPMB. The real producer is ShmBridgeDxeLA, which issues three SMCs
// and does not get the protocol installed on this port.
//
// Publishing it from here rather than from a separate driver is deliberate: a separate
// ShmBridgeStubDxe sat in the FV at #35 and never dispatched, while this driver
// demonstrably runs. One module that runs beats two where one is unproven.
//
STATIC CONST EFI_GUID  mShmBridgeProtocolGuid = {
  0x9C1EB71F, 0xDD6C, 0x4ED5, { 0x9F, 0x6A, 0x5C, 0xC0, 0xCA, 0x78, 0x9F, 0x16 }
};

typedef
VOID *
(EFIAPI *SHM_BRIDGE_ALLOCATE) (
  IN VOID    *This,
  IN UINT64  *Args,
  IN UINTN    Mode
  );

typedef
VOID
(EFIAPI *SHM_BRIDGE_FREE) (
  IN VOID  *This,
  IN VOID  *Buffer
  );

typedef struct {
  UINT64                Version;
  SHM_BRIDGE_ALLOCATE   Allocate;
  SHM_BRIDGE_FREE       Free;
  UINT64                Reserved0;
  UINT64                Reserved1;
  UINT64                Signature0;
  UINT64                Signature1;
} SHM_BRIDGE_PROTOCOL;

STATIC
VOID *
EFIAPI
ShmBridgeAllocate (
  IN VOID    *This,
  IN UINT64  *Args,
  IN UINTN    Mode
  )
{
  EFI_STATUS             Status;
  EFI_PHYSICAL_ADDRESS   Buffer;
  UINTN                  Size;
  UINTN                  Pages;

  (VOID)This;
  (VOID)Mode;

  if (Args == NULL) {
    UfsLog ("[SHMSTUB] alloc: null args\r\n");
    return NULL;
  }

  Size = (UINTN)Args[0];
  UfsLog ("[SHMSTUB] alloc 0x%lx\r\n", (UINT64)Size);

  if ((Size == 0) || (Size > 16 * 1024 * 1024)) {
    return NULL;
  }

  Pages  = EFI_SIZE_TO_PAGES (Size);
  Buffer = 0;
  Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, Pages, &Buffer);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  ZeroMem ((VOID *)(UINTN)Buffer, Pages * EFI_PAGE_SIZE);
  UfsLog ("[SHMSTUB] alloc -> 0x%lx\r\n", (UINT64)Buffer);
  return (VOID *)(UINTN)Buffer;
}

STATIC
VOID
EFIAPI
ShmBridgeFree (
  IN VOID  *This,
  IN VOID  *Buffer
  )
{
  (VOID)This;
  (VOID)Buffer;
}

STATIC SHM_BRIDGE_PROTOCOL  mShmBridge = {
  0x0000000000010002ULL,
  ShmBridgeAllocate,
  ShmBridgeFree,
  0,
  0,
  0x49EF56D78EC2BD8DULL,
  0xF8EBF87817639687ULL
};

STATIC
VOID
InstallShmBridge (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  VOID        *Check;

  //
  // Stand down if a real ShmBridge is already published.
  //
  // The stub exists only because RpmbListener asserts on LocateProtocol of this
  // GUID, and the real driver used to fail its three SMCs and never install. Once
  // TrustZone answers, the real one installs and its Allocate registers memory
  // with TZ - which is what smcinvoke requires. A stub-installed buffer is not
  // registered, and TZ rejects the invoke with -6 (0xFFFFFFFA).
  //
  // Both publish the same GUID, so whoever gets there first is what every later
  // LocateProtocol returns. That is why this check has to happen here rather than
  // relying on ordering alone.
  //
  Check  = NULL;
  Status = gBS->LocateProtocol ((EFI_GUID *)&mShmBridgeProtocolGuid, NULL, &Check);
  if (!EFI_ERROR (Status) && (Check != NULL)) {
    UfsLog ("[SHMSTUB] real ShmBridge already present at 0x%lx, skipping\r\n",
            (UINT64)(UINTN)Check);
    return;
  }

  UfsLog ("[SHMSTUB] installing...\r\n");

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  (EFI_GUID *)&mShmBridgeProtocolGuid,
                  &mShmBridge,
                  NULL
                  );
  UfsLog ("[SHMSTUB] install %r handle 0x%lx\r\n", Status, (UINT64)(UINTN)Handle);

  //
  // Read it straight back. If this fails, the install did not take and no amount of
  // fixing downstream will help.
  //
  Check  = NULL;
  Status = gBS->LocateProtocol ((EFI_GUID *)&mShmBridgeProtocolGuid, NULL, &Check);
  UfsLog ("[SHMSTUB] self-check %r at 0x%lx\r\n", Status, (UINT64)(UINTN)Check);
}

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

  InstallShmBridge ();

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
