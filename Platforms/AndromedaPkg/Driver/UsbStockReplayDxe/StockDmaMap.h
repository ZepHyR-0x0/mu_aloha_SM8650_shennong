// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "StockDmaStreamCore.h"

#define STOCK_SMMU_BASE 0x15000000U
#define STOCK_STREAM_COUNT 107
STATIC STOCK_HANDOFF mDmaHandoff;
STATIC BOOLEAN mDmaDone, mDmaRejected;
STATIC CONST UINTN mDmaOffsets[] = {0x608, 0x638, 0x640, 0xDD0};
STATIC CONST CHAR8 *mDmaNames[] = {"event", "trb0", "trb1", "setup"};

STATIC BOOLEAN StockDmaWindow (BOOLEAN Write) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  UINT64 Base = STOCK_SMMU_BASE;
  if (gDS == NULL || gDS->GetMemorySpaceDescriptor == NULL ||
      EFI_ERROR (gDS->GetMemorySpaceDescriptor (Base, &D)) ||
      (D.Attributes & (EFI_MEMORY_RP | (Write ? EFI_MEMORY_RO : 0))) != 0 ||
      D.BaseAddress > Base || D.Length < 0x100000 ||
      Base - D.BaseAddress > D.Length - 0x100000) return FALSE;
  // Exact DT window and IDR geometry: CB+80000, 4KiB stride, 71 banks.
  return MmioRead32 (Base + 0x20) == 0x4C017E6B && MmioRead32 (Base + 0x24) == 0x60000047;
}

STATIC VOID StockDmaStreams (STOCK_STREAM *Entries) {
  UINT32 I;
  for (I = 0; I < STOCK_STREAM_COUNT; I++) {
    Entries[I].Smr = MmioRead32 (STOCK_SMMU_BASE + 0x800 + I * 4);
    Entries[I].S2cr = MmioRead32 (STOCK_SMMU_BASE + 0xC00 + I * 4);
  }
}

STATIC VOID StockDmaCaptureHandoff (VOID) {
  STOCK_STREAM Entries[STOCK_STREAM_COUNT];
  UINT32 Slot = 0, I, Plan;
  mDmaHandoff.Valid = FALSE;
  if (!StockDmaWindow (FALSE)) return;
  StockDmaStreams (Entries);
  Plan = StockDmaStreamPlan (Entries, STOCK_STREAM_COUNT, NULL, 71, &Slot);
  if (Plan == StockStreamSingle) {
    for (I = 0; I < STOCK_STREAM_COUNT; I++) {
      if (Entries[I].Smr != 0x80000040 || (Entries[I].S2cr & 0xFF) >= 71) continue;
      mDmaHandoff.Valid = TRUE; mDmaHandoff.Slot = I; mDmaHandoff.Entry = Entries[I];
    }
  }
  StockSay ("[USBMAP b323] handoff valid=%u slot=%u smr=%08x s2cr=%08x\r\n",
    mDmaHandoff.Valid, mDmaHandoff.Slot, mDmaHandoff.Entry.Smr, mDmaHandoff.Entry.S2cr);
}

