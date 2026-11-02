// SPDX-License-Identifier: BSD-2-Clause-Patent
// Retain valid ABL board identity when the UEFI ADC converter is unavailable.
// No ADC measurements, project IDs or charging values are synthesized.
#include <Uefi.h>
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PrintLib.h>
#include "ProjectInfoCacheCore.h"
#include "ProjectInfoRows.h"

typedef EFI_STATUS (EFIAPI *PROJECT_GET)(VOID *, VOID *);
typedef struct { UINT64 Revision; PROJECT_GET GetInfo, GetProject; } PROJECT_API;
typedef struct { UINT64 Revision; VOID *Methods[2];
  EFI_STATUS (EFIAPI *GetChipId)(VOID *, UINT32 *); } PROJECT_CHIP;
typedef struct { UINT64 Revision;
  EFI_STATUS (EFIAPI *GetAlloc)(UINT32, UINT32, VOID **); } PROJECT_SMEM;
STATIC EFI_GUID mProjectGuid = {0x64EBEA42,0x7F8A,0x11EA,{0xB9,0xF7,0x2B,0xBC,0x47,0x3A,0xE4,0x4B}};
STATIC EFI_GUID mSmemGuid = {0xF4E5C7D0,0xD239,0x47CB,{0xAA,0xCD,0x7F,0x66,0xEF,0x76,0x32,0x38}};
STATIC EFI_GUID mChipGuid = {0xB0760469,0x970C,0x487A,{0xA4,0xB5,0x28,0xDB,0x7B,0x45,0xCE,0xF1}};
STATIC PROJECT_API *mProject;
STATIC PROJECT_GET mOriginalInfo, mOriginalProject;
STATIC UINT8 mCache[PROJECT_RECORD_BYTES];
STATIC BOOLEAN mValid;
STATIC EFI_EVENT mNotify;
STATIC VOID *mRegistration;
STATIC UINTN mReports;
#ifndef PROJECT_SMEM_BASE
#define PROJECT_SMEM_BASE 0x81D00000U
#endif
#define PROJECT_SMEM_END ((UINTN)PROJECT_SMEM_BASE + 0x200000U)

STATIC VOID EFIAPI Say (CONST CHAR8 *Format, ...) {
  CHAR8 Buffer[192]; VA_LIST Marker; UINTN Length;
  VA_START (Marker, Format); Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker); SerialPortWrite ((UINT8 *)Buffer, Length);
}

STATIC BOOLEAN Readable (UINTN Address, UINTN Bytes, BOOLEAN Smem) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  if (Address == 0 || Bytes == 0 || EFI_ERROR (gDS->GetMemorySpaceDescriptor (Address, &D)) ||
      (D.Attributes & EFI_MEMORY_RP) != 0 || Address < D.BaseAddress ||
      Address - D.BaseAddress > D.Length || Bytes > D.Length - (Address - D.BaseAddress)) return FALSE;
  // PlatformMemoryMapLib's SMEM is reserved, uncached DDR; do not follow a
  // foreign GetAlloc pointer into MMIO or another reserved region.
  if (Smem) return Address >= PROJECT_SMEM_BASE && Address < PROJECT_SMEM_END && Bytes <= PROJECT_SMEM_END - Address;
  return D.GcdMemoryType == EfiGcdMemoryTypeSystemMemory;
}

STATIC BOOLEAN AcquireCache (VOID) {
  PROJECT_SMEM *Smem = NULL; PROJECT_CHIP *Chip = NULL; VOID *Raw = NULL;
  UINT32 ChipId = 0; EFI_STATUS Status;
  if (mValid) return TRUE;
  Status = gBS->LocateProtocol (&mSmemGuid, NULL, (VOID **)&Smem);
  if (EFI_ERROR (Status) || !Readable ((UINTN)Smem, sizeof (*Smem), FALSE) ||
      Smem->Revision != 0x10001 || !Readable ((UINTN)Smem->GetAlloc, 4, FALSE)) return FALSE;
  Status = gBS->LocateProtocol (&mChipGuid, NULL, (VOID **)&Chip);
  if (EFI_ERROR (Status) || !Readable ((UINTN)Chip, sizeof (*Chip), FALSE) ||
      !Readable ((UINTN)Chip->GetChipId, 4, FALSE)) return FALSE;
  Status = Chip->GetChipId (Chip, &ChipId);
  if (EFI_ERROR (Status) || ChipId == 0) return FALSE;
  // The stock API finds/allocates RAM, never UFS. A new zero block cannot pass
  // the exact OEM table check. Copy before calling ADC-backed native getters.
  Status = Smem->GetAlloc (135, PROJECT_RECORD_BYTES, &Raw);
  if (EFI_ERROR (Status) || !Readable ((UINTN)Raw, sizeof (mCache), TRUE)) return FALSE;
  CopyMem (mCache, Raw, sizeof (mCache));
  mValid = ProjectCacheMatches (mCache, ChipId, mProjectRows, 247);
  Say ("[PROJECT b323] retained SMEM135 chip=%u valid=%u (OEM row and ADC ranges checked)\r\n", ChipId, mValid);
  if (!mValid) ZeroMem (mCache, sizeof (mCache));
  return mValid;
}

