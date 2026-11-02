/** @file A private FAT logging disk, bounded by two independently verified GPTs.
  Never unprotect a public partition. Every native write is checked against the
  pinned logfs extent and read back directly through the root BlockIo.
*/
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "LogfsDisk.h"
#include "LogfsBounds.h"

#define LOG_DATA_MAX (256 * 1024)
#define LOG_FILE_MAX (512 * 1024)
// The OEM dump is 8 MiB; its FAT12 volume is 5 MiB with ~4 MiB free.
// Still require both GPTs and the complete device identity at runtime.
#define LOG_PARTITION_BYTES (8 * 1024 * 1024)
typedef struct {
  EFI_HANDLE Handle;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_BLOCK_WRITE Write;
  EFI_BLOCK_FLUSH Flush;
} LOG_ROOT;
STATIC LOG_ROOT mRoots[16];
STATIC UINTN mRootCount;
STATIC LOG_ROOT *mRoot;
STATIC EFI_PARTITION_ENTRY mExtent;
STATIC UINT32 mRootMediaId;
STATIC UINT32 mRootBlockSize, mRootIoAlign;
STATIC UINT64 mRootLast;
STATIC UINT64 mVolumeBytes;
STATIC EFI_HANDLE mAlias;
STATIC EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *mFs;
STATIC BOOLEAN mLease, mBusy, mSession, mPoisoned;
STATIC UINTN mSlot;
STATIC UINT64 mSequence;
STATIC EFI_BLOCK_IO_MEDIA mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL mBlock;
STATIC EFI_DISK_IO_PROTOCOL mDisk;
#pragma pack(1)
typedef struct { VENDOR_DEVICE_PATH Vendor; EFI_DEVICE_PATH_PROTOCOL End; } LOG_PATH;
typedef struct {
  CHAR8 Magic[8];
  UINT32 Revision, HeaderBytes;
  UINT64 Sequence;
  UINT32 DataBytes, DataCrc;
  CHAR8 Phase[32];
  UINT32 HeaderCrc;
  UINT8 Reserved[60];
} LOG_HEADER;
#pragma pack()
STATIC LOG_PATH mPath = {
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},
   {0xe1b9a319,0x58e7,0x45e6,{0xb8,0x0d,0xa5,0x6c,0x17,0xb0,0x93,0x01}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{sizeof(EFI_DEVICE_PATH_PROTOCOL),0}}
};
STATIC CHAR16 *mNames[] = {L"\\mu-log0.bin", L"\\mu-log1.bin"};

