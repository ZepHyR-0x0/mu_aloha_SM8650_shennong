/** @file Guard all public storage writes, including access-time/dirty updates.
  Register before UFSDxe, PartitionDxe and FAT. Public writes never forwarded;
  the separate private logger validates GPT and bounds every logfs native write.
  Hardware initialization (SSU/hibernation exit) is separate from media data I/O.
*/
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DiskIo2.h>
#include <Protocol/EraseBlock.h>
#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SerialPortLib.h>
#include "ReadOnlyStorage.h"
#include "LogfsDisk.h"

STATIC BOOLEAN EFIAPI IsProtected (EFI_HANDLE Handle);
STATIC READ_ONLY_STORAGE_POLICY mPolicy = {3, 0, IsProtected, LogfsSave, LogfsRead, LogfsSnapshot};
STATIC EFI_GUID mPolicyGuid = READ_ONLY_STORAGE_GUID;
STATIC BOOLEAN mReady;

STATIC EFI_STATUS Deny (VOID)
{
  mPolicy.Denied++;
  if (mPolicy.Denied == 1) {
    CONST CHAR8 Text[] = "[ROFS b327] media write/erase blocked (no command forwarded)\r\n";
    SerialPortWrite ((UINT8 *)Text, sizeof (Text) - 1);
  }
  return EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI WriteBlocks (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId,
                                      EFI_LBA Lba, UINTN Size, VOID *Buffer)
{ return Deny (); }
STATIC EFI_STATUS EFIAPI FlushBlocks (EFI_BLOCK_IO_PROTOCOL *This)
{ return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI WriteBlocksEx (EFI_BLOCK_IO2_PROTOCOL *This, UINT32 MediaId,
                      EFI_LBA Lba, EFI_BLOCK_IO2_TOKEN *Token, UINTN Size, VOID *Buffer)
{
  if (Token != NULL) Token->TransactionStatus = EFI_WRITE_PROTECTED;
  return Deny ();
}
STATIC EFI_STATUS EFIAPI FlushBlocksEx (EFI_BLOCK_IO2_PROTOCOL *This, EFI_BLOCK_IO2_TOKEN *Token)
{
  if (Token != NULL) {
    Token->TransactionStatus = EFI_SUCCESS;
    if (Token->Event != NULL) gBS->SignalEvent (Token->Event);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI WriteDisk (EFI_DISK_IO_PROTOCOL *This, UINT32 MediaId,
                                     UINT64 Offset, UINTN Size, VOID *Buffer)
{ return Deny (); }
STATIC EFI_STATUS EFIAPI WriteDiskEx (EFI_DISK_IO2_PROTOCOL *This, UINT32 MediaId,
                       UINT64 Offset, EFI_DISK_IO2_TOKEN *Token, UINTN Size, VOID *Buffer)
{
  if (Token != NULL) Token->TransactionStatus = EFI_WRITE_PROTECTED;
  return Deny ();
}
STATIC EFI_STATUS EFIAPI FlushDiskEx (EFI_DISK_IO2_PROTOCOL *This, EFI_DISK_IO2_TOKEN *Token)
{
  if (Token != NULL) {
    Token->TransactionStatus = EFI_SUCCESS;
    if (Token->Event != NULL) gBS->SignalEvent (Token->Event);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI EraseBlocks (EFI_ERASE_BLOCK_PROTOCOL *This, UINT32 MediaId,
                                     EFI_LBA Lba, EFI_ERASE_BLOCK_TOKEN *Token, UINTN Size)
{
  if (Token != NULL) Token->TransactionStatus = EFI_WRITE_PROTECTED;
  return Deny ();
}

STATIC BOOLEAN EFIAPI IsProtected (EFI_HANDLE Handle)
{
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_DISK_IO_PROTOCOL *Disk;
  EFI_BLOCK_IO2_PROTOCOL *Block2;
  EFI_DISK_IO2_PROTOCOL *Disk2;
  EFI_ERASE_BLOCK_PROTOCOL *Erase;
  if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&Block)) ||
      Block == NULL || Block->Media == NULL || !Block->Media->ReadOnly ||
      Block->WriteBlocks != WriteBlocks || Block->FlushBlocks != FlushBlocks) return FALSE;
  // FAT requires DiskIo. Refuse unguarded volumes rather than treating the
  // absence of a write button as proof of a read-only filesystem.
  if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiDiskIoProtocolGuid, (VOID **)&Disk)) ||
      Disk->WriteDisk != WriteDisk) return FALSE;
  if (!EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiBlockIo2ProtocolGuid, (VOID **)&Block2)) &&
      (Block2->WriteBlocksEx != WriteBlocksEx || Block2->FlushBlocksEx != FlushBlocksEx)) return FALSE;
  if (!EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiDiskIo2ProtocolGuid, (VOID **)&Disk2)) &&
      (Disk2->WriteDiskEx != WriteDiskEx || Disk2->FlushDiskEx != FlushDiskEx)) return FALSE;
  if (!EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiEraseBlockProtocolGuid, (VOID **)&Erase)) &&
      Erase->EraseBlocks != EraseBlocks) return FALSE;
  return TRUE;
}