STATIC EFI_STATUS EFIAPI GetInfo (VOID *This, VOID *Output) {
  EFI_STATUS Status;
  if (This != mProject || Output == NULL) return EFI_INVALID_PARAMETER;
  AcquireCache ();
  Status = mOriginalInfo (This, Output);
  if (EFI_ERROR (Status) && mValid) {
    CopyMem (Output, mCache, sizeof (mCache));
    if (mReports++ < 4) Say ("[PROJECT b323] native GetInfo %r -> verified ABL cache\r\n", Status);
    return EFI_SUCCESS;
  }
  return Status;
}
STATIC EFI_STATUS EFIAPI GetProject (VOID *This, VOID *Output) {
  EFI_STATUS Status;
  if (This != mProject || Output == NULL) return EFI_INVALID_PARAMETER;
  AcquireCache ();
  Status = mOriginalProject (This, Output);
  if (EFI_ERROR (Status) && mValid) {
    CopyMem (Output, mCache + 0x10, sizeof (UINT32));
    if (mReports++ < 4) Say ("[PROJECT b323] native GetProject %r -> verified ABL cache\r\n", Status);
    return EFI_SUCCESS;
  }
  return Status;
}
STATIC VOID EFIAPI Notify (EFI_EVENT Event, VOID *Context) {
  PROJECT_API *Api = NULL; UINTN Base; EFI_STATUS Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Space;
  (VOID)Event; (VOID)Context;
  if (mProject != NULL) return;
  Status = gBS->LocateProtocol (&mProjectGuid, NULL, (VOID **)&Api);
  if (EFI_ERROR (Status) || !Readable ((UINTN)Api, sizeof (*Api), FALSE) ||
      Api->Revision != 0x30000 || (UINTN)Api < 0xB0C8 ||
      EFI_ERROR (gDS->GetMemorySpaceDescriptor ((UINTN)Api, &Space)) ||
      (Space.Attributes & EFI_MEMORY_RO) != 0) return;
  Base = (UINTN)Api - 0xB0C8;
  // Identity and method ABI of the extracted ProjectInfoDxe, not an arbitrary
  // protocol of the same revision. Instructions remain untouched.
  if ((UINTN)Api->GetInfo != Base + 0x13EC || (UINTN)Api->GetProject != Base + 0x17C0 ||
      !Readable (Base + 0x14A0, 0x34, FALSE) ||
      ReadUnaligned32 ((UINT32 *)(Base + 0x14B4)) != 0x9400014F ||
      ReadUnaligned32 ((UINT32 *)(Base + 0x14D0)) != 0x94000148 ||
      !Readable (Base + 0x4578, sizeof (mProjectRows), FALSE) ||
      CompareMem ((VOID *)(Base + 0x4578), mProjectRows, sizeof (mProjectRows)) != 0) return;
  mProject = Api; mOriginalInfo = Api->GetInfo; mOriginalProject = Api->GetProject;
  AcquireCache ();
  Api->GetInfo = GetInfo; Api->GetProject = GetProject;
  Say ("[PROJECT b323] native query wrappers installed; cache=%u; no private init\r\n", mValid);
}
EFI_STATUS EFIAPI ProjectInfoCacheEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *System) {
  EFI_STATUS Status; (VOID)Image; (VOID)System;
  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Notify, NULL, &mNotify);
  if (EFI_ERROR (Status)) return Status;
  Status = gBS->RegisterProtocolNotify (&mProjectGuid, mNotify, &mRegistration);
  if (EFI_ERROR (Status)) { gBS->CloseEvent (mNotify); return Status; }
  Notify (NULL, NULL);
  return EFI_SUCCESS;
}
