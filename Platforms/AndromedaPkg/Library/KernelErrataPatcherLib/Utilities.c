/** @file

  Patches NTOSKRNL to not cause a SError when reading/writing ACTLR_EL1
  Patches NTOSKRNL to not cause a SError when reading/writing AMCNTENSET0_EL0
  Patches NTOSKRNL to not cause a bugcheck when attempting to use
  PSCI_MEMPROTECT Due to an issue in QHEE

  Based on https://github.com/SamuelTulach/rainbow

  Copyright (c) 2021 Samuel Tulach
  Copyright (c) 2022-2023 DuoWoA authors

  SPDX-License-Identifier: MIT

**/
#include "KernelErrataPatcherLib.h"
#include "WinloadImageCore.h"

#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
STATIC BOOLEAN WiRamRange(EFI_PHYSICAL_ADDRESS Base,UINTN Bytes)
{
  if(Base>MAX_UINT64-Bytes)return FALSE;
  EFI_PHYSICAL_ADDRESS End=Base+Bytes,At=Base;
  while(At<End){
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR M;
    if(EFI_ERROR(gDS->GetMemorySpaceDescriptor(At,&M)) || M.GcdMemoryType!=EfiGcdMemoryTypeSystemMemory ||
       M.BaseAddress>At || M.BaseAddress>MAX_UINT64-M.Length || M.BaseAddress+M.Length<=At)return FALSE;
    At=MIN(End,M.BaseAddress+M.Length);
  }
  return TRUE;
}

EFI_PHYSICAL_ADDRESS LocateWinloadBase(EFI_PHYSICAL_ADDRESS base, UINTN *size)
{
  EFI_PHYSICAL_ADDRESS Pc=base;
  if(size==NULL)return 0;*size=0;base&=~(EFI_PHYSICAL_ADDRESS)(EFI_PAGE_SIZE-1);
  // No allocations or memory-map acquisition in the EBS interceptor. Never
  // scan outside a GCD RAM descriptor or wrap through physical address zero.
  for(UINTN Pages=0;Pages<8192;Pages++){
    UINT32 ImageBytes;
    if(!WiRamRange(base,EFI_PAGE_SIZE))break;
    if(WiInspect((CONST UINT8 *)(UINTN)base,EFI_PAGE_SIZE,base,Pc,&ImageBytes) &&
       WiRamRange(base,ImageBytes)){*size=ImageBytes;return base;}
    if(base<EFI_PAGE_SIZE)break;
    base-=EFI_PAGE_SIZE;
  }
  FirmwarePrint("[WINLOAD] bounded ARM64 image lookup failed pc=%lx\n",Pc);
  return 0;
}

VOID CopyMemory(
    EFI_PHYSICAL_ADDRESS destination, EFI_PHYSICAL_ADDRESS source, UINTN size)
{
  UINT8 *dst = (UINT8 *)(destination);
  UINT8 *src = (UINT8 *)(source);
  for (UINTN i = 0; i < size; i++) {
    dst[i] = src[i];
  }
}

EFI_PHYSICAL_ADDRESS
FindPattern(EFI_PHYSICAL_ADDRESS baseAddress, UINTN size, const CHAR8 *pattern)
{
  EFI_PHYSICAL_ADDRESS firstMatch     = 0;
  const CHAR8         *currentPattern = pattern;

  for (EFI_PHYSICAL_ADDRESS current = baseAddress; current < baseAddress + size;
       current++) {
    UINT8 byte = currentPattern[0];
    if (!byte)
      return firstMatch;
    if (byte == '\?' ||
        *(UINT8 *)(current) == GET_BYTE(byte, currentPattern[1])) {
      if (!firstMatch)
        firstMatch = current;
      if (!currentPattern[2])
        return firstMatch;
      ((byte == '\?') ? (currentPattern += 2) : (currentPattern += 3));
    }
    else {
      currentPattern = pattern;
      firstMatch     = 0;
    }
  }

  return 0;
}