typedef struct { EFI_GUID *Guid; VOID *Registration; EFI_EVENT Event; } WATCH;
STATIC WATCH mWatches[] = {
  {&gEfiBlockIoProtocolGuid, NULL, NULL}, {&gEfiBlockIo2ProtocolGuid, NULL, NULL},
  {&gEfiDiskIoProtocolGuid, NULL, NULL}, {&gEfiDiskIo2ProtocolGuid, NULL, NULL},
  {&gEfiEraseBlockProtocolGuid, NULL, NULL}
};
STATIC VOID Protect (WATCH *Watch, EFI_HANDLE Handle)
{
  VOID *Interface;
  if (EFI_ERROR (gBS->HandleProtocol (Handle, Watch->Guid, &Interface))) return;
  // Exact embedded interfaces, never a label or public protocol exemption.
  // Their own callbacks enforce a private lease and verified logfs bounds.
  if (LogfsOwnInterface (Interface)) return;
  if (Watch == &mWatches[0]) {
    EFI_BLOCK_IO_PROTOCOL *P = Interface;
    if (P->WriteBlocks != WriteBlocks) LogfsRememberRoot (Handle, P);
    if (P->Media != NULL) P->Media->ReadOnly = TRUE;
    P->WriteBlocks = WriteBlocks;
    P->FlushBlocks = FlushBlocks;
  } else if (Watch == &mWatches[1]) {
    EFI_BLOCK_IO2_PROTOCOL *P = Interface;
    if (P->Media != NULL) P->Media->ReadOnly = TRUE;
    P->WriteBlocksEx = WriteBlocksEx;
    P->FlushBlocksEx = FlushBlocksEx;
  } else if (Watch == &mWatches[2]) {
    ((EFI_DISK_IO_PROTOCOL *)Interface)->WriteDisk = WriteDisk;
  } else if (Watch == &mWatches[3]) {
    EFI_DISK_IO2_PROTOCOL *P = Interface;
    P->WriteDiskEx = WriteDiskEx;
    P->FlushDiskEx = FlushDiskEx;
  } else {
    ((EFI_ERASE_BLOCK_PROTOCOL *)Interface)->EraseBlocks = EraseBlocks;
  }
}
STATIC VOID EFIAPI Notify (EFI_EVENT Event, VOID *Context)
{
  WATCH *Watch = Context;
  EFI_HANDLE Handle;
  UINTN Size;
  if (!mReady) return;
  for (;;) {
    Size = sizeof (Handle);
    if (EFI_ERROR (gBS->LocateHandle (ByRegisterNotify, Watch->Guid, Watch->Registration, &Size, &Handle))) break;
    Protect (Watch, Handle);
  }
}
EFI_STATUS EFIAPI ReadOnlyStorageEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  UINTN I;
  EFI_STATUS Status;
  for (I = 0; I < sizeof (mWatches) / sizeof (mWatches[0]); I++) {
    Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, Notify, &mWatches[I], &mWatches[I].Event);
    if (EFI_ERROR (Status)) goto Failed;
    Status = gBS->RegisterProtocolNotify (mWatches[I].Guid, mWatches[I].Event, &mWatches[I].Registration);
    if (EFI_ERROR (Status)) goto Failed;
  }
  Status = gBS->InstallProtocolInterface (&ImageHandle, &mPolicyGuid, EFI_NATIVE_INTERFACE, &mPolicy);
  if (EFI_ERROR (Status)) goto Failed;
  mReady = TRUE;
  for (I = 0; I < sizeof (mWatches) / sizeof (mWatches[0]); I++) {
    gBS->SignalEvent (mWatches[I].Event); // Protect already-published protocols too.
  }
  {
    CONST CHAR8 Text[] = "[ROFS b327] BlockIo/2 DiskIo/2 EraseBlock guards registered before UFS/FAT\r\n";
    SerialPortWrite ((UINT8 *)Text, sizeof (Text) - 1);
  }
  return EFI_SUCCESS;
Failed:
  // No callback may alter a protocol until registration and policy publication
  // both succeed. Failure can then close every event without stale hooks.
  for (I = 0; I < sizeof (mWatches) / sizeof (mWatches[0]); I++) {
    if (mWatches[I].Event != NULL) gBS->CloseEvent (mWatches[I].Event);
    mWatches[I].Event = NULL;
    mWatches[I].Registration = NULL;
  }
  {
    CONST CHAR8 Text[] = "[ROFS b327] guard initialization FAILED; halt before UFS/FAT\r\n";
    SerialPortWrite ((UINT8 *)Text, sizeof (Text) - 1);
  }
  // Dispatching storage after this failure would violate the read-only contract.
  CpuDeadLoop ();
  return Status;
}
