// Copyright (c) 2026, ProjectMu shennong port.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Identify the volume, validate bounded MDT headers and required split-file sizes.
// File reachability does not establish PIL volume selection or authentication.
// No partition writes, subsystem loads or security calls are performed here.
//
#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include "AdspMdt.h"

STATIC BOOLEAN  mProbed = FALSE;
STATIC UINTN    mProbeCount = 0;
STATIC EFI_GUID mPilGuid = { 0x1F4F1DA3, 0x4D8D, 0x4894,
                            { 0xBA, 0x2A, 0x13, 0xD3, 0x20, 0x7F, 0x97, 0x39 } };

STATIC
VOID
AdspSay (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Buffer[224];
  VA_LIST  Marker;
  UINTN    Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  SerialPortWrite ((UINT8 *)Buffer, Length);
}

STATIC EFI_STATUS
AdspProbePilProtocol (VOID)
{
  EFI_STATUS Status;
  UINT64 *Pil = NULL;

  Status = gBS->LocateProtocol (&mPilGuid, NULL, (VOID **)&Pil);
  if (EFI_ERROR (Status) || Pil == NULL) {
    AdspSay ("[ADSP b213] PIL public protocol -> %r present=0\n", Status);
    return EFI_NOT_FOUND;
  }
  // Exact table layout verified in the extracted stock PILDxe at RVA191B8.
  // Only inspect a table of the verified revision; no load/security methods called.
  AdspSay ("[ADSP b213] PIL public protocol revision=%lx\n", Pil[0]);
  if (Pil[0] != 0x10005) {
    AdspSay ("[ADSP b213] unexpected PIL revision; extended table not inspected\n");
    return EFI_UNSUPPORTED;
  }
  AdspSay ("[ADSP b213] PIL load-by-name=%lx config-by-name=%lx; inspect only, no calls\n",
           Pil[1], Pil[6]);
  return EFI_SUCCESS;
}

STATIC VOID EFIAPI
AdspOnPilInstall (IN EFI_EVENT Event, IN VOID *Context)
{
  // Separate from filesystem probing: PILDxe dispatches after this APRIORI probe.
  AdspProbePilProtocol ();
  gBS->CloseEvent (Event);
}

STATIC EFI_STATUS
AdspFileSize (
  IN EFI_FILE_PROTOCOL *File,
  OUT UINT64           *Size
  )
{
  EFI_FILE_INFO *Info;
  EFI_STATUS    Status;
  UINTN         InfoSize;

  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if ((Status != EFI_BUFFER_TOO_SMALL) || (InfoSize < SIZE_OF_EFI_FILE_INFO) ||
      (InfoSize > 4096)) {
    return EFI_COMPROMISED_DATA;
  }

  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (!EFI_ERROR (Status)) {
    if ((InfoSize < SIZE_OF_EFI_FILE_INFO) || ((Info->Attribute & EFI_FILE_DIRECTORY) != 0)) {
      Status = EFI_COMPROMISED_DATA;
    } else {
      *Size = Info->FileSize;
    }
  }

  FreePool (Info);
  return Status;
}

