/** @file
    Copyright (c) 2024-2026. DuoWoA Authors. All rights reserved.
    Copyright (c) 2024-2026. Project Aloha Authors. All rights reserved.

    SPDX-License-Identifier: MIT
*/

#include "SecProtocolFinderLib.h"
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/ConfigurationMapHelperLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PlatformHobs.h>
#include <Library/PlatformMemoryMapLib.h>
#include <Library/PrintLib.h>
#include <Library/SecProtocolFinderLib.h>
#include <Library/SerialPortLib.h>

/* Variables */
STATIC UINTN             ScheIntrAddr = 0;
STATIC UINTN             SecDTOpsAddr = 0;
STATIC LK_INIT_FUNCTIONS LkInitFuncs  = {0};

/* Persistent copy of the trace, see SecFinderGetDiag(). */
#define SF_DIAG_MAX  3072
STATIC CHAR8             gSfDiag[SF_DIAG_MAX];
STATIC UINTN             gSfDiagLen = 0;

/**
  Diagnostics that survive a RELEASE build.

  DEBUG() is compiled out here (MDEPKG_NDEBUG), and this library is the reason
  EnvDxe never starts: InitProtocolFinder() silently returns 0 when the XBL TE
  scan fails, the ASSERT below is compiled away, and the gEfiSchedIntfGuid HOB
  ends up holding a null pointer. Every Qualcomm driver that reads that HOB
  (EnvDxe, UsbfnDwc3Dxe, UsbConfigDxe, UsbMsdDxe, ...) then fails with
  EFI_NOT_FOUND. These traces go straight to the framebuffer serial port so we
  can see exactly which step of the heuristic scan breaks.

  The serial port already keeps them: FrameBufferInMemorySerialPortLib rings
  every SerialPortWrite byte into the "UEFI Log" region, and the Front Page QR
  viewer reads that ring. Do not add a second log into the same region - an
  earlier version kept its own 'SFLG' header at offset 0 there and its magic
  bytes showed up as the first QR page.
**/

STATIC
VOID
SfTrace (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR8    Buffer[192];
  UINTN    Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  SerialPortWrite ((UINT8 *)Buffer, Length);

  //
  // Keep a copy for the HOB. The ring in "UEFI Log" is 64 KiB and the PrePi
  // traces are the first thing in it, so they are the first thing evicted.
  //
  if ((Length > 0) && (gSfDiagLen + Length < sizeof (gSfDiag))) {
    CopyMem (gSfDiag + gSfDiagLen, Buffer, Length);
    gSfDiagLen          += Length;
    gSfDiag[gSfDiagLen]  = '\0';
  }
}

VOID
SecFinderGetDiag (
  OUT UINT8 **Buffer,
  OUT UINTN  *Length
  )
{
  if ((Buffer == NULL) || (Length == NULL)) {
    return;
  }

  *Buffer = (UINT8 *)gSfDiag;
  *Length = (gSfDiagLen == 0) ? 0 : (gSfDiagLen + 1);
}

/**
  Sanity-check a pointer produced by the ADRP/ADD heuristics.

  A garbage value here is worse than zero: a driver that gets a non-null but
  invalid pointer dereferences it and takes a data abort, whereas a null makes
  it report "not found" and carry on. So anything that is null, unaligned, or
  outside the TE image gets forced to 0 and logged.
**/
STATIC
UINTN
SfValidateAddr (
  IN CONST CHAR8    *Tag,
  IN UINTN          Addr,
  IN TE_INFO_STRUCT *Te
  )
{
  UINT64  Low  = (UINT64)(UINTN)Te->TEBuffer;
  UINT64  High = Low + Te->fileSize;

  if ((Addr == 0) || ((Addr & 3) != 0) || (Addr < Low) || (Addr >= High)) {
    SfTrace ("[SF] %a 0x%lx REJECTED (TE 0x%lx..0x%lx)\n\r", Tag, (UINT64)Addr,
             Low, High);
    return 0;
  }

  SfTrace ("[SF] %a 0x%lx accepted\n\r", Tag, (UINT64)Addr);
  return Addr;
}

/* Funcs */
/**
 * Scan one memory window for the XBL core TE image.
 *
 * The signature is 0xAA645A56 ('VZ' + machine 0xAA64). The dword right before
 * the header carries the image size in its low 24 bits.
 **/
STATIC
BOOLEAN
ScanWindowForTe(
    IN  UINT64 Base, IN UINT64 Length, OUT TE_INFO_STRUCT *TEInfo)
{
  UINT64 Start = Base + 0x1000;   // skip the previous stage's header area
  UINT64 End   = Base + Length;

  for (UINT64 a = Start; a + 4 < End; a += 4) {
    if (*(UINT32 *)a != 0xAA645A56) {
      continue;
    }

    TEInfo->fileSize = 0xFFFFFF & *(UINT32 *)(a - 4);
    TEInfo->teSize   = TEInfo->fileSize - sizeof(EFI_TE_IMAGE_HEADER);

    UINT64 EndOfHeaderAddress =
        a + sizeof(EFI_TE_IMAGE_HEADER) +
        EFI_IMAGE_SIZEOF_SECTION_HEADER *
            ((EFI_TE_IMAGE_HEADER *)a)->NumberOfSections;

    for (UINT64 q = EndOfHeaderAddress; q + 4 < End; q += 4) {
      if (*(UINT32 *)q != 0x0) {
        TEInfo->TEBuffer      = (VOID *)a;
        TEInfo->programBuffer = (VOID *)q;
        return TRUE;
      }
    }
  }

  return FALSE;
}

/**
 * Find the buffer to the header of XBLCore.te
 *
 * The region that holds it is named differently on every port:
 *   Kailua (SM8550, OnePlus 13R) : "FD Reserved I" @0xA7000000
 *   Shennong (SM8650)            : "FD Reserved"   @0xA7000000
 * and the low "NOMAP" window carries the XBL reserved area on both.
 * If none of them contains the signature we fall back to scanning the whole
 * XBL + UEFI window.
 *
 * When this fails, ScheIntrAddr and SecDTOpsAddr stay 0, both HOBs end up
 * holding a null pointer, and every Qualcomm driver that reads them fails:
 *   EnvDxe ctor #12          -> EFI_NOT_FOUND
 *   PlatformInfoDxe/DMLibEnv -> ASSERT at DMLibEnv.c +115
 *   UsbConfigDxe             -> "Locate DTBExtnProtocol failed"
 *   UsbDeviceDxe             -> "Failed to get gQcomUsbConfigProtocolGuid"
 *
 * @param TEInfo provide filePath, will also set TEBuffer in it.
 * @return EFI_STATUS EFI_SUCCESS if found, EFI_NOT_FOUND if not found.
 */
