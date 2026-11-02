/** @file
  Boot log ring reader.

  Layout of the ring (written by FrameBufferInMemorySerialPortLib), see
  BootLogRing.h:
    <RingSize bytes of text><BOOT_LOG_RING_HEADER>
  The header's Offset field is the next write position, which equals the
  oldest byte once the ring has wrapped; Written counts every byte ever
  stored, so the logical length is known even before the first wrap.

  Copyright (c) 2026, Project Mu port. SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BootLogRing.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "../Driver/ReadOnlyStorageDxe/ReadOnlyStorage.h"

#include "BootLog.h"

STATIC EFI_GUID mLogPolicyGuid = READ_ONLY_STORAGE_GUID;
STATIC EFI_STATUS LogPolicy (READ_ONLY_STORAGE_POLICY **Policy)
{
  EFI_STATUS Status = gBS->LocateProtocol (&mLogPolicyGuid, NULL, (VOID **)Policy);
  if (EFI_ERROR (Status)) return Status;
  return *Policy != NULL && (*Policy)->Revision >= 2 && (*Policy)->SaveLog != NULL &&
    (*Policy)->ReadLog != NULL ? EFI_SUCCESS : EFI_UNSUPPORTED;
}
EFI_STATUS BootLogSave (CONST CHAR8 *Phase)
{
  READ_ONLY_STORAGE_POLICY *Policy;
  CHAR8 *Buffer;
  UINTN Length;
  EFI_STATUS Status = LogPolicy (&Policy);
  if (EFI_ERROR (Status)) return Status;
  Buffer = AllocatePool (BOOT_LOG_MAX_BYTES);
  if (Buffer == NULL) return EFI_OUT_OF_RESOURCES;
  Status = BootLogRead (Buffer, BOOT_LOG_MAX_BYTES, &Length);
  if (!EFI_ERROR (Status)) Status = Length == 0 ? EFI_NOT_FOUND : Policy->SaveLog (Buffer, Length, Phase);
  FreePool (Buffer);
  return Status;
}
EFI_STATUS BootLogReadStored (CHAR8 *Buffer, UINTN *Length, BOOLEAN Previous)
{
  READ_ONLY_STORAGE_POLICY *Policy;
  EFI_STATUS Status = LogPolicy (&Policy);
  return EFI_ERROR (Status) ? Status : Policy->ReadLog (Buffer, Length, Previous);
}

STATIC
EFI_STATUS
LocateRingRegion (
  OUT ARM_MEMORY_REGION_DESCRIPTOR_EX  *Desc
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  CHAR8       *Names[] = { "PStore", "UEFI Log" };

  for (Index = 0; Index < ARRAY_SIZE (Names); Index++) {
    Status = LocateMemoryMapAreaByName (Names[Index], Desc);
    if (!EFI_ERROR (Status) && (Desc->Address != 0) &&
        (Desc->Length > sizeof (BOOT_LOG_RING_HEADER)))
    {
      return EFI_SUCCESS;
    }
  }

  //
  // Same fallback as the writer in FrameBufferInMemorySerialPortLib. Using the
  // known address keeps reader and writer together even when the lookup is not
  // available; the Magic check below still rejects a region nobody owns.
  //
  ZeroMem (Desc, sizeof (*Desc));
  Desc->Address = BOOT_LOG_RING_BASE;
  Desc->Length  = BOOT_LOG_RING_SIZE;
  return EFI_SUCCESS;
}

EFI_STATUS
BootLogRead (
  OUT CHAR8  *Buffer,
  IN  UINTN  BufferSize,
  OUT UINTN  *LogLength
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR_EX  Desc;
  BOOT_LOG_RING_HEADER             *Header;
  UINT8                            *Ring;
  UINTN                            RingSize;
  UINTN                            Offset;
  UINTN                            Length;
  UINTN                            Start;
  UINTN                            Index;
  UINTN                            Out;
  UINT8                            Char;
  EFI_STATUS                       Status;

  if ((Buffer == NULL) || (LogLength == NULL) || (BufferSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *LogLength = 0;

  Status = LocateRingRegion (&Desc);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Desc.Length <= sizeof (BOOT_LOG_RING_HEADER)) {
    return EFI_VOLUME_CORRUPTED;
  }

  Ring     = (UINT8 *)(UINTN)Desc.Address;
  RingSize = Desc.Length - sizeof (BOOT_LOG_RING_HEADER);
  Header   = (BOOT_LOG_RING_HEADER *)(Ring + RingSize);

  InvalidateDataCacheRange (Ring, (UINTN)Desc.Length);

  //
  // No magic means the serial library has not written a byte yet this boot
  // (the region is zeroed in PrePi). Report "no log" instead of treating
  // whatever else lives there as text.
  //
  if (Header->Magic != BOOT_LOG_RING_MAGIC) {
    return EFI_NOT_FOUND;
  }

  Offset = (UINTN)(Header->Offset % RingSize);
  Length = (UINTN)MIN (Header->Written, RingSize);
  if (Length == 0) {
    return EFI_SUCCESS;
  }

  Length = MIN (Length, BufferSize);
  Start  = (Offset + RingSize - Length) % RingSize;

  for (Index = 0; Index < Length; Index++) {
    Buffer[Index] = (CHAR8)Ring[(Start + Index) % RingSize];
  }

  //
  // Never let the unwritten tail reach the QR: a NUL byte terminates the
  // payload inside the encoder (AsciiStrLen), so the rest of the page vanishes.
  //
  while ((Length > 0) && (Buffer[Length - 1] == '\0')) {
    Length--;
  }

  //
  // Byte mode can carry anything, but scanners display control bytes as
  // nothing (and stop at NUL). Keep printable ASCII plus the line breaks the
  // console emits, drop CR so "\r\n" does not become an empty line, and show
  // the rest as '.'.
  //
  for (Index = 0, Out = 0; Index < Length; Index++) {
    Char = (UINT8)Buffer[Index];
    if (Char == '\r') {
      continue;
    }

    if (((Char >= 0x20) && (Char <= 0x7E)) || (Char == '\n') || (Char == '\t')) {
      Buffer[Out++] = (CHAR8)Char;
    } else {
      Buffer[Out++] = '.';
    }
  }

  *LogLength = Out;
  return EFI_SUCCESS;
}
