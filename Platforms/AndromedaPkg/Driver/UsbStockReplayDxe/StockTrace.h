// Copyright (c) 2026, ProjectMu shennong port.
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded tracing. No native instructions or global service tables are patched.
// The verified CTRL_SETUP entry can run as a staged equivalent of native6E0C.
#include "StockSetupCore.h"
#include "StockUsbPolicy.h"
typedef EFI_STATUS (EFIAPI *STOCK_GET_CONNECTION)(VOID *, UINT32, BOOLEAN *);
typedef EFI_STATUS (EFIAPI *STOCK_GET_CHARGER)(UINT32, UINT32 *);
typedef struct { UINT8 Prefix[0x30]; STOCK_GET_CONNECTION GetConnection; } STOCK_CONFIG_TRACE;
typedef struct { UINT8 Prefix[0x20]; STOCK_GET_CHARGER GetCharger; } STOCK_POWER_TRACE;
STATIC_ASSERT (OFFSET_OF (STOCK_CONFIG_TRACE, GetConnection) == 0x30, "config ABI");
STATIC_ASSERT (OFFSET_OF (STOCK_POWER_TRACE, GetCharger) == 0x20, "power ABI");
STATIC_ASSERT (OFFSET_OF (EFI_DXE_SERVICES, GetMemorySpaceDescriptor) == 0x38, "GCD getter ABI");
STATIC_ASSERT (OFFSET_OF (EFI_DXE_SERVICES, SetMemorySpaceAttributes) == 0x40, "GCD setter ABI");

STATIC EFI_GUID mFnFileGuid = {0x94F8A6A7,0xDC34,0x4101,{0x88,0xC1,0x99,0x17,0x9C,0xCE,0xAE,0x83}};
STATIC EFI_GUID mConfigFileGuid = {0xCD823A4D,0x7DEC,0x4531,{0xAE,0x5D,0x41,0x34,0xFA,0x41,0x27,0xB8}};
STATIC EFI_GUID mPowerGuid = {0xE07DF17E,0xE79E,0x4150,{0x93,0x78,0x50,0x62,0x3A,0x14,0x99,0x4A}};
STATIC EFI_USBFN_IO_PROTOCOL *mTraceFn;
STATIC EFI_USBFN_IO_EVENTHANDLER mOriginalEvent;
STATIC EFI_USBFN_IO_DETECT_PORT mOriginalDetect;
STATIC STOCK_CONFIG_TRACE *mTraceConfig;
STATIC STOCK_GET_CONNECTION mOriginalConnection;
STATIC STOCK_POWER_TRACE *mTracePower;
STATIC STOCK_GET_CHARGER mOriginalCharger;
STATIC EFI_BOOT_SERVICES mTraceBs;
STATIC EFI_DXE_SERVICES mTraceDs;
STATIC EFI_RUNTIME_SERVICES mTraceRt;
STATIC EFI_RUNTIME_SERVICES **mNativeRtSlot;
STATIC EFI_BOOT_SERVICES **mNativeBsSlot;
STATIC EFI_DXE_SERVICES **mNativeDsSlot;
STATIC UINT8 *mTraceNativeBase;
STATIC UINT8 *mTraceContext;
STATIC UINT8 *mTraceConfigBase;
typedef VOID (EFIAPI *STOCK_USM_ENTRY)(VOID *, UINT32);
STATIC STOCK_USM_ENTRY *mUsmSlot;
STATIC STOCK_USM_ENTRY mOriginalUsm;
STATIC BOOLEAN mInsideUsm;
STATIC UINT32 mTraceCounts[10];
STATIC UINT32 mUsmCalls;
STATIC UINT32 mUsmStalls;
STATIC UINT32 mSetupPacketLogs, mFnMessageLogs;
// Staged SETUP calls the original primitives in their verified native order.
STATIC BOOLEAN mDoSetup = TRUE;

// Only resolve the immediate caller in the verified resident Usbfn image.
// Do not walk the stack or dereference the reported return address.
STATIC UINTN
StockTraceCaller (UINTN Address)
{
  UINTN Base = (UINTN)mTraceNativeBase;
  return Base != 0 && Address >= Base && Address - Base < 0x16578 ? Address - Base : 0;
}

// Constructor 15A8 allocates E18 bytes. Prepare validates that entire context
// before these fixed-offset reads. Values are observations, never followed.
STATIC VOID
StockTraceBuffers (CONST CHAR8 *Boundary)
{
  if (mTraceContext == NULL) return;
  StockSay ("[USBTRACE b319] buffers %a event=%lx trb0=%lx trb1=%lx\r\n", Boundary,
    *(UINT64 *)(mTraceContext + 0x608), *(UINT64 *)(mTraceContext + 0x638),
    *(UINT64 *)(mTraceContext + 0x640));
}

STATIC BOOLEAN
StockTraceMark (UINTN Kind, UINT32 Limit)
{
  return mPolling && (++mTraceCounts[Kind] <= Limit);
}

STATIC BOOLEAN StockTraceWritable (VOID *Pointer, UINTN Size);
STATIC BOOLEAN StockTraceReadable (VOID *Pointer, UINTN Size);
typedef EFI_STATUS (EFIAPI *STOCK_CORE_INIT)(VOID *, BOOLEAN);
STATIC VOID *mBoardCore;
STATIC STOCK_CORE_INIT *mCoreInitSlot;
STATIC STOCK_CORE_INIT mOriginalCoreInit;
STATIC UINTN mBoardInitReports;

// UsbConfigInit stores the allocated core0 object at +14410; +8 is 9160.
// Repair the board components around native common init on every attach.
// This runs before Usbfn core init/SETUP/RunStop, never during an active link.
STATIC EFI_STATUS EFIAPI
StockBoardCoreInit (VOID *Core, BOOLEAN ResetPhy)
{
  EFI_STATUS Status;
  UINT32 Eud;
  BOOLEAN Log = mBoardInitReports++ < 6;
  if (Core != mBoardCore || !StockTraceReadable (Core, 0xC8) ||
      *(UINT32 *)((UINT8 *)Core + 0x80) != 5 ||
      *(UINT64 *)((UINT8 *)Core + 0x88) != 0xA600000 || mOriginalCoreInit == NULL)
    return EFI_UNSUPPORTED;
  // Preserve the native skip when RunStop is already set. No live-link reset.
  if (*(UINT64 *)((UINT8 *)Core + 0xB0) != 0 && (MmioRead32 (0xA60C704) & BIT31) != 0)
    return mOriginalCoreInit (Core, ResetPhy);
  Eud = MmioRead32 (0x88E2000);
  if (Log) StockSay ("[USBBOARD b327] native init reset=%u EUD=%08x\r\n", ResetPhy, Eud);
  // Native EUD mode intentionally skips PHY reset; do not change its ownership.
  if ((Eud & BIT0) != 0) return mOriginalCoreInit (Core, ResetPhy);
  Status = StockConfigureRepeater ();
  if (!EFI_ERROR (Status)) Status = mOriginalCoreInit (Core, ResetPhy);
  // Native 8780 calls WCD reset again. Restore routing after it, without a
  // further reset. The readback records whether that reset lost our settings.
  if (!EFI_ERROR (Status)) Status = StockConfigureSwitch (FALSE);
  if (!EFI_ERROR (Status)) Status = StockConfigureRepeater ();
  if (Log) StockSay ("[USBBOARD b327] native/board ready -> %r phy94=%08x phy50=%08x phy54=%08x\r\n",
    Status, MmioRead32 (0x88E3094), MmioRead32 (0x88E3050), MmioRead32 (0x88E3054));
  return Status;
}
#include "StockDmaMap.h"

// The verbose snapshot is capped at four calls, but the enter/return markers
// must keep flowing well past that: a silent stall on call 5 or later is
// indistinguishable from "never called again" once every print is gated.
STATIC BOOLEAN
StockBrief (VOID)
{
  return mPolling && mUsmCalls <= 40;
}

// The reset point moves between runs (b237 last line was submit, b238 sg-write)
// and printing more moves it earlier, which is the signature of a time budget
// rather than one faulting instruction. Stamp the markers from the generic
// counter so the gap between USB TEST and the reset can be measured.
STATIC UINT64
StockTraceNowMs (VOID)
{
  UINT64 Count;
  UINT64 Freq;
  __asm__ __volatile__ ("mrs %0, cntfrq_el0" : "=r" (Freq));
  __asm__ __volatile__ ("mrs %0, cntpct_el0" : "=r" (Count));
  if (Freq == 0) return 0;
  return Count / (Freq / 1000);
}

