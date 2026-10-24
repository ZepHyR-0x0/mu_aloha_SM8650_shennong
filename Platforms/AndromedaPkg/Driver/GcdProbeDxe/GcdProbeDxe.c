/** @file
  GcdProbeDxe - ask the GCD what it actually contains.

  Everything that fails on this port fails at the same line:

    Failed to map PhysicalAddr:0xB000000, size:0x4000000, SetMemorySpaceAttributes
    DALLOC Device [0x2000070]: HWIO mapping failed

  SetMemorySpaceAttributes only fails when the range is not described in the GCD,
  so the question is not what our config declares but what actually landed there.
  Our PlatformMemoryMapLib entries are byte-for-byte the same as the working
  SM8550 port's (AOSS and TLMM both AddDev/MMAP_IO/UNCACHEABLE/MmIO/NS_DEVICE),
  and the HWIO table in DALSYSDxe already carries the right values
  (UFS_MEM_UFS = 0x1D84000 / 0x3000), so the declaration side is not the problem.

  This driver walks the GCD and reports what is registered for the addresses that
  matter. It uses only gDS->GetMemorySpaceMap, so it performs no MMIO access at
  all - unlike the TLMM read that reset the machine.

  Copyright (c) 2026, ProjectMu shennong port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>

typedef struct {
  CONST CHAR8  *Name;
  UINT64       Address;
} WATCHED;

STATIC WATCHED  mWatched[] = {
  { "AOSS   ", 0x0B000000ULL },
  { "TLMM   ", 0x0F000000ULL },
  { "UFS_MEM", 0x1D84000ULL  },
  { "UFS_ICE", 0x1D88000ULL  },
  { "CRYPTO ", 0x1DC0000ULL  },
  { "DWC3   ", 0x0A600000ULL },   // known good: FrontPage reads this fine
  { NULL,      0ULL          }
};

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
CONST CHAR8 *
GcdTypeName (
  IN EFI_GCD_MEMORY_TYPE  Type
  )
{
  switch (Type) {
    case EfiGcdMemoryTypeNonExistent:  return "nonexistent";
    case EfiGcdMemoryTypeReserved:     return "reserved   ";
    case EfiGcdMemoryTypeSystemMemory: return "sysmem     ";
    case EfiGcdMemoryTypeMemoryMappedIo: return "mmio       ";
    case EfiGcdMemoryTypePersistentMemory: return "persistent ";
    case EfiGcdMemoryTypeMoreReliable: return "morereliable";
    default:                           return "?          ";
  }
}

EFI_STATUS
EFIAPI
GcdProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                  Status;
  UINTN                       Count;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  *Map;
  UINTN                       Index;
  UINTN                       W;
  CHAR8                       Buf[192];
  UINTN                       Len;

  Say ("[GCD] probe start\n");

  Count = 0;
  Map   = NULL;
  Status = gDS->GetMemorySpaceMap (&Count, &Map);
  if (EFI_ERROR (Status) || (Map == NULL)) {
    Say ("[GCD] GetMemorySpaceMap failed\n");
    return EFI_SUCCESS;
  }

  Len = AsciiSPrint (Buf, sizeof (Buf), "[GCD] %d descriptors\n", Count);
  SerialPortWrite ((UINT8 *)Buf, Len);

  for (W = 0; mWatched[W].Name != NULL; W++) {
    BOOLEAN Found = FALSE;

    for (Index = 0; Index < Count; Index++) {
      if ((mWatched[W].Address >= Map[Index].BaseAddress) &&
          (mWatched[W].Address < Map[Index].BaseAddress + Map[Index].Length))
      {
        Len = AsciiSPrint (
                Buf, sizeof (Buf),
                "[GCD] %a @0x%lx in [0x%lx+0x%lx] type=%a attr=%lx cap=%lx\n",
                mWatched[W].Name,
                mWatched[W].Address,
                Map[Index].BaseAddress,
                Map[Index].Length,
                GcdTypeName (Map[Index].GcdMemoryType),
                Map[Index].Attributes,
                Map[Index].Capabilities
                );
        SerialPortWrite ((UINT8 *)Buf, Len);
        Found = TRUE;
        break;
      }
    }

    if (!Found) {
      Len = AsciiSPrint (
              Buf, sizeof (Buf),
              "[GCD] %a @0x%lx NOT DESCRIBED\n",
              mWatched[W].Name,
              mWatched[W].Address
              );
      SerialPortWrite ((UINT8 *)Buf, Len);
    }
  }

  gBS->FreePool (Map);

  //
  // Deliberately NO SetMemorySpaceAttributes here.
  //
  // An earlier version of this probe tried a matrix of attribute combinations to
  // find which one reproduces the failure. It reported Success for UC|RO - which
  // leaves the region read-only - and every later write into it took a data
  // abort. DALSYSDxe then died in DalmodEnv.c+115, and the crash looked like an
  // ordering problem for three rounds of investigation.
  //
  // A probe that changes the state it is measuring is not a probe. This one only
  // reads.
  //
  Say ("[GCD] probe done\n");
  return EFI_SUCCESS;
}
