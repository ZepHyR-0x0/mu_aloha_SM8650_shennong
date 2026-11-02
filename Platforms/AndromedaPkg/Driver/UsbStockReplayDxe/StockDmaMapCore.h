// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only 4 KiB AArch64 stage-1 walk. Unknown formats never imply DMA works.
typedef BOOLEAN (*STOCK_TABLE_READ)(UINT64, UINT64 *);
enum { StockMapUnknown, StockMapFault, StockMapTranslated };
typedef struct { UINT64 Physical, Descriptor; UINT32 Level; } STOCK_MAP_RESULT;
STATIC UINT32
StockDmaWalk (UINT64 Ttbr, UINT32 Tcr, UINT64 Iova, STOCK_TABLE_READ Read, STOCK_MAP_RESULT *Out)
{
  UINT32 Bits, Levels, Level, Shift;
  UINT64 Table, Entry, Mask;
  if (Out == NULL || Read == NULL) return StockMapUnknown;
  ZeroMem (Out, sizeof (*Out));
  // TG0=0 is 4KiB; EPD0=1 disables TTBR0. Limit to <=48-bit input/PA.
  Bits = 64 - (Tcr & 0x3F);
  if ((Tcr & (3U << 14)) != 0 || (Tcr & (1U << 7)) != 0 || Bits <= 12 || Bits > 48) return StockMapUnknown;
  if (Iova >= (1ULL << Bits)) return StockMapFault;
  Levels = (Bits - 12 + 8) / 9;
  Table = Ttbr & 0x0000FFFFFFFFF000ULL;
  if (Table == 0) return StockMapUnknown;
  for (Level = 4 - Levels; Level < 4; Level++) {
    Shift = 12 + (3 - Level) * 9;
    if (!Read (Table + ((Iova >> Shift) & 0x1FF) * 8, &Entry)) return StockMapUnknown;
    Out->Descriptor = Entry; Out->Level = Level;
    if ((Entry & 1) == 0) return StockMapFault;
    if (Level < 3 && (Entry & 3) == 3) {
      Table = Entry & 0x0000FFFFFFFFF000ULL;
      if (Table == 0) return StockMapUnknown;
      continue;
    }
    if ((Level == 0) || (Level == 3 && (Entry & 3) != 3)) return StockMapFault;
    Mask = (1ULL << Shift) - 1;
    if ((Entry & 0x0000FFFFFFFFF000ULL & Mask) != 0) return StockMapUnknown;
    Out->Physical = (Entry & 0x0000FFFFFFFFF000ULL & ~Mask) | (Iova & Mask);
    return StockMapTranslated;
  }
  return StockMapUnknown;
}
