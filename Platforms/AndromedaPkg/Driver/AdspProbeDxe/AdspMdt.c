// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AdspMdt.h"

STATIC UINT32
AdspLe32 (IN CONST UINT8 *Bytes)
{
  return (UINT32)Bytes[0] | ((UINT32)Bytes[1] << 8) |
         ((UINT32)Bytes[2] << 16) | ((UINT32)Bytes[3] << 24);
}

STATIC UINT32
AdspLe16 (IN CONST UINT8 *Bytes)
{
  return (UINT32)Bytes[0] | ((UINT32)Bytes[1] << 8);
}

CONST CHAR8 *
AdspMdtInspect (
  IN CONST UINT8    *Data,
  IN UINTN          Size,
  OUT ADSP_MDT_PLAN *Plan
  )
{
  UINT32        PhOff;
  UINT32        Index;
  UINT32        Flags;
  UINT32        FileSize;
  UINT32        MemorySize;
  UINT32        Address;
  UINT64        End;
  CONST UINT8   *Ph;
  ADSP_MDT_SEGMENT *Segment;

  if ((Data == NULL) || (Plan == NULL) || (Size < 52) || (Size > ADSP_MDT_MAX_SIZE)) {
    return "header size";
  }

  if ((Data[0] != 0x7F) || (Data[1] != 'E') || (Data[2] != 'L') ||
      (Data[3] != 'F') || (Data[4] != 1) || (Data[5] != 1) ||
      (Data[6] != 1) || (AdspLe32 (Data + 20) != 1)) {
    return "ELF32 little-endian version";
  }

  if ((AdspLe16 (Data + 40) != 52) || (AdspLe16 (Data + 42) != 32)) {
    return "ELF/PHDR entry size";
  }

  Plan->PhdrCount = AdspLe16 (Data + 44);
  PhOff = AdspLe32 (Data + 28);
  if ((Plan->PhdrCount == 0) || (Plan->PhdrCount > ADSP_MDT_MAX_PHDRS) ||
      (PhOff < 52) || (PhOff > Size) ||
      (Plan->PhdrCount > (Size - PhOff) / 32)) {
    return "PHDR table bounds";
  }

  // e_machine is reported, not assumed: the stock ADSP DTB uses machine 0x01.
  Plan->Machine     = AdspLe16 (Data + 18);
  Plan->Entry       = AdspLe32 (Data + 24);
  Plan->LoadCount   = 0;
  Plan->BssCount    = 0;
  Plan->Split       = FALSE;
  Plan->Relocatable = FALSE;
  Plan->MinAddress  = 0x100000000ULL;
  Plan->EndAddress  = 0;
  for (Index = 0; Index < Plan->PhdrCount; Index++) {
    Ph         = Data + PhOff + Index * 32;
    FileSize   = AdspLe32 (Ph + 16);
    MemorySize = AdspLe32 (Ph + 20);
    Flags      = AdspLe32 (Ph + 24);
    // Qualcomm split detection considers all headers, including metadata/hash.
    if ((UINT64)AdspLe32 (Ph + 4) + FileSize > Size) {
      Plan->Split = TRUE;
    }

    // Match the stock loader's load predicate. bNN is the PHDR index, so gaps
    // for metadata, hash and zero-memory entries must not be treated as missing.
    if ((AdspLe32 (Ph) != 1) || ((Flags & (7U << 24)) == (2U << 24)) ||
        (MemorySize == 0)) {
      continue;
    }

    Address = AdspLe32 (Ph + 12);
    End = (UINT64)Address + MemorySize;
    if ((FileSize > MemorySize) || (End > 0x100000000ULL) ||
        ((UINT64)AdspLe32 (Ph + 4) + FileSize > 0x100000000ULL)) {
      return "load segment bounds";
    }

    Segment = &Plan->Segments[Plan->LoadCount++];
    Segment->Index      = Index;
    Segment->Address    = Address;
    Segment->FileSize   = FileSize;
    Segment->MemorySize = MemorySize;
    if (FileSize == 0) {
      Plan->BssCount++;
    }

    if ((Flags & (1U << 27)) != 0) {
      Plan->Relocatable = TRUE;
    }

    if (Address < Plan->MinAddress) {
      Plan->MinAddress = Address;
    }

    End = (End + 4095) & ~4095ULL;
    if (End > Plan->EndAddress) {
      Plan->EndAddress = End;
    }
  }

  return (Plan->LoadCount == 0) ? "no load segments" : NULL;
}