STATIC VOID LogTrace (CONST CHAR8 *Message, EFI_STATUS Status)
{
  CHAR8 Text[160];
  UINTN N = AsciiSPrint (Text, sizeof (Text), "[LOGFS b327] %a -> %r\r\n", Message, Status);
  SerialPortWrite ((UINT8 *)Text, N);
}
STATIC EFI_TPL LogTpl (VOID)
{
  EFI_TPL Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Tpl);
  return Tpl;
}
VOID LogfsRememberRoot (EFI_HANDLE Handle, EFI_BLOCK_IO_PROTOCOL *Block)
{
  UINTN I;
  if (Block->Media == NULL || Block->Media->LogicalPartition || Block->WriteBlocks == NULL) return;
  for (I = 0; I < mRootCount; I++) if (mRoots[I].Block == Block) return;
  if (mRootCount == ARRAY_SIZE (mRoots)) return; // No capability: ordinary guard still denies writes.
  mRoots[mRootCount++] = (LOG_ROOT){Handle, Block, Block->WriteBlocks, Block->FlushBlocks};
}
BOOLEAN LogfsOwnInterface (VOID *Interface)
{ return Interface == &mBlock || Interface == &mDisk; }
STATIC VOID *LogBuffer (LOG_ROOT *Root, UINTN Bytes)
{
  UINTN Alignment = MAX ((UINTN)8, Root->Block->Media->IoAlign);
  VOID *Raw, *Buffer;
  if (Alignment > 65536 || (Alignment & (Alignment - 1)) != 0 ||
      Bytes > MAX_UINTN - Alignment - sizeof (VOID *)) return NULL;
  Raw = AllocatePool (Bytes + Alignment - 1 + sizeof (VOID *));
  if (Raw == NULL) return NULL;
  Buffer = (VOID *)ALIGN_VALUE ((UINTN)Raw + sizeof (VOID *), Alignment);
  ((VOID **)Buffer)[-1] = Raw;
  return Buffer;
}
STATIC VOID LogFree (VOID *Buffer) { if (Buffer != NULL) FreePool (((VOID **)Buffer)[-1]); }
STATIC BOOLEAN PinnedMedia (VOID)
{
  EFI_BLOCK_IO_MEDIA *M = mRoot == NULL ? NULL : mRoot->Block->Media;
  return M != NULL && M->MediaPresent && M->MediaId == mRootMediaId &&
    M->LastBlock == mRootLast && M->BlockSize == mRootBlockSize && M->IoAlign == mRootIoAlign;
}
STATIC EFI_STATUS ReadRoot (LOG_ROOT *Root, EFI_LBA Lba, UINTN Bytes, VOID *Data)
{
  EFI_BLOCK_IO_MEDIA *M = Root->Block->Media;
  if (M == NULL || !M->MediaPresent || M->BlockSize == 0 || Bytes == 0 || Bytes % M->BlockSize != 0 ||
      Lba > M->LastBlock || Bytes / M->BlockSize > M->LastBlock - Lba + 1) return EFI_INVALID_PARAMETER;
  return Root->Block->ReadBlocks (Root->Block, M->MediaId, Lba, Bytes, Data);
}
STATIC BOOLEAN HeaderValid (EFI_PARTITION_TABLE_HEADER *H, UINT32 BlockSize, UINT64 Here, UINT64 Other)
{
  UINT32 Crc, Saved;
  BOOLEAN Good;
  if (H->Header.Signature != EFI_PTAB_HEADER_ID || H->Header.Revision != 0x10000 ||
      H->Header.HeaderSize < sizeof (*H) || H->Header.HeaderSize > BlockSize || H->Header.Reserved != 0 ||
      H->MyLBA != Here || H->AlternateLBA != Other || H->FirstUsableLBA > H->LastUsableLBA ||
      H->NumberOfPartitionEntries == 0 || H->NumberOfPartitionEntries > 2048 ||
      H->SizeOfPartitionEntry != sizeof (EFI_PARTITION_ENTRY)) return FALSE;
  Saved = H->Header.CRC32; H->Header.CRC32 = 0;
  Good = (BOOLEAN)(!EFI_ERROR (gBS->CalculateCrc32 (H, H->Header.HeaderSize, &Crc)) && Crc == Saved);
  H->Header.CRC32 = Saved;
  return Good;
}
STATIC EFI_STATUS VerifyGpt (LOG_ROOT *Root, EFI_PARTITION_INFO_PROTOCOL *Info,
  HARDDRIVE_DEVICE_PATH *Hd, EFI_PARTITION_ENTRY *Extent, UINT64 *VolumeBytes)
{
  EFI_BLOCK_IO_MEDIA *M = Root->Block->Media;
  EFI_PARTITION_TABLE_HEADER *P = NULL, *B = NULL;
  VOID *Table = NULL, *Backup = NULL, *Boot = NULL;
  UINTN Bytes, Rounded, Blocks;
  UINT32 Crc;
  CONST CHAR8 *Stage = "verify root geometry";
  EFI_STATUS Status = EFI_SECURITY_VIOLATION;
  if (M == NULL || M->LogicalPartition || !M->MediaPresent || M->BlockSize < 512 || M->BlockSize > 4096 ||
      (M->BlockSize & (M->BlockSize - 1)) != 0 || M->LastBlock < 4 || M->LastBlock == MAX_UINT64 ||
      M->IoAlign > 65536 || (M->IoAlign > 1 && (M->IoAlign & (M->IoAlign - 1)) != 0) ||
      Root->Block->ReadBlocks == NULL || Root->Write == NULL) return Status;
  P = LogBuffer (Root, M->BlockSize); B = LogBuffer (Root, M->BlockSize);
  if (P == NULL || B == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Stage = "verify primary/backup GPT headers";
  if (EFI_ERROR (ReadRoot (Root, 1, M->BlockSize, P)) || EFI_ERROR (ReadRoot (Root, M->LastBlock, M->BlockSize, B)) ||
      !HeaderValid (P, M->BlockSize, 1, M->LastBlock) || !HeaderValid (B, M->BlockSize, M->LastBlock, 1) ||
      CompareMem (&P->DiskGUID, &B->DiskGUID, sizeof (EFI_GUID)) != 0 || P->FirstUsableLBA <= 1 ||
      P->LastUsableLBA >= M->LastBlock || P->FirstUsableLBA != B->FirstUsableLBA || P->LastUsableLBA != B->LastUsableLBA ||
      P->NumberOfPartitionEntries != B->NumberOfPartitionEntries || P->PartitionEntryArrayCRC32 != B->PartitionEntryArrayCRC32) goto Done;
  Bytes = (UINTN)P->NumberOfPartitionEntries * sizeof (EFI_PARTITION_ENTRY);
  Rounded = ALIGN_VALUE (Bytes, M->BlockSize); Blocks = Rounded / M->BlockSize;
  Stage = "verify GPT table locations";
  if (P->PartitionEntryLBA < 2 || P->PartitionEntryLBA >= P->FirstUsableLBA || Blocks > P->FirstUsableLBA - P->PartitionEntryLBA ||
      B->PartitionEntryLBA <= P->LastUsableLBA || B->PartitionEntryLBA >= M->LastBlock || Blocks > M->LastBlock - B->PartitionEntryLBA) goto Done;
  Table = LogBuffer (Root, Rounded); Backup = LogBuffer (Root, Rounded);
  if (Table == NULL || Backup == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Stage = "verify GPT tables, identity, overlap and exact 8MiB logfs";
  if (EFI_ERROR (ReadRoot (Root, P->PartitionEntryLBA, Rounded, Table)) || EFI_ERROR (ReadRoot (Root, B->PartitionEntryLBA, Rounded, Backup)) ||
      EFI_ERROR (gBS->CalculateCrc32 (Table, Bytes, &Crc)) || Crc != P->PartitionEntryArrayCRC32 ||
      CompareMem (Table, Backup, Bytes) != 0 ||
      !LogfsSelect (Table, P->NumberOfPartitionEntries, &Info->Info.Gpt, P->FirstUsableLBA, P->LastUsableLBA, Hd->PartitionNumber, Extent) ||
      Hd->PartitionStart != Extent->StartingLBA || Hd->PartitionSize != Extent->EndingLBA - Extent->StartingLBA + 1 ||
      CompareMem (Hd->Signature, &Extent->UniquePartitionGUID, sizeof (EFI_GUID)) != 0 ||
      Hd->PartitionSize != LOG_PARTITION_BYTES / M->BlockSize) goto Done;
  Boot = LogBuffer (Root, M->BlockSize);
  if (Boot == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Stage = "verify FAT BPB/volume bound";
  if (!EFI_ERROR (ReadRoot (Root, Extent->StartingLBA, M->BlockSize, Boot)) &&
      LogfsVolume (Boot, M->BlockSize, LOG_PARTITION_BYTES, VolumeBytes)) Status = EFI_SUCCESS;
Done:
  if (EFI_ERROR (Status)) LogTrace (Stage, Status);
  LogFree (P); LogFree (B); LogFree (Table); LogFree (Backup); LogFree (Boot);
  return Status;
}
STATIC EFI_STATUS WriteRoot (EFI_LBA Lba, UINTN Bytes, VOID *Data)
{
  EFI_STATUS Status;
  BOOLEAN OldReadOnly;
  VOID *Aligned = NULL, *Verify = NULL;
  EFI_BLOCK_IO_MEDIA *M;
  // EnhancedFat holds FatFsLock at TPL_CALLBACK during synchronous file I/O.
  // The lease is acquired only by the application; no notify-level I/O.
  if (!mLease || mPoisoned || !PinnedMedia () || LogTpl () > TPL_CALLBACK) return EFI_WRITE_PROTECTED;
  M = mRoot->Block->Media;
  if (M == NULL || !M->MediaPresent || M->MediaId != mRootMediaId || M->LastBlock != mRootLast ||
      Data == NULL || mVolumeBytes == 0 ||
      !LogfsRange (mExtent.StartingLBA, mExtent.EndingLBA, mRootLast, Lba, Bytes, M->BlockSize) ||
      !LogfsRange (mExtent.StartingLBA, mExtent.StartingLBA + mVolumeBytes / M->BlockSize - 1,
        mRootLast, Lba, Bytes, M->BlockSize)) return EFI_WRITE_PROTECTED;
  Aligned = LogBuffer (mRoot, Bytes); Verify = LogBuffer (mRoot, Bytes);
  if (Aligned == NULL || Verify == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  CopyMem (Aligned, Data, Bytes);
  // The public write callbacks remain guards throughout. Only this saved native
  // function receives the checked logfs LBA. Restore even on native error.
  OldReadOnly = M->ReadOnly; M->ReadOnly = FALSE;
  Status = mRoot->Write (mRoot->Block, mRootMediaId, Lba, Bytes, Aligned);
  M->ReadOnly = OldReadOnly;
  if (!EFI_ERROR (Status)) Status = ReadRoot (mRoot, Lba, Bytes, Verify);
  if (!EFI_ERROR (Status) && CompareMem (Aligned, Verify, Bytes) != 0) Status = EFI_CRC_ERROR;
  if (EFI_ERROR (Status)) { mLease = FALSE; mPoisoned = TRUE; }
Done:
  LogFree (Aligned); LogFree (Verify);
  return Status;
}
STATIC EFI_STATUS EFIAPI AliasReset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Extended) { return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI AliasRead (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Bytes, VOID *Data)
{
  if (!PinnedMedia () || Data == NULL || MediaId != mMedia.MediaId || Lba > mMedia.LastBlock || Bytes == 0 ||
      Bytes % mMedia.BlockSize != 0 || Bytes / mMedia.BlockSize > mMedia.LastBlock - Lba + 1) return EFI_INVALID_PARAMETER;
  return ReadRoot (mRoot, mExtent.StartingLBA + Lba, Bytes, Data);
}
STATIC EFI_STATUS EFIAPI AliasWrite (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Bytes, VOID *Data)
{
  if (MediaId != mMedia.MediaId || Lba > mMedia.LastBlock || Bytes == 0 || Bytes % mMedia.BlockSize != 0 ||
      Bytes / mMedia.BlockSize > mMedia.LastBlock - Lba + 1) return EFI_WRITE_PROTECTED;
  return WriteRoot (mExtent.StartingLBA + Lba, Bytes, Data);
}
STATIC EFI_STATUS EFIAPI AliasFlush (EFI_BLOCK_IO_PROTOCOL *This)
{
  EFI_STATUS Status;
  BOOLEAN OldReadOnly;
  if (mPoisoned || !PinnedMedia () || LogTpl () > TPL_CALLBACK) return EFI_WRITE_PROTECTED;
  // EnhancedFat also flushes after read-only Open/Close. No native flush and
  // no write lease are needed for that path; every actual cache write still
  // reaches WriteRoot and is denied outside LogfsSave's private lease.
  if (!mLease) return EFI_SUCCESS;
  OldReadOnly = mRoot->Block->Media->ReadOnly;
  mRoot->Block->Media->ReadOnly = FALSE;
  Status = mRoot->Flush == NULL ? EFI_SUCCESS : mRoot->Flush (mRoot->Block);
  mRoot->Block->Media->ReadOnly = OldReadOnly;
  if (EFI_ERROR (Status)) { mLease = FALSE; mPoisoned = TRUE; }
  return Status;
}
STATIC EFI_STATUS AliasDiskIo (BOOLEAN Write, UINT32 MediaId, UINT64 Offset, UINTN Bytes, VOID *Data)
{
  VOID *Block;
  EFI_STATUS Status = EFI_SUCCESS;
  UINT64 Total = (mMedia.LastBlock + 1) * mMedia.BlockSize;
  if (!PinnedMedia () || Data == NULL || MediaId != mMedia.MediaId || Offset > Total || Bytes > Total - Offset ||
      (Write && !mLease)) return EFI_WRITE_PROTECTED;
  if (Bytes == 0) return EFI_SUCCESS;
  Block = LogBuffer (mRoot, mMedia.BlockSize);
  if (Block == NULL) return EFI_OUT_OF_RESOURCES;
  while (Bytes != 0) {
    UINTN InBlock = (UINTN)(Offset % mMedia.BlockSize);
    UINTN Chunk = MIN (Bytes, mMedia.BlockSize - InBlock);
    EFI_LBA Lba = Offset / mMedia.BlockSize;
    Status = AliasRead (&mBlock, MediaId, Lba, mMedia.BlockSize, Block);
    if (EFI_ERROR (Status)) break;
    if (Write) {
      CopyMem ((UINT8 *)Block + InBlock, Data, Chunk);
      Status = AliasWrite (&mBlock, MediaId, Lba, mMedia.BlockSize, Block);
    } else CopyMem (Data, (UINT8 *)Block + InBlock, Chunk);
    if (EFI_ERROR (Status)) break;
    Bytes -= Chunk; Offset += Chunk; Data = (UINT8 *)Data + Chunk;
  }
  LogFree (Block);
  return Status;
}
STATIC EFI_STATUS EFIAPI AliasDiskRead (EFI_DISK_IO_PROTOCOL *This, UINT32 MediaId, UINT64 Offset, UINTN Bytes, VOID *Data)
{ return AliasDiskIo (FALSE, MediaId, Offset, Bytes, Data); }
STATIC EFI_STATUS EFIAPI AliasDiskWrite (EFI_DISK_IO_PROTOCOL *This, UINT32 MediaId, UINT64 Offset, UINTN Bytes, VOID *Data)
{ return AliasDiskIo (TRUE, MediaId, Offset, Bytes, Data); }

STATIC EFI_STATUS FindLogfs (VOID)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count, I, R, Hits = 0;
  EFI_STATUS Status;
  LOG_ROOT *Root = NULL;
  EFI_PARTITION_ENTRY Extent;
  UINT64 VolumeBytes = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) return Status;
  Status = EFI_NOT_FOUND;
  for (I = 0; I < Count; I++) {
    EFI_PARTITION_INFO_PROTOCOL *Info;
    EFI_BLOCK_IO_PROTOCOL *Part;
    EFI_DEVICE_PATH_PROTOCOL *Path, *Node;
    HARDDRIVE_DEVICE_PATH *Hd = NULL;
    UINTN Prefix;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info)) ||
        Info->Type != PARTITION_TYPE_GPT || !LogfsName (&Info->Info.Gpt)) continue;
    if (++Hits > 1) { Status = EFI_SECURITY_VIOLATION; break; }
    Status = EFI_SECURITY_VIOLATION;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiBlockIoProtocolGuid, (VOID **)&Part)) || Part->Media == NULL ||
        !Part->Media->LogicalPartition || EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiDevicePathProtocolGuid, (VOID **)&Path)) ||
        !IsDevicePathValid (Path, 4096)) break;
    for (Node = Path; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
      if (DevicePathType (Node) == MEDIA_DEVICE_PATH && DevicePathSubType (Node) == MEDIA_HARDDRIVE_DP) {
        if (DevicePathNodeLength (Node) != sizeof (HARDDRIVE_DEVICE_PATH) || Hd != NULL) break;
        Hd = (HARDDRIVE_DEVICE_PATH *)Node;
      }
    }
    if (Hd == NULL || !IsDevicePathEnd (NextDevicePathNode (&Hd->Header)) || Hd->MBRType != MBR_TYPE_EFI_PARTITION_TABLE_HEADER || Hd->SignatureType != SIGNATURE_TYPE_GUID) break;
    Prefix = (UINTN)(UINT8 *)Hd - (UINTN)(UINT8 *)Path;
    for (R = 0; R < mRootCount; R++) {
      EFI_DEVICE_PATH_PROTOCOL *Parent;
      if (EFI_ERROR (gBS->HandleProtocol (mRoots[R].Handle, &gEfiDevicePathProtocolGuid, (VOID **)&Parent)) ||
          !IsDevicePathValid (Parent, 4096) || GetDevicePathSize (Parent) != Prefix + END_DEVICE_PATH_LENGTH ||
          CompareMem (Parent, Path, Prefix) != 0) continue;
      if (Root != NULL) { Status = EFI_SECURITY_VIOLATION; goto Done; }
      Status = VerifyGpt (&mRoots[R], Info, Hd, &Extent, &VolumeBytes);
      if (EFI_ERROR (Status) || Part->Media->BlockSize != mRoots[R].Block->Media->BlockSize ||
          Part->Media->LastBlock != Extent.EndingLBA - Extent.StartingLBA) goto Done;
      Root = &mRoots[R];
    }
  }
