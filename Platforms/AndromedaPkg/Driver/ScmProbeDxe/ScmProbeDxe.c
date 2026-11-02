/** @file
  One boot, several answers.

  Every open question in this bring-up currently costs a flash cycle, and the
  boot log is read from a photograph. This driver runs once, right after
  ScmDxeCompat and before TzDxeLA, and reports the state that the next few
  decisions depend on:

    1. Is gQcomScmProtocolGuid published, and where does it point?
    2. How far did ScmDxeCompat's smcinvoke init actually get? Its own globals
       are readable through the protocol pointer, because the protocol instance
       lives at RVA 0xB100 inside that module.
    3. Does the plain-SMC path reach TrustZone? ScmSipSysCall with
       TZ_INFO_GET_FEATURE_VERSION_ID is the cheapest possible probe, and it is
       the path QSEECom and TzDxe actually use - unlike smcinvoke.
    4. Is the ShmBridge protocol the stub or the real driver?
    5. Is the DXE-side configuration database alive, and is the block stack
       still up? Those decide whether ConfigWordsDxe and SecParti can work.

  Output goes through SerialPortWrite, not DEBUG: DEBUG is unreliable here and
  every other probe in this port does the same.
**/

//
// Bumped by hand on every image handed over. See the print in the entry point.
//
#define PROBE_BUILD_TAG  "b319-tz-abi-readonly"

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/ConfigurationMapHelperLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/PartitionInfo.h>

//
// ConfigurationMapHelperLib is safe to use now.
//
// Its implementation pulls in PlatformConfigurationMapLib, which reaches PcdLib,
// which used to be DxePcdLib - and DxePcdLib's DEPEX on gEfiPcdProtocolGuid was
// merged into this module, so it never dispatched. PcdLib now maps to
// BasePcdLibNull and the dependency is gone, which is why the config lookups are
// back. SecurityFlag matters in particular: LoadSecureApps gates the whole
// app-start block on bit 7 of it.
//

//
// gQcomScmProtocolGuid, read out of ScmDxeCompat's .data at RVA 0xB098.
//
STATIC EFI_GUID  mScmProtocolGuid = {
  0x77ED108D, 0x8524, 0x4B8B, { 0x9D, 0x2E, 0x34, 0x98, 0x7A, 0xEC, 0xB9, 0xC1 }
};

//
// The Qualcomm ShmBridge protocol.
//
STATIC EFI_GUID  mShmBridgeGuid = {
  0x9C1EB71F, 0xDD6C, 0x4ED5, { 0x9F, 0x6A, 0x5C, 0xC0, 0xCA, 0x78, 0x9F, 0x16 }
};

//
// Only the two methods that matter here. The table in ScmDxeCompat has
// Revision at +0x00, ScmSendCommand at +0x28 and ScmSipSysCall at +0x38.
//
typedef
EFI_STATUS
(EFIAPI *SCM_SIP_SYS_CALL)(
  IN VOID     *This,
  IN UINT32   SmcId,
  IN UINT32   ParamId,
  IN UINT64   *Params,
  OUT UINT64  *Results
  );

typedef EFI_STATUS (EFIAPI *SCM_SEND_CMD)(VOID *, UINT32, UINT32 *, VOID *, UINTN, VOID *, UINTN);

typedef struct {
  UINT64            Revision;
  UINT64            Method08;
  UINT64            Method10;
  UINT64            Method18;
  UINT64            Method20;
  SCM_SEND_CMD      ScmSendCommand;   // +0x28
  UINT64            Method30;
  SCM_SIP_SYS_CALL  ScmSipSysCall;    // +0x38
} SCM_PROTOCOL_MIN;

//
// ScmDxeCompat's smcinvoke state, addressed relative to the protocol instance.
//
#define SCM_RVA_PROTOCOL  0xB100
#define SCM_RVA_B548      0xB548
#define SCM_RVA_B550      0xB550
#define SCM_RVA_B558      0xB558
#define SCM_RVA_B560      0xB560
#define SCM_RVA_B568      0xB568