// Bound traversal before invoking the stock library's million-node validator.
// Every node is checked as a complete TD before reading links. No repairs or
// writes are made to a corrupt/foreign list; report it and leave GUI responsive.
STATIC BOOLEAN
StockTraceList (UINT8 *Head, UINT32 *Count)
{
  UINT8 *Node, *Previous = Head;
  UINT32 Seen = 0;
  if (((UINTN)Head & 7) != 0 || !StockTraceWritable (Head, 16)) return FALSE;
  Node = *(VOID **)Head;
  while (Node != Head) {
    if (Seen >= 64 || ((UINTN)Node & 7) != 0 || !StockTraceWritable (Node, 0x460) ||
        *(VOID **)(Node + 8) != Previous) return FALSE;
    Previous = Node;
    Node = *(VOID **)Node;
    Seen++;
  }
  if (*(VOID **)(Head + 8) != Previous) return FALSE;
  *Count = Seen;
  return TRUE;
}

// Usbfn+1E0 holds the 64-bit core base, but the native wait builds a 32-bit
// address (core + 0xC80C). Comparing the 64-bit field against 0xA600000 can
// never be true - it reads as 0x00000000A6000000 - so the DCTL/DALEPENA/EP0CMD
// line never appeared in any b233 log. Read the low word, guard the range, and
// dump the whole block the native path actually depends on.
STATIC VOID
StockTraceCore (CONST CHAR8 *Tag)
{
  UINT32 Core;
  if (mTraceContext == NULL) return;
  Core = *(UINT32 *)(mTraceContext + 0x1E0);
  if (Core < 0x100000 || Core >= 0x10000000) {
    StockSay ("[USBTRACE b319] %a core=%08x out of range\r\n", Tag, Core);
    return;
  }
  StockSay ("[USBTRACE b319] %a core=%08x DCTL=%08x DSTS=%08x DALEPENA=%08x EP0CMD=%08x\r\n",
    Tag, Core, MmioRead32 (Core + 0xC704), MmioRead32 (Core + 0xC70C),
    MmioRead32 (Core + 0xC720), MmioRead32 (Core + 0xC80C));
  /* DISABLED (b241) - three extra lines per dump cost a screen flip. Kept for
     later re-enable; the one DCTL/DSTS/DALEPENA/EP0CMD line above carries the
     fields that actually decided every finding so far.
  StockSay ("[USBTRACE b319] %a GCTL=%08x GSNPSID=%08x PHYCFG=%08x EVTADR=%08x:%08x EVTSIZ=%08x\r\n",
    Tag, MmioRead32 (Core + 0xC110), MmioRead32 (Core + 0xC120), MmioRead32 (Core + 0xC200),
    MmioRead32 (Core + 0xC404), MmioRead32 (Core + 0xC400), MmioRead32 (Core + 0xC408));
  // GTXFIFOSIZ lives at 0xC300 in this variant (Dwc3.h), not 0xC130: reading
  // 0xC130 made a healthy FIFO map look like "size zero" in b237. GHWPARAMS3
  // [22:18] is the FIFO count the stock init loop walks.
  StockSay ("[USBTRACE b319] %a GTXFIFO=%08x/%08x/%08x GRXFIFO=%08x HWPARAMS1=%08x HWPARAMS3=%08x\r\n",
    Tag, MmioRead32 (Core + 0xC300), MmioRead32 (Core + 0xC304), MmioRead32 (Core + 0xC308),
    MmioRead32 (Core + 0xC380), MmioRead32 (Core + 0xC144), MmioRead32 (Core + 0xC14C));
  // The native image also waits on +C714 bit10 (0x4828), so read the device
  // command pair too; a stuck DGCMD has the same symptom as a stuck DEPCMD.
  StockSay ("[USBTRACE b319] %a DGCMD=%08x DGCMDPAR=%08x DCFG=%08x DEVTEN=%08x PAR2/1/0=%08x/%08x/%08x\r\n",
    Tag, MmioRead32 (Core + 0xC710), MmioRead32 (Core + 0xC714),
    MmioRead32 (Core + 0xC700), MmioRead32 (Core + 0xC708),
    MmioRead32 (Core + 0xC800), MmioRead32 (Core + 0xC804), MmioRead32 (Core + 0xC808));
  */
}

STATIC VOID EFIAPI
StockTraceSetupStage (UINT32 Stage, BOOLEAN Enter, UINT32 Status, VOID *Object)
{
  // One growing line for the whole staged path. A page of per-stage lines costs
  // a screen flip, and every flip runs the framebuffer console's page hold.
  // E=empty T=take S=sg P=prepare U=update B=submit W=wait Q=queue, so the last
  // character on screen is the last stage that completed.
  STATIC CONST CHAR8 Marks[] = "ETSPUBWQ";
  STATIC UINTN CheckpointSetups;
  (VOID)Status;
  (VOID)Object;
  if (Stage == StockSetupEmpty && Enter) {
    CheckpointSetups++;
    StockSay ("\r\n[USB] t=%lu usm%u setup:", StockTraceNowMs (), mUsmCalls);
  }
  if (Stage < sizeof (Marks) - 1 && !Enter) StockSay ("%c", Marks[Stage]);
  if (Stage == StockSetupInsert && !Enter) StockSay ("\r\n");

  // b241 reached setup:ETSPU, i.e. Submit died before its first Stall, and its
  // only external effect is the DEPCMDPAR2/1/0 then DEPCMD sequence in 0x90E0.
  // Write the same values back here to separate "writing this block faults" from
  // "only the DEPCMD write faults". Runs once, right before the real Submit.
  // b245 showed the TRB address is fine (ring == phys == trb, off=0) but its
  // word 3 is 0x00000008: HWO (bit0) clear and TRBCTL (bits 11:4) zero, so the
  // controller would never fetch it and a control SETUP wants TRBCTL=2. Dump the
  // same TRB before and after Update to see whether Update is what drops them,
  // and read DEPCMD at submit so CMDACT can be compared across runs.
  if (mTraceContext != NULL && (Stage == StockSetupUpdate || Stage == StockSetupSubmit)) {
    UINT8 *Urb = Object == NULL ? NULL : (UINT8 *)Object + 0x10;
    UINT8 *Trb = NULL;
    if (Object != NULL && StockTraceReadable (Urb, 0x30)) Trb = *(UINT8 **)(Urb + 0x28);
    StockSay ("\r\n[USB] t=%lu %c %a trb=%lx w=%08x %08x %08x %08x\r\n",
      StockTraceNowMs (), Marks[Stage], Enter ? "in" : "out", (UINT64)(UINTN)Trb,
      (Trb != NULL && StockTraceReadable (Trb, 16)) ? *(UINT32 *)Trb : 0,
      (Trb != NULL && StockTraceReadable (Trb, 16)) ? *(UINT32 *)(Trb + 4) : 0,
      (Trb != NULL && StockTraceReadable (Trb, 16)) ? *(UINT32 *)(Trb + 8) : 0,
      (Trb != NULL && StockTraceReadable (Trb, 16)) ? *(UINT32 *)(Trb + 0xC) : 0);
    if (Stage == StockSetupSubmit) {
      UINT32 CoreBase = *(UINT32 *)(mTraceContext + 0x1E0);
      if (CoreBase >= 0x100000 && CoreBase < 0x10000000) {
        StockSay ("[USB] t=%lu depcmd=%08x dctl=%08x\r\n", StockTraceNowMs (),
          MmioRead32 (CoreBase + 0xC80C), MmioRead32 (CoreBase + 0xC704));
      }
    }
    StockSay ("[USB] setup:");
  }
  // Persist the TRB/register observations above before entering the native
  // primitive. The old order saved before those observations were printed.
  if (CheckpointSetups <= 3) {
    if (Stage == StockSetupPrepare && Enter) StockCheckpoint ("usb-prepare-enter");
    if (Stage == StockSetupUpdate && Enter) StockCheckpoint ("usb-update-enter");
    if (Stage == StockSetupSubmit) StockCheckpoint (Enter ? "usb-submit-enter" : "usb-submit-leave");
    if (Stage == StockSetupWait) StockCheckpoint (Enter ? "usb-wait-enter" : "usb-wait-leave");
  }
  /* DISABLED (b241) - verbose per-stage trace and register dumps, kept for
     later re-enable. Same information, ~26 lines per setup call.
  STATIC CONST CHAR8 *Names[] = { "pending-empty", "take-td", "sg-write", "prepare",
    "update-trb", "submit", "wait-ep0", "queue-td" };
  BOOLEAN Full = mUsmCalls <= 4;
  if (Stage < ARRAY_SIZE (Names) &&
      (Full || Stage == StockSetupSubmit || Stage == StockSetupWait))
    StockSay ("[USBTRACE b319] t=%lu usm %u setup %a %a status=%08x object=%lx\r\n",
      StockTraceNowMs (), mUsmCalls, Names[Stage], Enter ? "enter" : "leave", Status, (UINT64)(UINTN)Object);
  // The stock Submit builds a four word TRB at Td+10 before ringing DEPCMD. HWO
  // is bit0 of word 3, TRBCTL bits 11:4, IOC bit 13 - a TRB the controller will
  // never fetch is visible here instead of being inferred later.
  if (Full && Stage == StockSetupSubmit && Enter && Object != NULL &&
      StockTraceReadable ((UINT8 *)Object + 0x10, 16)) {
    UINT32 Word0 = MmioRead32 ((UINTN)Object + 0x10);
    UINT32 Word1 = MmioRead32 ((UINTN)Object + 0x14);
    UINT32 Word2 = MmioRead32 ((UINTN)Object + 0x18);
    UINT32 Word3 = MmioRead32 ((UINTN)Object + 0x1C);
    StockSay ("[USBTRACE b319] trb %08x %08x %08x %08x hwo=%u trbctl=%u ioc=%u\r\n",
      Word0, Word1, Word2, Word3, Word3 & 1, (Word3 >> 4) & 0xFF, (Word3 >> 13) & 1);
  }
  if (Full && (Stage == StockSetupSubmit || Stage == StockSetupWait))
    StockTraceCore (Stage == StockSetupSubmit ? (Enter ? "pre-submit" : "post-submit") :
                                               (Enter ? "pre-wait" : "post-wait"));
  */
}