// Private layout is accepted only in the identified resident stock image.
// Real UsbConfig 4270/428C/4314/43A4 uses +148B0, and +148B9 records Attach.
// Real HAL Create/Configure(0)/Attach constructs this exclusive USB0 chain.
STATIC BOOLEAN StockDmaOwnedBypass (UINT32 *Cb) {
  UINT8 *Domain, *Node, *Client, *Image = mTraceConfigBase;
  UINT32 I;
  if (Image == NULL || !StockTraceReadable (Image, 0x148BA) ||
      *(UINT32 *)(Image + 0x4270) != 0x9122A000 ||
      *(UINT32 *)(Image + 0x428C) != 0x9122C000 ||
      *(UINT32 *)(Image + 0x4314) != 0xF9445920 ||
      *(UINT32 *)(Image + 0x43A4) != 0xF9445920 || Image[0x148B9] != 1) return FALSE;
  Domain = *(VOID **)(Image + 0x148B0);
  if (((UINTN)Domain & 7) != 0 || !StockTraceReadable (Domain, 0x40) ||
      *(UINT32 *)Domain != 0 || *(UINT64 *)(Domain + 8) != 0 ||
      *(UINT64 *)(Domain + 0x10) != 0 || *(UINT64 *)(Domain + 0x18) != 0 ||
      *(UINT32 *)(Domain + 0x20) != 0xE0 || *(UINT32 *)(Domain + 0x24) != 0 ||
      *(UINT32 *)(Domain + 0x28) != 0 ||
      *(UINT32 *)(Domain + 0x38) != 1) return FALSE;
  Node = *(VOID **)(Domain + 0x30);
  if (((UINTN)Node & 7) != 0 || !StockTraceReadable (Node, 0x20) ||
      *(UINT64 *)Node != STOCK_SMMU_BASE || *(UINT32 *)(Node + 0xC) >= 71 ||
      *(UINT64 *)(Node + 0x18) != 0) return FALSE;
  Client = *(VOID **)(Node + 0x10);
  if (((UINTN)Client & 7) != 0 || !StockTraceReadable (Client, 0x68) ||
      CompareMem (Client, "USB0", 5) != 0 || *(UINT32 *)(Client + 0x20) != 0x03000000 ||
      *(UINT64 *)(Client + 0x60) != 0) return FALSE;
  // A second input ID would share the bank even if it has the same name.
  for (I = 0x24; I < 0x60; I += 4) if (*(UINT32 *)(Client + I) != MAX_UINT32) return FALSE;
  *Cb = *(UINT32 *)(Node + 0xC);
  return TRUE;
}

STATIC BOOLEAN StockDmaAddresses (UINT64 *Addresses, BOOLEAN Identity, BOOLEAN Log) {
  UINT32 I, Core;
  UINT64 Expected, Address;
  UINT8 *Record;
  if (!StockTraceReadable (mTraceContext, 0xE18)) return FALSE;
  Core = *(UINT32 *)(mTraceContext + 0x1E0);
  if (Core != 0xA600000) return FALSE;
  for (I = 0; I < ARRAY_SIZE (mDmaOffsets); I++) {
    Expected = *(UINT64 *)(mTraceContext + mDmaOffsets[I]); Address = Expected;
    if (!StockTraceReadable ((VOID *)(UINTN)Expected, I == 3 ? 8 : 4096)) return FALSE;
    if (I == 0) {
      Address = MmioRead32 (Core + 0xC400) | ((UINT64)MmioRead32 (Core + 0xC404) << 32);
    } else if (I == 1 || I == 2) {
      Record = *(VOID **)(mTraceContext + 0x210 + (I - 1) * 0x20);
      if (((UINTN)Record & 7) != 0 || !StockTraceReadable (Record, 0x38)) return FALSE;
      Address = *(UINT64 *)(Record + 8);
    }
    if (Log) StockSay ("[USBMAP b323] %a dma=%lx cpu=%lx identity=%u\r\n",
      mDmaNames[I], Address, Expected, Address == Expected);
    Addresses[I] = Address;
    if (Identity && Address != Expected) return FALSE;
  }
  return TRUE;
}