STATIC
EFI_STATUS
FindTeAddr(TE_INFO_STRUCT *TEInfo)
{
  STATIC CONST CHAR8 *CandidateNames[] = {"FD Reserved I", "FD Reserved"};
  ARM_MEMORY_REGION_DESCRIPTOR_EX Region = {0};
  UINTN                           Index;

  for (Index = 0; Index < ARRAY_SIZE(CandidateNames); Index++) {
    ZeroMem(&Region, sizeof(Region));

    if (EFI_ERROR(
            LocateMemoryMapAreaByName((CHAR8 *)CandidateNames[Index], &Region))) {
      SfTrace("[SF] region %a -> NOT FOUND\n\r", CandidateNames[Index]);
      continue;
    }

    SfTrace("[SF] region %a @0x%lx len 0x%lx\n\r", CandidateNames[Index],
            (UINT64)Region.Address, (UINT64)Region.Length);

    if (ScanWindowForTe((UINT64)Region.Address, (UINT64)Region.Length, TEInfo)) {
      SfTrace("[SF] TE found in %a @0x%lx fileSize=0x%x prog=0x%lx\n\r",
              CandidateNames[Index], (UINT64)(UINTN)TEInfo->TEBuffer,
              TEInfo->fileSize, (UINT64)(UINTN)TEInfo->programBuffer);
      return EFI_SUCCESS;
    }

    SfTrace("[SF] no TE signature inside %a\n\r", CandidateNames[Index]);
  }

  /* The whole XBL + UEFI window. Every byte of it is a mapped region. */
  SfTrace("[SF] fallback scan 0xA7000000 + 0x600000\n\r");
  if (ScanWindowForTe(0xA7000000UL, 0x600000UL, TEInfo)) {
    SfTrace("[SF] TE found in fallback @0x%lx\n\r",
            (UINT64)(UINTN)TEInfo->TEBuffer);
    return EFI_SUCCESS;
  }

  /*
   * Last resort: the low "NOMAP" window, whose comment in
   * PlatformMemoryMapLib.c says it covers "XBL reserved". It is declared
   * NoHob, so it may or may not be mapped - keep it last so that everything
   * above has already been written to the persistent log before we touch it.
   */
  ZeroMem(&Region, sizeof(Region));
  if (!EFI_ERROR(LocateMemoryMapAreaByName("NOMAP", &Region))) {
    SfTrace("[SF] NOMAP @0x%lx len 0x%lx\n\r", (UINT64)Region.Address,
            (UINT64)Region.Length);
    if (ScanWindowForTe((UINT64)Region.Address, (UINT64)Region.Length, TEInfo)) {
      SfTrace("[SF] TE found in NOMAP @0x%lx\n\r",
              (UINT64)(UINTN)TEInfo->TEBuffer);
      return EFI_SUCCESS;
    }
    SfTrace("[SF] no TE signature inside NOMAP\n\r");
  }

  SfTrace("[SF] TE NOT FOUND anywhere -> sched and dtb will be 0\n\r");
  return EFI_NOT_FOUND;
}

/**
 * @param TEInfo TE information struct.
 * @param Data is the buffer need to find in buffer.
 * @param Size is the size of Data buffer.
 * @param DataInBuffer offset of data in buffer.
 * @retval status
 **/