STATIC BOOLEAN
StockTraceRing (UINT8 *Context, UINTN Direction)
{
  UINT8 *Record, *Ring, *Produce, *Consume;
  UINT32 Length, Outstanding;
  Record = *(VOID **)(Context + 0x210 + Direction * 0x20);
  if (*(UINT32 *)(Context + 0x208 + Direction * 0x20) != 1 ||
      ((UINTN)Record & 7) != 0 || !StockTraceWritable (Record, 0x38)) return FALSE;
  Ring = *(VOID **)(Record + 0x10);
  Produce = *(VOID **)(Record + 0x20);
  Consume = *(VOID **)(Record + 0x28);
  Length = *(UINT32 *)(Record + 0x18);
  Outstanding = *(UINT32 *)(Record + 0x30);
  return Length >= 0x20 && Length <= 0x1000 && (Length & 0xF) == 0 &&
    ((UINTN)Ring & 0xF) == 0 && StockTraceWritable (Ring, Length) &&
    (UINTN)Produce >= (UINTN)Ring && (UINTN)Produce - (UINTN)Ring < Length &&
    (UINTN)Consume >= (UINTN)Ring && (UINTN)Consume - (UINTN)Ring < Length &&
    ((UINTN)Produce & 0xF) == 0 && ((UINTN)Consume & 0xF) == 0 &&
    Outstanding <= Length / 16;
}

// The stock EventHandler is what turns a cable edge into CoreInitImpl, so a
// reset inside it leaves no trace anywhere. Sample the cached cable flag on
// both sides of the call: an attach that never prints the post line died inside.
STATIC VOID StockPhySnapshot (CONST CHAR8 *Boundary);
STATIC UINT8 mLastCable = 0xFF;
STATIC BOOLEAN mCablePending;
// UsbfnDetectPort (RVA 0x234c) is the whole story behind "Failed to detect port
// type": it switches on Usbfn+0x198 and only four values are handled -
//   0 -> PortState 1 (SDP), 1 -> 2 (CDP), 2 -> 3 (DCP), 7 -> 4 (ACA).
// Anything else falls to the default arm, which returns EFI_DEVICE_ERROR and
// writes 0 to PortState. The b307 log reports chg=8, so the charger handler is
// publishing a value this driver does not know, DetectPort fails, and the device
// stack never enumerates - no matter how healthy the link is.
//
// The charger path reads the PMIC, and the PMIC GPIO chain in this port still
// returns 0x7, so a garbage 8 is unsurprising. Clamp it to 0 (SDP, which is what
// a PC port actually is) before and after each HandleEvent, so whatever the
// charger handler writes is normalised before DetectPort ever sees it.
STATIC UINT32 mChargerFixCount;

STATIC VOID
StockClampCharger (CONST CHAR8 *Where)
{
  UINT32 *Charger;
  UINT32 Value;

  if (mTraceContext == NULL) return;
  Charger = (UINT32 *)(mTraceContext + 0x198);
  Value = *Charger;
  if (Value == 0 || Value == 1 || Value == 2 || Value == 7) return;
  *Charger = 0;
  if (mChargerFixCount < 6) {
    StockSay ("[USB] t=%lu charger %a %u -> 0 (SDP)\r\n", StockTraceNowMs (), Where, Value);
    mChargerFixCount++;
  }
}


STATIC VOID
StockCableProbe (UINT32 Call, BOOLEAN Enter)
{
  UINT8 Cable;
  UINT32 Core;
  if (mTraceContext == NULL) return;
  Cable = *(UINT8 *)(mTraceContext + 0x181);
  Core = *(UINT32 *)(mTraceContext + 0x1E0);
  if (Core < 0x100000 || Core >= 0x10000000) Core = 0;
  StockClampCharger (Enter ? "pre" : "post");
  if (Enter) {
    if (Cable != mLastCable) {
      UINT8 Previous = mLastCable;
      mLastCable = Cable;
      mCablePending = TRUE;
      StockPhySnapshot ("cable-edge");
      StockSay ("[USB] t=%lu cab %u->%u p%u pre dctl=%08x dale=%08x\r\n",
        StockTraceNowMs (), Previous, Cable, Call,
        Core ? MmioRead32 (Core + 0xC704) : 0, Core ? MmioRead32 (Core + 0xC720) : 0);
    }
    return;
  }
  if (!mCablePending) return;
  mCablePending = FALSE;
  StockPhySnapshot ("cable-return");
  StockSay ("[USB] t=%lu cab %u post dctl=%08x dsts=%08x dale=%08x\r\n",
    StockTraceNowMs (), Cable,
    Core ? MmioRead32 (Core + 0xC704) : 0, Core ? MmioRead32 (Core + 0xC70C) : 0,
    Core ? MmioRead32 (Core + 0xC720) : 0);
}

// Native UsbConfigCoresInit 8DF0 assigns 9160 to core0 (mode5/baseA600000).
// 9160 chooses PIPE or UTMI after the HS-only flag and SS PHY result. A
// pre-poll unconditional replay selected UTMI even after SS PHY success and
// omitted the native two 100us clock-switch waits. Leave all writes to stock.
STATIC UINT32 mPhySnapshots;
STATIC VOID
StockPhySnapshot (CONST CHAR8 *Boundary)
{
  UINT32 Core;
  if (mPhySnapshots >= 12 || mTraceContext == NULL || mTraceConfigBase == NULL ||
      !StockTraceReadable (mTraceConfigBase + 0x148BA, 1)) return;
  Core = *(UINT32 *)(mTraceContext + 0x1E0);
  if (Core != 0xA600000) return;
  ++mPhySnapshots;
  StockSay ("[USBPHY b327] %a hs-only=%u qs08=%08x pipe=%08x phycfg=%08x dcfg=%08x dsts=%08x\r\n",
    Boundary, mTraceConfigBase[0x148BA], MmioRead32 (Core + 0xF8808),
    MmioRead32 (Core + 0xC2C0), MmioRead32 (Core + 0xC200),
    MmioRead32 (Core + 0xC700), MmioRead32 (Core + 0xC70C));
}

// Read-only snapshots at the native consumer cursor, before HandleEvent.
// Never acknowledge GEVNTCOUNT or change native metadata. 64 raw words/boot.
STATIC UINT32 mEventWords, mEventSummaries;
STATIC UINT64 mLastEventCursor, mLastEventBase;
STATIC UINT32 mLastEventCount, mLastEventFirst;