Done:
  FreePool (Handles);
  if (Hits != 1 || Root == NULL || EFI_ERROR (Status)) return EFI_ERROR (Status) ? Status : EFI_SECURITY_VIOLATION;
  if (mRoot != NULL) return (mRoot == Root && mVolumeBytes == VolumeBytes && CompareMem (&mExtent, &Extent, sizeof (Extent)) == 0 &&
    PinnedMedia ()) ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
  mRoot = Root; CopyMem (&mExtent, &Extent, sizeof (Extent));
  mRootMediaId = Root->Block->Media->MediaId; mRootLast = Root->Block->Media->LastBlock;
  mRootBlockSize = Root->Block->Media->BlockSize; mRootIoAlign = Root->Block->Media->IoAlign;
  mVolumeBytes = VolumeBytes;
  {
    CHAR8 Text[240];
    UINTN N = AsciiSPrint (Text, sizeof (Text), "[LOGFS b327] verified logfs guid=%g root=%lx LBA=%lu..%lu partition=8388608 volume=%lu GPT-primary/backup equal\r\n",
      &mExtent.UniquePartitionGUID, (UINT64)(UINTN)Root->Handle, mExtent.StartingLBA, mExtent.EndingLBA, mVolumeBytes);
    SerialPortWrite ((UINT8 *)Text, N);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS OpenLogfs (VOID)
{
  EFI_STATUS Status;
  if (LogTpl () != TPL_APPLICATION) return EFI_NOT_READY;
  if (mPoisoned) return EFI_DEVICE_ERROR;
  Status = FindLogfs ();
  if (EFI_ERROR (Status)) { LogTrace ("find/verify logfs", Status); return Status; }
  if (mFs != NULL) return EFI_SUCCESS;
  CopyMem (&mMedia, mRoot->Block->Media, sizeof (mMedia));
  mMedia.ReadOnly = FALSE; mMedia.LogicalPartition = TRUE; mMedia.LastBlock = mVolumeBytes / mRootBlockSize - 1;
  mBlock = (EFI_BLOCK_IO_PROTOCOL){EFI_BLOCK_IO_PROTOCOL_REVISION, &mMedia, AliasReset, AliasRead, AliasWrite, AliasFlush};
  mDisk = (EFI_DISK_IO_PROTOCOL){EFI_DISK_IO_PROTOCOL_REVISION, AliasDiskRead, AliasDiskWrite};
  mLease = TRUE;
  Status = mAlias != NULL ? EFI_SUCCESS : gBS->InstallMultipleProtocolInterfaces (&mAlias, &gEfiDevicePathProtocolGuid, &mPath,
      &gEfiBlockIoProtocolGuid, &mBlock, &gEfiDiskIoProtocolGuid, &mDisk, NULL);
  if (!EFI_ERROR (Status)) Status = gBS->ConnectController (mAlias, NULL, NULL, TRUE);
  if (EFI_ERROR (Status)) LogTrace ("connect private FAT alias", Status);
  if (!EFI_ERROR (Status)) Status = gBS->HandleProtocol (mAlias, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&mFs);
  // Keep the FAT object for this logger but remove its public filesystem
  // publication. Otherwise another app could dirty its writable file cache
  // outside the lease and have those changes flushed by a later LOGSAVE.
  // FAT owns this produced interface; it remains bound through DiskIo.
  if (!EFI_ERROR (Status)) Status = gBS->UninstallProtocolInterface (mAlias, &gEfiSimpleFileSystemProtocolGuid, mFs);
  if (EFI_ERROR (Status)) mFs = NULL;
  if (EFI_ERROR (Status)) LogTrace ("open/unpublish private FAT filesystem", Status);
  mLease = FALSE;
  return Status;
}
STATIC BOOLEAN FileRecordValid (EFI_FILE_INFO *Info, UINTN Bytes, UINTN Capacity)
{
  UINTN I, Characters;
  if (Bytes < SIZE_OF_EFI_FILE_INFO + sizeof (CHAR16) || Bytes > Capacity ||
      Info->Size < SIZE_OF_EFI_FILE_INFO + sizeof (CHAR16) || Info->Size > Bytes ||
      (Info->Size - SIZE_OF_EFI_FILE_INFO) % sizeof (CHAR16) != 0 ||
      Info->FileSize > LOG_FILE_MAX || (Info->Attribute & EFI_FILE_DIRECTORY)) return FALSE;
  Characters = (UINTN)(Info->Size - SIZE_OF_EFI_FILE_INFO) / sizeof (CHAR16);
  for (I = 0; I < Characters; I++) if (Info->FileName[I] == 0) return TRUE;
  return FALSE;
}
STATIC EFI_STATUS ReadSlot (UINTN Slot, LOG_HEADER *Header, CHAR8 *Data, BOOLEAN *Owned)
{
  EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
  EFI_STATUS Status;
  UINTN N;
  UINT32 Saved, Crc;
  union { UINT64 Align; UINT8 Data[sizeof (EFI_FILE_INFO) + 128]; } InfoBuffer;
  EFI_FILE_INFO *Info = (EFI_FILE_INFO *)InfoBuffer.Data;
  if (Owned != NULL) *Owned = FALSE;
  ZeroMem (Header, sizeof (*Header));
  Status = mFs->OpenVolume (mFs, &Root);
  if (EFI_ERROR (Status)) return Status;
  Status = Root->Open (Root, &File, mNames[Slot], EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status)) return Status;
  ZeroMem (&InfoBuffer, sizeof (InfoBuffer));
  N = sizeof (InfoBuffer);
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &N, Info);
  if (!EFI_ERROR (Status) && !FileRecordValid (Info, N, sizeof (InfoBuffer))) Status = EFI_SECURITY_VIOLATION;
  N = sizeof (*Header);
  if (!EFI_ERROR (Status)) Status = File->Read (File, &N, Header);
  if (!EFI_ERROR (Status) && (N != sizeof (*Header) || CompareMem (Header->Magic, "MULOGFS1", 8) != 0 ||
      Header->Revision != 1 || Header->HeaderBytes != sizeof (*Header) || Header->Sequence == 0 ||
      Header->Phase[sizeof (Header->Phase) - 1] != 0 || Header->DataBytes > LOG_DATA_MAX ||
      sizeof (*Header) + Header->DataBytes > LOG_FILE_MAX)) Status = EFI_VOLUME_CORRUPTED;
  if (!EFI_ERROR (Status)) {
    Saved = Header->HeaderCrc; Header->HeaderCrc = 0;
    Status = gBS->CalculateCrc32 (Header, sizeof (*Header), &Crc); Header->HeaderCrc = Saved;
    if (!EFI_ERROR (Status) && Crc != Saved) Status = EFI_CRC_ERROR;
  }
  // A valid ownership header with interrupted payload can be reused. Unknown
  // files and torn/unverified headers are never overwritten automatically.
  if (!EFI_ERROR (Status) && Owned != NULL) *Owned = TRUE;
  if (!EFI_ERROR (Status)) {
    if (Info->FileSize != sizeof (*Header) + Header->DataBytes) Status = EFI_VOLUME_CORRUPTED;
    N = Header->DataBytes;
    if (!EFI_ERROR (Status)) Status = File->Read (File, &N, Data);
    if (!EFI_ERROR (Status) && N != Header->DataBytes) Status = EFI_VOLUME_CORRUPTED;
    if (!EFI_ERROR (Status)) Status = gBS->CalculateCrc32 (Data, N, &Crc);
    if (!EFI_ERROR (Status) && Crc != Header->DataCrc) Status = EFI_CRC_ERROR;
  }
  { EFI_STATUS S = File->Close (File); if (!EFI_ERROR (Status)) Status = S; }
  return Status;
}
STATIC EFI_STATUS StartSession (CHAR8 *Scratch)
{
  LOG_HEADER H[2];
  EFI_STATUS S[2];
  BOOLEAN Owned[2];
  UINT64 Latest;
  if (mSession) return EFI_SUCCESS;
  S[0] = ReadSlot (0, &H[0], Scratch, &Owned[0]); S[1] = ReadSlot (1, &H[1], Scratch, &Owned[1]);
  LogTrace ("session slot0", S[0]); LogTrace ("session slot1", S[1]);
  if ((EFI_ERROR (S[0]) && S[0] != EFI_NOT_FOUND && !Owned[0]) ||
      (EFI_ERROR (S[1]) && S[1] != EFI_NOT_FOUND && !Owned[1])) return EFI_SECURITY_VIOLATION;
  if (Owned[0] && Owned[1] && H[0].Sequence == H[1].Sequence) return EFI_SECURITY_VIOLATION;
  if (S[0] == EFI_NOT_FOUND && S[1] == EFI_NOT_FOUND) { mSlot = 0; Latest = 0; }
  else if (S[1] == EFI_NOT_FOUND) { mSlot = 1; Latest = H[0].Sequence; }
  else if (S[0] == EFI_NOT_FOUND) { mSlot = 0; Latest = H[1].Sequence; }
  else {
    mSlot = EFI_ERROR (S[0]) ? 0 : EFI_ERROR (S[1]) ? 1 : H[0].Sequence > H[1].Sequence ? 1 : 0;
    Latest = MAX (H[0].Sequence, H[1].Sequence);
  }
  if (Latest == MAX_UINT64) return EFI_OUT_OF_RESOURCES;
  mSequence = Latest + 1; mSession = TRUE;
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI LogfsSave (CONST CHAR8 *Buffer, UINTN Length, CONST CHAR8 *Phase)
{
  EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
  LOG_HEADER Header, ReadBack;
  CHAR8 *Scratch = NULL;
  CONST CHAR8 *Stage = "open logfs";
  EFI_STATUS Status;
  UINTN N;
  union { UINT64 Align; UINT8 Data[sizeof (EFI_FILE_INFO) + 128]; } InfoBuffer;
  EFI_FILE_INFO *Info = (EFI_FILE_INFO *)InfoBuffer.Data;
  if (Buffer == NULL || Length == 0 || Length > LOG_DATA_MAX || sizeof (Header) + Length > LOG_FILE_MAX || Phase == NULL) return EFI_BAD_BUFFER_SIZE;
  if (mBusy || LogTpl () != TPL_APPLICATION) return EFI_NOT_READY;
  mBusy = TRUE;
  Status = OpenLogfs (); if (EFI_ERROR (Status)) goto Done;
  Scratch = AllocatePool (LOG_DATA_MAX); if (Scratch == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Stage = "start session";
  Status = StartSession (Scratch); if (EFI_ERROR (Status)) goto Done;
  ZeroMem (&Header, sizeof (Header)); CopyMem (Header.Magic, "MULOGFS1", 8);
  Header.Revision = 1; Header.HeaderBytes = sizeof (Header); Header.Sequence = mSequence;
  Header.DataBytes = (UINT32)Length; AsciiStrnCpyS (Header.Phase, sizeof (Header.Phase), Phase, sizeof (Header.Phase) - 1);
  Status = gBS->CalculateCrc32 ((VOID *)Buffer, Length, &Header.DataCrc); if (EFI_ERROR (Status)) goto Done;
  Status = gBS->CalculateCrc32 (&Header, sizeof (Header), &Header.HeaderCrc); if (EFI_ERROR (Status)) goto Done;
  mLease = TRUE;
  Stage = "open save root";
  Status = mFs->OpenVolume (mFs, &Root); if (EFI_ERROR (Status)) goto Done;
  Stage = "open save slot";
  Status = Root->Open (Root, &File, mNames[mSlot], EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) goto Done;
  ZeroMem (&InfoBuffer, sizeof (InfoBuffer));
  Stage = "get save file info";
  N = sizeof (InfoBuffer); Status = File->GetInfo (File, &gEfiFileInfoGuid, &N, Info);
  if (EFI_ERROR (Status)) goto Done;
  if (!FileRecordValid (Info, N, sizeof (InfoBuffer))) {
    Status = EFI_SECURITY_VIOLATION; goto Done;
  }
  Stage = "position save slot";
  Status = File->SetPosition (File, 0); if (EFI_ERROR (Status)) goto Done;
  Stage = "write save header";
  N = sizeof (Header); Status = File->Write (File, &N, &Header);
  if (!EFI_ERROR (Status) && N != sizeof (Header)) Status = EFI_VOLUME_FULL;
  if (EFI_ERROR (Status)) goto Done;
  Stage = "write save payload";
  N = Length; Status = File->Write (File, &N, (VOID *)Buffer);
  if (!EFI_ERROR (Status) && N != Length) Status = EFI_VOLUME_FULL;
  if (EFI_ERROR (Status)) goto Done;
  Info->FileSize = sizeof (Header) + Length;
  // SetInfo is used only for truncation. UEFI zero time fields mean preserve;
  // never resubmit GetInfo timestamps (e.g. the old virtual RTC's January 0).
  // FAT writes content before this validation, explaining error + valid CRC.
  ZeroMem (&Info->CreateTime, sizeof (Info->CreateTime));
  ZeroMem (&Info->LastAccessTime, sizeof (Info->LastAccessTime));
  ZeroMem (&Info->ModificationTime, sizeof (Info->ModificationTime));
  Stage = "set save length (timestamps preserved)";
  Status = File->SetInfo (File, &gEfiFileInfoGuid, (UINTN)Info->Size, Info);
  if (!EFI_ERROR (Status)) {
    Stage = "flush save slot";
    Status = File->Flush (File);
  }
Done:
  if (File != NULL) { EFI_STATUS S = File->Close (File); if (!EFI_ERROR (Status) && EFI_ERROR (S)) { Stage = "close save slot"; Status = S; } }
  if (Root != NULL) { EFI_STATUS S = Root->Close (Root); if (!EFI_ERROR (Status) && EFI_ERROR (S)) { Stage = "close save root"; Status = S; } }
  mLease = FALSE;
  if (!EFI_ERROR (Status) && Scratch != NULL) {
    Stage = "read back saved slot";
    Status = ReadSlot (mSlot, &ReadBack, Scratch, NULL);
    if (!EFI_ERROR (Status) && (ReadBack.Sequence != Header.Sequence || ReadBack.DataBytes != Length || CompareMem (Scratch, Buffer, Length) != 0)) Status = EFI_CRC_ERROR;
  }
  if (Scratch != NULL) FreePool (Scratch);
  mBusy = FALSE;
  if (EFI_ERROR (Status)) LogTrace (Stage, Status);
  LogTrace (EFI_ERROR (Status) ? "save refused/failed; other partitions remain guarded" : "saved one 256KiB snapshot; native writes read back", Status);
  return Status;
}
EFI_STATUS EFIAPI LogfsRead (CHAR8 *Buffer, UINTN *Length, BOOLEAN Previous)
{
  LOG_HEADER H[2];
  EFI_STATUS Status, S[2];
  CHAR8 *Scratch;
  UINTN Slot;
  if (Length == NULL || Buffer == NULL) return EFI_INVALID_PARAMETER;
  if (*Length < LOG_DATA_MAX) { *Length = LOG_DATA_MAX; return EFI_BUFFER_TOO_SMALL; }
  if (mBusy || LogTpl () != TPL_APPLICATION) return EFI_NOT_READY;
  mBusy = TRUE; Status = OpenLogfs (); if (EFI_ERROR (Status)) goto Done;
  Scratch = AllocatePool (LOG_DATA_MAX); if (Scratch == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  S[0] = ReadSlot (0, &H[0], Buffer, NULL); S[1] = ReadSlot (1, &H[1], Scratch, NULL);
  if (Previous && mSession && EFI_ERROR (S[mSlot ^ 1])) { Status = S[mSlot ^ 1]; goto FreeScratch; }
  if (EFI_ERROR (S[0]) && EFI_ERROR (S[1])) Status = S[0] == EFI_NOT_FOUND ? S[1] : S[0];
  else if (!EFI_ERROR (S[0]) && !EFI_ERROR (S[1]) && H[0].Sequence == H[1].Sequence) Status = EFI_SECURITY_VIOLATION;
  else {
    if (EFI_ERROR (S[0])) Slot = 1;
    else if (EFI_ERROR (S[1])) Slot = 0;
    else if (Previous && mSession) Slot = mSlot ^ 1;
    else Slot = H[0].Sequence > H[1].Sequence ? 0 : 1;
    if (Slot == 1) CopyMem (Buffer, Scratch, H[1].DataBytes);
    *Length = H[Slot].DataBytes; Status = EFI_SUCCESS;
    {
      CHAR8 Text[128];
      UINTN N = AsciiSPrint (Text, sizeof (Text), "[LOGFS b327] read slot=%u seq=%lu phase=%a bytes=%u CRC verified\r\n", Slot, H[Slot].Sequence, H[Slot].Phase, H[Slot].DataBytes);
      SerialPortWrite ((UINT8 *)Text, N);
    }
  }
FreeScratch:
  FreePool (Scratch);
Done:
  mBusy = FALSE;
  if (EFI_ERROR (Status)) LogTrace (Previous ? "read previous snapshot" : "read latest snapshot", Status);
  return Status;
}

// Create a coherent read-only USB export. Never publish a writable BlockIo.
EFI_STATUS EFIAPI LogfsSnapshot (VOID **Buffer, UINTN *Bytes)
{
  EFI_STATUS Status;
  UINT8 *Copy=NULL, *Scratch=NULL;
  UINTN At, N;
  if (Buffer==NULL || Bytes==NULL) return EFI_INVALID_PARAMETER;
  *Buffer=NULL;*Bytes=0;
  if (mBusy || mLease || mPoisoned || LogTpl()!=TPL_APPLICATION) return EFI_NOT_READY;
  mBusy=TRUE;
  Status=FindLogfs ();if (EFI_ERROR(Status)) goto Done;
  if (!PinnedMedia() || mVolumeBytes==0 || mVolumeBytes>LOG_PARTITION_BYTES || mVolumeBytes%512!=0) {
    Status=EFI_SECURITY_VIOLATION;goto Done;
  }
  Copy=AllocatePool ((UINTN)mVolumeBytes);Scratch=LogBuffer (mRoot,65536);
  if (Copy==NULL || Scratch==NULL) { Status=EFI_OUT_OF_RESOURCES;goto Done; }
  for (At=0;At<mVolumeBytes;At+=N) {
    N=MIN ((UINTN)mVolumeBytes-At,65536);
    if (!PinnedMedia() || At%mRootBlockSize!=0 || N%mRootBlockSize!=0 || N>mVolumeBytes-At) {
      Status=EFI_SECURITY_VIOLATION;goto Done;
    }
    Status=ReadRoot (mRoot,mExtent.StartingLBA+At/mRootBlockSize,N,Scratch);
    if (EFI_ERROR(Status)) goto Done;
    CopyMem (Copy+At,Scratch,N);
  }
  if (!PinnedMedia()) { Status=EFI_SECURITY_VIOLATION;goto Done; }
  *Buffer=Copy;*Bytes=(UINTN)mVolumeBytes;Copy=NULL;Status=EFI_SUCCESS;
Done:
  if (Copy!=NULL) FreePool(Copy);
  if (Scratch!=NULL) LogFree(Scratch);
  mBusy=FALSE;
  LogTrace ("read-only FAT snapshot; no GPT/tail/other partition export",Status);
  return Status;
}
