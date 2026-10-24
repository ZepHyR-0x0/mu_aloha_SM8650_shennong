/** @file
  Read-only diagnostics for the storage and I2C paths.

  There is no keyboard and no touch on this device, so a UEFI Shell cannot be used to
  poke around - everything here has to be reachable from the FrontPage menu and its
  output has to land in the log ring so LOGAUTO can carry it off the device.

  Nothing here writes to hardware. It reads three things:

    1. The GCD descriptor for the UFS register windows, to confirm what the firmware
       believes those ranges are.
    2. The I2C protocol handle counts, because UsbConfigDxe reports
       "usb_shared_i2c_write, not open" - so either no master exists, or one exists and
       the switch's device handle was never opened.
    3. A snapshot of the UFS host controller registers, which is the only way to see
       whether the link ever came up: UFSDxe prints nothing at all on this port.

  Copyright (c) 2024, Project Mu contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include <Protocol/BlockIo.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/HobLib.h>

#include "FrontPage.h"

//
// gAndromedaSfDiagHobGuid, declared locally for the same reason as the protocol
// GUIDs below: keeps the INF free of Guids entries that only this file needs.
//
STATIC CONST EFI_GUID  mSfDiagHobGuid = {
  0x7d1f4a52, 0x3c68, 0x4b19, { 0x8e, 0x02, 0x51, 0xa7, 0x6d, 0x93, 0x2c, 0xf4 }
};

//
// gEfiSecDtbGuid - PrePi puts the DTBExtnProtocol pointer XBL handed it here.
//
STATIC CONST EFI_GUID  mSecDtbHobGuid = {
  0x3a06e1e8, 0x61f6, 0x11eb, { 0xbb, 0xed, 0x4b, 0x47, 0x6e, 0x2f, 0xf6, 0xa7 }
};

//
// From MdePkg.dec. Declared locally so the INF does not need protocol entries.
//
STATIC CONST EFI_GUID  mI2cMasterProtocolGuid = {
  0xcd72881f, 0x45b5, 0x4feb, { 0x98, 0xc8, 0x31, 0x3d, 0xa8, 0x11, 0x74, 0x62 }
};

STATIC CONST EFI_GUID  mI2cIoProtocolGuid = {
  0xb60a3e6b, 0x18c4, 0x46e5, { 0xa2, 0x9a, 0xc9, 0xa1, 0x06, 0x65, 0xa2, 0x8e }
};

//
// Whether UFSDxe produced anything usable. The controller registers show the device
// present and the controller enabled, so the open question is whether a block device
// was published - a zero here means UFSDxe never got that far, a non-zero one means
// the problem is partitions rather than the link.
//
STATIC CONST EFI_GUID  mBlockIoGuid = {
  0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

STATIC CONST EFI_GUID  mDiskIoGuid = {
  0xce345171, 0xba0b, 0x11d2, { 0x8e, 0x4f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

STATIC CONST EFI_GUID  mSimpleFsGuid = {
  0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

//
// Register windows, taken from this platform's own memory map. UFS_MEM and UFS_ICE
// sit in one 0x80000 window that the GCD merges with CRYPTO.
//
#define UFS_MEM_BASE  0x1D84000ULL
#define UFS_MEM_LEN   0x00030000ULL
#define UFS_ICE_BASE  0x1D88000ULL
#define UFS_ICE_LEN   0x00001000ULL

//
// UFSHCI registers worth naming. The dump prints the whole first 0x100 anyway, but
// these are the ones that answer "did the link come up".
//
#define UFSHCI_CAP    0x00   // Controller capabilities
#define UFSHCI_VER    0x08   // Version
#define UFSHCI_IS     0x20   // Interrupt status
#define UFSHCI_HCS    0x30   // Host controller status
#define UFSHCI_HCE    0x34   // Host controller enable

STATIC
VOID
FpDiagPrint (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR8    Buffer[256];
  UINTN    Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  if (Length > 0) {
    SerialPortWrite ((UINT8 *)Buffer, MIN (Length, sizeof (Buffer) - 1));
  }
}

STATIC
VOID
FpDiagPrintGcd (
  IN CONST CHAR8       *Name,
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Length
  )
{
  EFI_STATUS             Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;

  ZeroMem (&Desc, sizeof (Desc));
  Status = gDS->GetMemorySpaceDescriptor (Base, &Desc);
  if (EFI_ERROR (Status)) {
    FpDiagPrint ("[DIAG] gcd %a 0x%llx+0x%llx -> %r\r\n", Name, Base, Length, Status);
    return;
  }

  FpDiagPrint (
    "[DIAG] gcd %a 0x%llx+0x%llx type=0x%x attr=0x%llx\r\n",
    Name,
    Desc.BaseAddress,
    Desc.Length,
    Desc.GcdMemoryType,
    Desc.Attributes
    );
}

STATIC
VOID
FpDiagPrintI2c (
  IN CONST CHAR8     *Name,
  IN CONST EFI_GUID  *Protocol
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, (EFI_GUID *)Protocol, NULL, &Count, &Handles);

  //
  // Not found is the interesting answer, not an error to report as a failure: it means
  // nothing published the protocol at all.
  //
  FpDiagPrint ("[DIAG] i2c %a handles=%u status=%r\r\n", Name, Count, Status);

  if (Handles != NULL) {
    FreePool (Handles);
  }
}


//
// Walk every BlockIo handle: device path type, media geometry, and whether LBA 1 holds
// an "EFI PART" header. The stock partition tables in the engineering package put the
// GPT header at byte offset 0x1000, which is LBA 1 only if the device reports a 4096
// byte block - so the block size the media reports is the thing to look at first.
//
STATIC
VOID
FpDiagDumpBlockIo (
  VOID
  )
{
  EFI_STATUS              Status;
  EFI_HANDLE              *Handles;
  UINTN                   Count;
  UINTN                   Index;
  EFI_BLOCK_IO_PROTOCOL   *BlockDev;
  UINT8                   *Buf;
  UINTN                   Shown;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    FpDiagPrint ("[DIAG] blockio enumerate %r\r\n", Status);
    return;
  }

  Buf   = AllocatePool (4096);
  Shown = 0;

  for (Index = 0; (Index < Count) && (Shown < 8); Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockDev))) {
      continue;
    }

    if ((BlockDev->Media == NULL) || !BlockDev->Media->MediaPresent || BlockDev->Media->LogicalPartition) {
      continue;
    }

    FpDiagPrint (
      "[DIAG] bio%u blockSize=%u lastLba=0x%lx ro=%u\r\n",
      (UINT32)Shown,
      BlockDev->Media->BlockSize,
      BlockDev->Media->LastBlock,
      BlockDev->Media->ReadOnly ? 1 : 0
      );

    if (Buf == NULL) {
      continue;
    }

    //
    // LBA 1 with the media's own block size. On a 4096-byte device this is the GPT
    // header; on a 512-byte device it is not, which is exactly what we want to see.
    //
    Status = BlockDev->ReadBlocks (BlockDev, BlockDev->Media->MediaId, 1, BlockDev->Media->BlockSize, Buf);
    if (EFI_ERROR (Status)) {
      FpDiagPrint ("[DIAG] bio%u read lba1 %r\r\n", (UINT32)Shown, Status);
    } else {
      FpDiagPrint (
        "[DIAG] bio%u lba1: %02x %02x %02x %02x %02x %02x %02x %02x  sig=%a\r\n",
        (UINT32)Shown,
        Buf[0], Buf[1], Buf[2], Buf[3], Buf[4], Buf[5], Buf[6], Buf[7],
        (Buf[0] == 'E' && Buf[1] == 'F' && Buf[2] == 'I' && Buf[3] == ' ') ? "EFI PART" : "-"
        );
    }

    Shown++;
  }

  if (Buf != NULL) {
    FreePool (Buf);
  }

  FreePool (Handles);
  FpDiagPrint ("[DIAG] blockio total=%u\r\n", (UINT32)Count);
}


//
// Find the ELF images among the partitions.
//
// TzDxeLA.efi loads cmnlib32 and cmnlib64 first ("Early Storage"), then Keymaster, and
// our boot dies in ValidateElfHeader with "the Early Storage is not valid!" - so the
// cmnlib load is what fails. Android partitions are ext4/f2fs/raw, so anything whose
// first block starts with 0x7f 'E' 'L' 'F' is a firmware image worth looking at.
//
// Read-only: one block per partition.
//
STATIC
VOID
FpDiagScanPartitions (
  VOID
  )
{
  EFI_STATUS              Status;
  EFI_HANDLE              *Handles;
  UINTN                   Count;
  UINTN                   Index;
  EFI_BLOCK_IO_PROTOCOL   *Part;
  UINT8                   *Buf;
  UINTN                   Printed;
  UINT64                  PhOff;
  UINT64                  PhEnd;
  UINT64                  MediaSize;
  UINT16                  PhEnt;
  UINT16                  PhNum;
  UINT32                  Class;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  Buf     = AllocatePool (4096);
  Printed = 0;

  for (Index = 0; (Index < Count) && (Printed < 12); Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Part))) {
      continue;
    }

    if ((Part->Media == NULL) || !Part->Media->MediaPresent || (Buf == NULL)) {
      continue;
    }

    if (EFI_ERROR (Part->ReadBlocks (Part, Part->Media->MediaId, 0, Part->Media->BlockSize, Buf))) {
      continue;
    }

    if ((Buf[0] != 0x7F) || (Buf[1] != 'E') || (Buf[2] != 'L') || (Buf[3] != 'F')) {
      continue;
    }

    //
    // Branch on e_ident[EI_CLASS]: real Qualcomm images are a mix. abl.elf and
    // aop.mbn are ELF32 (e_phoff at +0x1C, = 0x34), devcfg.mbn is ELF64
    // (e_phoff at +0x20, = 0x40). Reading the ELF64 offsets on an ELF32 file
    // produces nonsense such as phoff=0x500000000.
    //
    // Section headers are stripped in every shipping image (e_shoff = 0,
    // e_shnum = 0), so report the program header table - that is what a loader
    // maps, and its end offset is what "Bad Buffer Size" is about.
    //
    if (Buf[4] == 2) {
      PhOff = *(UINT64 *)(Buf + 0x20);
      PhEnt = *(UINT16 *)(Buf + 0x36);
      PhNum = *(UINT16 *)(Buf + 0x38);
      Class = 64;
    } else {
      PhOff = (UINT64)*(UINT32 *)(Buf + 0x1C);
      PhEnt = *(UINT16 *)(Buf + 0x2A);
      PhNum = *(UINT16 *)(Buf + 0x2C);
      Class = 32;
    }

    MediaSize = (Part->Media->LastBlock + 1) * Part->Media->BlockSize;
    PhEnd     = PhOff + ((UINT64)PhNum * PhEnt);

    FpDiagPrint (
      "[DIAG] elf#%u class=%u bytes=%lu phoff=0x%lx phnum=%u phend=0x%lx %a\r\n",
      (UINT32)Printed,
      (UINT32)Class,
      MediaSize,
      PhOff,
      (UINT32)PhNum,
      PhEnd,
      (PhEnd > MediaSize) ? "OVERRUN" : "ok"
      );

    Printed++;
  }

  FpDiagPrint ("[DIAG] elf partitions found=%u\r\n", (UINT32)Printed);

  if (Buf != NULL) {
    FreePool (Buf);
  }

  FreePool (Handles);
}

STATIC
VOID
FpDiagDumpUfs (
  VOID
  )
{
  UINT32  Offset;
  UINT32  Value;

  FpDiagPrint ("[DIAG] ufs registers @0x%llx\r\n", UFS_MEM_BASE);

  for (Offset = 0; Offset < 0x100; Offset += 0x10) {
    FpDiagPrint (
      "[DIAG]   %03x: %08x %08x %08x %08x\r\n",
      Offset,
      MmioRead32 (UFS_MEM_BASE + Offset + 0x0),
      MmioRead32 (UFS_MEM_BASE + Offset + 0x4),
      MmioRead32 (UFS_MEM_BASE + Offset + 0x8),
      MmioRead32 (UFS_MEM_BASE + Offset + 0xC)
      );
  }

  Value = MmioRead32 (UFS_MEM_BASE + UFSHCI_HCS);

  //
  // HCS bit0 is Device Present. Reading bit16 here was wrong and made a healthy
  // controller look like it had no device attached.
  //
  FpDiagPrint (
    "[DIAG] ufs HCS=0x%08x dp=%u utrlrdy=%u utmrlrdy=%u\r\n",
    Value,
    (Value >> 0) & 0x1,
    (Value >> 1) & 0x1,
    (Value >> 2) & 0x1
    );

  Value = MmioRead32 (UFS_MEM_BASE + UFSHCI_HCE);
  FpDiagPrint ("[DIAG] ufs HCE=0x%08x\r\n", Value);

  Value = MmioRead32 (UFS_MEM_BASE + UFSHCI_IS);
  FpDiagPrint ("[DIAG] ufs IS =0x%08x\r\n", Value);
}

/**
  Re-print the SecProtocolFinderLib trace that PrePi captured.

  RamPartitionDxe already dumps this early in DXE, but "early in DXE" is at the
  head of a 64 KiB ring - hundreds of QC driver prints follow it, so on a busy
  boot it can be evicted before LOGAUTO reads the ring. The HOB is still there,
  so print it again here, at the tail.
**/
STATIC
VOID
FpDiagDumpSf (
  VOID
  )
{
  VOID   *Hob;
  UINT8  *Text;

  Hob = GetFirstGuidHob ((EFI_GUID *)&mSfDiagHobGuid);
  if (Hob == NULL) {
    FpDiagPrint ("[DIAG] sf: no diag HOB\r\n");
    return;
  }

  Text = (UINT8 *)GET_GUID_HOB_DATA (Hob);
  FpDiagPrint ("[DIAG] sf ---- begin ----\r\n");
  SerialPortWrite (Text, AsciiStrLen ((CHAR8 *)Text));
  FpDiagPrint ("[DIAG] sf ---- end ----\r\n");
}