STATIC VOID
StockEventProbe (VOID)
{
  UINT8 *Dwc, *Meta;
  UINT32 Core, Count, Length, RegLength, First, Limit, Index, Word;
  UINT64 Base, Cursor, Address, At;
  if (mTraceContext == NULL || mEventWords >= 64) return;
  Dwc = mTraceContext + 0x1F0;
  Core = *(UINT32 *)Dwc;
  if (Core != 0xA600000 || Dwc[9] == 0 || Dwc[9] > 32) return;
  Meta = *(VOID **)(Dwc + 0x10);
  if (((UINTN)Meta & 7) != 0 || !StockTraceReadable (Meta, 24)) return;
  Base = *(UINT64 *)Meta; Cursor = *(UINT64 *)(Meta + 8);
  Length = *(UINT32 *)(Meta + 16);
  Count = MmioRead32 (Core + 0xC40C) & 0xFFFF;
  Address = MmioRead32 (Core + 0xC400) | ((UINT64)MmioRead32 (Core + 0xC404) << 32);
  RegLength = MmioRead32 (Core + 0xC408) & 0xFFFF;
  if (!StockUsbEventGeometry (Base, Cursor, Length, *(UINT64 *)(mTraceContext + 0x608),
        Address, RegLength, Count) || !StockTraceReadable ((VOID *)(UINTN)Base, Length)) return;
  First = *(volatile UINT32 *)(UINTN)Cursor;
  if (Base == mLastEventBase && Cursor == mLastEventCursor &&
      Count == mLastEventCount && First == mLastEventFirst) return;
  mLastEventBase = Base; mLastEventCursor = Cursor;
  mLastEventCount = Count; mLastEventFirst = First;
  if (mEventSummaries++ < 16)
    StockSay ("[USBEVT b327] cnt=%u base=%lx off=%u busErr=%08x dcfg=%08x dsts=%08x\r\n",
      Count, Base, (UINT32)(Cursor - Base), MmioRead32 (Core + 0xC4C0) | MmioRead32 (Core + 0xC4C4),
      MmioRead32 (Core + 0xC700), MmioRead32 (Core + 0xC70C));
  Limit = Count / 4; if (Limit > 8) Limit = 8;
  if (Limit > 64 - mEventWords) Limit = 64 - mEventWords;
  At = Cursor;
  for (Index = 0; Index < Limit; Index++) {
    Word = *(volatile UINT32 *)(UINTN)At;
    StockSay ("[USBEVT b327] off=%u raw=%08x kind=%a type=%u ep=%u\r\n",
      (UINT32)(At - Base), Word, (Word & 1) ? "device" : "endpoint",
      (Word & 1) ? ((Word >> 8) & 15) : ((Word >> 6) & 15), (Word >> 1) & 31);
    At = Base + ((At - Base + 4) % Length);
    // Native A440..A494 consumes 12 bytes for event type C, and keeps
    // its 8-byte payload contiguous by wrapping before it if necessary.
    if ((Word & 0xF01) == 0xC01) {
      if (Limit - Index < 3) { Limit = Index + 1; break; }
      if (Base + Length - At < 8) At = Base;
      StockSay ("[USBEVT b327] extended-payload=%08x/%08x\r\n",
        *(volatile UINT32 *)(UINTN)At, *(volatile UINT32 *)(UINTN)(At + 4));
      At = Base + ((At - Base + 8) % Length);
      Index += 2;
    }
  }
  mEventWords += Limit;
}

STATIC BOOLEAN mLinkProbed;
STATIC VOID StockLinkProbe (VOID)
{
  UINT32 Core;
  if (mLinkProbed || mTraceContext == NULL) return;
  Core = *(UINT32 *)(mTraceContext + 0x1E0);
  if (Core != 0xA600000 || MmioRead32 (Core + 0xC720) == 0) return;
  mLinkProbed = TRUE;
  StockSay ("[USB b327] native link qs=%08x speed-byte=%u dcfg=%08x dsts=%08x\r\n",
    MmioRead32 (Core + 0xF8808), mTraceContext[0x188], MmioRead32 (Core + 0xC700),
    MmioRead32 (Core + 0xC70C));
}

// Observation only. CLIENTPD (sCR0 bit0) is active-high: zero is not proof
// that SMMU clients are disabled. No stream or security policy is modified.
STATIC BOOLEAN mSmmuReported;

STATIC VOID
StockSmmuReport (VOID)
{
  UINT32 Core;
  UINT32 Smmu = 0x15000000;

  if (mSmmuReported || mTraceContext == NULL) return;
  Core = *(UINT32 *)(mTraceContext + 0x1E0);
  if (Core < 0x100000 || Core >= 0x10000000) return;
  mSmmuReported = TRUE;
  StockSay ("[USB] t=%lu smmu %08x %08x | %08x %08x | %08x %08x\r\n",
    StockTraceNowMs (), MmioRead32 (Smmu + 0x00), MmioRead32 (Smmu + 0x04),
    MmioRead32 (Smmu + 0x20), MmioRead32 (Smmu + 0x24),
    MmioRead32 (Smmu + 0x800), MmioRead32 (Smmu + 0xC00));
  StockSay ("[USB] t=%lu gsbus cfg0=%08x cfg1=%08x gctl=%08x gctl1=%08x\r\n",
    StockTraceNowMs (), MmioRead32 (Core + 0xC100), MmioRead32 (Core + 0xC104),
    MmioRead32 (Core + 0xC110), MmioRead32 (Core + 0xC11C));
}

STATIC BOOLEAN
StockTraceSetup (VOID *State, BOOLEAN Log)
{
  STOCK_SETUP_OPS Ops;
  UINT8 *Base = mTraceNativeBase;
  UINT32 Pending = 0, Free = 0, Status;
  BOOLEAN PendingValid, FreeValid;
  VOID *Td, *Sg;
  // native6A68 passes Usbfn+161A0 as the callback state, and only +248 is ever
  // read from it. The previous gate demanded 0x260 bytes be writable, which
  // returns FALSE silently whenever the image data page is mapped read-only -
  // the whole staged path then skips without emitting a single trace line.
  // Report every condition, then require readability of the one field used.
  if (Base == NULL || mTraceContext == NULL || State != Base + 0x161A0 ||
      !StockTraceReadable ((UINT8 *)State + 0x248, 8)) {
    if (StockBrief ())
      StockSay ("[USBTRACE b319] setup bail base=%lx ctx=%lx state=%lx expected=%lx readable=%u writable=%u\r\n",
        (UINT64)(UINTN)Base, (UINT64)(UINTN)mTraceContext, (UINT64)(UINTN)State,
        Base != NULL ? (UINT64)(UINTN)(Base + 0x161A0) : 0,
        (UINT8)(State != NULL && StockTraceReadable ((UINT8 *)State + 0x248, 8)),
        (UINT8)(State != NULL && StockTraceWritable (State, 0x260)));
    return FALSE;
  }
  if (*(VOID **)((UINT8 *)State + 0x248) != mTraceContext) {
    if (StockBrief ())
      StockSay ("[USBTRACE b319] setup bail: state holds %lx, protocol context %lx\r\n",
        (UINT64)(UINTN)*(VOID **)((UINT8 *)State + 0x248), (UINT64)(UINTN)mTraceContext);
    return TRUE;
  }
  PendingValid = StockTraceList (mTraceContext + 0x7B8, &Pending);
  FreeValid = StockTraceList (mTraceContext + 0x9B8, &Free);
  if (Log) StockSay ("[USBTRACE b319] setup lists pending=%u valid=%u free=%u valid=%u expected=%u\r\n",
    Pending, PendingValid, Free, FreeValid, *(UINT32 *)(mTraceContext + 0x9C8));
  if (!PendingValid || !FreeValid || Free != *(UINT32 *)(mTraceContext + 0x9C8)) {
    if (Log) StockSay ("[USBTRACE b319] setup blocked: list integrity/count\r\n");
    return TRUE;
  }
  if (!StockTraceRing (mTraceContext, 0) || !StockTraceRing (mTraceContext, 1)) {
    if (Log) StockSay ("[USBTRACE b319] setup blocked: EP0 record/TRB geometry\r\n");
    return TRUE;
  }
  Td = *(VOID **)(mTraceContext + 0x9B8);
  if (Free != 0) {
    Sg = *(VOID **)((UINT8 *)Td + 0x450);
    if (((UINTN)Sg & 7) != 0 || !StockTraceWritable (Sg, 0x40) ||
        !StockTraceWritable (*(VOID **)(mTraceContext + 0xDD0), 8)) {
      if (Log) StockSay ("[USBTRACE b319] setup blocked: SG/SETUP RAM\r\n");
      return TRUE;
    }
  }
  // Check native DMA buffers before RunStop/STARTTRANSFER. A proven missing
  // or non-identity translation would target the wrong RAM; preserve GUI/logs.
  if (!StockDmaPreflight ()) return TRUE;
  // Native CableStatus owns RunStop after CoreInit returns. STARTTRANSFER
  // is legal while halted; native 6E0C makes no early DCTL write.
  Ops.Empty = (VOID *)(Base + 0xC8E8);
  Ops.Take = (VOID *)(Base + 0x53B0);
  Ops.Prepare = (VOID *)(Base + 0x9388);
  Ops.Update = (VOID *)(Base + 0x94E4);
  Ops.Submit = (VOID *)(Base + 0x9C08);
  Ops.Wait = (VOID *)(Base + 0xA884);
  Ops.Insert = (VOID *)(Base + 0xC808);
  if (StockBrief ()) StockSay ("[USB b327] setup begin usm%u\r\n", StockTraceNowMs (), mUsmCalls);
  // Differential ARM64 execution verified the full context, TD pool, MMIO
  // writes, stall counts and error paths against native 6E0C. Checkpoints add
  // timing overhead but do not replace Prepare/Update/Submit/Wait primitives.
  if (mDoSetup) {
    Status = StockSetupCore (mTraceContext, &Ops, StockBrief () ? StockTraceSetupStage : NULL);
    if (StockBrief ()) StockSay ("[USBTRACE b319] t=%lu setup staged-return status=%08x\r\n",
      StockTraceNowMs (), Status);
  } else {
    StockSay ("[USB] t=%lu S5 forward native SETUP; raw probes disabled\r\n", StockTraceNowMs ());
  }
  // With raw probes disabled, the native driver must own the real transfer.
  // Returning TRUE here would deliberately suppress EP0 and prevent enumeration.
  return mDoSetup;
}