STATIC BOOLEAN StockDmaIdleBypass (UINT32 Cb, BOOLEAN Log) {
  UINTN Bank = STOCK_SMMU_BASE + 0x80000 + Cb * 0x1000;
  UINT32 Sctlr = MmioRead32 (Bank), Cbar = MmioRead32 (STOCK_SMMU_BASE + 0x1000 + Cb * 4);
  UINT32 Cba2 = MmioRead32 (STOCK_SMMU_BASE + 0x1800 + Cb * 4);
  UINT32 Control = MmioRead32 (0xA60C704), Status = MmioRead32 (0xA60C70C);
  UINT32 Count = MmioRead32 (0xA60C40C) & 0xFFFF;
  if (Log) StockSay ("[USBMAP b323] idle cb=%u sctlr=%08x cbar=%08x cba2=%08x dctl=%08x dsts=%08x count=%u\r\n",
    Cb, Sctlr, Cbar, Cba2, Control, Status, Count);
  return Sctlr == 0xE0 && ((Cbar >> 16) & 3) == 1 && (Cba2 & 1) != 0 &&
    (Control & BIT31) == 0 && (Status & BIT22) != 0 && Count == 0 &&
    MmioRead32 (Bank + 0x20) == 0 && MmioRead32 (Bank + 0x24) == 0 &&
    MmioRead32 (Bank + 0x28) == 0 && MmioRead32 (Bank + 0x2C) == 0;
}

STATIC BOOLEAN StockDmaStable (CONST STOCK_STREAM *Entries, UINT32 Cb, CONST UINT64 *Addresses) {
  STOCK_STREAM Now[STOCK_STREAM_COUNT];
  UINT64 Dma[4]; UINT32 Owned = 71;
  if (!StockDmaWindow (TRUE)) return FALSE;
  StockDmaStreams (Now);
  if (CompareMem (Now, Entries, sizeof (Now)) != 0 || !StockDmaOwnedBypass (&Owned) || Owned != Cb ||
      !StockDmaAddresses (Dma, TRUE, FALSE) || CompareMem (Dma, Addresses, sizeof (Dma)) != 0) return FALSE;
  return StockDmaIdleBypass (Cb, FALSE);
}

STATIC BOOLEAN StockDmaReject (CONST CHAR8 *Reason) {
  mDmaRejected = TRUE;
  StockSay ("[USBMAP b323] SETUP blocked: %a\r\n", Reason);
  StockCheckpoint ("usb-stream-rejected");
  return FALSE;
}