//
// Which device tree do the Qualcomm drivers actually read?
//
// I2CDxe and DALTLMM both take their configuration from DTBExtnProtocol, and
// both fail on this board: I2CDxe publishes zero I2C masters (so UsbConfigDxe
// reports "usb_shared_i2c_write, not open" and the WCD939x USB switch is never
// programmed) and DALTLMM reports "Failed to enable GPIO status : 0x7". Neither
// prints anything about the tree itself.
//
// DTBExtnProtocol is not a DXE driver - PrePi finds it inside XBL's own image
// (0xA703F0E8 on this board) and hands it over in gEfiSecDtbGuid. So the tree it
// wraps is whatever XBL built, not the complete shennong tree that ships in the
// FV. If XBL's tree is minimal, that is the whole explanation.
//
// This looks for an FDT in the two places it could be and reports whether the
// nodes I2CDxe needs are in it. Pure reads.
//
#define FD_RESERVED_BASE  0xA7000000UL
#define FD_RESERVED_LEN   0x00100000U
#define XBL_DT_BASE       0x81A00000UL
#define XBL_DT_LEN        0x00040000U

STATIC
VOID
FpDiagScanFdtWindow (
  IN CONST CHAR8  *Name,
  IN UINTN        Base,
  IN UINTN        Length
  )
{
  volatile UINT8  *P;
  UINTN           Off;
  UINTN           FdtOff;
  UINT32          FdtSize;
  UINT32          I2cNodes;
  UINT32          SpmiNodes;
  UINT32          ControllerCompat;
  BOOLEAN         HasSocI2cPath;

  P                = (volatile UINT8 *)Base;
  FdtOff           = (UINTN)-1;
  FdtSize          = 0;
  I2cNodes         = 0;
  SpmiNodes        = 0;
  ControllerCompat = 0;
  HasSocI2cPath    = FALSE;

  for (Off = 0; Off + 32 < Length; Off++) {
    if ((P[Off] == 0xD0) && (P[Off + 1] == 0x0D) &&
        (P[Off + 2] == 0xFE) && (P[Off + 3] == 0xED) && (FdtOff == (UINTN)-1)) {
      FdtOff  = Off;
      FdtSize = ((UINT32)P[Off + 4] << 24) | ((UINT32)P[Off + 5] << 16) |
                ((UINT32)P[Off + 6] << 8) | (UINT32)P[Off + 7];
    }

    if ((P[Off] == 'i') && (P[Off + 1] == '2') && (P[Off + 2] == 'c') && (P[Off + 3] == '@')) {
      I2cNodes++;
    }

    if ((P[Off] == 's') && (P[Off + 1] == 'p') && (P[Off + 2] == 'm') && (P[Off + 3] == 'i')) {
      SpmiNodes++;
    }

    if ((P[Off] == 'q') && (P[Off + 1] == 'c') && (P[Off + 2] == 'o') && (P[Off + 3] == 'm') &&
        (P[Off + 4] == ',') && (P[Off + 5] == 'i') && (P[Off + 6] == '2') && (P[Off + 7] == 'c')) {
      ControllerCompat++;
    }

    if ((P[Off] == '/') && (P[Off + 1] == 's') && (P[Off + 2] == 'o') && (P[Off + 3] == 'c') &&
        (P[Off + 4] == '/') && (P[Off + 5] == 'i') && (P[Off + 6] == '2') && (P[Off + 7] == 'c')) {
      HasSocI2cPath = TRUE;
    }
  }

  if (FdtOff == (UINTN)-1) {
    FpDiagPrint ("[DIAG] fdt %a: no FDT magic in 0x%lx+0x%lx\r\n",
                 Name, (UINT64)Base, (UINT64)Length);
  } else {
    FpDiagPrint ("[DIAG] fdt %a: @0x%lx size=0x%x\r\n",
                 Name, (UINT64)(Base + FdtOff), FdtSize);
  }

  FpDiagPrint ("[DIAG] fdt %a: i2c@=%u spmi=%u qcom-i2c=%u /soc/i2c=%a\r\n",
               Name, I2cNodes, SpmiNodes, ControllerCompat,
               HasSocI2cPath ? "yes" : "no");
}

