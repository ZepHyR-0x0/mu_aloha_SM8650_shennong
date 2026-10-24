/** @file

  Minimal producer of the Qualcomm ShmBridge protocol (9C1EB71F-DD6C-4ED5-9F6A-
  5CC0CA789F16).

  Why this exists: UFSDxe's RPMB listener does LocateProtocol(9C1EB71F-...) and
  asserts at RpmbListener.c +160 when it fails, before it ever looks at RPMB. The
  real producer is ShmBridgeDxeLA, whose entry installs the protocol
  unconditionally but also issues three SMC calls; on this port those calls are
  refused and the protocol ends up not being available. The listener only needs
  the protocol's allocate entry point (offset +8) to obtain a working buffer, so
  a plain page allocation is enough to get past initialisation. No SMC, no RPMB
  access, nothing shared with the secure world.

  Layout and call convention were taken from ShmBridgeDxeLA's own interface
  table (version 0x00010002; allocate at +8, free at +0x10) and from the
  listener's call site (RpmbListener init passes a one-QWORD argument block
  whose first field is the size, 0x23020 for the RPMB listener).

  Copyright (c) 2024, Project Mu contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

STATIC CONST EFI_GUID  mShmBridgeProtocolGuid = {
  0x9C1EB71F, 0xDD6C, 0x4ED5, { 0x9F, 0x6A, 0x5C, 0xC0, 0xCA, 0x78, 0x9F, 0x16 }
};

//
// The two callers pass the argument block in x1, not x0: allocate(This, Args,
// Mode) reads Args[0] as the size, free(This, Buffer) takes the buffer in x1.
//
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

/**
  Allocate a page-aligned, zeroed buffer for the caller.

  @param[in]  This   Interface pointer (unused, kept for the call convention).
  @param[in]  Args   Argument block; Args[0] is the requested size in bytes.
  @param[in]  Mode   Allocation mode (unused).

  @return Buffer, or NULL when the request is invalid or cannot be satisfied.
**/
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
    return NULL;
  }

  Size = (UINTN)Args[0];
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
  {
    CHAR8  Line[80];
    UINTN  Length;

    Length = AsciiSPrint (Line, sizeof (Line), "[SHMSTUB] alloc 0x%lx -> 0x%lx\r\n", (UINT64)Size, (UINT64)Buffer);
    SerialPortWrite ((UINT8 *)Line, Length);
  }

  return (VOID *)(UINTN)Buffer;
}

/**
  Free entry point. The allocation size is not stored next to the pointer, and
  the RAM is held for the rest of boot, so this is deliberately a no-op.

  @param[in]  This    Interface pointer (unused).
  @param[in]  Buffer  Buffer returned by ShmBridgeAllocate (unused).
**/
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

/**
  Install the ShmBridge protocol.

  @param[in]  ImageHandle  This driver's image handle.
  @param[in]  SystemTable  EFI system table.

  @retval EFI_SUCCESS  Protocol installed.
  @return              Status from InstallMultipleProtocolInterfaces.
**/
EFI_STATUS
EFIAPI
ShmBridgeStubInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle;

  SerialPortInitialize ();
  SerialPortWrite ((UINT8 *)"[SHMSTUB] install\r\n", sizeof ("[SHMSTUB] install\r\n") - 1);

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                (EFI_GUID *)&mShmBridgeProtocolGuid,
                &mShmBridge,
                NULL
                );
}