STATIC
VOID
Sp (
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

  SerialPortWrite ((UINT8 *)Buffer, Length);
}

STATIC
VOID
CountHandles (
  IN CONST CHAR8  *Tag,
  IN EFI_GUID     *Guid
  )
{
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count    = 0;
  EFI_STATUS  Status;

  Status = gBS->LocateHandleBuffer (ByProtocol, Guid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Sp ("[SP] %a: none (%r)\n\r", Tag, Status);
    return;
  }

  Sp ("[SP] %a: %d handle(s)\n\r", Tag, (UINT64)Count);
  if (Handles != NULL) {
    gBS->FreePool (Handles);
  }
}

/**
  Look for the Keymaster partition and report what is actually in it.

  TzDxeLA asks the loader for a partition named "keymaster" and this device only
  has keymaster_a / keymaster_b, so the label was patched to the suffixed name.
  That moved the failure from EFI_NOT_FOUND to EFI_LOAD_ERROR, which means the
  partition is now found and the content is what fails.

  So print the name, the first sixteen bytes and the size. An ELF starts 7F 45 4C 46,
  a PE starts 4D 5A. If the loader wants one and the partition holds the other, that
  is the whole answer.
**/
STATIC
VOID
ProbeKeymasterPartition (
  VOID
  )
{
  EFI_HANDLE                     *Handles = NULL;
  UINTN                          Count    = 0;
  UINTN                          Index;
  UINTN                          Hits     = 0;
  EFI_STATUS                     Status;
  EFI_PARTITION_INFO_PROTOCOL    *Info;
  EFI_BLOCK_IO_PROTOCOL          *BlockIo;
  UINT8                          *Buffer;
  CHAR16                         *Name;
  CHAR8                          Ascii[37];
  UINTN                          NameLen;
  UINTN                          I;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPartitionInfoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    Sp ("[SP] partitions: cannot enumerate (%r)\n\r", Status);
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Info = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info)) ||
        (Info == NULL)) {
      continue;
    }

    //
    // Only GPT entries carry a name. Type 2 is EFI_PARTITION_INFO_PROTOCOL_GPT.
    //
    if (Info->Type != 2) {
      continue;
    }

    Name    = Info->Info.Gpt.PartitionName;
    NameLen = 0;
    while ((NameLen < 36) && (Name[NameLen] != 0)) {
      NameLen++;
    }
    if (NameLen == 0) {
      continue;
    }

    //
    // Substring match for "keymaster" without pulling in a string library.
    //
    {
      UINTN   J;
      BOOLEAN Match = FALSE;

      for (I = 0; (I + 9) <= NameLen; I++) {
        for (J = 0; J < 9; J++) {
          CHAR16 C = Name[I + J];
          if ((C >= L'A') && (C <= L'Z')) {
            C = (CHAR16)(C - L'A' + L'a');
          }
          if (C != (CHAR16)"keymaster"[J]) {
            break;
          }
        }
        if (J == 9) {
          Match = TRUE;
          break;
        }
      }
      if (!Match) {
        continue;
      }
    }

    Hits++;

    for (I = 0; I < NameLen && I < 36; I++) {
      Ascii[I] = (Name[I] < 0x80) ? (CHAR8)Name[I] : '?';
    }
    Ascii[I] = '\0';

    BlockIo = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo)) ||
        (BlockIo == NULL) || (BlockIo->Media == NULL)) {
      Sp ("[SP] part %a: no BlockIo\n\r", Ascii);
      continue;
    }

    Sp ("[SP] part %a: %d blocks x %d bytes = %d KiB\n\r",
        Ascii,
        (UINT64)BlockIo->Media->LastBlock + 1,
        (UINT64)BlockIo->Media->BlockSize,
        ((UINT64)BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize / 1024);

    //
    // ReadBlocks requires a buffer that is a whole number of blocks. The first
    // version of this probe passed 16 bytes and got EFI_BAD_BUFFER_SIZE back -
    // which is worth remembering, because a loader that asks for a partial block
    // fails the same way, and this device's logical block is 4096, not 512.
    //
    Buffer = NULL;
    Status = gBS->AllocatePool (EfiBootServicesData, BlockIo->Media->BlockSize, (VOID **)&Buffer);
    if (EFI_ERROR (Status) || (Buffer == NULL)) {
      Sp ("[SP]   no buffer for a %d byte block (%r)\n\r",
          (UINT64)BlockIo->Media->BlockSize, Status);
      continue;
    }

    ZeroMem (Buffer, BlockIo->Media->BlockSize);
    Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, BlockIo->Media->BlockSize, Buffer);
    Sp ("[SP]   read block 0 -> %r  head=%02x%02x%02x%02x %02x%02x%02x%02x  %c%c%c%c\n\r",
        Status,
        Buffer[0], Buffer[1], Buffer[2], Buffer[3],
        Buffer[4], Buffer[5], Buffer[6], Buffer[7],
        (Buffer[0] >= 32 && Buffer[0] < 127) ? Buffer[0] : '.',
        (Buffer[1] >= 32 && Buffer[1] < 127) ? Buffer[1] : '.',
        (Buffer[2] >= 32 && Buffer[2] < 127) ? Buffer[2] : '.',
        (Buffer[3] >= 32 && Buffer[3] < 127) ? Buffer[3] : '.');

    //
    // If it is an ELF, the class matters: Qualcomm ships both. abl.elf and
    // aop.mbn are ELF32, devcfg.mbn is ELF64. A loader that assumes one class
    // rejects the other, and all it reports is EFI_LOAD_ERROR.
    //
    if (!EFI_ERROR (Status) &&
        (Buffer[0] == 0x7F) && (Buffer[1] == 'E') && (Buffer[2] == 'L') && (Buffer[3] == 'F')) {
      if (Buffer[4] == 1) {
        UINT32  Entry = *(UINT32 *)(Buffer + 0x18);
        UINT32  PhOff = *(UINT32 *)(Buffer + 0x1C);
        UINT16  PhNum = *(UINT16 *)(Buffer + 0x2C);
        UINT16  Machine = *(UINT16 *)(Buffer + 0x12);
        Sp ("[SP]   ELF32  machine=%d entry=0x%x phoff=0x%x phnum=%d\n\r",
            (UINT64)Machine, (UINT64)Entry, (UINT64)PhOff, (UINT64)PhNum);
      } else if (Buffer[4] == 2) {
        UINT64  Entry = *(UINT64 *)(Buffer + 0x18);
        UINT64  PhOff = *(UINT64 *)(Buffer + 0x20);
        UINT16  PhNum = *(UINT16 *)(Buffer + 0x38);
        UINT16  Machine = *(UINT16 *)(Buffer + 0x12);
        Sp ("[SP]   ELF64  machine=%d entry=0x%lx phoff=0x%lx phnum=%d\n\r",
            (UINT64)Machine, Entry, PhOff, (UINT64)PhNum);
      } else {
        Sp ("[SP]   ELF with unknown class %d\n\r", (UINT64)Buffer[4]);
      }
    }

    //
    // Then read the whole partition in one call. This is what an app loader does,
    // and it is where a 512-byte assumption breaks: the logical block here is 4096,
    // so any buffer size that is not a multiple of it comes back EFI_BAD_BUFFER_SIZE
    // rather than short data. If this succeeds and the loader still fails, the
    // problem is not the read.
    //
    {
      UINT64  Total = ((UINT64)BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize;
      UINT8   *Whole = NULL;

      if (Total > (16 * 1024 * 1024)) {
        Sp ("[SP]   whole-partition read skipped, %d KiB is too large\n\r", Total / 1024);
      } else if (EFI_ERROR (gBS->AllocatePool (EfiBootServicesData, (UINTN)Total, (VOID **)&Whole)) ||
                 (Whole == NULL)) {
        Sp ("[SP]   whole-partition read skipped, cannot allocate %d KiB\n\r", Total / 1024);
      } else {
        ZeroMem (Whole, (UINTN)Total);
        Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, (UINTN)Total, Whole);
        Sp ("[SP]   whole partition read (%d KiB) -> %r\n\r", Total / 1024, Status);
        if (!EFI_ERROR (Status)) {
          UINT64  Last = Total - 1;
          while ((Last > 0) && (Whole[Last] == 0)) {
            Last--;
          }
          Sp ("[SP]   last non-zero byte at 0x%lx (%d bytes of content)\n\r",
              Last, Last + 1);
        }
        gBS->FreePool (Whole);
      }
    }

    gBS->FreePool (Buffer);
  }

  Sp ("[SP] keymaster partitions found: %d of %d\n\r", (UINT64)Hits, (UINT64)Count);
  gBS->FreePool (Handles);
}


