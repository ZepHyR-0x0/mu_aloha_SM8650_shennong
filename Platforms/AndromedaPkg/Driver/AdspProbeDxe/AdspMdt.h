// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded, read-only ELF32/Qualcomm MDT inspection. No authentication or loading.
#ifndef ADSP_MDT_H_
#define ADSP_MDT_H_

#include <Uefi.h>

#define ADSP_MDT_MAX_PHDRS  128
#define ADSP_MDT_MAX_SIZE   65536

typedef struct {
  UINT32  Index;
  UINT32  Address;
  UINT32  FileSize;
  UINT32  MemorySize;
} ADSP_MDT_SEGMENT;

typedef struct {
  UINT32            Machine;
  UINT32            Entry;
  UINT32            PhdrCount;
  UINT32            LoadCount;
  UINT32            BssCount;
  BOOLEAN           Split;
  BOOLEAN           Relocatable;
  UINT64            MinAddress;
  UINT64            EndAddress; // exclusive, rounded up to 4 KiB
  ADSP_MDT_SEGMENT  Segments[ADSP_MDT_MAX_PHDRS];
} ADSP_MDT_PLAN;

// NULL means structurally valid. Error strings are static; input is never changed.
CONST CHAR8 *
AdspMdtInspect (
  IN CONST UINT8    *Data,
  IN UINTN          Size,
  OUT ADSP_MDT_PLAN *Plan
  );

#endif
