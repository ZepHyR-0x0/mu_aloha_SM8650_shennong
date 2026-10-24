#ifndef _SEC_PROTOCOL_FINDER_LIB_H_
#define _SEC_PROTOCOL_FINDER_LIB_H_

#include <Library/BaseLib.h>

VOID InitProtocolFinder(
    EFI_PHYSICAL_ADDRESS *ScheAddr,
    EFI_PHYSICAL_ADDRESS *XBLDTOpsAddr
);

/**
  Hand the trace text that InitProtocolFinder accumulated over to the caller.

  The trace also goes out through SerialPortWrite, but that lands in the 64 KiB
  "UEFI Log" ring and is overwritten long before DXE gets a chance to read it.
  PlatformPeiLib copies this buffer into gAndromedaSfDiagHobGuid so the result
  survives into DXE, where RamPartitionDxe dumps it.

  @param Buffer  Receives the text (NUL terminated).
  @param Length  Receives the number of bytes, header included.
**/
VOID SecFinderGetDiag(
    OUT UINT8 **Buffer,
    OUT UINTN  *Length
);

VOID StartUpScheduler(
  VOID *PeiContinueBoot,
  VOID *AuxCoresEntryPoint
);

VOID BootSecondaryCpu(
  UINTN CpuIdx
);

#endif // _SEC_PROTOCOL_FINDER_LIB_H_