STATIC SCM_SEND_CMD mNativeSend;
STATIC UINTN mCommandCount;

STATIC BOOLEAN
TraceReadable (UINTN Address, UINTN Length)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  if (Address == 0 || Length == 0 || EFI_ERROR (gDS->GetMemorySpaceDescriptor (Address, &D))) return FALSE;
  return (BOOLEAN)(D.GcdMemoryType == EfiGcdMemoryTypeSystemMemory && !(D.Attributes & EFI_MEMORY_RP) &&
    Address >= D.BaseAddress && Address - D.BaseAddress <= D.Length &&
    Length <= D.Length - (Address - D.BaseAddress));
}

STATIC VOID
TraceAppBuffer (VOID *Req, UINTN ReqSize)
{
  UINT64 Size, Address, Metadata;
  UINT32 Crc;
  UINT8 *Elf;
  UINT16 Count, Stride;
  UINT64 Offset;
  UINTN I;
  if ((ReqSize != 24 && ReqSize != 56) || !TraceReadable ((UINTN)Req, ReqSize)) return;
  // ScmSendCommand RVA 2610/2624/2630 reads three UINT64 values.
  // The compat path RVA 1FE4 passes {address=word2, size=word1} to 4F04.
  Metadata = ReadUnaligned64 ((UINT64 *)Req);
  Size = ReadUnaligned64 ((UINT64 *)((UINT8 *)Req + 8));
  Address = ReadUnaligned64 ((UINT64 *)((UINT8 *)Req + 16));
  Sp ("[TZ b319] load request bytes=%u words=%lx/%lx/%lx\r\n", ReqSize, Metadata, Size, Address);
  if (Size < 64 || Size > 16 * 1024 * 1024 || !TraceReadable ((UINTN)Address, (UINTN)Size)) {
    Sp ("[TZ b319] load buffer not safely readable size=%lu address=%lx\r\n", Size, Address);
    return;
  }
  Elf = (UINT8 *)(UINTN)Address;
  gBS->CalculateCrc32 (Elf, (UINTN)Size, &Crc);
  Sp ("[TZ b319] load ELF bytes=%lu metadata=%lu crc32=%08x magic=%08x class=%u\r\n",
      Size, Metadata, Crc, ReadUnaligned32 ((UINT32 *)Elf), Elf[4]);
  if (ReadUnaligned32 ((UINT32 *)Elf) != 0x464C457F || Elf[4] != 2 || Elf[5] != 1) return;
  Offset = ReadUnaligned64 ((UINT64 *)(Elf + 32));
  Stride = ReadUnaligned16 ((UINT16 *)(Elf + 54));
  Count = ReadUnaligned16 ((UINT16 *)(Elf + 56));
  if (Stride < 56 || Offset > Size || Count > 64 || (UINT64)Count * Stride > Size - Offset) return;
  for (I = 0; I < Count; I++) {
    UINT8 *P = Elf + Offset + I * Stride;
    UINT32 Flags = ReadUnaligned32 ((UINT32 *)(P + 4));
    UINT64 FileOffset = ReadUnaligned64 ((UINT64 *)(P + 8));
    UINT64 FileSize = ReadUnaligned64 ((UINT64 *)(P + 32));
    if (FileOffset > Size || FileSize > Size - FileOffset) {
      Sp ("[TZ b319] ELF ph=%u out-of-range off=%lx size=%lx\r\n", I, FileOffset, FileSize);
    } else if (I < 12) {
      gBS->CalculateCrc32 (Elf + FileOffset, (UINTN)FileSize, &Crc);
      Sp ("[TZ b319] ELF ph=%u flags=%08x off=%lx bytes=%lx crc32=%08x\r\n", I, Flags, FileOffset, FileSize, Crc);
    }
  }
}