STATIC BOOLEAN
AdspCheckImage (
  IN EFI_FILE_PROTOCOL *Dir,
  IN CONST CHAR16      *Prefix,
  IN UINT64            ReservedStart,
  IN UINT64            ReservedSize,
  IN BOOLEAN           OverrideAddress
  )
{
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS        Status;
  CHAR16            Name[48];
  UINT64            Size;
  UINTN             ReadSize;
  UINT8             *Data;
  ADSP_MDT_PLAN     *Plan;
  CONST CHAR8       *Error;
  UINTN             Index;
  UINT32            Required;
  UINT32            Bad;
  BOOLEAN           RawFits;
  BOOLEAN           SpanFits;
  BOOLEAN           Valid;

  File = NULL;
  UnicodeSPrint (Name, sizeof (Name), L"%s.mdt", Prefix);
  Status = Dir->Open (Dir, &File, Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || (File == NULL)) {
    AdspSay ("[ADSP] %s -> %r\n", Name, Status);
    return FALSE;
  }

  Size = 0;
  Status = AdspFileSize (File, &Size);
  if (EFI_ERROR (Status) || (Size < 52) || (Size > ADSP_MDT_MAX_SIZE)) {
    AdspSay ("[ADSP] %s size=%Lu rejected (%r)\n", Name, Size, Status);
    File->Close (File);
    return FALSE;
  }

  Data = AllocatePool ((UINTN)Size);
  Plan = AllocateZeroPool (sizeof (*Plan));
  if ((Data == NULL) || (Plan == NULL)) {
    if (Data != NULL) { FreePool (Data); }
    if (Plan != NULL) { FreePool (Plan); }
    File->Close (File);
    AdspSay ("[ADSP] %s allocation failed\n", Name);
    return FALSE;
  }

  ReadSize = (UINTN)Size;
  Status = File->Read (File, &ReadSize, Data);
  File->Close (File);
  if (EFI_ERROR (Status) || (ReadSize != Size)) {
    AdspSay ("[ADSP] %s short/failed read=%u (%r)\n", Name, (UINT32)ReadSize, Status);
    FreePool (Data);
    FreePool (Plan);
    return FALSE;
  }

  Error = AdspMdtInspect (Data, ReadSize, Plan);
  FreePool (Data);
  if (Error != NULL) {
    AdspSay ("[ADSP] %s invalid: %a\n", Name, Error);
    FreePool (Plan);
    return FALSE;
  }

  RawFits = (BOOLEAN)((Plan->MinAddress >= ReservedStart) &&
                      (Plan->EndAddress <= ReservedStart + ReservedSize));
  SpanFits = (BOOLEAN)(Plan->EndAddress - Plan->MinAddress <= ReservedSize);
  AdspSay ("[ADSP] %s size=%Lu machine=%x entry=%08x ph=%u load=%u bss=%u split=%u reloc=%u\n",
           Name, Size, Plan->Machine, Plan->Entry, Plan->PhdrCount, Plan->LoadCount,
           Plan->BssCount, Plan->Split, Plan->Relocatable);
  AdspSay ("[ADSP] %s range=%Lx..%Lx cfg=%Lx+%Lx rawfits=%u spanfits=%u override=%u\n",
           Prefix, Plan->MinAddress, Plan->EndAddress, ReservedStart, ReservedSize,
           RawFits, SpanFits, OverrideAddress);

  Required = 0;
  Bad = 0;
  if (Plan->Split) {
    for (Index = 0; Index < Plan->LoadCount; Index++) {
      if (Plan->Segments[Index].FileSize == 0) {
        continue; // BSS has no file payload.
      }

      Required++;
      UnicodeSPrint (Name, sizeof (Name), L"%s.b%02u", Prefix, Plan->Segments[Index].Index);
      File = NULL;
      Size = 0;
      Status = Dir->Open (Dir, &File, Name, EFI_FILE_MODE_READ, 0);
      if (!EFI_ERROR (Status) && (File != NULL)) {
        Status = AdspFileSize (File, &Size);
      }

      if (EFI_ERROR (Status) || (File == NULL) || (Size < Plan->Segments[Index].FileSize)) {
        Bad++;
        if (Bad <= 8) {
          AdspSay ("[ADSP] %s need=%u size=%Lu (%r)\n", Name,
                   Plan->Segments[Index].FileSize, Size, Status);
        }
      }

      if (File != NULL) { File->Close (File); }
    }
  }

  // An override span check does not validate the vendor loader's relocation ABI.
  Valid = (BOOLEAN)((Bad == 0) && SpanFits && (OverrideAddress || RawFits));
  AdspSay ("[ADSP] %s payload files=%u bad=%u structural=%u; auth/start unverified\n",
           Prefix, Required, Bad, Valid);
  FreePool (Plan);
  return Valid;
}

STATIC
VOID
AdspProbeOneFs (
  IN UINTN               Index,
  IN EFI_FILE_PROTOCOL   *Root,
  IN BOOLEAN             Verbose,
  OUT BOOLEAN            *Found
  )
{
  EFI_FILE_PROTOCOL  *ImageDir;
  EFI_STATUS         Status;

  ImageDir = NULL;
  Status   = Root->Open (Root, &ImageDir, L"\\image", EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || (ImageDir == NULL)) {
    if (Verbose) {
      AdspSay ("[ADSP] fs %u \\image -> %r\n", (UINT32)Index, Status);
    }

    return;
  }

  AdspSay ("[ADSP] fs %u \\image found\n", (UINT32)Index);
  {
    BOOLEAN AdspValid;
    BOOLEAN DtbValid;
    // Correspond to uefipil.cfg FULL_ADSP and FULL_ADSP_DTB. No AUTO or PIL call.
    AdspValid = AdspCheckImage (ImageDir, L"adsp", 0x9DF00000ULL, 0x4680000ULL, TRUE);
    DtbValid = AdspCheckImage (ImageDir, L"adsp_dtb", 0x9DE80000ULL, 0x80000ULL, FALSE);
    if (AdspValid && DtbValid) {
      *Found = TRUE; // Both images must be on this same volume.
    }
  }

  ImageDir->Close (ImageDir);
}

