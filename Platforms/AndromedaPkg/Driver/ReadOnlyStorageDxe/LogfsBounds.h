/** Pure validation shared by the production logger and host regression tests. */
#ifndef LOGFS_BOUNDS_H
#define LOGFS_BOUNDS_H
#include <Uefi/UefiGpt.h>
#include <Library/BaseMemoryLib.h>
STATIC CONST EFI_GUID mLogfsType = {0xbc0330eb,0x3410,0x4951,{0xa6,0x17,0x03,0x89,0x8d,0xbe,0x33,0x72}};
STATIC BOOLEAN LogfsName (CONST EFI_PARTITION_ENTRY *Entry)
{
  STATIC CONST CHAR16 Name[36] = {L'l',L'o',L'g',L'f',L's'};
  return CompareMem (Entry->PartitionName, Name, sizeof (Name)) == 0;
}
STATIC BOOLEAN LogfsRange (UINT64 Start, UINT64 End, UINT64 RootLast, UINT64 Lba, UINTN Bytes, UINT32 BlockSize)
{
  UINT64 Count;
  if (BlockSize == 0 || Bytes == 0 || Bytes % BlockSize != 0 || Start > End || End >= RootLast) return FALSE;
  Count = Bytes / BlockSize;
  return Lba >= Start && Lba <= End && Count <= End - Lba + 1;
}
// Partition capacity and free space are different. Derive the FAT volume's
// smaller write boundary only after verifying the exact GPT logfs extent.
STATIC BOOLEAN LogfsVolume (CONST UINT8 *Boot, UINT32 BlockSize,
  UINT64 PartitionBytes, UINT64 *VolumeBytes)
{
  UINT32 Sector, Total, Reserved, Fats, FatSectors, RootEntries, ClusterSectors;
  UINT64 Overhead, Clusters, FatBytes, Bytes;
  UINT32 Bits;
  if (Boot == NULL || VolumeBytes == NULL || ReadUnaligned16 ((CONST UINT16 *)(Boot + 510)) != 0xaa55) return FALSE;
  Sector = ReadUnaligned16 ((CONST UINT16 *)(Boot + 11));
  Total = ReadUnaligned16 ((CONST UINT16 *)(Boot + 19));
  if (Total == 0) Total = ReadUnaligned32 ((CONST UINT32 *)(Boot + 32));
  Reserved = ReadUnaligned16 ((CONST UINT16 *)(Boot + 14));
  Fats = Boot[16]; ClusterSectors = Boot[13];
  RootEntries = ReadUnaligned16 ((CONST UINT16 *)(Boot + 17));
  FatSectors = ReadUnaligned16 ((CONST UINT16 *)(Boot + 22));
  if (FatSectors == 0) FatSectors = ReadUnaligned32 ((CONST UINT32 *)(Boot + 36));
  if (Sector != BlockSize || Sector < 512 || Sector > 4096 || (Sector & (Sector - 1)) != 0 ||
      ClusterSectors == 0 || ClusterSectors > 128 || (ClusterSectors & (ClusterSectors - 1)) != 0 ||
      Reserved == 0 || Fats == 0 || Fats > 2 || FatSectors == 0 || Total == 0) return FALSE;
  Bytes = (UINT64)Total * Sector;
  Overhead = Reserved + (UINT64)Fats * FatSectors + ((UINT64)RootEntries * 32 + Sector - 1) / Sector;
  if (Bytes > PartitionBytes || Overhead >= Total) return FALSE;
  Clusters = (Total - Overhead) / ClusterSectors;
  Bits = Clusters < 4085 ? 12 : Clusters < 65525 ? 16 : 32;
  FatBytes = (UINT64)FatSectors * Sector;
  if (Clusters == 0 || (Clusters + 2) * Bits > FatBytes * 8 ||
      ((Bits == 32) != (RootEntries == 0))) return FALSE;
  if (Bits == 32 && (ReadUnaligned16 ((CONST UINT16 *)(Boot + 22)) != 0 ||
      ReadUnaligned16 ((CONST UINT16 *)(Boot + 42)) != 0 ||
      (ReadUnaligned16 ((CONST UINT16 *)(Boot + 40)) & 0x80) != 0)) return FALSE;
  *VolumeBytes = Bytes;
  return TRUE;
}
// Headers and table CRCs are checked by the caller before this selection.
STATIC BOOLEAN LogfsSelect (CONST EFI_PARTITION_ENTRY *Entries, UINT32 Count,
  CONST EFI_PARTITION_ENTRY *Reported, UINT64 FirstUsable, UINT64 LastUsable,
  UINT32 PartitionNumber, EFI_PARTITION_ENTRY *Selected)
{
  UINT32 I, Match = MAX_UINT32;
  EFI_GUID Zero = {0};
  if (Count == 0 || Count > 2048 || FirstUsable > LastUsable) return FALSE;
  for (I = 0; I < Count; I++) {
    CONST EFI_PARTITION_ENTRY *E = &Entries[I];
    if (CompareMem (&E->PartitionTypeGUID, &Zero, sizeof (Zero)) == 0) continue;
    if (E->StartingLBA < FirstUsable || E->EndingLBA > LastUsable || E->StartingLBA > E->EndingLBA ||
        CompareMem (&E->UniquePartitionGUID, &Zero, sizeof (Zero)) == 0) return FALSE;
    if (LogfsName (E)) {
      if (Match != MAX_UINT32 || CompareMem (&E->PartitionTypeGUID, &mLogfsType, sizeof (EFI_GUID)) != 0) return FALSE;
      Match = I;
    }
  }
  if (Match == MAX_UINT32 || PartitionNumber != Match + 1 ||
      CompareMem (&Entries[Match], Reported, sizeof (*Reported)) != 0) return FALSE;
  for (I = 0; I < Count; I++) {
    CONST EFI_PARTITION_ENTRY *E = &Entries[I];
    if (I == Match || CompareMem (&E->PartitionTypeGUID, &Zero, sizeof (Zero)) == 0) continue;
    if (E->StartingLBA <= Entries[Match].EndingLBA && Entries[Match].StartingLBA <= E->EndingLBA) return FALSE;
    if (CompareMem (&E->UniquePartitionGUID, &Entries[Match].UniquePartitionGUID, sizeof (EFI_GUID)) == 0) return FALSE;
  }
  CopyMem (Selected, &Entries[Match], sizeof (*Selected));
  return TRUE;
}
#endif
