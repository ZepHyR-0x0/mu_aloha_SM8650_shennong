// SPDX-License-Identifier: BSD-2-Clause-Patent
// Plan a single exact USB handoff retirement; never alter shared/masked SIDs.
typedef struct { UINT32 Smr, S2cr; } STOCK_STREAM;
typedef struct { BOOLEAN Valid; UINT32 Slot; STOCK_STREAM Entry; } STOCK_HANDOFF;
enum { StockStreamSingle, StockStreamRetire, StockStreamConflict };
STATIC UINT32
StockDmaStreamPlan (CONST STOCK_STREAM *Entries, UINT32 Count,
                   CONST STOCK_HANDOFF *Handoff, UINT32 OwnedCb, UINT32 *Retire)
{
  UINT32 I, Hits = 0, Old = Count, Own = Count;
  if (Entries == NULL || Retire == NULL || Count == 0 || Count > 107) return StockStreamConflict;
  for (I = 0; I < Count; I++) {
    UINT32 Smr = Entries[I].Smr;
    if ((Smr & BIT31) == 0 || (((Smr & 0x7FFF) ^ 0x40) & ~((Smr >> 16) & 0x7FFF)) != 0) continue;
    Hits++;
    if (Smr != 0x80000040 || ((Entries[I].S2cr >> 16) & 3) != 0) return StockStreamConflict;
    if (Handoff != NULL && Handoff->Valid && I == Handoff->Slot &&
        Smr == Handoff->Entry.Smr && Entries[I].S2cr == Handoff->Entry.S2cr) Old = I;
    if (OwnedCb < 71 && (Entries[I].S2cr & 0xFF) == OwnedCb) {
      if (Entries[I].S2cr != OwnedCb) return StockStreamConflict;
      if (Own != Count) return StockStreamConflict;
      Own = I;
    }
  }
  if (Hits == 1) return StockStreamSingle;
  if (Hits != 2 || Old == Count || Own == Count || Old == Own) return StockStreamConflict;
  *Retire = Old;
  return StockStreamRetire;
}
