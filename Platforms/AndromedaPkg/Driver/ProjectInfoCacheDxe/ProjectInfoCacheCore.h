// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native ProjectInfoDxe copies one 0x58-byte OEM row into SMEM item 135,
// replacing only project ADC (+4) and hardware ADC (+0x24).
#define PROJECT_RECORD_BYTES 0x58
STATIC BOOLEAN
ProjectCacheMatches (CONST UINT8 *Cache, UINT32 Chip, CONST UINT8 *Rows, UINTN Count)
{
  UINTN Row, Byte;
  UINT32 ProjectAdc, HardwareAdc;
  if (Cache == NULL || Rows == NULL || Count == 0 || Count > 247 || Chip == 0 ||
      ReadUnaligned32 ((CONST UINT32 *)Cache) != Chip ||
      CompareMem (Cache + 0x14, "shennong\0", 9) != 0) return FALSE;
  ProjectAdc = ReadUnaligned32 ((CONST UINT32 *)(Cache + 4));
  HardwareAdc = ReadUnaligned32 ((CONST UINT32 *)(Cache + 0x24));
  for (Row = 0; Row < Count; Row++) {
    CONST UINT8 *Template = Rows + Row * PROJECT_RECORD_BYTES;
    if (ProjectAdc < ReadUnaligned32 ((CONST UINT32 *)(Template + 8)) ||
        ProjectAdc > ReadUnaligned32 ((CONST UINT32 *)(Template + 0xC)) ||
        HardwareAdc < ReadUnaligned32 ((CONST UINT32 *)(Template + 0x28)) ||
        HardwareAdc > ReadUnaligned32 ((CONST UINT32 *)(Template + 0x2C))) continue;
    for (Byte = 0; Byte < PROJECT_RECORD_BYTES; Byte++) {
      if ((Byte >= 4 && Byte < 8) || (Byte >= 0x24 && Byte < 0x28)) continue;
      if (Cache[Byte] != Template[Byte]) break;
    }
    if (Byte == PROJECT_RECORD_BYTES) return TRUE;
  }
  return FALSE;
}