STATIC VOID
StockTraceControl (VOID)
{
  UINT8 *Td, *Sg;
  if (mTraceContext == NULL) return;
  StockSay ("[USBTRACE b319] ep0 configured=%u/%u setup=%lx free-td=%u records=%lx/%lx\r\n",
    *(UINT32 *)(mTraceContext + 0x208), *(UINT32 *)(mTraceContext + 0x228),
    *(UINT64 *)(mTraceContext + 0xDD0), *(UINT32 *)(mTraceContext + 0x9C8),
    *(UINT64 *)(mTraceContext + 0x210), *(UINT64 *)(mTraceContext + 0x230));
  Td = *(VOID **)(mTraceContext + 0x9B8);
  if (Td == mTraceContext + 0x9B8 || ((UINTN)Td & 7) != 0 || !StockTraceWritable (Td, 0x460)) {
    StockSay ("[USBTRACE b319] td first=%lx unavailable/empty\r\n", (UINT64)(UINTN)Td);
    return;
  }
  Sg = *(VOID **)(Td + 0x450);
  StockSay ("[USBTRACE b319] td first=%lx sg=%lx sg-ram=%u\r\n",
    (UINT64)(UINTN)Td, (UINT64)(UINTN)Sg, StockTraceWritable (Sg, 0x40));
}

STATIC VOID EFIAPI
StockTraceUsm (VOID *State, UINT32 Event)
{
  UINT32 Index = ++mUsmCalls;
  BOOLEAN Log = StockTraceMark (7, 12);
  BOOLEAN Brief = StockBrief ();
  if (Brief) StockSay ("[USBTRACE b319] t=%lu usm %u enter state=%lx event=%u callback=%lx\r\n",
    StockTraceNowMs (), Index, (UINT64)(UINTN)State, Event, (UINT64)(UINTN)mOriginalUsm);
  // 6E0C does ldr x20,[x0,#248] then IsListEmptyUnsafe(x20+7B8): empty takes the
  // transfer path, non-empty prints "The previous URB is still ACTIVE" and
  // returns. Reading the same head here predicts that branch without needing the
  // native module's own debug output, and pending==usbfn+7B8 means self-loop.
  if (Brief) {
    VOID *Usbfn = StockTraceReadable ((UINT8 *)State + 0x248, 8) ?
      *(VOID **)((UINT8 *)State + 0x248) : NULL;
    BOOLEAN PendingOk = Usbfn != NULL && StockTraceReadable ((UINT8 *)Usbfn + 0x7B8, 16);
    BOOLEAN FreeOk = Usbfn != NULL && StockTraceReadable ((UINT8 *)Usbfn + 0x9B8, 16);
    StockSay ("[USBTRACE b319] usm %u native-view usbfn=%lx pending=%lx/%lx free=%lx count=%u\r\n",
      Index, (UINT64)(UINTN)Usbfn,
      PendingOk ? *(UINT64 *)((UINT8 *)Usbfn + 0x7B8) : 0,
      PendingOk ? *(UINT64 *)((UINT8 *)Usbfn + 0x7C0) : 0,
      FreeOk ? *(UINT64 *)((UINT8 *)Usbfn + 0x9B8) : 0,
      FreeOk ? *(UINT32 *)((UINT8 *)Usbfn + 0x9C8) : 0);
    // CorePoll's first gate is Usbfn+180 (enabled) and +181 is the cached cable
    // flag; +17C is the core index and +A8 the USB_CONFIG protocol. DCTL bit31
    // is RunStop in this build, so this line states whether the core has been
    // started by the time the setup path runs.
    if (Usbfn != NULL && StockTraceReadable ((UINT8 *)Usbfn + 0xA8, 8) &&
        StockTraceReadable ((UINT8 *)Usbfn + 0x178, 0x70)) {
      UINT32 Core = *(UINT32 *)((UINT8 *)Usbfn + 0x1E0);
      BOOLEAN CoreOk = Core >= 0x100000 && Core < 0x10000000;
      StockSay ("[USBTRACE b319] usm %u gates en=%u cable=%u core=%u cfg=%lx tree=%lx corebase=%08x dctl=%08x dalepena=%08x depcmd=%08x dsts=%08x\r\n",
        Index, (UINT32)*(UINT8 *)((UINT8 *)Usbfn + 0x180), (UINT32)*(UINT8 *)((UINT8 *)Usbfn + 0x181),
        *(UINT32 *)((UINT8 *)Usbfn + 0x17C), *(UINT64 *)((UINT8 *)Usbfn + 0xA8),
        *(UINT64 *)((UINT8 *)Usbfn + 0x1B0), Core,
        CoreOk ? MmioRead32 (Core + 0xC704) : 0, CoreOk ? MmioRead32 (Core + 0xC720) : 0,
        CoreOk ? MmioRead32 (Core + 0xC80C) : 0, CoreOk ? MmioRead32 (Core + 0xC70C) : 0);
      // Native 3E98..3F84 overwrites the variable-query result later;
      // a missing UsbfnMaxSpeed alone does not imply CoreInit failure.
      StockSay ("[USBTRACE b319] usm %u state spd=%u chg=%u devaddr=%u conf=%u pwr=%08x\r\n",
        Index, (UINT32)*(UINT8 *)((UINT8 *)Usbfn + 0x188), *(UINT32 *)((UINT8 *)Usbfn + 0x198),
        (UINT32)*(UINT8 *)((UINT8 *)Usbfn + 0x184), (UINT32)*(UINT8 *)((UINT8 *)Usbfn + 0x182),
        *(UINT32 *)((UINT8 *)Usbfn + 0x18C));
    }
  }
  if (Log) { StockTraceBuffers ("usm-enter"); StockTraceControl (); }
  mInsideUsm = TRUE;
  mUsmStalls = 0;
  if (!StockTraceSetup (State, Log)) mOriginalUsm (State, Event);
  mInsideUsm = FALSE;
  if (Brief) StockSay ("[USBTRACE b319] t=%lu usm %u native-return stalls=%u\r\n",
    StockTraceNowMs (), Index, mUsmStalls);
}

STATIC EFI_STATUS EFIAPI __attribute__((noinline))
StockTraceStall (UINTN Microseconds)
{
  EFI_STATUS Status;
  UINTN Caller = StockTraceCaller ((UINTN)__builtin_extract_return_addr (__builtin_return_address (0)));
  BOOLEAN Log = StockTraceMark (mInsideUsm ? 8 : 9, mInsideUsm ? 8 : 4);
  if (mInsideUsm) mUsmStalls++;
  if (Log) StockSay ("[USBTRACE b319] stall enter caller=%lx us=%lu usm=%u\r\n",
    (UINT64)Caller, (UINT64)Microseconds, mInsideUsm);
  // The native wait polls DEPCMD.CMDACT in a 500000us/10us loop; a bare enter
  // marker cannot tell "one stall" from "fifty thousand stalls". Emit the core
  // block for the first few and a coarse counter afterwards.
  if (Log && mInsideUsm) StockTraceCore ("control");
  if (mInsideUsm && mUsmStalls != 0 && (mUsmStalls & 0xFFF) == 0)
    StockSay ("[USBTRACE b319] stall spin %u caller=%lx us=%lu\r\n",
      mUsmStalls, (UINT64)Caller, (UINT64)Microseconds);
  Status = gBS->Stall (Microseconds);
  if (Log) StockSay ("[USBTRACE b319] stall leave %r\r\n", Status);
  return Status;
}