STATIC VOID TraceLoadedSlots (VOID *This)
{
  UINTN Base = (UINTN)This - SCM_RVA_PROTOCOL;
  UINTN Table, I;
  if (!TraceReadable (Base + SCM_RVA_B560, 8)) return;
  Table = (UINTN)ReadUnaligned64 ((UINT64 *)(Base + SCM_RVA_B560));
  // Native 4F04/522C use 32 slots, stride C0, name bytes 38..B7,
  // length B8, object method/context 20/28 and app handle 34.
  if (!TraceReadable (Table, 32 * 0xC0)) return;
  for (I = 0; I < 32; I++) {
    UINT8 *Slot = (UINT8 *)Table + I * 0xC0;
    UINT64 Length = ReadUnaligned64 ((UINT64 *)(Slot + 0xB8));
    UINT32 Id = ReadUnaligned32 ((UINT32 *)(Slot + 0x34));
    CHAR8 Name[129];
    UINTN J;
    if (Id == 0) continue;
    if (Length > 128) { Sp ("[TZ b319] slot=%u invalid name length=%lu\r\n", I, Length); continue; }
    ZeroMem (Name, sizeof (Name));
    for (J = 0; J < Length; J++) Name[J] = Slot[0x38+J] >= 0x20 && Slot[0x38+J] <= 0x7e ? Slot[0x38+J] : '.';
    Sp ("[TZ b319] slot=%u id=%08x state=%u name-bytes=%lu name=%a object=%lx/%lx (loader result, service untested)\r\n",
      I, Id, ReadUnaligned32 ((UINT32 *)(Slot + 0x30)), Length, Name,
      ReadUnaligned64 ((UINT64 *)(Slot + 0x20)), ReadUnaligned64 ((UINT64 *)(Slot + 0x28)));
  }
}