STATIC
EFI_STATUS
find_data_in_buffer(
    IN OUT TE_INFO_STRUCT *TEInfo, IN VOID *Data, IN UINTN Size,
    OUT UINTN *DataInBuffer)
{
  for (UINTN i = 0; i <= TEInfo->teSize - Size; i++) {
    if (CompareMem(TEInfo->programBuffer + i, Data, Size) == 0) {
      *DataInBuffer = (UINTN)i;
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND; // Not found
}

/**
 * @param TEInfo TE information struct.
 * @param KeyGuid is the buffer need to find in buffer.
 * @param GuidInBuffer offset of guid in buffer.
 * @retval status
 **/
STATIC
EFI_STATUS
find_guid_in_buffer(
    IN OUT TE_INFO_STRUCT *TEInfo, IN GUID *KeyGuid, OUT UINTN *GuidInBuffer)
{
  return find_data_in_buffer(TEInfo, KeyGuid, 16, GuidInBuffer);
}

STATIC
BOOLEAN
validate_adrp(IN OUT INST *inst)
{
  // Store Values by macros
  inst->adrp.op1   = ADRP_OP1(inst->val);
  inst->adrp.op2   = ADRP_OP2(inst->val);
  inst->adrp.Rd    = ADRP_RD(inst->val);
  inst->adrp.immhi = ADRP_IMMHI(inst->val);
  inst->adrp.immlo = ADRP_IMMLO(inst->val);
  return (inst->adrp.op1 == 1 && inst->adrp.op2 == 16 && inst->adrp.Rd <= 30);
}

STATIC
BOOLEAN
validate_add(IN OUT INST *inst)
{
  // Store Values by macros
  inst->add.op1   = ADD_OP1(inst->val);
  inst->add.op2   = ADD_OP2(inst->val);
  inst->add.Rd    = ADD_RD(inst->val);
  inst->add.Rn    = ADD_RN(inst->val);
  inst->add.imm12 = ADD_IMM12(inst->val);
  inst->add.s     = ADD_S(inst->val);
  inst->add.sf    = ADD_SF(inst->val);
  inst->add.sh    = ADD_SH(inst->val);
  return (inst->add.op1 == 0 && inst->add.s == 0 && inst->add.op2 == 34);
}

STATIC
BOOLEAN
validate_bl(IN OUT INST *inst)
{
  inst->bl.imm26 = BL_IMM26(inst->val);
  inst->bl.op    = BL_OP(inst->val);
  return (inst->bl.op == 0b100101);
}

STATIC
BOOLEAN
validate_ret(IN INST *inst) { return (inst->val == 0xD65F03C0); }

STATIC
BOOLEAN
validate_movz(INST *inst)
{
  inst->movz.sf    = MOV_SF(inst->val);
  inst->movz.opc   = MOV_OPC(inst->val);
  inst->movz.hw    = MOV_HW(inst->val);
  inst->movz.imm16 = MOV_IMM16(inst->val);
  inst->movz.Rd    = MOV_RD(inst->val);

  return (inst->movz.opc == 0b10100101);
}

STATIC
BOOLEAN
validate_movk(INST *inst)
{
  inst->movk.sf    = MOV_SF(inst->val);
  inst->movk.opc   = MOV_OPC(inst->val);
  inst->movk.hw    = MOV_HW(inst->val);
  inst->movk.imm16 = MOV_IMM16(inst->val);
  inst->movk.Rd    = MOV_RD(inst->val);

  return (inst->movk.opc == 0b11100101);
}

STATIC
VOID parse_adrp(IN OUT INST *inst, IN UINT32 offset)
{
  // Store immediate number
  inst->adrp.imm = (inst->adrp.immhi << 2 | inst->adrp.immlo) << 12;
  // Store PC and Register(after executing adrp) address
  inst->adrp.pc               = offset;
  inst->adrp.RdAfterExecution = ((inst->adrp.pc >> 12) << 12) + inst->adrp.imm;
};

STATIC
VOID parse_bl(IN OUT INST *inst, IN UINT32 offset)
{
  // Store immediate number
  inst->bl.imm = (inst->bl.imm26 & (1 << 25))
                     ? (inst->bl.imm26 | (~(0ULL) & 0xfc000000)) << 2
                     : (inst->bl.imm26) << 2;
  // Store PC and Register(after executing bl) address
  inst->bl.pc       = offset;
  inst->bl.jumpAddr = inst->bl.pc + inst->bl.imm;
};

STATIC
UINT64
parse_movz_with_movk(INST *movz_inst, INST *movk_inst)
{
  movz_inst->movz.imm = movz_inst->movz.hw * 16;
  movk_inst->movk.imm = movk_inst->movk.hw * 16;
  return (movz_inst->movz.imm16 << movz_inst->movz.imm) |
         (movk_inst->movk.imm16 << movk_inst->movk.imm);
}

STATIC
UINT64
offset_to_phy_addr(IN TE_INFO_STRUCT *Binary, IN UINT64 offset)
{
  return Binary->teHeader->BaseOfCode + Binary->teHeader->ImageBase + offset;
}

STATIC
UINT64
phy_addr_to_offset(IN TE_INFO_STRUCT *Binary, IN UINT64 PhyAddr)
{
  return PhyAddr - Binary->teHeader->BaseOfCode - Binary->teHeader->ImageBase;
}

STATIC
EFI_STATUS
find_first_ret_before_offset(
    IN TE_INFO_STRUCT *Binary, IN UINTN offset, OUT UINTN *PhyAddr)
{
  if (NULL == Binary || 0 == offset)
    return EFI_INVALID_PARAMETER;

  while (offset >= 4) {
    INST instruction = {.val = *(UINT32 *)(Binary->programBuffer + offset - 4)};

    // Check RET
    if (validate_ret(&instruction)) {
      // Got it
      *PhyAddr = offset_to_phy_addr(Binary, offset - 4);
      return EFI_SUCCESS;
    }
    offset -= 4;
  }

  DEBUG(
      (DEBUG_WARN, "RET instruction not found before offset 0x%lX\n", offset));
  *PhyAddr = 0;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
find_first_bl_before_offset(
    IN TE_INFO_STRUCT *Binary, IN UINTN offset, OUT UINTN *PhyAddr)
{
  if (NULL == Binary || 0 == offset)
    return EFI_INVALID_PARAMETER;

  while (offset >= 4) {
    INST instruction = {.val = *(UINT32 *)(Binary->programBuffer + offset - 4)};

    // Check BL
    if (validate_bl(&instruction)) {
      // Got it
      *PhyAddr = offset_to_phy_addr(Binary, offset - 4);
      return EFI_SUCCESS;
    }
    offset -= 4;
  }
  DEBUG((DEBUG_WARN, "BL instruction not found before offset 0x%lX\n", offset));
  *PhyAddr = 0;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
find_first_bl_after_offset(
    IN TE_INFO_STRUCT *Binary, IN UINT64 offset, OUT UINT64 *TargetAddress)
{
  if (NULL == Binary || 0 == offset)
    return EFI_INVALID_PARAMETER;

  while (offset < Binary->fileSize - 4) {
    offset += 4;
    INST instruction = {.val = *(UINT32 *)(Binary->programBuffer + offset)};

    // Check BL
    if (validate_bl(&instruction)) {
      // Got it
      *TargetAddress = offset_to_phy_addr(Binary, offset);
      return EFI_SUCCESS;
    }
  }
  // not found
  DEBUG((DEBUG_WARN, "BL instruction not found after offset 0x%lX\n", offset));
  *TargetAddress = 0;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
get_bl_target_addr(
    IN TE_INFO_STRUCT *Binary, IN UINT64 bl_offset, OUT UINT64 *TargetAddress)
{
  if (NULL == Binary || 0 == bl_offset)
    return EFI_INVALID_PARAMETER;

  INST instruction = {.val = *(UINT32 *)(Binary->programBuffer + bl_offset)};

  // Check BL
  if (validate_bl(&instruction)) {
    // Found a valid BL instruction
    parse_bl(&instruction, bl_offset);
    *TargetAddress = offset_to_phy_addr(Binary, instruction.bl.jumpAddr);
    return EFI_SUCCESS;
  }

  DEBUG((DEBUG_WARN, "BL instruction not found at offset 0x%lX\n", bl_offset));
  *TargetAddress = 0;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS find_caller_addr_of_func_with_offset(
    IN TE_INFO_STRUCT *Binary, IN UINT64 target_offset,
    OUT UINTN *TargetAddress)
{
  // enumerate all BL instructions
  for (UINT32 offset = 0;
       offset < Binary->fileSize - 8 - sizeof(EFI_TE_IMAGE_HEADER);
       offset += 4) {
    INST instruction = {.val = *(UINT32 *)(Binary->programBuffer + offset)};

    // Check BL
    if (validate_bl(&instruction)) {
      // Found a valid BL instruction
      parse_bl(&instruction, offset);
      // Trying jump to address before te region or after te region
      if (instruction.bl.jumpAddr < 0 ||
          instruction.bl.jumpAddr > Binary->teSize)
        continue;

      // Check if the offset equals to our function offset
      if (target_offset == instruction.bl.jumpAddr) {
        *TargetAddress = offset_to_phy_addr(Binary, instruction.bl.pc);
        return EFI_SUCCESS;
      }
    }
  }

  DEBUG((DEBUG_WARN, "Caller Address not found\n"));
  *TargetAddress = 0;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
find_caller_addr_of_func_with_codes(
    IN TE_INFO_STRUCT *Binary, IN UINT8 *insts_buf, IN UINTN insts_size,
    OUT UINTN *PhyAddr)
{
  EFI_STATUS Status       = EFI_SUCCESS;
  UINT64     insts_offset = 0;

  if (NULL == Binary || NULL == insts_buf || 0 == insts_size)
    return EFI_INVALID_PARAMETER;

  Status = find_data_in_buffer(Binary, insts_buf, insts_size, &insts_offset);
  if (EFI_ERROR(Status))
    goto exit;

  Status = find_caller_addr_of_func_with_offset(Binary, insts_offset, PhyAddr);
  if (EFI_ERROR(Status))
    goto exit;

  return EFI_SUCCESS;

exit:
  DEBUG((DEBUG_WARN, "Caller Address not found\n"));
  *PhyAddr = 0;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
find_protocol_scheduler(
    IN TE_INFO_STRUCT *Binary, IN GUID *KeyGuid, OUT UINTN *TargetAddress)
{
  // Find Guid Offset
  UINTN      GuidOffset = 0;
  EFI_STATUS status     = find_guid_in_buffer(Binary, KeyGuid, &GuidOffset);
  if (EFI_ERROR(status)) {
    SfTrace ("[SF] sched GUID not present in TE buffer at all\n\r");
    DEBUG((DEBUG_WARN, "Scheduler guid not found in buffer\n"));
    return EFI_NOT_FOUND;
  }

  SfTrace ("[SF] sched GUID found at TE offset 0x%x\n\r", (UINT32)GuidOffset);

  // Find all ADRP function
  for (UINT32 offset = 0;
       offset < Binary->fileSize - 8 - sizeof(EFI_TE_IMAGE_HEADER);
       offset += 4) {

    INST instruction = {.val = *(UINT32 *)(Binary->TEBuffer + offset)};
    // Check adrp
    if (validate_adrp(&instruction)) {
      // Found a valid adrp instruction
      parse_adrp(&instruction, offset);
      // Check if there is an add function at next next instruction
      INST nnInst = {.val = *(UINT32 *)(Binary->TEBuffer + offset + 8)};
      if (validate_add(&nnInst)) {

        // Get target address
        UINT32 target_address =
            instruction.adrp.RdAfterExecution + nnInst.add.imm12;
        if (target_address == GuidOffset) {
          // Get the adrp instruction before current adrp.
          INST bInst = {.val = *(UINT32 *)(Binary->TEBuffer + offset - 4)};
          // Check adrp
          if (validate_adrp(&bInst)) {
            // Found a valid adrp instruction
            parse_adrp(&bInst, offset);

            // Check Add
            INST nInst = {.val = *(UINT32 *)(Binary->TEBuffer + offset + 4)};
            if (validate_add(&nInst)) {
              // Get target address
              *TargetAddress = bInst.adrp.RdAfterExecution + nInst.add.imm12 +
                               Binary->teHeader->BaseOfCode +
                               Binary->teHeader->ImageBase;
              return EFI_SUCCESS;
            }
          }
        }
      }
    }
  }

  DEBUG((DEBUG_WARN, "Scheduler Protocol Address not found (variant 1)\n"));
  SfTrace ("[SF] sched ADRP/ADD pair NOT found (variant 1)\n\r");
  TargetAddress = 0;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
find_protocol_scheduler_v2(
    IN TE_INFO_STRUCT *Binary, IN GUID *KeyGuid, OUT UINTN *TargetAddress)
{
  // Find Guid Offset
  UINTN      GuidOffset = 0;
  EFI_STATUS status     = find_guid_in_buffer(Binary, KeyGuid, &GuidOffset);
  if (EFI_ERROR(status)) {
    SfTrace ("[SF] sched GUID not present in TE buffer at all\n\r");
    DEBUG((DEBUG_WARN, "Scheduler guid not found in buffer\n"));
    return EFI_NOT_FOUND;
  }

  SfTrace ("[SF] sched GUID found at TE offset 0x%x\n\r", (UINT32)GuidOffset);

  // Find all ADRP function
  for (UINT32 offset = 0;
       offset < Binary->fileSize - 8 - sizeof(EFI_TE_IMAGE_HEADER);
       offset += 4) {

    INST instruction = {.val = *(UINT32 *)(Binary->TEBuffer + offset)};
    // Check adrp
    if (validate_adrp(&instruction)) {
      // Found a valid adrp instruction
      parse_adrp(&instruction, offset);
      // Check if there is an add function at next instruction
      INST nInst = {.val = *(UINT32 *)(Binary->TEBuffer + offset + 4)};
      if (validate_add(&nInst)) {

        // Get target address
        UINT32 target_address =
            instruction.adrp.RdAfterExecution + nInst.add.imm12;
        if (target_address == GuidOffset) {
          // Get the adrp instruction before current adrp.
          INST bInst = {.val = *(UINT32 *)(Binary->TEBuffer + offset - 8)};
          // Check adrp
          if (validate_adrp(&bInst)) {
            // Found a valid adrp instruction
            parse_adrp(&bInst, offset);

            // Check Add
            INST nInst = {.val = *(UINT32 *)(Binary->TEBuffer + offset - 4)};
            if (validate_add(&nInst)) {
              // Get target address
              *TargetAddress = bInst.adrp.RdAfterExecution + nInst.add.imm12 +
                               Binary->teHeader->BaseOfCode +
                               Binary->teHeader->ImageBase;
              return EFI_SUCCESS;
            }
          }
        }
      }
    }
  }

  DEBUG((DEBUG_WARN, "Scheduler Protocol Address not found (variant v2)\n"));
  SfTrace ("[SF] sched ADRP/ADD pair NOT found (variant 2)\n\r");
  TargetAddress = 0;
  return EFI_NOT_FOUND;
}

/**
  Find a protocol by its static registration record instead of by instruction
  patterns.

  The ADRP/ADD heuristics assume the registration code builds the record at
  run time. When XBL instead ships a static table, the record is simply two
  adjacent 64-bit pointers in the data section:

      { &gEfiSecDtbGuid, &SecDtbProtocol }

  So: locate the GUID in the image, compute its run-time address
  (TEBuffer + offset), then scan for a word holding exactly that address and
  check whether the word right after it is a plausible code address.

  @retval TRUE   TargetAddress filled in.
  @retval FALSE  no such record.
**/
STATIC
BOOLEAN
FindPointerPair(
    IN TE_INFO_STRUCT *Binary, IN GUID *KeyGuid, OUT UINTN *TargetAddress)
{
  UINTN  GuidOffset = 0;
  UINT64 ImageBaseTotal;
  UINT64 GuidAddrA;
  UINT64 GuidAddrB;
  UINT64 Low;
  UINT64 High;

  if (EFI_ERROR(find_guid_in_buffer(Binary, KeyGuid, &GuidOffset))) {
    return FALSE;
  }

  //
  // Two candidate run-time addresses for the GUID, because the two bases in
  // play are not necessarily the same:
  //   A = TEBuffer + offset          (the image is loaded at TEBuffer, so this
  //                                   is literally where the bytes sit)
  //   B = BaseOfCode + ImageBase + offset
  //                                  (what find_protocol_scheduler() uses)
  // A pointer stored in .data uses whichever the linker emitted, so test both.
  //
  ImageBaseTotal = (UINT64)(UINTN)Binary->teHeader->BaseOfCode +
                   (UINT64)(UINTN)Binary->teHeader->ImageBase;

  GuidAddrA = (UINT64)(UINTN)Binary->TEBuffer + GuidOffset;
  GuidAddrB = ImageBaseTotal + GuidOffset;

  Low  = (UINT64)(UINTN)Binary->TEBuffer;
  High = Low + Binary->fileSize;

  SfTrace("[SF] pair search: te=0x%lx imgBase+code=0x%lx guidOff=0x%x\n\r",
          (UINT64)(UINTN)Binary->TEBuffer, ImageBaseTotal, (UINT32)GuidOffset);
  SfTrace("[SF]   guidA=0x%lx guidB=0x%lx\n\r", GuidAddrA, GuidAddrB);

  for (UINT32 off = 0; off + 16 <= Binary->fileSize; off += 8) {
    UINT64 *Slot = (UINT64 *)((UINT8 *)Binary->TEBuffer + off);

    if ((Slot[0] != GuidAddrA) && (Slot[0] != GuidAddrB)) {
      continue;
    }

    SfTrace("[SF] pair@+0x%x slot0=0x%lx slot1=0x%lx\n\r", off, Slot[0],
            Slot[1]);

    if ((Slot[1] >= Low) && (Slot[1] < High) && ((Slot[1] & 3) == 0)) {
      *TargetAddress = (UINTN)Slot[1];
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
EFI_STATUS
find_protocol_xbldt(
    IN TE_INFO_STRUCT *Binary, IN GUID *KeyGuid, OUT UINTN *TargetAddress)
{
  // Find Guid Offset
  UINTN      GuidOffset = 0;
  EFI_STATUS status     = find_guid_in_buffer(Binary, KeyGuid, &GuidOffset);
  if (EFI_ERROR(status)) {
    DEBUG((DEBUG_WARN, "XBLDT guid not found in buffer\n"));
    SfTrace("[SF] xbldt GUID not present in TE buffer\n\r");
    return EFI_NOT_FOUND;
  }

  //
  // Try the scheduler patterns FIRST.
  //
  // Both protocols are registered by the same XBL code shape, and the sched
  // search is proven to work on this exact image (it returns 0xA703FD30 for
  // gEfiSchedIntfGuid). The -12/-8 pattern below is a different guess and on
  // shennong it yields 0xA703F0EB - an ODD address, which cannot be an
  // AArch64 function pointer (instructions are 4-byte aligned). That bogus
  // value is what makes "Locate DTBExtnProtocol failed: Not Found" appear.
  //
  status = find_protocol_scheduler(Binary, KeyGuid, TargetAddress);
  if (!EFI_ERROR(status) && (*TargetAddress != 0) &&
      ((*TargetAddress & 3) == 0)) {
    SfTrace("[SF] xbldt via sched-v1 = 0x%lx\n\r", (UINT64)*TargetAddress);
    return EFI_SUCCESS;
  }

  status = find_protocol_scheduler_v2(Binary, KeyGuid, TargetAddress);
  if (!EFI_ERROR(status) && (*TargetAddress != 0) &&
      ((*TargetAddress & 3) == 0)) {
    SfTrace("[SF] xbldt via sched-v2 = 0x%lx\n\r", (UINT64)*TargetAddress);
    return EFI_SUCCESS;
  }

  SfTrace("[SF] xbldt falling back to its own -12/-8 pattern\n\r");

  //
  // Before the instruction-pattern fallback, try the static registration
  // record. Self-test it against gEfiSchedIntfGuid first: that GUID is known
  // to resolve to 0xA703FD38 through the ADRP/ADD path, so if the pair scan
  // finds the same address the method is sound.
  //
  {
    UINTN SelfTest = 0;

    if (FindPointerPair(Binary, &gEfiSchedIntfGuid, &SelfTest)) {
      SfTrace("[SF] selftest sched pair -> 0x%lx (expect 0xA703FD38)\n\r",
              (UINT64)SelfTest);
    } else {
      SfTrace("[SF] selftest: no pointer pair for sched GUID\n\r");
    }
  }

  if (FindPointerPair(Binary, KeyGuid, TargetAddress)) {
    SfTrace("[SF] xbldt via pointer pair = 0x%lx\n\r", (UINT64)*TargetAddress);
    return EFI_SUCCESS;
  }

  SfTrace("[SF] xbldt: no pointer pair either\n\r");

  // Find all ADRP function
  for (UINT32 offset = 0;
       offset < Binary->fileSize - 8 - sizeof(EFI_TE_IMAGE_HEADER);
       offset += 4) {
    INST instruction = {.val = *(UINT32 *)(Binary->TEBuffer + offset)};

    // Check adrp
    if (validate_adrp(&instruction)) {
      // Found a valid adrp instruction
      parse_adrp(&instruction, offset);

      // Check if there is an add function at next instruction
      INST nInst = {.val = *(UINT32 *)(Binary->TEBuffer + offset + 4)};
      if (validate_add(&nInst)) {
        // Get target address
        UINT32 target_address =
            instruction.adrp.RdAfterExecution + nInst.add.imm12;
        if (target_address == GuidOffset) {
          // Get the adrp instruction at 3 instructions before current one.
          INST bbbInst = {.val = *(UINT32 *)(Binary->TEBuffer + offset - 12)};
          // Check adrp
          if (validate_adrp(&bbbInst)) {
            // Found a valid adrp instruction
            parse_adrp(&bbbInst, offset);

            // Check Add
            INST bbInst = {.val = *(UINT32 *)(Binary->TEBuffer + offset - 8)};
            if (validate_add(&bbInst)) {
              // Get target address
              UINTN Candidate = bbbInst.adrp.RdAfterExecution +
                                bbInst.add.imm12 + Binary->teHeader->BaseOfCode +
                                Binary->teHeader->ImageBase;
              // An AArch64 code address must be 4-byte aligned.
              if ((Candidate & 3) == 0) {
                *TargetAddress = Candidate;
                return EFI_SUCCESS;
              }
            }
          }
        }
      }
    }
  }

  DEBUG((DEBUG_WARN, "XBLDT Protocol Address not found\n"));
  SfTrace("[SF] xbldt: no valid aligned address found\n\r");
  *TargetAddress = 0;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
get_scheduler_revision(
    IN TE_INFO_STRUCT *Binary,
    IN UINT64          CallerAddress, // caller of set_scheduler_config
    OUT UINT64        *Revision)
{
  EFI_STATUS Status = EFI_SUCCESS;

  // Find sec protocol revision
  Status = find_first_bl_before_offset(
      Binary, phy_addr_to_offset(Binary, CallerAddress), &CallerAddress);
  if (EFI_ERROR(Status))
    goto err_exit;

  Status = find_first_bl_before_offset(
      Binary, phy_addr_to_offset(Binary, CallerAddress), &CallerAddress);
  if (EFI_ERROR(Status))
    goto err_exit;

  Status = get_bl_target_addr(
      Binary, phy_addr_to_offset(Binary, CallerAddress), &CallerAddress);
  if (EFI_ERROR(Status))
    goto err_exit;

  INST movz = {
      .val = *(UINT32 *)(Binary->programBuffer +
                         phy_addr_to_offset(Binary, CallerAddress))};
  INST movk = {
      .val = *(UINT32 *)(Binary->programBuffer + 4 +
                         phy_addr_to_offset(Binary, CallerAddress))};
  if (validate_movz(&movz) && validate_movk(&movk)) {
    *Revision = parse_movz_with_movk(&movz, &movk);
    return EFI_SUCCESS;
  }

err_exit:
  DEBUG((DEBUG_ERROR, "Failed to parse scheduler revision\n"));
  *Revision = 0;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
FindLKFuncs(IN TE_INFO_STRUCT *Binary, OUT LK_INIT_FUNCTIONS *LkFuncs)
{
  EFI_STATUS Status           = EFI_SUCCESS;
  UINT64     TmpCallerOffset  = 0;
  UINT64     TmpTargetAddress = 0;

  STATIC UINT8 set_scheduler_config_bin[] = {
      0x01, 0x00, 0x40, 0xB9, // ldr w1, [x0]
      0x3F, 0x04, 0x00, 0x71  // cmp w1, #1
  };
  STATIC UINT8 aux_core_entry_bin[] = {
      0x02, 0x20, 0x18, 0xD5, // msr ttbr0_el1, x2
      0xDF, 0x3F, 0x03, 0xD5, // isb
      0x02, 0x00, 0x80, 0x52, // mov w2, #0
      0x22, 0x20, 0x18, 0xD5  // msr ttbr1_el1, x2
  };

  if (NULL == Binary || NULL == LkFuncs)
    return EFI_INVALID_PARAMETER;

  // Check if already initialized
  if (LkFuncs->Initialized == TRUE)
    return EFI_SUCCESS;

  LkFuncs->Initialized = FALSE;

  // Find the place that calls this function
  Status = find_caller_addr_of_func_with_codes(
      Binary, set_scheduler_config_bin, sizeof(set_scheduler_config_bin),
      &TmpCallerOffset);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "Failed to find caller of set_scheduler_config\n"));
    goto exit;
  }

  // Get the first RET before the caller
  Status = find_first_ret_before_offset(
      Binary, phy_addr_to_offset(Binary, TmpCallerOffset), &TmpCallerOffset);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (DEBUG_ERROR,
         "Failed to find RET before caller of set_scheduler_config\n"));
    goto exit;
  }

  // Find the caller of the function after RET
  Status = find_caller_addr_of_func_with_offset(
      Binary, phy_addr_to_offset(Binary, TmpCallerOffset) + 4,
      &TmpCallerOffset);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "Failed to find caller of function after RET\n"));
    goto exit;
  }
  Status = get_bl_target_addr(
      Binary, phy_addr_to_offset(Binary, TmpCallerOffset), &TmpTargetAddress);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (DEBUG_ERROR, "Failed to find target address of function after RET\n"));
    goto exit;
  }
  LkFuncs->lk_gic_config = (VOID *)TmpTargetAddress;

  // Find sec protocol revision
  Status = get_scheduler_revision(Binary, TmpCallerOffset, &LkFuncs->Revision);
  if (EFI_ERROR(Status) || LkFuncs->Revision <= 0x10000) {
    DEBUG((DEBUG_ERROR, "Failed to find scheduler revision\n"));
    goto exit;
  }

  if (LkFuncs->Revision >= 0x10003) {
    UINT64 ret_address                 = 0;
    UINT64 start_scheduler_func_caller = 0;
    // Find revision 1.3 arguements
    Status = find_first_ret_before_offset(
        Binary, phy_addr_to_offset(Binary, TmpCallerOffset), &ret_address);
    // Find who calls this function
    Status = find_caller_addr_of_func_with_offset(
        Binary, phy_addr_to_offset(Binary, ret_address + 4),
        &start_scheduler_func_caller);
    DEBUG(
        (DEBUG_WARN, "StartSchedulerFunc caller at 0x%lX\n",
         start_scheduler_func_caller));
    // Parse ADRL X1, #immediate, it should be right before the BL
    INST adrp_inst = {
        .val = *(
            UINT32 *)(Binary->programBuffer +
                      phy_addr_to_offset(Binary, start_scheduler_func_caller) -
                      8)};
    if (validate_adrp(&adrp_inst)) {
      parse_adrp(
          &adrp_inst,
          phy_addr_to_offset(Binary, start_scheduler_func_caller) - 4);
      INST add_inst = {
          .val = *(UINT32 *)(Binary->programBuffer +
                             phy_addr_to_offset(
                                 Binary, start_scheduler_func_caller) -
                             4)};
      if (validate_add(&add_inst)) {
        LkFuncs->arg1_of_config_and_start_scheduler_func =
            (VOID *)offset_to_phy_addr(
                Binary, adrp_inst.adrp.RdAfterExecution + add_inst.add.imm12);
        DEBUG(
            (EFI_D_WARN, "StartSchedulerFunc arguement at 0x%lX\n",
             (UINTN)LkFuncs->arg1_of_config_and_start_scheduler_func));
      }
    }
  }

  // Find next next BL
  Status = find_first_bl_after_offset(
      Binary, phy_addr_to_offset(Binary, TmpCallerOffset), &TmpCallerOffset);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "Failed to find next BL after lk_gic_config\n"));
    goto exit;
  }
  Status = find_first_bl_after_offset(
      Binary, phy_addr_to_offset(Binary, TmpCallerOffset), &TmpCallerOffset);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "Failed to find next next BL after lk_gic_config\n"));
    goto exit;
  }
  Status = get_bl_target_addr(
      Binary, phy_addr_to_offset(Binary, TmpCallerOffset), &TmpTargetAddress);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (DEBUG_ERROR,
         "Failed to find target address of next BL after lk_gic_config\n"));
    goto exit;
  }
  LkFuncs->lk_init_shutdown1 = (VOID *)TmpTargetAddress;

  if (0x10000 <= LkFuncs->Revision && LkFuncs->Revision <= 0x10002) {
    // Find next BL
    Status = find_first_bl_after_offset(
        Binary, phy_addr_to_offset(Binary, TmpCallerOffset), &TmpCallerOffset);
    if (EFI_ERROR(Status)) {
      DEBUG((DEBUG_ERROR, "Failed to find next BL after lk_init_shutdown1\n"));
      goto exit;
    }
    Status = get_bl_target_addr(
        Binary, phy_addr_to_offset(Binary, TmpCallerOffset), &TmpTargetAddress);
    if (EFI_ERROR(Status)) {
      DEBUG(
          (DEBUG_ERROR, "Failed to find target address of next BL after "
                        "lk_init_shutdown1\n"));
      goto exit;
    }
    LkFuncs->lk_init_shutdown2 = (VOID *)TmpTargetAddress;
  }

  // Find kernel init
  Status = find_first_bl_after_offset(
      Binary, phy_addr_to_offset(Binary, TmpCallerOffset), &TmpCallerOffset);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "Failed to find next BL after lk_init_shutdown2\n"));
    goto exit;
  }
  Status = get_bl_target_addr(
      Binary, phy_addr_to_offset(Binary, TmpCallerOffset), &TmpTargetAddress);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (DEBUG_ERROR,
         "Failed to find target address of next BL after lk_init_shutdown2\n"));
    goto exit;
  }
  LkFuncs->lk_init_scheduler = (VOID *)TmpTargetAddress;

  // Find aux_core_entry
  Status = find_data_in_buffer(
      Binary, aux_core_entry_bin, sizeof(aux_core_entry_bin),
      &TmpTargetAddress);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "Failed to find aux_core_entry\n"));
    goto exit;
  }
  TmpTargetAddress = offset_to_phy_addr(Binary, TmpTargetAddress) -
                     4 * 3; // 3 instructions before
  LkFuncs->lk_aux_cores_entry_point = (VOID *)TmpTargetAddress;

  // Validate all functions physical addresses
  // The physical address Should locate in TE region
  if ((UINTN)LkFuncs->lk_gic_config <
          Binary->teHeader->ImageBase + Binary->teHeader->BaseOfCode ||
      (UINTN)LkFuncs->lk_gic_config > Binary->teHeader->ImageBase +
                                          Binary->teHeader->BaseOfCode +
                                          Binary->teSize ||
      (UINTN)LkFuncs->lk_init_shutdown1 <
          Binary->teHeader->ImageBase + Binary->teHeader->BaseOfCode ||
      (UINTN)LkFuncs->lk_init_shutdown1 > Binary->teHeader->ImageBase +
                                              Binary->teHeader->BaseOfCode +
                                              Binary->teSize ||
      ((LkFuncs->Revision) <= 0x10002 &&
       ((UINTN)LkFuncs->lk_init_shutdown2 <
            Binary->teHeader->ImageBase + Binary->teHeader->BaseOfCode ||
        (UINTN)LkFuncs->lk_init_shutdown2 > Binary->teHeader->ImageBase +
                                                Binary->teHeader->BaseOfCode +
                                                Binary->teSize)) ||
      (UINTN)LkFuncs->lk_init_scheduler <
          Binary->teHeader->ImageBase + Binary->teHeader->BaseOfCode ||
      (UINTN)LkFuncs->lk_init_scheduler > Binary->teHeader->ImageBase +
                                              Binary->teHeader->BaseOfCode +
                                              Binary->teSize ||
      (UINTN)LkFuncs->lk_aux_cores_entry_point <
          Binary->teHeader->ImageBase + Binary->teHeader->BaseOfCode ||
      (UINTN)LkFuncs->lk_aux_cores_entry_point >
          Binary->teHeader->ImageBase + Binary->teHeader->BaseOfCode +
              Binary->teSize) {
    DEBUG((DEBUG_ERROR, "One or more LK function addresses are invalid\n"));
    Status = EFI_NOT_FOUND;
    goto exit;
  }
  else
    LkFuncs->Initialized = TRUE;