STATIC EFI_STATUS EFIAPI
StockTraceEvent (EFI_USBFN_IO_PROTOCOL *This, EFI_USBFN_MESSAGE *Message,
                 UINTN *Size, EFI_USBFN_MESSAGE_PAYLOAD *Payload)
{
  EFI_STATUS Status;
  BOOLEAN Log = StockTraceMark (0, 4);
  if (Log) StockSay ("[USBTRACE b319] fn-event enter this=%lx\r\n", (UINT64)(UINTN)This);
  if (Log) StockTraceBuffers ("fn-enter");
  StockEventProbe ();
  Status = mOriginalEvent (This, Message, Size, Payload);
  if (!EFI_ERROR (Status) && Message != NULL && Size != NULL && Payload != NULL) {
    if (*Message == EfiUsbMsgSetupPacket && *Size >= sizeof (EFI_USB_DEVICE_REQUEST) && mSetupPacketLogs < 32) {
      EFI_USB_DEVICE_REQUEST *Request = &Payload->udr;
      mSetupPacketLogs++;
      StockSay ("[USBREQ b327] type=%02x req=%02x value=%04x index=%04x len=%u\r\n",
        Request->RequestType, Request->Request, Request->Value, Request->Index, Request->Length);
    }
    if (*Message != EfiUsbMsgNone && mFnMessageLogs++ < 48)
      StockSay ("[USBMSG b327] native=%u size=%u cable=%u dctl=%08x\r\n", (UINT32)*Message,
        (UINT32)*Size, mTraceContext != NULL ? mTraceContext[0x181] : 0xFF, MmioRead32 (0xA60C704));
  }
  if (!EFI_ERROR(Status) && Message!=NULL && Size!=NULL && Payload!=NULL &&
      StockFunctionMessage (*Message,*Size,Payload)) {
    // Only client-owned outcomes are consumed here. SETUP, standard-request
    // buffers and all bus events still pass through the native device layer.
    // StockFunctionEvent delivers the saved class outcome after HandleEvent.
    *Message=EfiUsbMsgNone;
  }
  if (Log) StockSay ("[USBTRACE b319] fn-event leave %r message=%u size=%u\r\n", Status,
    (!EFI_ERROR (Status) && Message != NULL) ? (UINT32)*Message : 0xFFFFFFFF,
    Size != NULL ? (UINT32)*Size : 0);
  if (Log) StockTraceBuffers ("fn-leave");
  return Status;
}

STATIC EFI_STATUS EFIAPI
StockTraceDetect (EFI_USBFN_IO_PROTOCOL *This, EFI_USBFN_PORT_TYPE *Port)
{
  EFI_STATUS Status;
  BOOLEAN Log = StockTraceMark (1, 12);
  if (Log) StockSay ("[USBTRACE b319] detect-port enter this=%lx\r\n", (UINT64)(UINTN)This);
  Status = mOriginalDetect (This, Port);
  if (Log) StockSay ("[USBTRACE b319] detect-port leave %r port=%u\r\n", Status,
    (!EFI_ERROR (Status) && Port != NULL) ? (UINT32)*Port : 0xFFFFFFFF);
  return Status;
}

STATIC EFI_STATUS EFIAPI
StockTraceConnection (VOID *This, UINT32 Core, BOOLEAN *Connected)
{
  EFI_STATUS Status;
  BOOLEAN Log = StockTraceMark (2, 12);
  if (Log) StockSay ("[USBTRACE b319] connection enter core=%u this=%lx\r\n", Core, (UINT64)(UINTN)This);
  Status = mOriginalConnection (This, Core, Connected);
  if (Log) StockSay ("[USBTRACE b319] connection leave %r connected=%u\r\n", Status,
    (!EFI_ERROR (Status) && Connected != NULL) ? (UINT32)*Connected : 0xFFFFFFFF);
  return Status;
}

STATIC EFI_STATUS EFIAPI
StockTraceCharger (UINT32 Core, UINT32 *Port)
{
  EFI_STATUS Status;
  BOOLEAN Log = StockTraceMark (3, 12);
  if (Log) StockSay ("[USBTRACE b319] charger enter core=%u\r\n", Core);
  Status = mOriginalCharger (Core, Port);
  if (Log) StockSay ("[USBTRACE b319] charger leave %r raw=%x\r\n", Status,
    (!EFI_ERROR (Status) && Port != NULL) ? *Port : 0xFFFFFFFF);
  return Status;
}

STATIC EFI_STATUS EFIAPI
StockTracePages (EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                 UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory)
{
  EFI_STATUS Status;
  BOOLEAN Log = StockTraceMark (4, 12);
  if (Log) StockSay ("[USBTRACE b319] pages enter type=%u memtype=%u pages=%u\r\n",
    (UINT32)Type, (UINT32)MemoryType, (UINT32)Pages);
  Status = gBS->AllocatePages (Type, MemoryType, Pages, Memory);
  if (Log) StockSay ("[USBTRACE b319] pages leave %r address=%lx\r\n", Status,
    (!EFI_ERROR (Status) && Memory != NULL) ? *Memory : 0);
  return Status;
}

STATIC EFI_STATUS EFIAPI __attribute__((noinline))
StockTracePool (EFI_MEMORY_TYPE MemoryType, UINTN Size, VOID **Buffer)
{
  EFI_STATUS Status;
  UINTN Caller = StockTraceCaller ((UINTN)__builtin_extract_return_addr (__builtin_return_address (0)));
  BOOLEAN Log = StockTraceMark (6, 12);
  if (Log) StockSay ("[USBTRACE b319] pool enter caller=%lx memtype=%u size=%lx\r\n",
    (UINT64)Caller, (UINT32)MemoryType, (UINT64)Size);
  // E614 requests 30 hex bytes after DD38 (XN update) and F514 (cache).
  // DwcConfigureEP 8D88 requests 38 hex bytes after publishing an EP0 TRB.
  // CED8 identifies their common native AllocatePool helper; the pointer
  // snapshot distinguishes published event/TRB stages without patching code.
  if (Log && (Size == 0x30 || Size == 0x38)) StockTraceBuffers ("pool-enter");
  Status = gBS->AllocatePool (MemoryType, Size, Buffer);
  if (Log) StockSay ("[USBTRACE b319] pool leave %r caller=%lx size=%lx address=%lx\r\n",
    Status, (UINT64)Caller, (UINT64)Size,
    (!EFI_ERROR (Status) && Buffer != NULL) ? (UINT64)(UINTN)*Buffer : 0);
  return Status;
}

STATIC EFI_STATUS EFIAPI
StockTraceDescriptor (EFI_PHYSICAL_ADDRESS Address, EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Descriptor)
{
  EFI_STATUS Status;
  BOOLEAN Log = StockTraceMark (5, 6);
  if (Log) StockSay ("[USBTRACE b319] gcd-get enter address=%lx\r\n", Address);
  Status = gDS->GetMemorySpaceDescriptor (Address, Descriptor);
  if (Log) StockSay ("[USBTRACE b319] gcd-get leave %r attr=%lx caps=%lx\r\n", Status,
    (!EFI_ERROR (Status) && Descriptor != NULL) ? Descriptor->Attributes : 0,
    (!EFI_ERROR (Status) && Descriptor != NULL) ? Descriptor->Capabilities : 0);
  return Status;
}

STATIC EFI_STATUS EFIAPI
StockTraceAttributes (EFI_PHYSICAL_ADDRESS Address, UINT64 Length, UINT64 Attributes)
{
  EFI_STATUS Status;
  BOOLEAN Log = StockTraceMark (5, 6);
  if (Log) StockSay ("[USBTRACE b319] gcd-set enter address=%lx length=%lx attr=%lx\r\n", Address, Length, Attributes);
  Status = gDS->SetMemorySpaceAttributes (Address, Length, Attributes);
  if (Log) StockSay ("[USBTRACE b319] gcd-set leave %r\r\n", Status);
  return Status;
}