STATIC BOOLEAN StockDmaPreflight (VOID) {
  STOCK_STREAM Entries[STOCK_STREAM_COUNT], After[STOCK_STREAM_COUNT];
  UINT32 Plan, Retire = STOCK_STREAM_COUNT, Cb = 71, Owned = 71, I, Cbar, Cba2, Sctlr, Tcr;
  UINT64 Bank, Ttbr, Addresses[4];
  // Done limits logging only. Native detach/reattach can replace the domain
  // and DMA buffers: never cache permission to submit another SETUP.
  BOOLEAN Log = !mDmaDone;
  if (mDmaRejected) return FALSE;
  mDmaDone = TRUE;
  if (!StockDmaWindow (FALSE)) return StockDmaReject ("unverified SMMU window/geometry");
  StockDmaStreams (Entries);
  StockDmaOwnedBypass (&Owned);
  Plan = StockDmaStreamPlan (Entries, STOCK_STREAM_COUNT, &mDmaHandoff, Owned, &Retire);
  if (Log) StockSay ("[USBMAP b324] plan=%u native-owned-cb=%u retire=%u\r\n", Plan, Owned, Retire);
  for (I = 0; I < STOCK_STREAM_COUNT; I++) {
    UINT32 Smr = Entries[I].Smr;
    if ((Smr & BIT31) == 0 || (((Smr & 0x7FFF) ^ 0x40) & ~((Smr >> 16) & 0x7FFF)) != 0) continue;
    if (Log) StockSay ("[USBMAP b323] sid40 slot=%u smr=%08x s2cr=%08x\r\n", I, Smr, Entries[I].S2cr);
    Cb = Entries[I].S2cr & 0xFF;
  }
  if (Plan == StockStreamConflict) return StockDmaReject ("ambiguous/shared/foreign SID40");
  if (Plan == StockStreamRetire) Cb = Owned;
  if (Cb >= 71) return StockDmaReject ("invalid context bank");
  if (Owned != Cb) return StockDmaReject ("single stream not owned by native USB0");
  Bank = STOCK_SMMU_BASE + 0x80000 + Cb * 0x1000;
  Cbar = MmioRead32 (STOCK_SMMU_BASE + 0x1000 + Cb * 4);
  Cba2 = MmioRead32 (STOCK_SMMU_BASE + 0x1800 + Cb * 4);
  Sctlr = MmioRead32 (Bank); Tcr = MmioRead32 (Bank + 0x30);
  Ttbr = MmioRead32 (Bank + 0x20) | ((UINT64)MmioRead32 (Bank + 0x24) << 32);
  if (Log) StockSay ("[USBMAP b323] cb=%u cbar=%08x cba2=%08x sctlr=%08x tcr=%08x ttbr0=%lx\r\n", Cb,Cbar,Cba2,Sctlr,Tcr,Ttbr);
  if (Log) StockSay ("[USBMAP b323] fsr=%08x far=%08x:%08x syn=%08x/%08x GFSR=%08x\r\n",
    MmioRead32 (Bank + 0x58),MmioRead32 (Bank + 0x64),MmioRead32 (Bank + 0x60),
    MmioRead32 (Bank + 0x68),MmioRead32 (Bank + 0x6C),MmioRead32 (STOCK_SMMU_BASE + 0x48));
  if (!StockDmaAddresses (Addresses, TRUE, Log)) return StockDmaReject ("DMA buffer/address validation");
  if (Plan == StockStreamRetire) {
    // The controller has not run yet. Retire only the unchanged ABL entry;
    // the native HAL already owns/configured the new identity-bypass bank.
    // No S2CR/CB/page table/TLB/global SMMU setting is changed.
    if (!StockDmaIdleBypass (Cb, TRUE) || !StockDmaStable (Entries, Cb, Addresses))
      return StockDmaReject ("native bypass/idle/snapshot validation");
    StockSay ("[USBMAP b323] retire-enter slot=%u old=%08x new=%08x\r\n",
      Retire, Entries[Retire].Smr, Entries[Retire].Smr & ~BIT31);
    StockCheckpoint ("usb-stream-retire-enter");
    // Logging may use UFS. Recheck ALL streams and ownership after its return.
    if (!StockDmaStable (Entries, Cb, Addresses)) return StockDmaReject ("state changed during log checkpoint");
    MmioWrite32 (STOCK_SMMU_BASE + 0x800 + Retire * 4, Entries[Retire].Smr & ~BIT31);
    MemoryFence ();
#if defined (MDE_CPU_AARCH64)
    ArmDataSynchronizationBarrier ();
#endif
    Entries[Retire].Smr &= ~BIT31;
    StockDmaStreams (After);
    if (CompareMem (After, Entries, sizeof (After)) != 0) return StockDmaReject ("retirement readback/other stream changed");
    StockSay ("[USBMAP b323] retire-verified slot=%u kept-native-cb=%u; S2CR/other SMRs unchanged\r\n", Retire, Cb);
    StockCheckpoint ("usb-stream-retired");
    if (!StockDmaStable (Entries, Cb, Addresses)) return StockDmaReject ("state changed after retirement checkpoint");
    return TRUE;
  }
  if (Sctlr == 0xE0 && (Cba2 & 1) != 0 && ((Cbar >> 16) & 3) == 1 &&
      MmioRead32 (Bank + 0x20) == 0 && MmioRead32 (Bank + 0x24) == 0 &&
      MmioRead32 (Bank + 0x28) == 0 && MmioRead32 (Bank + 0x2C) == 0) {
    // Read-only validation is also legal while running. Retirement above
    // still requires idle and full-table checks before/after checkpoints.
    StockDmaStreams (After);
    if (CompareMem (Entries, After, sizeof (After)) != 0)
      return StockDmaReject ("stream changed during SETUP validation");
    return TRUE;
  }
  return StockDmaReject ("native USB0 bypass context changed");
}