exit:
  return Status;
}

VOID InitProtocolFinder(
    IN EFI_PHYSICAL_ADDRESS *ScheAddr, IN EFI_PHYSICAL_ADDRESS *XBLDTOpsAddr)
{
  EFI_STATUS    status               = EFI_SUCCESS;
  STATIC UINT32 EnableMultiThreading = 0;

  // Do search only once
  if (ScheIntrAddr != 0 || SecDTOpsAddr != 0) {
    if (NULL != ScheAddr) {
      *ScheAddr = ScheIntrAddr;
    }

    if (NULL != XBLDTOpsAddr) {
      *XBLDTOpsAddr = SecDTOpsAddr;
    }
    return;
  }

  TE_INFO_STRUCT CoreTE = {0};

  // Find and fill TE info in memory
  if (EFI_ERROR(FindTeAddr(&CoreTE))) {
    SfTrace ("[SF] FindTeAddr failed -> scheduler will stay 0\n\r");
    DEBUG((DEBUG_ERROR, "Failed to find TE address\n"));
    return;
  };

  // Find Scheduler address
  if (NULL != ScheAddr) {
    status =
        find_protocol_scheduler(&CoreTE, &gEfiSchedIntfGuid, &ScheIntrAddr);

    if (!ScheIntrAddr) {
      // Try variant 2
      DEBUG((EFI_D_WARN, "try find scheduler variant 2\n"));
      status = find_protocol_scheduler_v2(
          &CoreTE, &gEfiSchedIntfGuid, &ScheIntrAddr);
    }

    ASSERT(!EFI_ERROR(status));

    if (!EFI_ERROR(status)) {
      ScheIntrAddr = SfValidateAddr ("sched", ScheIntrAddr, &CoreTE);
      // Fill caller's address
      *ScheAddr = ScheIntrAddr;
    }
  }

  // Find XBLDT address
  if (NULL != XBLDTOpsAddr) {
    status = find_protocol_xbldt(&CoreTE, &gEfiSecDtbGuid, &SecDTOpsAddr);
    ASSERT(!EFI_ERROR(status));

    if (!EFI_ERROR(status)) {
      SecDTOpsAddr = SfValidateAddr ("xbldt", SecDTOpsAddr, &CoreTE);
      // Fill caller's address
      *XBLDTOpsAddr = SecDTOpsAddr;
    }
  }

  // Check MultiThreading Enable Flag
  status = LocateConfigurationMapUINT32ByName(
      "EnableMultiThreading", &EnableMultiThreading);
  if (!EFI_ERROR(status) && EnableMultiThreading != 0 &&
      LkInitFuncs.Initialized == FALSE) {
    // Initialize LK functions
    status = FindLKFuncs(&CoreTE, &LkInitFuncs);
  }

  /* Ensure cache coherence */
  WriteBackInvalidateDataCacheRange(&LkInitFuncs, sizeof(LkInitFuncs));

  SfTrace ("[SF] RESULT sched=0x%lx xbldt=0x%lx\n\r", (UINT64)ScheIntrAddr,
           (UINT64)SecDTOpsAddr);
  if (ScheIntrAddr == 0) {
    SfTrace ("[SF] RESULT sched is NULL -> gEfiSchedIntfGuid HOB will be 0,\n\r");
    SfTrace ("[SF]   every Qualcomm driver reading it returns EFI_NOT_FOUND.\n\r");
  }
}