//
// APRIORI places this probe after UFSDxe, PartitionDxe and FatDxe. Connecting
// earlier than FatDxe's entry cannot bind a filesystem, regardless of round count.
// This only establishes file reachability. It does not explain every historical
// PIL status:9 or validate the firmware's segments, authentication or memory map.
//
STATIC
UINTN
AdspCountFilesystems (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  EFI_STATUS  Status;
  UINTN       Count;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &Count,
                   &Handles
                   );
  if (!EFI_ERROR (Status) && (Handles != NULL)) {
    FreePool (Handles);
  }

  return EFI_ERROR (Status) ? 0 : Count;
}

//
// Repeat while BlockIo or filesystem handles grow. ConnectController success is
// not a progress counter: b211 kept succeeding on the same six bound volumes.
//
STATIC
VOID
AdspConnectBlockDevices (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  EFI_STATUS  Status;
  UINTN       Count;
  UINTN       Index;
  UINTN       Connected;
  UINTN       Round;
  UINTN       Before;
  UINTN       After;
  UINTN       PreviousCount;

  Before = AdspCountFilesystems ();
  PreviousCount = MAX_UINTN;
  AdspSay ("[ADSP] filesystems before connect: %u\n", (UINT32)Before);

  for (Round = 1; Round <= 6; Round++) {
    Handles   = NULL;
    Count     = 0;
    Connected = 0;
    Status    = gBS->LocateHandleBuffer (
                      ByProtocol,
                      &gEfiBlockIoProtocolGuid,
                      NULL,
                      &Count,
                      &Handles
                      );
    if (EFI_ERROR (Status) || (Handles == NULL)) {
      AdspSay ("[ADSP] round %u: no block devices (%r)\n", (UINT32)Round, Status);
      return;
    }

    for (Index = 0; Index < Count; Index++) {
      if (!EFI_ERROR (gBS->ConnectController (Handles[Index], NULL, NULL, TRUE))) {
        Connected++;
      }
    }

    FreePool (Handles);

    After = AdspCountFilesystems ();
    AdspSay ("[ADSP] round %u: %u block device(s), connected %u, filesystems %u (+%u)\n",
             (UINT32)Round, (UINT32)Count, (UINT32)Connected,
             (UINT32)After, (UINT32)((After >= Before) ? After - Before : 0));

    // The second snapshot also catches new partition handles from the first pass.
    if ((Connected == 0) || ((Count == PreviousCount) && (After == Before))) {
      break;
    }

    PreviousCount = Count;
    Before = After;
  }
}

STATIC
VOID
AdspProbeFilesystems (
  IN BOOLEAN  Verbose
  )
{
  EFI_HANDLE             *Handles;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL      *Root;
  EFI_STATUS             Status;
  UINTN                  Count;
  UINTN                  Index;
  BOOLEAN                Found;
  EFI_PARTITION_INFO_PROTOCOL *Partition;
  CHAR16                 Label[37];
  EFI_GUID               TypeGuid;

  Handles = NULL;
  Count   = 0;
  Found   = FALSE;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &Count,
                   &Handles
                   );

  AdspSay ("[ADSP] filesystems=%u (%r)\n", (UINT32)Count, Status);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Fs = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
        (Fs == NULL)) {
      continue;
    }

    Partition = NULL;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Partition);
    if (!EFI_ERROR (Status) && (Partition != NULL) && (Partition->Type == PARTITION_TYPE_GPT)) {
      CopyMem (Label, Partition->Info.Gpt.PartitionName, 36 * sizeof (CHAR16));
      Label[36] = 0;
      CopyMem (&TypeGuid, &Partition->Info.Gpt.PartitionTypeGUID, sizeof (TypeGuid));
      AdspSay ("[ADSP] fs %u GPT label=%s type=%g\n", (UINT32)Index, Label, &TypeGuid);
    } else {
      AdspSay ("[ADSP] fs %u partition info unavailable/type other (%r)\n", (UINT32)Index, Status);
    }

    Root = NULL;
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || (Root == NULL)) {
      AdspSay ("[ADSP] fs %u open volume failed\n", (UINT32)Index);
      continue;
    }

    AdspProbeOneFs (Index, Root, Verbose, &Found);
    Root->Close (Root);
  }

  FreePool (Handles);

  if (Found) {
    AdspSay ("[ADSP] RESULT: MDT pair/payload sizes validated on one volume; PIL selection/auth/start unverified\n");
    mProbed = TRUE;
  }
}

