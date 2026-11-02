#ifndef LOGFS_DISK_H
#define LOGFS_DISK_H
#include <Protocol/BlockIo.h>
VOID LogfsRememberRoot (EFI_HANDLE Handle, EFI_BLOCK_IO_PROTOCOL *Block);
BOOLEAN LogfsOwnInterface (VOID *Interface);
EFI_STATUS EFIAPI LogfsSave (CONST CHAR8 *Buffer, UINTN Length, CONST CHAR8 *Phase);
EFI_STATUS EFIAPI LogfsRead (CHAR8 *Buffer, UINTN *Length, BOOLEAN Previous);
EFI_STATUS EFIAPI LogfsSnapshot (VOID **Buffer, UINTN *Bytes);
#endif
