/** @file Read-only storage boundary used by the GUI browser. */
#ifndef READ_ONLY_STORAGE_H
#define READ_ONLY_STORAGE_H
#define READ_ONLY_STORAGE_GUID {0x55da0c71,0x5a7d,0x4942,{0xa4,0x7e,0x85,0x7c,0x99,0x44,0x07,0x31}}
typedef struct {
  UINT32 Revision;
  UINT32 Denied;
  BOOLEAN (EFIAPI *IsProtected)(EFI_HANDLE Handle);
  // Revision 2: only this logger may write the independently verified logfs.
  EFI_STATUS (EFIAPI *SaveLog)(CONST CHAR8 *Buffer, UINTN Length, CONST CHAR8 *Phase);
  EFI_STATUS (EFIAPI *ReadLog)(CHAR8 *Buffer, UINTN *Length, BOOLEAN Previous);
  // Revision 3: caller owns a FreePool-compatible immutable FAT-volume copy.
  // Excludes both GPTs, other partitions and the non-FAT logfs tail.
  EFI_STATUS (EFIAPI *SnapshotLogfs)(VOID **Buffer, UINTN *Bytes);
} READ_ONLY_STORAGE_POLICY;
#endif