STATIC
VOID
FpDiagDumpDtb (
  VOID
  )
{
  VOID    *Hob;
  UINT64  DtbExtn;
  UINT32  Word;
  UINTN   Index;

  FpDiagScanFdtWindow ("FDRes", FD_RESERVED_BASE, FD_RESERVED_LEN);
  FpDiagScanFdtWindow ("XblDt", XBL_DT_BASE, XBL_DT_LEN);

  //
  // The DTBExtnProtocol pointer itself. Its layout is not in any header we
  // have, so dump the first words and look for one that points at an FDT.
  //
  Hob = GetFirstGuidHob ((EFI_GUID *)&mSecDtbHobGuid);
  if (Hob == NULL) {
    FpDiagPrint ("[DIAG] dtbextn: no HOB\r\n");
    return;
  }

  DtbExtn = *(UINT64 *)GET_GUID_HOB_DATA (Hob);
  FpDiagPrint ("[DIAG] dtbextn=0x%lx\r\n", DtbExtn);
  if (DtbExtn == 0) {
    return;
  }

  for (Index = 0; Index < 8; Index++) {
    Word = MmioRead32 ((UINTN)(DtbExtn + Index * 4));
    FpDiagPrint ("[DIAG]   +%02x = %08x\r\n", (UINT32)(Index * 4), Word);
  }
}

/**
  Dump read-only diagnostics to the log ring.

  Output goes through SerialPortWrite, which is what puts it in the ring, so the
  LOGAUTO page can carry it off the device.
**/
VOID
FpDiagDump (
  VOID
  )
{
  SerialPortInitialize ();

  FpDiagPrint ("\r\n[DIAG] ---- begin ----\r\n");

  FpDiagDumpSf ();

  FpDiagDumpDtb ();

  FpDiagPrintGcd ("UFS_MEM", UFS_MEM_BASE, UFS_MEM_LEN);
  FpDiagPrintGcd ("UFS_ICE", UFS_ICE_BASE, UFS_ICE_LEN);

  FpDiagPrintI2c ("master", &mI2cMasterProtocolGuid);
  FpDiagPrintI2c ("io", &mI2cIoProtocolGuid);

  FpDiagPrintI2c ("blockio", &mBlockIoGuid);
  FpDiagPrintI2c ("diskio", &mDiskIoGuid);
  FpDiagPrintI2c ("fs", &mSimpleFsGuid);

  FpDiagDumpBlockIo ();

  FpDiagScanPartitions ();

  FpDiagDumpUfs ();

  FpDiagPrint ("[DIAG] ---- end ----\r\n");
}