STATIC EFI_STATUS EFIAPI
TraceSendCommand (VOID *This, UINT32 Cmd, UINT32 *AppId, VOID *Req, UINTN ReqSize, VOID *Rsp, UINTN RspSize)
{
  EFI_STATUS Status;
  UINTN Call = ++mCommandCount;
  CHAR8 Name[33];
  ZeroMem (Name, sizeof (Name));
  // Only QUERY_APPID has a verified plain-string request layout. Do not dump
  // keys, credentials or opaque TA requests into the boot log.
  if (Cmd == 3 && ReqSize <= 32 && TraceReadable ((UINTN)Req, ReqSize)) CopyMem (Name, Req, ReqSize);
  if (Call <= 48) Sp ("[TZ b319] call=%u cmd=%u name=%a app-in=%08x req=%u rsp=%u enter\r\n",
    Call, Cmd, Name, AppId == NULL ? 0 : *AppId, ReqSize, RspSize);
  if (Call <= 48 && Cmd == 1) TraceAppBuffer (Req, ReqSize);
  // EFIScm.h commands 15/16 provision and erase RPMB. This session never
  // authorizes either operation; do not rely on a TA returning an error.
  if (Cmd == 15 || Cmd == 16 || Cmd == 6) {
    if (Call <= 48) Sp ("[TZ b319] storage mutation / opaque TA data request refused\r\n");
    return EFI_WRITE_PROTECTED;
  }
  Status = mNativeSend (This, Cmd, AppId, Req, ReqSize, Rsp, RspSize);
  if (Call <= 48) Sp ("[TZ b319] call=%u cmd=%u leave %r app-out=%08x\r\n",
    Call, Cmd, Status, AppId == NULL ? 0 : *AppId);
  if (Call <= 48 && Cmd == 1) TraceLoadedSlots (This);
  return Status;
}