// The firmware maintains the native protocol objects and pool context. Reject
// protected/non-RAM ranges instead of changing memory protection for tracing.
STATIC BOOLEAN
StockTraceWritable (VOID *Pointer, UINTN Size)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  UINT64 Address = (UINTN)Pointer;
  if (Pointer == NULL || Size == 0 || gDS == NULL || gDS->GetMemorySpaceDescriptor == NULL) return FALSE;
  if (EFI_ERROR (gDS->GetMemorySpaceDescriptor (Address, &Descriptor))) return FALSE;
  return Descriptor.GcdMemoryType == EfiGcdMemoryTypeSystemMemory &&
    (Descriptor.Attributes & (EFI_MEMORY_RO | EFI_MEMORY_RP)) == 0 &&
    Address >= Descriptor.BaseAddress && Address - Descriptor.BaseAddress < Descriptor.Length &&
    Size <= Descriptor.Length - (Address - Descriptor.BaseAddress);
}

// Read-only is not unreadable. DXE image data is commonly mapped RO after
// relocation, so demanding writability just to read one field silently
// disables the entire staged path.
STATIC BOOLEAN
StockTraceReadable (VOID *Pointer, UINTN Size)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  UINT64 Address = (UINTN)Pointer;
  if (Pointer == NULL || Size == 0 || gDS == NULL || gDS->GetMemorySpaceDescriptor == NULL) return FALSE;
  if (EFI_ERROR (gDS->GetMemorySpaceDescriptor (Address, &Descriptor))) return FALSE;
  return Descriptor.GcdMemoryType == EfiGcdMemoryTypeSystemMemory &&
    (Descriptor.Attributes & EFI_MEMORY_RP) == 0 &&
    Address >= Descriptor.BaseAddress && Address - Descriptor.BaseAddress < Descriptor.Length &&
    Size <= Descriptor.Length - (Address - Descriptor.BaseAddress);
}

STATIC UINT8 *
StockTraceImage (EFI_GUID *FileGuid, UINTN Function, UINTN Rva, UINTN Minimum)
{
  EFI_HANDLE *Handles = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *Image;
  UINTN Count = 0, Index;
  UINT8 *Base = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Count, &Handles))) return NULL;
  for (Index = 0; Index < Count; Index++) {
    Image = NULL;
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiLoadedImageProtocolGuid, (VOID **)&Image)) &&
        Image != NULL && Image->FilePath != NULL && StockPathHasFileGuid (Image->FilePath, FileGuid) &&
        Image->ImageSize >= Minimum && Function == (UINTN)Image->ImageBase + Rva) {
      Base = Image->ImageBase;
      break;
    }
  }
  if (Handles != NULL) FreePool (Handles);
  return Base;
}

STATIC VOID
StockTracePrepare (VOID *Config, EFI_USBFN_IO_PROTOCOL *Fn)
{
  UINT8 *FnBase, *ConfigBase, *Context;
  STOCK_CONFIG_TRACE *NativeConfig;
  UINT64 Signature;
  VOID *Power = NULL;
  mTraceContext = mTraceNativeBase = NULL;
  mTraceConfigBase = NULL;
  mUsmSlot = NULL;
  mBoardCore = NULL;
  mCoreInitSlot = NULL;
  mOriginalCoreInit = NULL;
  FnBase = StockTraceImage (&mFnFileGuid, (UINTN)Fn->EventHandler, 0x2E40, 0x16578);
  ConfigBase = StockTraceImage (&mConfigFileGuid,
    (UINTN)((STOCK_CONFIG_TRACE *)Config)->GetConnection, 0x1540, 0x14588);
  if (FnBase == NULL || ConfigBase == NULL || !StockTraceWritable ((UINT8 *)Fn - 8, 0x200)) {
    StockSay ("[USBTRACE b319] skipped: native identity/RAM range unverified\r\n");
    return;
  }
  mTraceConfigBase = ConfigBase;
  if (StockTraceReadable (ConfigBase + 0x14410, 8) && StockTraceReadable (ConfigBase + 0x9160, 8) &&
      StockTraceReadable (ConfigBase + 0xE25C, 4) && StockTraceReadable (ConfigBase + 0xE6D0, 4) &&
      *(UINT32 *)(ConfigBase + 0x9160) == 0xD102C3FF &&
      *(UINT32 *)(ConfigBase + 0xE25C) == 0xA9BD7BFD &&
      *(UINT32 *)(ConfigBase + 0xE6D0) == 0xA9BF7BFD) {
    UINT8 *Core = *(VOID **)(ConfigBase + 0x14410);
    if (StockTraceWritable (Core, 0xC8) && *(UINT32 *)(Core + 0x80) == 5 &&
        *(UINT64 *)(Core + 0x88) == 0xA600000 &&
        *(UINT8 **)(Core + 8) == ConfigBase + 0x9160) {
      mBoardCore = Core;
      mCoreInitSlot = (VOID *)(Core + 8);
      mOriginalCoreInit = (STOCK_CORE_INIT)(ConfigBase + 0x9160);
    }
  }
  StockSay ("[USBBOARD b327] verified native core0 hook=%u\r\n", mCoreInitSlot != NULL);
  Context = (UINT8 *)Fn - 8;
  Signature = *(UINT64 *)Context;
  NativeConfig = *(VOID **)(Context + 0xA8);
  StockSay ("[USBTRACE b319] context sig=%lx config=%lx public=%lx\r\n",
    Signature, (UINT64)(UINTN)NativeConfig, (UINT64)(UINTN)Config);
  if (Signature != 0x66627375) {
    StockSay ("[USBTRACE b319] skipped: native context signature mismatch\r\n");
    return;
  }
  // This is a newly started native session after Stop/StartEx. A rejected DMA
  // submission belongs to the previous session; validate the new domain and
  // new buffers from scratch instead of keeping USB permanently blocked.
  // StockDmaPreflight still checks ownership, addresses and stream geometry
  // on every Send. The original ABL handoff snapshot stays intact.
  mDmaRejected=FALSE;mDmaDone=FALSE;
  ZeroMem(mTraceCounts,sizeof(mTraceCounts));
  mUsmCalls=mUsmStalls=mSetupPacketLogs=mFnMessageLogs=0;
  mLastCable=0xFF;mCablePending=FALSE;
  mTraceNativeBase = FnBase;
  // Whether the image data page is writable decides if the staged path can run
  // at all; record the descriptor instead of inferring it from a silent skip.
  {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR ImageSpace;
    if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor ((UINT64)(UINTN)FnBase, &ImageSpace)))
      StockSay ("[USBTRACE b319] image base=%lx type=%u attr=%lx caps=%lx len=%lx\r\n",
        (UINT64)(UINTN)FnBase, (UINT32)ImageSpace.GcdMemoryType, ImageSpace.Attributes,
        ImageSpace.Capabilities, ImageSpace.Length);
  }
  if (StockTraceWritable (Context, 0xE18)) mTraceContext = Context;
  else StockSay ("[USBTRACE b319] buffer trace skipped: full context RAM range unverified\r\n");
  if (StockTraceWritable (FnBase + 0x161B0, sizeof (STOCK_USM_ENTRY))) {
    STOCK_USM_ENTRY Entry = *(STOCK_USM_ENTRY *)(FnBase + 0x161B0);
    StockSay ("[USBTRACE b319] usm entry=%lx expected=%lx match=%u\r\n",
      (UINT64)(UINTN)Entry, (UINT64)(UINTN)(FnBase + 0x6E0C), (UINT8 *)Entry == FnBase + 0x6E0C);
    if ((UINT8 *)Entry == FnBase + 0x6E0C) {
      mUsmSlot = (VOID *)(FnBase + 0x161B0);
      mOriginalUsm = Entry;
    }
  }
  mTraceFn = Fn;
  mOriginalEvent = Fn->EventHandler;
  mOriginalDetect = Fn->DetectPort;
  // UsbConfigInit copies the same template twice: public protocol at RVA14550
  // (3F0C), controller/core0 protocol at RVA14420 (4074). Usbfn19DC opens
  // the protocol on its controller into Context+A8; CorePoll62D0 uses that
  // object. LocateProtocol's public object need not equal the opened object.
  // Verify a known instance in the exact resident image before dereferencing.
  if ((NativeConfig == (VOID *)(ConfigBase + 0x14420) ||
       NativeConfig == (VOID *)(ConfigBase + 0x14550)) &&
      StockTraceWritable (NativeConfig, sizeof (STOCK_CONFIG_TRACE)) &&
      *(UINT64 *)NativeConfig == 0x20001 &&
      (UINTN)NativeConfig->GetConnection == (UINTN)ConfigBase + 0x1540) {
    mTraceConfig = NativeConfig;
    mOriginalConnection = mTraceConfig->GetConnection;
  } else {
    // Keep independent event/allocator diagnostics if the config is unverified.
    StockSay ("[USBTRACE b319] connection trace skipped: native config identity/protection\r\n");
  }
  if (!EFI_ERROR (gBS->LocateProtocol (&mPowerGuid, NULL, &Power)) &&
      StockTraceWritable (Power, sizeof (STOCK_POWER_TRACE))) {
    mTracePower = Power;
    mOriginalCharger = mTracePower->GetCharger;
  }
  // E334 looks up the DXE Services GUID 05AD34BA, not CpuArch. E5A8/E5D4
  // call the standard no-This GCD ABI. Only this USB module sees the copies.
  if (gBS->Hdr.HeaderSize >= sizeof (EFI_TABLE_HEADER) && gBS->Hdr.HeaderSize <= sizeof (mTraceBs) &&
      gDS->Hdr.HeaderSize >= sizeof (EFI_TABLE_HEADER) && gDS->Hdr.HeaderSize <= sizeof (mTraceDs) &&
      gBS->AllocatePages != NULL && gBS->AllocatePool != NULL && gDS->SetMemorySpaceAttributes != NULL &&
      gBS->CalculateCrc32 != NULL && StockTraceWritable (FnBase + 0x16500, 8) &&
      StockTraceWritable (FnBase + 0x16570, 8) &&
      *(EFI_BOOT_SERVICES **)(FnBase + 0x16500) == gBS &&
      *(EFI_DXE_SERVICES **)(FnBase + 0x16570) == gDS) {
    mTraceBs = *gBS;
    mTraceDs = *gDS;
    mTraceBs.AllocatePages = StockTracePages;
    mTraceBs.AllocatePool = StockTracePool;
    if (gBS->Stall != NULL) mTraceBs.Stall = StockTraceStall;
    mTraceDs.GetMemorySpaceDescriptor = StockTraceDescriptor;
    mTraceDs.SetMemorySpaceAttributes = StockTraceAttributes;
    mTraceBs.Hdr.CRC32 = mTraceDs.Hdr.CRC32 = 0;
    if (!EFI_ERROR (gBS->CalculateCrc32 (&mTraceBs, mTraceBs.Hdr.HeaderSize, &mTraceBs.Hdr.CRC32)) &&
        !EFI_ERROR (gBS->CalculateCrc32 (&mTraceDs, mTraceDs.Hdr.HeaderSize, &mTraceDs.Hdr.CRC32))) {
      mNativeBsSlot = (VOID *)(FnBase + 0x16500);
      mNativeDsSlot = (VOID *)(FnBase + 0x16570);
    }
  }
  // Native 3BC4 loads its private gRT at RVA164E8. Override only the
  // verified one-byte speed query during polling; no variable is stored.
  if (gRT != NULL && gRT->Hdr.HeaderSize >= sizeof (EFI_TABLE_HEADER) &&
      gRT->Hdr.HeaderSize <= sizeof (mTraceRt) && gRT->GetVariable != NULL &&
      gBS->CalculateCrc32 != NULL && StockTraceWritable (FnBase + 0x164E8, 8) &&
      *(EFI_RUNTIME_SERVICES **)(FnBase + 0x164E8) == gRT) {
    mTraceRt = *gRT; mTraceRt.GetVariable = StockUsbGetVariable;
    mTraceRt.Hdr.CRC32 = 0;
    if (!EFI_ERROR (gBS->CalculateCrc32 (&mTraceRt, mTraceRt.Hdr.HeaderSize, &mTraceRt.Hdr.CRC32)))
      mNativeRtSlot = (VOID *)(FnBase + 0x164E8);
  }
  StockSay ("[USB b327] speed policy private-rt=%u DEVSPD=%u; native cable edges\r\n", mNativeRtSlot != NULL,mRequestedSpeed);
  StockSay ("[USBTRACE b319] ready fn=%u connection=%u charger=%u private-bs-ds=%u\r\n",
    mTraceFn != NULL, mTraceConfig != NULL, mTracePower != NULL, mNativeDsSlot != NULL);
  StockSay ("[USBTRACE b319] context enabled=%u cable=%u core=%u base=%lx/%lx phy=%lx\r\n",
    Context[0x180], Context[0x181], *(UINT32 *)(Context + 0xB0),
    *(UINT64 *)(Context + 0x1D8), *(UINT64 *)(Context + 0x1E0), *(UINT64 *)(Context + 0x1E8));
  // A self-loop here means the list was initialized; anything else means the
  // head still holds the zero from AllocateZeroPool or a queued transfer.
  StockSay ("[USBTRACE b319] lists pending=%lx/%lx free=%lx/%lx count=%u setup=%lx\r\n",
    *(UINT64 *)(Context + 0x7B8), *(UINT64 *)(Context + 0x7C0),
    *(UINT64 *)(Context + 0x9B8), *(UINT64 *)(Context + 0x9C0),
    *(UINT32 *)(Context + 0x9C8), *(UINT64 *)(Context + 0xDD0));
  // Baseline taken after StartEx but before the first EventHandler. The stock
  // EventHandler runs UsbfnDwcCorePoll -> UsbfnDwcCoreCableStatus -> (on connect)
  // UsbSharedPhyEnable, UsbfnDwcCoreInitImpl and finally DCTL |= 0x80000000
  // (bit31 = RunStop in this build, not bit0). Comparing this block with the
  // per-USM one proves whether the core was ever initialised and started.
  StockTraceCore ("prepared");
  StockPhySnapshot ("before-native-poll");
  StockSay ("[USBTRACE b319] pool tracing=%u buffers=%u caller=Usbfn-RVA(hex) size=hex\r\n",
    mNativeBsSlot != NULL, mTraceContext != NULL);
  StockTraceBuffers ("prepared");
  StockSay ("[USBTRACE b319] downstream usm=%u stall=%u\r\n", mUsmSlot != NULL,
    mNativeBsSlot != NULL && mTraceBs.Stall == StockTraceStall);
}