//
// If the bounded early connection missed a volume, later filesystem installs
// re-probe until a pair validates. Limit notification attempts to avoid log spam.
//
STATIC
VOID
EFIAPI
AdspProbeOnFsInstall (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mProbed) {
    gBS->CloseEvent (Event);
    return;
  }

  mProbeCount++;
  if (mProbeCount > 12) {
    AdspSay ("[ADSP] RESULT: no structurally valid MDT pair after 12 installs\n");
    gBS->CloseEvent (Event);
    return;
  }

  AdspSay ("[ADSP] filesystem install #%u\n", (UINT32)mProbeCount);
  AdspProbeFilesystems (mProbeCount == 1);
}

EFI_STATUS
EFIAPI
AdspProbeDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  *Handles;
  EFI_EVENT   Event;
  EFI_STATUS  Status;
  VOID        *Registration;
  UINTN       Count;
  VOID        *Dal;
  EFI_GUID    DalGuid = { 0xAE37B942, 0x457F, 0x4C91,
                         { 0xA1, 0x96, 0xD9, 0x66, 0x9F, 0xD3, 0x47, 0xA3 } };

  AdspSay ("[ADSP] adsp-pil-b213 %a %a\n", __DATE__, __TIME__);
  // PILDxe's actual binary DEPEX GUID; only locate it, never call unknown methods.
  Dal = NULL;
  Status = gBS->LocateProtocol (&DalGuid, NULL, &Dal);
  AdspSay ("[ADSP] PIL DAL SYS dependency -> %r present=%u\n", Status, Dal != NULL);
  if (AdspProbePilProtocol () == EFI_NOT_FOUND) {
    Event = NULL;
    Registration = NULL;
    Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                              AdspOnPilInstall, NULL, &Event);
    if (!EFI_ERROR (Status)) {
      Status = gBS->RegisterProtocolNotify (&mPilGuid, Event, &Registration);
      if (EFI_ERROR (Status)) {
        gBS->CloseEvent (Event);
      }
    }
    AdspSay ("[ADSP b213] waiting for PIL public protocol (%r)\n", Status);
  }

  //
  // The binding drivers have run in APRIORI order. Connect their block handles.
  //
  AdspConnectBlockDevices ();

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &Count,
                   &Handles
                   );
  AdspSay ("[ADSP] entry: filesystems=%u (%r)\n", (UINT32)Count, Status);
  if (!EFI_ERROR (Status) && (Handles != NULL)) {
    FreePool (Handles);
  }

  AdspProbeFilesystems (TRUE);
  if (mProbed) {
    return EFI_SUCCESS;
  }

  //
  // Otherwise wait for FAT to bind. BDS calls EfiBootManagerConnectAll() after this
  // driver's entry point, so the first install is the earliest useful moment.
  //
  Event        = NULL;
  Registration = NULL;
  Status       = gBS->CreateEvent (
                        EVT_NOTIFY_SIGNAL,
                        TPL_CALLBACK,
                        AdspProbeOnFsInstall,
                        NULL,
                        &Event
                        );
  if (EFI_ERROR (Status)) {
    AdspSay ("[ADSP] no event (%r)\n", Status);
    return EFI_SUCCESS;
  }

  Status = gBS->RegisterProtocolNotify (&gEfiSimpleFileSystemProtocolGuid, Event, &Registration);
  AdspSay ("[ADSP] waiting for a filesystem (%r)\n", Status);
  return EFI_SUCCESS;
}