EFI_STATUS
EFIAPI
ScmProbeDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS        Status;
  SCM_PROTOCOL_MIN  *Scm = NULL;
  UINTN             Base;
  UINT64            Params[10];
  UINT64            Results[6];
  VOID              *Shm  = NULL;

  Sp ("\n\r[SP] ===== SCM / ShmBridge / config probe =====\n\r");
  //
  // Build stamp. Several times now a log arrived and there was no way to tell which
  // image had produced it - a fix that changes no visible output looks exactly like a
  // build that was never flashed.
  //
  // __DATE__ / __TIME__ turned out to be useless for this: they only change when this
  // file is recompiled, and most fixes touch the FDF or the APRIORI list instead. So
  // the tag below is bumped by hand on every image that is handed over.
  //
  Sp ("[SP] probe build tag %a  (compiled %a %a)\n\r", PROBE_BUILD_TAG, __DATE__, __TIME__);

  //
  // 1. Is the SCM protocol there at all?
  //
  Status = gBS->LocateProtocol (&mScmProtocolGuid, NULL, (VOID **)&Scm);
  if (EFI_ERROR (Status) || (Scm == NULL)) {
    Sp ("[SP] SCM protocol: NOT FOUND (%r)\n\r", Status);
    goto Config;
  }

  Sp ("[SP] SCM protocol: 0x%lx  Revision=0x%lx\n\r",
      (UINT64)(UINTN)Scm, Scm->Revision);

  // Only interpret RVAs and replace a method on the known native image.
  Base = (UINTN)Scm - SCM_RVA_PROTOCOL;
  if (Scm->Revision != 0x50002 || (UINTN)Scm->ScmSipSysCall != Base + 0x2410 ||
      (UINTN)Scm->ScmSendCommand != Base + 0x2554) {
    Sp ("[TZ b319] unknown SCM image layout; native state/hook skipped\r\n");
    goto Config;
  }
  mNativeSend = Scm->ScmSendCommand;
  Scm->ScmSendCommand = TraceSendCommand;
  Sp ("[TZ b319] command trace installed; ordinary SIP and TA results are separate\r\n");
  Sp ("[TZ b319] smcinvoke slots local=%lu remote=%lu handles=%lu stride=%lu\r\n",
      *(UINT64 *)(Base+0xB170), *(UINT64 *)(Base+0xB178), *(UINT64 *)(Base+0xB180), *(UINT64 *)(Base+0xB440));

  //
  // 2. ScmDxeCompat's own globals. The protocol instance sits at RVA 0xB100, so
  //    subtracting that gives the module base and the smcinvoke slots become
  //    readable. Zero here means the transport setup never completed.
  //
  Base = (UINTN)Scm - SCM_RVA_PROTOCOL;
  Sp ("[SP] ScmDxeCompat base = 0x%lx\n\r", (UINT64)Base);
  Sp ("[SP]   +0xB548=0x%lx +0xB550=0x%lx\n\r",
      (UINT64)*(UINTN *)(Base + SCM_RVA_B548),
      (UINT64)*(UINTN *)(Base + SCM_RVA_B550));
  Sp ("[SP]   +0xB558=0x%lx +0xB560=0x%lx\n\r",
      (UINT64)*(UINTN *)(Base + SCM_RVA_B558),
      (UINT64)*(UINTN *)(Base + SCM_RVA_B560));
  Sp ("[SP]   +0xB568=0x%lx (object count)\n\r",
      (UINT64)*(UINTN *)(Base + SCM_RVA_B568));

  //
  // clientEnvObj. This is the whole story in one word.
  //
  // SmcInvokeInit stores the smcinvoke client environment here, and
  // FUN_00003318 refuses to hand out the transport unless it is non-NULL:
  //
  //   if (DAT_0000b430 == 0) { "clientEnvObj is NULL object !!!"; return DEVICE_ERROR; }
  //
  // FUN_00004d28 then returns before it ever writes 0xB550, which is why the object
  // layer later calls a null pointer. If this reads 0, smcinvoke never came up and
  // the rest of the chain follows from it.
  //
  Sp ("[SP]   +0xB430=0x%lx (clientEnvObj)  +0xB448=0x%lx (buffer)\n\r",
      (UINT64)*(UINTN *)(Base + 0xB430),
      (UINT64)*(UINTN *)(Base + 0xB448));

  //
  // 3. The plain-SMC path, which is the one QSEECom and TzDxe use. If this
  //    answers, SCM works and only smcinvoke is broken.
  //
  if (Scm->ScmSipSysCall != NULL) {
    ZeroMem (Params, sizeof (Params));
    ZeroMem (Results, sizeof (Results));
    Params[0] = 6;                                 // QSEE feature ID, not nargs

    Status = Scm->ScmSipSysCall (Scm, 0x02000603, 1, Params, Results);
    Sp ("[SP] ScmSipSysCall(0x02000603) -> %r\n\r", Status);
    Sp ("[SP]   results: %016lx %016lx %016lx\n\r",
        Results[0], Results[1], Results[2]);
    {
      STATIC CONST UINT32 Calls[] = { 0x02000205, 0x32000101, 0x32000103, 0x02000C02, 0x02001501 };
      UINTN Index;
      // INFO/IS_CALL_AVAILABLE only queries capabilities; it does not execute
      // PIL authentication, load a TA, or change SMMU/security configuration.
      for (Index = 0; Index < ARRAY_SIZE (Calls); Index++) {
        ZeroMem (Params, sizeof (Params));
        ZeroMem (Results, sizeof (Results));
        Params[0] = Calls[Index];
        Status = Scm->ScmSipSysCall (Scm, 0x02000601, 1, Params, Results);
        Sp ("[SMC b319] available %08x -> %r response=%lx/%lx/%lx\n\r",
            Calls[Index], Status, Results[0], Results[1], Results[2]);
      }
    }
  } else {
    Sp ("[SP] ScmSipSysCall pointer is NULL - table is not what we think\n\r");
  }

  //
  // 4. ShmBridge: stub or real? The first word is its Version, and the first
  //    method pointer tells us which image it lives in.
  //
  Status = gBS->LocateProtocol (&mShmBridgeGuid, NULL, &Shm);
  if (EFI_ERROR (Status) || (Shm == NULL)) {
    Sp ("[SP] ShmBridge: NOT FOUND (%r)\n\r", Status);
  } else {
    Sp ("[SP] ShmBridge: 0x%lx  rev=0x%lx  alloc=0x%lx\n\r",
        (UINT64)(UINTN)Shm,
        (UINT64)*(UINTN *)Shm,
        (UINT64)*(UINTN *)((UINT8 *)Shm + 8));
  }

