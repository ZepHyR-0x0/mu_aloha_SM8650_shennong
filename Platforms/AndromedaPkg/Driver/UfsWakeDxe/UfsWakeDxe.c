/** @file

  Wake the UFS device to Active before anything tries to read it.

  Ported from the m2381 (SM8550) Mu port's UfsWakeDxe, which solved the same symptom
  on a sibling SoC. The problem it addresses: the boot chain can leave the UFS device
  in the Sleep/PowerDown family. UFSDxe only issues SSU-Active from its own
  sleep-to-wake path, which boot never takes, so the first BlockIo read comes back with
  sense 04/02 and the volume never enumerates.

  The sequence, unchanged from the reference:

    probe READ_FLAG fDeviceInit (retry after DME_HIBER_EXIT if it times out)
    SSU power condition 1 (Active) to WLUN 0xD0, poll bCurrentPowerMode to 0x11
    NOP OUT
    SET fDeviceInit, then poll READ_FLAG until it clears

  Two things this deliberately does NOT do, both learned the hard way upstream:

    - It never touches TLMM. The UFS reset line is GPIO 210, and driving TLMM from
      post-EBS DXE killed the SoC into EDL (900e) on the reference device.
    - It never reprograms UTRLBA or UTRL_RSR. It reads the transfer list base and rides
      the list that is already running.

  It runs AFTER UFSDxe, because that list is UFSDxe's. If UFSDxe never gets far enough
  to build one, UTRLBA reads zero and this driver returns without doing anything.

  Copyright (c) 2024, Project Mu contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/BlockIo.h>
#include <Protocol/Cpu.h>

#define UFSHCI_BASE  0x01D84000UL

#define UFSHCI_CAP         0x00
#define UFSHCI_VER         0x08
#define UFSHCI_IS          0x20
#define UFSHCI_IE          0x24
#define UFSHCI_HCS         0x30
#define UFSHCI_UTRLBA_L    0x50
#define UFSHCI_UTRLBA_H    0x54
#define UFSHCI_UTRLDBR     0x58
#define UFSHCI_UICC        0x90
#define UFSHCI_UICC_ARG1   0x94
#define UFSHCI_UICC_ARG2   0x98
#define UFSHCI_UICC_ARG3   0x9C

#define IS_UTP_TRANSFER_COMPL     0x1
#define IS_UIC_COMMAND_COMPL      0x400
#define UIC_CMD_DME_HIBER_EXIT    0x18
#define MASK_UIC_RESULT           0xFF
#define OCS_SUCCESS               0x0
#define OCS_INVALID               0x0F

#define UPIU_TRANSACTION_NOP_OUT      0x00
#define UPIU_TRANSACTION_COMMAND      0x01
#define UPIU_TRANSACTION_QUERY_REQ    0x16
#define UPIU_COMMAND_SET_TYPE_SCSI    0x00

#define UPIU_QUERY_FUNC_READ          0x01
#define UPIU_QUERY_FUNC_WRITE         0x81
#define UPIU_QUERY_OPCODE_READ_FLAG   0x05
#define UPIU_QUERY_OPCODE_SET_FLAG    0x06
#define UPIU_QUERY_OPCODE_READ_ATTR   0x03
#define QUERY_FLAG_IDN_FDEVICEINIT    0x01
#define QUERY_ATTR_IDN_POWER_MODE     0x02

#define WLUN_UFS_DEVICE           0xD0
#define SCSI_START_STOP_UNIT      0x1B
#define SSU_PWR_COND_ACTIVE       0x01

#define RSP_UPIU_OFFSET   512
#define PRDT_OFFSET       1024
#define UCD_CLEAR_SIZE    0x800
#define RESP_LEN_DW       (RSP_UPIU_OFFSET / 4)
#define RESP_OFF_DW       (RSP_UPIU_OFFSET / 4)
#define PRDT_OFF_DW       (PRDT_OFFSET / 4)

#define UTRD_CT_UFS_STORAGE  0x1
#define UTP_NO_DATA_TRANSFER  0x0

#define WAIT_LOOPS          2000000UL
#define FLAG_CLEAR_POLLS    300

#define FDEVICEINIT_BIT(v)  (((UINT32)(v) >> 24) & 0x1)

STATIC UINT8   *gUcd;
STATIC UINT64   gUtrlBase;
STATIC UINT32   gSavedIe;
STATIC UINT32   gSlot;
STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu;


STATIC
VOID
UfsLog (
  IN CONST CHAR8  *Format,
  ...
  );

//
// Look for ELF images among the partitions, before TzDxeLA runs.
//
// TzDxeLA loads cmnlib32 and cmnlib64 as "Early Storage" and then fails with
// ValidateElfHeader: the Early Storage is not valid!. The DIAG page cannot report on it
// because that assert halts the boot before FrontPage ever runs, so the scan happens
// here instead - after UFSDxe has published block devices, before TzDxeLA reads them.
//
// Read-only: one block per partition.
//
STATIC
VOID
ScanElfPartitions (
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
  UINT16                  Machine;
  UINT32                  Class;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    UfsLog ("[ELFSCAN] no blockio (%r)\r\n", Status);
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
    // Real Qualcomm firmware is a mix of classes: abl.elf and aop.mbn are ELF32 with
    // e_phoff = 0x34, devcfg.mbn is ELF64 with e_phoff = 0x40. Reading the ELF64 offsets
    // unconditionally produced nonsense like phoff = 0x500000000, so branch on
    // e_ident[EI_CLASS] first.
    //
    // Section headers are stripped in every shipping image (e_shoff = 0, e_shnum = 0),
    // so the field a loader validates is the program header table, not the section one.
    // The check that maps to "Bad Buffer Size" is whether e_phoff + e_phnum * e_phentsize
    // fits inside the partition.
    //
    if (Buf[4] == 2) {
      // ELF64
      PhOff   = *(UINT64 *)(Buf + 0x20);
      PhEnt   = *(UINT16 *)(Buf + 0x36);
      PhNum   = *(UINT16 *)(Buf + 0x38);
      Machine = *(UINT16 *)(Buf + 0x12);
      Class   = 64;
    } else {
      // ELF32
      PhOff   = (UINT64)*(UINT32 *)(Buf + 0x1C);
      PhEnt   = *(UINT16 *)(Buf + 0x2A);
      PhNum   = *(UINT16 *)(Buf + 0x2C);
      Machine = *(UINT16 *)(Buf + 0x12);
      Class   = 32;
    }

    MediaSize = (Part->Media->LastBlock + 1) * Part->Media->BlockSize;
    PhEnd     = PhOff + ((UINT64)PhNum * PhEnt);

    UfsLog (
      "[ELFSCAN] #%u class=%u mach=0x%x bytes=%lu phoff=0x%lx phnum=%u phend=0x%lx %a\r\n",
      (UINT32)Printed,
      (UINT32)Class,
      (UINT32)Machine,
      MediaSize,
      PhOff,
      (UINT32)PhNum,
      PhEnd,
      (PhEnd > MediaSize) ? "OVERRUN" : "ok"
      );

    Printed++;
  }

  UfsLog ("[ELFSCAN] found=%u of %u blockio handles\r\n", (UINT32)Printed, (UINT32)Count);

  if (Buf != NULL) {
    FreePool (Buf);
  }

  FreePool (Handles);
}

STATIC
VOID
UfsLog (
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
UINT32
Reg32 (
  IN UINT32  Offset
  )
{
  return MmioRead32 (UFSHCI_BASE + Offset);
}

STATIC
EFI_STATUS
MapUncached (
  IN UINT64  Phys,
  IN UINT64  Length
  )
{
  EFI_PHYSICAL_ADDRESS  Page = Phys & ~(EFI_PHYSICAL_ADDRESS)0xFFF;
  UINT64                End  = (Phys + Length + 0xFFF) & ~(UINT64)0xFFF;

  if (mCpu == NULL) {
    return EFI_NOT_READY;
  }

  return mCpu->SetMemoryAttributes (mCpu, Page, End - Page, EFI_MEMORY_UC);
}

STATIC
EFI_STATUS
WaitBit32 (
  IN UINT32   Offset,
  IN UINT32   Mask,
  IN BOOLEAN  Set,
  IN UINTN    Loops
  )
{
  UINTN   Index;
  UINT32  Value;

  for (Index = 0; Index < Loops; Index++) {
    Value = Reg32 (Offset);
    if (Set ? ((Value & Mask) != 0) : ((Value & Mask) == 0)) {
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SendUicCommand (
  IN  UINT8   Cmd,
  OUT UINT32  *Result
  )
{
  EFI_STATUS  Status;

  MmioWrite32 (UFSHCI_BASE + UFSHCI_IS, 0xFFFFFFFF);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_UICC_ARG1, 0);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_UICC_ARG2, 0);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_UICC_ARG3, 0);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_UICC, Cmd);

  Status = WaitBit32 (UFSHCI_IS, IS_UIC_COMMAND_COMPL, TRUE, WAIT_LOOPS);
  if (Result != NULL) {
    *Result = Reg32 (UFSHCI_UICC_ARG2) & MASK_UIC_RESULT;
  }

  MmioWrite32 (UFSHCI_BASE + UFSHCI_IS, IS_UIC_COMMAND_COMPL);
  return Status;
}

STATIC
VOID
FillUtrd (
  IN UINT8    *Utrd,
  IN UINT8     Direction,
  IN UINT32    PrdtEntries,
  IN BOOLEAN   WithData,
  IN UINT64    DataPhys,
  IN UINT32    DataLen
  )
{
  SetMem (Utrd, 32, 0);
  Utrd[3]                = (UINT8)((UTRD_CT_UFS_STORAGE << 4) | (Direction << 1) | 1);
  Utrd[8]                = OCS_INVALID;
  *(UINT64 *)(Utrd + 16) = (UINT64)(UINTN)gUcd;
  *(UINT16 *)(Utrd + 24) = RESP_LEN_DW;
  *(UINT16 *)(Utrd + 26) = RESP_OFF_DW;
  *(UINT16 *)(Utrd + 28) = (UINT16)PrdtEntries;
  *(UINT16 *)(Utrd + 30) = WithData ? (UINT16)PRDT_OFF_DW : 0;

  if (WithData) {
    UINT8  *Prdt = gUcd + PRDT_OFFSET;

    SetMem (Prdt, 16, 0);
    *(UINT64 *)(Prdt + 0)  = DataPhys;
    *(UINT32 *)(Prdt + 8)  = 0;
    *(UINT32 *)(Prdt + 12) = DataLen - 1;
  }
}

STATIC
EFI_STATUS
SubmitUtp (
  OUT UINT8   *Ocs,
  OUT UINT32  *IsSnapshot
  )
{
  UINT8      *Utrd = (UINT8 *)(UINTN)(gUtrlBase + (UINT64)gSlot * 32);
  EFI_STATUS  Status;

  WriteBackInvalidateDataCacheRange (Utrd, 32);
  WriteBackInvalidateDataCacheRange (gUcd, UCD_CLEAR_SIZE);

  //
  // W1C: only the bits we own. ORing into IS wipes events that are still pending.
  //
  MmioWrite32 (UFSHCI_BASE + UFSHCI_IS, IS_UTP_TRANSFER_COMPL);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_UTRLDBR, 1u << gSlot);

  Status      = WaitBit32 (UFSHCI_IS, IS_UTP_TRANSFER_COMPL, TRUE, WAIT_LOOPS);
  *IsSnapshot = Reg32 (UFSHCI_IS);
  MmioWrite32 (UFSHCI_BASE + UFSHCI_IS, IS_UTP_TRANSFER_COMPL);

  InvalidateDataCacheRange (Utrd, 32);
  InvalidateDataCacheRange (gUcd, UCD_CLEAR_SIZE);

  *Ocs = Utrd[8];
  if (EFI_ERROR (Status)) {
    return EFI_TIMEOUT;
  }

  if (*Ocs != OCS_SUCCESS) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SendQuery (
  IN  UINT8    Opcode,
  IN  UINT8    Idn,
  IN  UINT8    Index,
  IN  UINT16   Length,
  IN  UINT32   ValueIn,
  OUT UINT32   *ValueRaw OPTIONAL
  )
{
  UINT8       *Utrd = (UINT8 *)(UINTN)(gUtrlBase + (UINT64)gSlot * 32);
  UINT8       *Req  = gUcd;
  UINT8       *Rsp  = gUcd + RSP_UPIU_OFFSET;
  UINT32      IsSnap = 0;
  UINT8       Ocs    = 0;
  EFI_STATUS  Status;

  SetMem (gUcd, UCD_CLEAR_SIZE, 0);
  FillUtrd (Utrd, UTP_NO_DATA_TRANSFER, 0, FALSE, 0, 0);

  Req[0] = UPIU_TRANSACTION_QUERY_REQ;
  Req[3] = (UINT8)gSlot;
  Req[5] = (Opcode == UPIU_QUERY_OPCODE_SET_FLAG) ? UPIU_QUERY_FUNC_WRITE
                                                 : UPIU_QUERY_FUNC_READ;
  Req[12] = Opcode;
  Req[13] = Idn;
  Req[14] = Index;
  Req[18] = (UINT8)(Length >> 8);
  Req[19] = (UINT8)(Length & 0xFF);
  Req[20] = (UINT8)(ValueIn >> 24);
  Req[21] = (UINT8)(ValueIn >> 16);
  Req[22] = (UINT8)(ValueIn >> 8);
  Req[23] = (UINT8)(ValueIn);

  Status = SubmitUtp (&Ocs, &IsSnap);
  if (EFI_ERROR (Status)) {
    UfsLog ("[UFSWAKE] query 0x%x idn 0x%x failed: %r ocs 0x%x IS 0x%x HCS 0x%x\r\n",
            Opcode, Idn, Status, Ocs, IsSnap, Reg32 (UFSHCI_HCS));
    return Status;
  }

  if (Rsp[6] != 0) {
    UfsLog ("[UFSWAKE] query 0x%x idn 0x%x response 0x%x\r\n", Opcode, Idn, Rsp[6]);
    return EFI_DEVICE_ERROR;
  }

  if (ValueRaw != NULL) {
    *ValueRaw = *(UINT32 *)(Rsp + 12 + 8);
  }

  return EFI_SUCCESS;
}

STATIC
VOID
SendSsu (
  IN UINT8  PwrCond
  )
{
  UINT8       *Utrd = (UINT8 *)(UINTN)(gUtrlBase + (UINT64)gSlot * 32);
  UINT8       *Req  = gUcd;
  UINT8       *Rsp  = gUcd + RSP_UPIU_OFFSET;
  UINT32      IsSnap = 0;
  UINT8       Ocs    = 0;
  EFI_STATUS  Status;

  SetMem (gUcd, UCD_CLEAR_SIZE, 0);
  FillUtrd (Utrd, UTP_NO_DATA_TRANSFER, 0, FALSE, 0, 0);

  Req[0]  = UPIU_TRANSACTION_COMMAND;
  Req[2]  = WLUN_UFS_DEVICE;
  Req[3]  = (UINT8)gSlot;
  Req[4]  = UPIU_COMMAND_SET_TYPE_SCSI;
  Req[16] = SCSI_START_STOP_UNIT;
  Req[20] = (UINT8)(PwrCond << 4);

  Status = SubmitUtp (&Ocs, &IsSnap);
  UfsLog ("[UFSWAKE] SSU pwr 0x%x -> wlun 0xd0: %r ocs 0x%x type 0x%x status 0x%x\r\n",
          PwrCond, Status, Ocs, Rsp[0], Rsp[7]);
}

STATIC
VOID
SendNopOut (
  VOID
  )
{
  UINT8       *Utrd = (UINT8 *)(UINTN)(gUtrlBase + (UINT64)gSlot * 32);
  UINT8       *Req  = gUcd;
  UINT8       *Rsp  = gUcd + RSP_UPIU_OFFSET;
  UINT32      IsSnap = 0;
  UINT8       Ocs    = 0;
  EFI_STATUS  Status;

  SetMem (gUcd, UCD_CLEAR_SIZE, 0);
  FillUtrd (Utrd, UTP_NO_DATA_TRANSFER, 0, FALSE, 0, 0);

  Req[0] = UPIU_TRANSACTION_NOP_OUT;
  Req[3] = (UINT8)gSlot;

  Status = SubmitUtp (&Ocs, &IsSnap);
  UfsLog ("[UFSWAKE] NOP OUT %r ocs 0x%x type 0x%x (0x20 = NOP IN)\r\n",
          Status, Ocs, Rsp[0]);
}

STATIC
VOID
WaitDevActive (
  VOID
  )
{
  UINTN   Index;
  UINT32  Raw = 0;
  UINT8   Mode;

  for (Index = 0; Index < 20; Index++) {
    if (EFI_ERROR (SendQuery (UPIU_QUERY_OPCODE_READ_ATTR, QUERY_ATTR_IDN_POWER_MODE, 0, 0, 0, &Raw))) {
      return;
    }

    //
    // Attribute values are big-endian on the wire and read raw, so the mode byte lands
    // in the high byte of the little-endian word.
    //
    Mode = (UINT8)(Raw >> 24);
    if (Mode == 0x11) {
      UfsLog ("[UFSWAKE] power mode ACTIVE after %u polls\r\n", (UINT32)Index);
      return;
    }

    gBS->Stall (10000);
  }

  UfsLog ("[UFSWAKE] power mode still 0x%x after 200 ms\r\n", (UINT32)(Raw >> 24));
}

STATIC
EFI_STATUS
SetDeviceInit (
  VOID
  )
{
  EFI_STATUS  Status = EFI_DEVICE_ERROR;
  UINT32      Value  = 0;
  UINTN       Retry;
  UINTN       Poll;

  for (Retry = 0; Retry < 3; Retry++) {
    Status = SendQuery (UPIU_QUERY_OPCODE_SET_FLAG, QUERY_FLAG_IDN_FDEVICEINIT, 0, 0, 0, NULL);
    if (!EFI_ERROR (Status)) {
      UfsLog ("[UFSWAKE] SET_FLAG fDeviceInit ok (try %u)\r\n", (UINT32)(Retry + 1));
      break;
    }

    gBS->Stall (1000);
  }

  //
  // A refused SET is not fatal: the device reports the flag as already clear and then
  // rejects the redundant write. The flag read is the statement that counts.
  //
  for (Poll = 0; Poll < FLAG_CLEAR_POLLS; Poll++) {
    gBS->Stall (1000);
    Status = SendQuery (UPIU_QUERY_OPCODE_READ_FLAG, QUERY_FLAG_IDN_FDEVICEINIT, 0, 0, 0, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (FDEVICEINIT_BIT (Value) == 0) {
      UfsLog ("[UFSWAKE] fDeviceInit clear after %u polls\r\n", (UINT32)Poll);
      return EFI_SUCCESS;
    }
  }

  UfsLog ("[UFSWAKE] fDeviceInit never cleared\r\n");
  return EFI_TIMEOUT;
}

/**
  Wake the device to Active and hand it back to whoever reads it next.
**/
EFI_STATUS
EFIAPI
UfsWakeDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Cap;
  UINT32      Value = 0;
  UINT32      Dbr;
  UINTN       Slot;
  UINT8       *Ucd;
  BOOLEAN     Found = FALSE;

  SerialPortInitialize ();

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
  if (EFI_ERROR (Status)) {
    UfsLog ("[UFSWAKE] no CPU arch protocol, abort\r\n");
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (MapUncached (UFSHCI_BASE, 0x20000))) {
    UfsLog ("[UFSWAKE] cannot map UFSHCI uncached, abort\r\n");
    return EFI_SUCCESS;
  }

  Cap = Reg32 (UFSHCI_CAP);
  if ((Cap == 0xFFFFFFFF) || (Cap == 0)) {
    UfsLog ("[UFSWAKE] CAP dead (0x%x), abort\r\n", Cap);
    return EFI_SUCCESS;
  }

  UfsLog ("[UFSWAKE] CAP=0x%08x VER=0x%08x HCS=0x%08x\r\n",
          Cap, Reg32 (UFSHCI_VER), Reg32 (UFSHCI_HCS));

  gSavedIe = Reg32 (UFSHCI_IE);

  //
  // Ride the list that is already running. Never reprogram UTRLBA or UTRL_RSR - on the
  // reference device that hung on XBL's read-only page.
  //
  gUtrlBase = (UINT64)Reg32 (UFSHCI_UTRLBA_L) | ((UINT64)Reg32 (UFSHCI_UTRLBA_H) << 32);
  UfsLog ("[UFSWAKE] UTRLBA=0x%lx\r\n", gUtrlBase);

  if (gUtrlBase == 0) {
    //
    // Nothing to ride: UFSDxe never got far enough to build a transfer list. Nothing
    // this driver can do in that case.
    //
    UfsLog ("[UFSWAKE] no transfer list (UFSDxe did not get there), abort\r\n");
    return EFI_SUCCESS;
  }

  Dbr = Reg32 (UFSHCI_UTRLDBR);
  UfsLog ("[UFSWAKE] DBR=0x%08x IS=0x%08x\r\n", Dbr, Reg32 (UFSHCI_IS));

  for (Slot = 31; Slot > 0; Slot--) {
    if ((Dbr & (1u << Slot)) == 0) {
      gSlot  = (UINT32)Slot;
      Found  = TRUE;
      break;
    }
  }

  if (!Found) {
    UfsLog ("[UFSWAKE] no idle slot, abort\r\n");
    return EFI_SUCCESS;
  }

  UfsLog ("[UFSWAKE] using slot %u\r\n", gSlot);

  Ucd = AllocatePages (1);
  if (Ucd == NULL) {
    UfsLog ("[UFSWAKE] UCD alloc failed\r\n");
    return EFI_SUCCESS;
  }

  //
  // The command descriptor must not land inside the transfer list it is describing.
  //
  if ((((UINT64)(UINTN)Ucd + 0x1000) > gUtrlBase) &&
      ((UINT64)(UINTN)Ucd < gUtrlBase + 32 * 32))
  {
    UfsLog ("[UFSWAKE] UCD overlaps UTRL, abort\r\n");
    return EFI_SUCCESS;
  }

  gUcd = Ucd;
  UfsLog ("[UFSWAKE] UCD=0x%lx\r\n", (UINT64)(UINTN)gUcd);

  //
  // Liveness probe. A timeout here means the link is hibernated, which DME_HIBER_EXIT
  // is meant to fix.
  //
  Status = SendQuery (UPIU_QUERY_OPCODE_READ_FLAG, QUERY_FLAG_IDN_FDEVICEINIT, 0, 0, 0, &Value);
  UfsLog ("[UFSWAKE] probe READ_FLAG %r raw 0x%08x\r\n", Status, Value);

  if (Status == EFI_TIMEOUT) {
    UINT32  UicResult = 0;

    Status = SendUicCommand (UIC_CMD_DME_HIBER_EXIT, &UicResult);
    UfsLog ("[UFSWAKE] HIBER_EXIT %r result 0x%x\r\n", Status, UicResult);

    if (!EFI_ERROR (Status)) {
      Status = SendQuery (UPIU_QUERY_OPCODE_READ_FLAG, QUERY_FLAG_IDN_FDEVICEINIT, 0, 0, 0, &Value);
      UfsLog ("[UFSWAKE] probe retry %r raw 0x%08x\r\n", Status, Value);
    }
  }

  if (EFI_ERROR (Status)) {
    UfsLog ("[UFSWAKE] probe failed (%r), restoring IE and aborting\r\n", Status);
    MmioWrite32 (UFSHCI_BASE + UFSHCI_IE, gSavedIe);
    return EFI_SUCCESS;
  }

  UfsLog ("[UFSWAKE] sending SSU power condition 1 (Active)\r\n");
  SendSsu (SSU_PWR_COND_ACTIVE);
  WaitDevActive ();
  SendNopOut ();
  Status = SetDeviceInit ();
  UfsLog ("[UFSWAKE] fDeviceInit %r - device handed back\r\n", Status);

  MmioWrite32 (UFSHCI_BASE + UFSHCI_IE, gSavedIe);

  //
  // Last thing before handing over to TzDxeLA, which reads cmnlib from a partition and
  // is the module that currently halts the boot. Anything printed here lands in the log
  // before that assert.
  //
  ScanElfPartitions ();

  return EFI_SUCCESS;
}