STATIC VOID
StockTraceBegin (VOID)
{
  if (mCoreInitSlot != NULL && *mCoreInitSlot == mOriginalCoreInit) *mCoreInitSlot = StockBoardCoreInit;
  if (mTraceFn != NULL) {
    if (mTraceFn->EventHandler == mOriginalEvent) mTraceFn->EventHandler = StockTraceEvent;
    if (mOriginalDetect != NULL && mTraceFn->DetectPort == mOriginalDetect) mTraceFn->DetectPort = StockTraceDetect;
  }
  if (mTraceConfig != NULL && mTraceConfig->GetConnection == mOriginalConnection)
    mTraceConfig->GetConnection = StockTraceConnection;
  if (mTracePower != NULL && mOriginalCharger != NULL && mTracePower->GetCharger == mOriginalCharger)
    mTracePower->GetCharger = StockTraceCharger;
  if (mNativeBsSlot != NULL && *mNativeBsSlot == gBS) *mNativeBsSlot = &mTraceBs;
  if (mNativeDsSlot != NULL && *mNativeDsSlot == gDS) *mNativeDsSlot = &mTraceDs;
  if (mNativeRtSlot != NULL && *mNativeRtSlot == gRT) *mNativeRtSlot = &mTraceRt;
  if (mUsmSlot != NULL && *mUsmSlot == mOriginalUsm) *mUsmSlot = StockTraceUsm;
}

STATIC VOID
StockTraceEnd (VOID)
{
  // Do not overwrite a pointer changed by another component while polling.
  if (mCoreInitSlot != NULL && *mCoreInitSlot == StockBoardCoreInit) *mCoreInitSlot = mOriginalCoreInit;
  if (mTraceFn != NULL) {
    if (mTraceFn->EventHandler == StockTraceEvent) mTraceFn->EventHandler = mOriginalEvent;
    if (mTraceFn->DetectPort == StockTraceDetect) mTraceFn->DetectPort = mOriginalDetect;
  }
  if (mTraceConfig != NULL && mTraceConfig->GetConnection == StockTraceConnection)
    mTraceConfig->GetConnection = mOriginalConnection;
  if (mTracePower != NULL && mTracePower->GetCharger == StockTraceCharger)
    mTracePower->GetCharger = mOriginalCharger;
  if (mNativeBsSlot != NULL && *mNativeBsSlot == &mTraceBs) *mNativeBsSlot = gBS;
  if (mNativeDsSlot != NULL && *mNativeDsSlot == &mTraceDs) *mNativeDsSlot = gDS;
  if (mNativeRtSlot != NULL && *mNativeRtSlot == &mTraceRt) *mNativeRtSlot = gRT;
  if (mUsmSlot != NULL && *mUsmSlot == StockTraceUsm) *mUsmSlot = mOriginalUsm;
}