VOID StartUpScheduler(VOID *PeiContinueBoot, VOID *AuxCoresEntryPoint)
{
  EFI_STATUS status = EFI_SUCCESS;

  // Check if LK functions are initialized
  if (LkInitFuncs.Initialized == FALSE) {
    DEBUG(
        (DEBUG_ERROR, "LK functions not initialized or Multithreading not "
                      "enabled, cannot start scheduler!\n"));
    return;
  }

  if (!EFI_ERROR(status)) {
    UINT32                          MaxCoreCnt       = 0;
    UINT32                          EarlyInitCoreCnt = 0;
    ARM_MEMORY_REGION_DESCRIPTOR_EX SchedHeap        = {0};

    // Get Max Core Count
    status = LocateConfigurationMapUINT32ByName("MaxCoreCnt", &MaxCoreCnt);
    if (EFI_ERROR(status) || MaxCoreCnt == 0) {
      MaxCoreCnt = 8; // Default max core count
    }

    // Get Early Init Core Count
    status = LocateConfigurationMapUINT32ByName(
        "EarlyInitCoreCnt", &EarlyInitCoreCnt);
    if (EFI_ERROR(status) || EarlyInitCoreCnt == 0) {
      EarlyInitCoreCnt = 2; // Default early init core count
    }

    status = LocateMemoryMapAreaByName("Sched Heap", &SchedHeap);
    if (EFI_ERROR(status)) {
      DEBUG((DEBUG_ERROR, "Failed to locate \"Sched Heap\"\r\n"));
    }

    // Find init function addresses
    LkInitFuncs.lk_gic_config();
    LkInitFuncs.lk_init_shutdown1(MaxCoreCnt);
    DEBUG(
        (DEBUG_WARN, "\tScheduler Revision\t\t: %d.%d\r\n",
         LkInitFuncs.Revision >> 16, LkInitFuncs.Revision & 0xFFFF));
    DEBUG(
        (EFI_D_WARN,
         "\tMultiThreading\t\t: ON\r\n\tCPU Cores\t\t: %d (init %d)\r\n",
         MaxCoreCnt, EarlyInitCoreCnt));

    if (LkInitFuncs.Revision <= 0x10002) {
      LkInitFuncs.lk_init_shutdown2();

      LkInitFuncs.lk_init_scheduler(
          (VOID *)PeiContinueBoot,
          0, // Args?
          SchedHeap.Address, SchedHeap.Length, AuxCoresEntryPoint, MaxCoreCnt,
          EarlyInitCoreCnt);
    }
    else if (LkInitFuncs.Revision >= 0x10003) {
      DEBUG(
          (EFI_D_ERROR, "                  Init scheduler v3: 0x%08lX",
           (UINT64)LkInitFuncs.arg1_of_config_and_start_scheduler_func
               ->CoreRelatedMask));
      LkInitFuncs.lk_init_scheduler_v3(
          (VOID *)PeiContinueBoot,
          0, // Args?
          SchedHeap.Address, SchedHeap.Length, AuxCoresEntryPoint,
          AuxCoresEntryPoint, MaxCoreCnt, EarlyInitCoreCnt,
          LkInitFuncs.arg1_of_config_and_start_scheduler_func->CoreRelatedMask);
    }

    // Should never reach here
    DEBUG((EFI_D_WARN, "MultiThreading Scheduler failed to start!\n"));
  }

  DEBUG((EFI_D_WARN, "\tMultiThreading\t\t: OFF\n"));
}

VOID BootSecondaryCpu(UINTN CpuIdx)
{
  if (LkInitFuncs.lk_aux_cores_entry_point != NULL)
    LkInitFuncs.lk_aux_cores_entry_point(CpuIdx);

  DEBUG((EFI_D_WARN, "\tCore %d hang up\t\t\n", CpuIdx));

  // Boot Failed?
  while (1) {
    asm volatile("wfi\n\t" : : : "memory");
  }
}