Config:
  //
  // Configuration values that decide whether the app-start path is even taken.
  // LoadSecureApps returns early unless SecurityFlag has bit 7 set, and
  // QseeComProtocolInit looks for LoadCommonLibEarly.
  //
  {
    STATIC CONST CHAR8  *Keys[] = {
      "SecurityFlag",
      "LoadCommonLibEarly",
      "SharedIMEMBaseAddr",
      "SmcInvokeConfig",
      "SmcInvokeMaxLocalObj",
      "EnableMultiThreading"
    };
    UINTN   K;
    UINT32  V;

    for (K = 0; K < sizeof (Keys) / sizeof (Keys[0]); K++) {
      V      = 0;
      Status = LocateConfigurationMapUINT32ByName ((CHAR8 *)Keys[K], &V);
      Sp ("[SP] cfg %a -> %r value=0x%x\n\r", Keys[K], Status, (UINT64)V);
    }
  }

  //
  // 5. Is the block stack still up? That decides whether a partition lookup can
  //    work at all, which is what Keymaster and ConfigWords both depend on.
  //
  CountHandles ("BlockIo", &gEfiBlockIoProtocolGuid);
  CountHandles ("PartitionInfo", &gEfiPartitionInfoProtocolGuid);
  CountHandles ("SimpleFileSystem", &gEfiSimpleFileSystemProtocolGuid);

  ProbeKeymasterPartition ();

  Sp ("[SP] ===== end probe =====\n\r");
  return EFI_SUCCESS;
}
