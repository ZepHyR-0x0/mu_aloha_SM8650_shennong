#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Protocol/Cpu.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>

//
// Start of DDR Bank 0 on this platform, from PlatformMemoryMapLib.
//
#define DWC3_DRAM_BASE              0x80000000ULL

//
// Upper bound for DMA allocations. DXE Heap runs to 0xD8000000, and a TRB placed at
// 0xD124F000 hung the controller when it tried to fetch it.
//
#define DWC3_DMA_MAX_ADDRESS        0xBFFFFFFFULL
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "UsbfnDwc3Dxe.h"

/**
  Report whether the page containing Buffer is uncached, by asking the GCD.

  Allocation-time answers scroll off the top of the log long before the USB start
  sequence runs, so the same question is asked again at the point where it matters.

  @param[in] Buffer  Any address inside the buffer.

  @retval TRUE   The range is EFI_MEMORY_UC.
  @retval FALSE  It is not, or the descriptor could not be read.
**/
//
// Set by InitUsbfnDwc3Driver when the CPU architectural protocol is available.
//
EFI_CPU_ARCH_PROTOCOL  *mDwc3Cpu = NULL;

/**
  Force a range uncached through the CPU architectural protocol.

  gDS->SetMemorySpaceAttributes only updates the GCD. The translation tables are changed
  by the CPU driver, and DxeCore reaches it through gCpu - which is not installed yet when
  this driver is dispatched. So the GCD can say EFI_MEMORY_UC while the MMU still maps the
  pages write-back, which is a state where every cache check agrees with itself and the
  DMA master still reads stale memory.

  @param[in] Base  Start of the range.
  @param[in] Size  Length of the range.

  @retval EFI_SUCCESS  The CPU driver accepted the change.
**/
EFI_STATUS
Dwc3ForceUncached (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  if (mDwc3Cpu == NULL) {
    RefSay ("[REF] no CPU arch protocol, page tables not updated for 0x%016lx\n", (UINT64)Base);
    return EFI_NOT_AVAILABLE_YET;
  }

  return mDwc3Cpu->SetMemoryAttributes (mDwc3Cpu, Base, Size, EFI_MEMORY_UC);
}

/**
  Re-apply uncached to every DMA buffer, now that the CPU driver is certainly loaded.

  The buffers are allocated when the driver is dispatched, which is early enough that
  gCpu may not exist yet; without it SetMemorySpaceAttributes only touches the GCD. By the
  time the controller is started the CPU driver is up, so the mapping is redone here and
  the result of each call is printed.

  @param[in] Dev  The controller.
**/
VOID
Dwc3ForceAllBuffersUncached (
  IN DWC3_DEV  *Dev
  )
{
  UINT8   Index;
  UINT64  TrbSize;

  TrbSize = sizeof (DWC3_TRB) * DWC3_TRB_PER_EP;

  RefSay ("[REF] uncached: evt=%r ep0=%r\n",
          Dwc3ForceUncached (Dev->EventBufferPhys, DWC3_EVENT_BUFFER_SIZE),
          Dwc3ForceUncached (Dev->Ep0BufferPhys, DWC3_EP0_BOUNCE_SIZE));

  for (Index = 0; Index < Dev->NumPhysicalEps; Index++) {
    if (Dev->Endpoints[Index].TrbPoolPhys == 0) {
      continue;
    }

    if (Index < 4) {
      RefSay ("[REF] uncached: trb%u=%r\n", (UINT32)Index,
              Dwc3ForceUncached (Dev->Endpoints[Index].TrbPoolPhys, TrbSize));
    } else {
      Dwc3ForceUncached (Dev->Endpoints[Index].TrbPoolPhys, TrbSize);
    }
  }

  RefSay ("[REF] uncached: trb pool still %a\n",
          Dwc3DmaBufferIsUc (Dev->Endpoints[DWC3_EP0_OUT].TrbPool) ? "uc" : "WB");
}

/**
  Check that every buffer the controller will read or write lives in DDR.

  The DWC3 data master can only reach DDR Bank 0 on this platform. Handing it a lower
  address does not fail the register write: the endpoint command that names the TRB is
  accepted and then never completes, which looks exactly like a core hang.

  @param[in] Dev  The controller.

  @retval EFI_SUCCESS       All DMA buffers are in DDR.
  @retval EFI_DEVICE_ERROR  At least one is not.
**/
EFI_STATUS
Dwc3ValidateDmaBuffers (
  IN DWC3_DEV *Dev
  )
{
  UINT8   Index;
  UINT64  Phys;

  Phys = (UINT64)Dev->EventBufferPhys;
  if (Phys < DWC3_DRAM_BASE) {
    RefSay ("[REF] DMA fault: event buffer 0x%016lx below DDR\n", Phys);
    return EFI_DEVICE_ERROR;
  }

  Phys = (UINT64)Dev->Ep0BufferPhys;
  if (Phys < DWC3_DRAM_BASE) {
    RefSay ("[REF] DMA fault: EP0 buffer 0x%016lx below DDR\n", Phys);
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < Dev->NumPhysicalEps; Index++) {
    Phys = (UINT64)Dev->Endpoints[Index].TrbPoolPhys;
    if (Phys == 0) {
      continue;
    }

    if (Phys < DWC3_DRAM_BASE) {
      RefSay ("[REF] DMA fault: EP%u TRB ring 0x%016lx below DDR\n",
              (UINT32)Index, Phys);
      return EFI_DEVICE_ERROR;
    }
  }

  return EFI_SUCCESS;
}

BOOLEAN
Dwc3DmaBufferIsUc (
  IN VOID  *Buffer
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;

  if (Buffer == NULL) {
    return FALSE;
  }

  ZeroMem (&Desc, sizeof (Desc));
  if (EFI_ERROR (gDS->GetMemorySpaceDescriptor ((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer, &Desc))) {
    return FALSE;
  }

  return (BOOLEAN)((Desc.Attributes & EFI_MEMORY_UC) != 0);
}

VOID *
Dwc3AllocateDmaBuffer (
  IN  UINTN                 Size,
  OUT EFI_PHYSICAL_ADDRESS *PhysicalAddress)
{
  EFI_STATUS Status;
  VOID      *Buffer;
  UINTN      Pages = EFI_SIZE_TO_PAGES (Size);
  UINTN      Attempt;
  EFI_PHYSICAL_ADDRESS Address;

  if (PhysicalAddress != NULL) {
    *PhysicalAddress = 0;
  }

  for (Attempt = 0; Attempt < 4; Attempt++) {
    //
    // Ask for the buffer below 3 GB rather than wherever the allocator likes.
    //
    // The TRB pool came back at 0xD124F000, in the upper half of DXE Heap, and the
    // controller hangs the machine the moment it is asked to fetch a TRB from there -
    // the write to DEPCMD for STARTTRANSFER never returns, while DEPCFG on the same
    // register succeeds. The event buffer, allocated after this guard went in, landed at
    // 0x81250000, far lower. Constraining the allocation is the cheapest way to find out
    // whether the controller can simply not reach the top of RAM.
    //
    Address = DWC3_DMA_MAX_ADDRESS;
    Status  = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Pages, &Address);
    if (EFI_ERROR (Status)) {
      RefSay ("[REF] DmaBuffer: AllocatePages(%u) below 0x%lx FAILED: %r\n",
              (UINT32)Pages, (UINT64)DWC3_DMA_MAX_ADDRESS, Status);
      return NULL;
    }

    Buffer = (VOID *)(UINTN)Address;

    if ((UINT64)(UINTN)Buffer >= DWC3_DRAM_BASE) {
      break;
    }

    //
    // The allocator handed out something below DDR Bank 0. Free it and say what the GCD
    // thinks that range is - if it calls it system memory, the platform's memory
    // description has a hole in it and that is worth knowing rather than papering over.
    //
    {
      EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;

      if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor ((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer, &Desc))) {
        RefSay ("[REF] DmaBuffer: 0x%lx below DRAM, gcd type=0x%x base=0x%lx len=0x%lx attr=0x%lx\n",
                (UINT64)(UINTN)Buffer, (UINT32)Desc.GcdMemoryType,
                (UINT64)Desc.BaseAddress, (UINT64)Desc.Length, (UINT64)Desc.Attributes);
      }
    }

    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer, Pages);
    Buffer = NULL;
  }

  if (Buffer == NULL) {
    RefSay ("[REF] DmaBuffer: no DRAM buffer after 4 attempts, giving up\n");
    return NULL;
  }

  Status = gDS->SetMemorySpaceAttributes ((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer, EFI_PAGES_TO_SIZE (Pages), EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    //
    // Event rings and TRBs have no cache maintenance; a WB fallback is unsafe.
    //
    {
      EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;

      if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor ((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer, &Desc))) {
        RefSay ("[REF] DmaBuffer: gcd base=0x%lx len=0x%lx attr=0x%lx cap=0x%lx\n",
                (UINT64)Desc.BaseAddress, (UINT64)Desc.Length,
                (UINT64)Desc.Attributes, (UINT64)Desc.Capabilities);
      } else {
        RefSay ("[REF] DmaBuffer: no GCD descriptor covers 0x%lx\n", (UINT64)(UINTN)Buffer);
      }
    }

    RefSay ("[REF] DmaBuffer: SetMemorySpaceAttributes(UC) phys=0x%lx size=0x%lx -> %r, freeing\n",
            (UINT64)(UINTN)Buffer, (UINT64)EFI_PAGES_TO_SIZE (Pages), Status);
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer, Pages);
    return NULL;
  }

  //
  // Do not take SetMemorySpaceAttributes at its word.
  //
  // If these pages are still write-back, every store the CPU makes to a TRB or a transfer
  // buffer sits in cache while the controller reads DRAM over AXI and finds zeros. A
  // zeroed TRB has HWO clear, so the core never starts the transfer and STARTTRANSFER is
  // accepted and never completes - the whole symptom set. Several GCD calls in this port
  // have returned success without taking effect, so read the descriptor back.
  //
  {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
    BOOLEAN                          IsUc;

    ZeroMem (&Desc, sizeof (Desc));
    IsUc = FALSE;
    if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor ((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer, &Desc))) {
      IsUc = (BOOLEAN)((Desc.Attributes & EFI_MEMORY_UC) != 0);
    }

      //
    // The GCD's opinion is not the hardware's opinion. Push it into the page tables.
    //
    {
      EFI_STATUS  CpuStatus;

      CpuStatus = Dwc3ForceUncached ((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer,
                                     EFI_PAGES_TO_SIZE (Pages));
      RefSay ("[REF] DmaBuffer: %u pages at 0x%016lx gcd_uc=%a attr=0x%lx cpu=%r\n",
              (UINT32)Pages, (UINT64)(UINTN)Buffer,
              IsUc ? "yes" : "NO",
              (UINT64)Desc.Attributes,
              CpuStatus);
    }
  }

  ZeroMem (Buffer, EFI_PAGES_TO_SIZE (Pages));

  //
  // Push the zeroed contents to DRAM regardless. If the pages are uncached this is a
  // no-op; if they are not, it is the difference between the controller seeing zeros and
  // seeing whatever was there before.
  //
  WriteBackDataCacheRange (Buffer, EFI_PAGES_TO_SIZE (Pages));

  if (PhysicalAddress != NULL) {
    *PhysicalAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)Buffer;
  }

  return Buffer;
}

VOID
Dwc3FreeDmaBuffer (
  IN VOID  *Buffer,
  IN UINTN  Size)
{
  UINTN Pages = EFI_SIZE_TO_PAGES (Size);

  if (Buffer == NULL) {
    return;
  }

  gDS->SetMemorySpaceAttributes ((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer, EFI_PAGES_TO_SIZE (Pages), EFI_MEMORY_WB);

  gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer, Pages);
}

/**
  Print the whole device register block once.

  One boot should be enough to say what state the core is in, instead of one line at a
  time: GSNPSID and GHWPARAMS describe the core, GCTL/GSTS/DCTL/DCFG/DSTS the control
  state, the PHY registers whether the interface is parked, and the event and FIFO
  registers which parts of the CSR still accept writes.
**/
STATIC
VOID
Dwc3DumpState (IN DWC3_DEV *Dev)
{
  RefSay ("[REF] st1 GSNPSID=%08x GHWPARAMS1=%08x GHWPARAMS3=%08x GHWPARAMS4=%08x\n",
          MmioRead32 (Dev->Base + DWC3_GSNPSID),
          MmioRead32 (Dev->Base + DWC3_GHWPARAMS1),
          MmioRead32 (Dev->Base + DWC3_GHWPARAMS3),
          MmioRead32 (Dev->Base + DWC3_GHWPARAMS4));

  RefSay ("[REF] st2 GCTL=%08x GSTS=%08x GUCTL1=%08x GSBUSCFG0=%08x GSBUSCFG1=%08x\n",
          MmioRead32 (Dev->Base + DWC3_GCTL),
          MmioRead32 (Dev->Base + 0xC118),
          MmioRead32 (Dev->Base + 0xC11C),
          MmioRead32 (Dev->Base + DWC3_GSBUSCFG0),
          MmioRead32 (Dev->Base + DWC3_GSBUSCFG1));

  RefSay ("[REF] st3 DCTL=%08x DCFG=%08x DSTS=%08x DEVTEN=%08x DALEPENA=%08x\n",
          MmioRead32 (Dev->Base + DWC3_DCTL),
          MmioRead32 (Dev->Base + DWC3_DCFG),
          MmioRead32 (Dev->Base + DWC3_DSTS),
          MmioRead32 (Dev->Base + DWC3_DEVTEN),
          MmioRead32 (Dev->Base + DWC3_DALEPENA));

  RefSay ("[REF] st4 GUSB2PHYCFG0=%08x GUSB3PIPECTL0=%08x GTXFIFO0=%08x GRXFIFO0=%08x\n",
          MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0)),
          MmioRead32 (Dev->Base + DWC3_GUSB3PIPECTL (0)),
          MmioRead32 (Dev->Base + DWC3_GTXFIFOSIZ (0)),
          MmioRead32 (Dev->Base + DWC3_GRXFIFOSIZ (0)));
}

STATIC
VOID
Dwc3PostResetSetup (IN DWC3_DEV *Dev)
{
  UINT32 Register;

  //
  // A core soft reset clears the global programming as well, so restore what the Qualcomm
  // driver restores immediately after its own reset - UsbfnDwcCoreInitImpl @0x3868..0x38c8
  // in reverse/src/UsbfnDwc3Dxe.c:
  //
  //   0x386c  DCTL      |= 0x00F00000          literal stock write, field name unverified
  //   0x3880  DCTL       = (v & 0xE0FFFFFF) | 0x10000000   bits [28:24] HIRDTHRES
  //   0x38a0  GSBUSCFG0 |= 0x0000000E          bits [3:0] burst enables
  //   0x38bc  GSBUSCFG1  = (v & 0xFFFFF0FF) | 0x00000E00   bits [11:8] ARQOS
  //
  // None of this exists in the reference driver, and leaving it at reset defaults is a
  // plausible reason for the event ring to stay silent even once its address is programmed.
  //
  Register  = MmioRead32 (Dev->Base + DWC3_DCTL);
  Register |= (0xFUL << 20);
  MmioWrite32 (Dev->Base + DWC3_DCTL, Register);

  Register &= ~(0x1FUL << 24);
  Register |= (1UL << 28);
  MmioWrite32 (Dev->Base + DWC3_DCTL, Register);

  //
  // The vendor writes 0x0E here (0x38a0), which enables INCR4/8/16 only. Linux enables all
  // of them - bits 0 through 7, including undefined-length INCR and INCR32/64/128/256 - and
  // this port drives the core from firmware without the rest of the vendor's init, so the
  // two are not the same situation. If the NoC is refusing transactions, an incomplete set
  // of burst types is a cheap thing to rule out.
  //
  //
  // Clear the REQINFO fields as a single-variable test.
  //
  // GSBUSCFG0[31:16] is four request-attribute fields - data read, descriptor read, data
  // write, descriptor write - that the controller puts on every AXI transaction it issues.
  // The value read back is 0x2222, which came from ABL and has never been checked against
  // what this interconnect expects. A mismatch there does not produce an error: the
  // transaction is dropped, and a read that is dropped never completes, which is exactly
  // what the doorbell looks like.
  //
  // Everything else in this register is left alone - the burst enables in the low byte are
  // the vendor's 0x0E plus the ones Linux also sets.
  //
  Register = MmioRead32 (Dev->Base + DWC3_GSBUSCFG0);
  MmioWrite32 (Dev->Base + DWC3_GSBUSCFG0, (Register & 0x0000FFFF) | 0xFF);
  RefSay ("[REF] GSBUSCFG0 reqinfo cleared: before=0x%08x after=0x%08x\n",
          Register, MmioRead32 (Dev->Base + DWC3_GSBUSCFG0));

  Register = MmioRead32 (Dev->Base + DWC3_GSBUSCFG1);
  MmioWrite32 (Dev->Base + DWC3_GSBUSCFG1, (Register & 0xFFFFF0FF) | 0xE00);

  RefSay ("[REF] post-reset: DCTL=0x%08x GSBUSCFG0=0x%08x GSBUSCFG1=0x%08x\n",
          MmioRead32 (Dev->Base + DWC3_DCTL),
          MmioRead32 (Dev->Base + DWC3_GSBUSCFG0),
          MmioRead32 (Dev->Base + DWC3_GSBUSCFG1));
}

STATIC
EFI_STATUS
Dwc3SoftReset (IN DWC3_DEV *Dev)
{
  UINT32 Register;
  UINT32 Timeout;

  //
  // Preserve the existing PHY preparation without asserting PHYSOFTRST. A stuck
  // CSFTRST is a failure, not evidence that an alternate reset sequence is safe.
  //
  Register = MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0));
  RefSay ("[REF] SoftReset: GUSB2PHYCFG before=0x%08x DCTL=0x%08x GCTL=0x%08x\n",
          Register,
          MmioRead32 (Dev->Base + DWC3_DCTL),
          MmioRead32 (Dev->Base + DWC3_GCTL));

  Register &= ~(DWC3_GUSB2PHYCFG_PHYSOFTRST |
                DWC3_GUSB2PHYCFG_SUSPHY |
                DWC3_GUSB2PHYCFG_ENBLSLPM);
  MmioWrite32 (Dev->Base + DWC3_GUSB2PHYCFG (0), Register);

  gBS->Stall (1000);

  RefSay ("[REF] SoftReset: PHY left running GUSB2PHYCFG=0x%08x\n",
          MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0)));

  Register = MmioRead32 (Dev->Base + DWC3_DCTL);
  MmioWrite32 (Dev->Base + DWC3_DCTL, Register | DWC3_DCTL_CSFTRST);

  RefSay ("[REF] SoftReset: CSFTRST written, DCTL=0x%08x\n",
          MmioRead32 (Dev->Base + DWC3_DCTL));

  //
  // Sample DCTL at three points during the wait. A bit that never moves and a bit that
  // is slowly working through clock synchronisation look the same at the end of a
  // 500 ms timeout, and they mean different things: the first says the core's clock
  // domain is dead, the second says the wait is merely too short.
  //
  for (Timeout = 0; Timeout < DWC3_RESET_TIMEOUT_US; Timeout++) {
    if ((Timeout == 0) || (Timeout == 1000) || (Timeout == 50000)) {
      RefSay ("[REF] SoftReset: t=%u us DCTL=0x%08x GCTL=0x%08x DSTS=0x%08x\n",
              (UINT32)Timeout,
              MmioRead32 (Dev->Base + DWC3_DCTL),
              MmioRead32 (Dev->Base + DWC3_GCTL),
              MmioRead32 (Dev->Base + DWC3_DSTS));
    }

    if ((MmioRead32 (Dev->Base + DWC3_DCTL) & DWC3_DCTL_CSFTRST) == 0) {
      RefSay ("[REF] SoftReset: CSFTRST self-cleared after %u us\n", (UINT32)Timeout);

      if (Dev->IsUsb31) {
        gBS->Stall (50000);
      }

      Dwc3PostResetSetup (Dev);

      return EFI_SUCCESS;
    }

    gBS->Stall (1);
  }

  RefSay ("[REF] SoftReset TIMEOUT after %u us, DCTL=0x%08x GCTL=0x%08x PHYCFG=0x%08x\n",
          (UINT32)DWC3_RESET_TIMEOUT_US,
          MmioRead32 (Dev->Base + DWC3_DCTL),
          MmioRead32 (Dev->Base + DWC3_GCTL),
          MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0)));

  //
  // Three more questions, all answerable here, all needed before the next step.
  //
  // 1. Does a second CSFTRST write behave differently? If the first one latched and the
  //    reset engine is stuck, a retry tells us whether the state is latched or transient.
  // 2. Does GCTL.CORESOFTRESET self-clear? This is the whole-core reset, a different
  //    engine from CSFTRST. If it clears, the core's clock domain is alive and CSFTRST
  //    specifically is the problem, with a workaround available. If it also hangs, the
  //    clock domain itself is dead and no register in this block will help.
  // 3. What does the core say it needs? GHWPARAMS3 reports the PHY interface it was
  //    built for, and GCTL reports how it is currently configured.
  //
  Register = MmioRead32 (Dev->Base + DWC3_DCTL);
  MmioWrite32 (Dev->Base + DWC3_DCTL, Register | DWC3_DCTL_CSFTRST);
  gBS->Stall (10000);
  RefSay ("[REF] SoftReset: second CSFTRST -> DCTL=0x%08x\n",
          MmioRead32 (Dev->Base + DWC3_DCTL));

  //
  // Did the VBUS override survive the reset attempt? QSCRATCH lives at core_base +
  // 0xF8800 and this driver's own platform layer sets UTMI_OTG_VBUS_VALID |
  // SW_SESSVLD_SEL and LANE0_PWR_PRESENT there. If the core cleared them, that is a
  // different problem from the core ignoring them.
  //
  RefSay ("[REF] QSCRATCH after: hs=0x%08x ss=0x%08x\n",
          MmioRead32 (Dev->Base + 0xF8810),
          MmioRead32 (Dev->Base + 0xF8830));

  Register = MmioRead32 (Dev->Base + DWC3_GCTL);
  MmioWrite32 (Dev->Base + DWC3_GCTL, Register | DWC3_GCTL_CORESOFTRESET);
  for (Timeout = 0; Timeout < 10000; Timeout++) {
    if ((MmioRead32 (Dev->Base + DWC3_GCTL) & DWC3_GCTL_CORESOFTRESET) == 0) {
      break;
    }

    gBS->Stall (1);
  }

  RefSay ("[REF] CORESOFTRESET after %u us: GCTL=0x%08x (bit11=%u)\n",
          (UINT32)Timeout,
          MmioRead32 (Dev->Base + DWC3_GCTL),
          (MmioRead32 (Dev->Base + DWC3_GCTL) & DWC3_GCTL_CORESOFTRESET) ? 1U : 0U);

  Register = MmioRead32 (Dev->Base + DWC3_GHWPARAMS3);
  RefSay ("[REF] HW wants: hsphy_ifc=%u ssphy_ifc=%u num_eps=%u in_eps=%u\n",
          (Register >> 2) & 3U,
          Register & 3U,
          (Register >> 12) & 0x3FU,
          (Register >> 18) & 0x1FU);

  // Leave the failed reset intact; do not probe RUN_STOP.
  return EFI_TIMEOUT;
}

STATIC
VOID
Dwc3PhyInterfaceSetup (IN DWC3_DEV *Dev)
{
  UINT32 Register;

  Register = MmioRead32 (Dev->Base + DWC3_GUSB3PIPECTL (0));

  if (Dev->Platform.SuperSpeedPhyDisabled) {
    Register |= DWC3_GUSB3PIPECTL_SUSPHY;
  } else {
    Register &= ~DWC3_GUSB3PIPECTL_SUSPHY;
    Register |= DWC3_GUSB3PIPECTL_DEP1P2P3_EN;
  }

  MmioWrite32 (Dev->Base + DWC3_GUSB3PIPECTL (0), Register);

  Register  = MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0));
  Register &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK | DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
  Register &= ~DWC3_GUSB2PHYCFG_ULPI_UTMI;

  if (Dev->Platform.UtmiWidth == UsbfnUtmiWidth16Bit) {
    Register |= DWC3_GUSB2PHYCFG_PHYIF (UTMI_PHYIF_16_BIT);
    Register |= DWC3_GUSB2PHYCFG_USBTRDTIM (USBTRDTIM_UTMI_16_BIT);
  } else {
    Register |= DWC3_GUSB2PHYCFG_PHYIF (UTMI_PHYIF_8_BIT);
    Register |= DWC3_GUSB2PHYCFG_USBTRDTIM (USBTRDTIM_UTMI_8_BIT);
  }

  Register &= ~(DWC3_GUSB2PHYCFG_SUSPHY | DWC3_GUSB2PHYCFG_ENBLSLPM);

  if (!Dev->Platform.Usb2FreeClockExists) {
    Register &= ~DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS;
  }

  MmioWrite32 (Dev->Base + DWC3_GUSB2PHYCFG (0), Register);
}

STATIC
VOID
Dwc3FrameLengthAdjust (IN DWC3_DEV *Dev)
{
  UINT32 Register;

  if (Dev->Platform.FrameLengthAdjustMask == 0) {
    return;
  }

  Register  = MmioRead32 (Dev->Base + DWC3_GFLADJ);
  Register &= ~Dev->Platform.FrameLengthAdjustMask;
  Register |= Dev->Platform.FrameLengthAdjustValue;
  MmioWrite32 (Dev->Base + DWC3_GFLADJ, Register);
}

EFI_STATUS
Dwc3EventBufferSetup (IN DWC3_DEV *Dev)
{
  UINT32 AddressLow;
  UINT32 AddressHigh;
  UINT32 Size;
  UINT32 Register;
  UINT32 ReadLow;
  UINT32 ReadHigh;

  Size = DWC3_EVENT_BUFFER_SIZE;
  // The ring must fit EVNTSIZ[15:0], contain whole events and be size-aligned.
  if ((Dev->EventBuffer == NULL) || (Dev->EventBufferPhys == 0) ||
      (Size == 0) || (DWC3_GEVNTSIZ_SIZE (Size) != Size) ||
      ((Size & 3) != 0) || ((Size & (Size - 1)) != 0) ||
      ((Dev->EventBufferPhys & (Size - 1)) != 0))
  {
    RefSay ("[REF] EventBuf REJECT buffer=0x%lx phys=0x%lx siz=0x%x\n",
            (UINT64)(UINTN)Dev->EventBuffer, (UINT64)Dev->EventBufferPhys, Size);
    return EFI_INVALID_PARAMETER;
  }

  AddressLow  = (UINT32)Dev->EventBufferPhys;
  AddressHigh = (UINT32)(Dev->EventBufferPhys >> 32);

  // Disable the ring and mask interrupts before programming its address.
  MmioWrite32 (Dev->Base + DWC3_GEVNTSIZ (0), DWC3_GEVNTSIZ_INTMASK);
  Register = MmioRead32 (Dev->Base + DWC3_GEVNTSIZ (0));
  if ((DWC3_GEVNTSIZ_SIZE (Register) != 0) ||
      ((Register & DWC3_GEVNTSIZ_INTMASK) == 0))
  {
    RefSay ("[REF] EventBuf disable mismatch: want SIZ=0x%08x got=0x%08x\n",
            (UINT32)DWC3_GEVNTSIZ_INTMASK, Register);
    MmioWrite32 (Dev->Base + DWC3_GEVNTSIZ (0), DWC3_GEVNTSIZ_INTMASK);
    return EFI_DEVICE_ERROR;
  }

  MmioWrite32 (Dev->Base + DWC3_GEVNTADRLO (0), AddressLow);
  MmioWrite32 (Dev->Base + DWC3_GEVNTADRHI (0), AddressHigh);
  MmioWrite32 (Dev->Base + DWC3_GEVNTSIZ (0), DWC3_GEVNTSIZ_INTMASK | Size);

  ReadLow  = MmioRead32 (Dev->Base + DWC3_GEVNTADRLO (0));
  ReadHigh = MmioRead32 (Dev->Base + DWC3_GEVNTADRHI (0));
  Register = MmioRead32 (Dev->Base + DWC3_GEVNTSIZ (0));
  if ((ReadLow != AddressLow) || (ReadHigh != AddressHigh) ||
      (DWC3_GEVNTSIZ_SIZE (Register) != Size) ||
      ((Register & DWC3_GEVNTSIZ_INTMASK) == 0))
  {
    RefSay ("[REF] EventBuf mismatch: want ADRLO=0x%08x ADRHI=0x%08x SIZ=0x%08x\n",
            AddressLow, AddressHigh, (UINT32)(DWC3_GEVNTSIZ_INTMASK | Size));
    RefSay ("[REF] EventBuf mismatch: got  ADRLO=0x%08x ADRHI=0x%08x SIZ=0x%08x\n",
            ReadLow, ReadHigh, Register);
    // Best effort if the CSR is not accepting writes; never acknowledge events.
    MmioWrite32 (Dev->Base + DWC3_GEVNTSIZ (0), DWC3_GEVNTSIZ_INTMASK);
    return EFI_DEVICE_ERROR;
  }

  // Only acknowledge stale events after verification. This polled driver keeps
  // interrupts masked even on success.
  MmioWrite32 (Dev->Base + DWC3_GEVNTCOUNT (0), MmioRead32 (Dev->Base + DWC3_GEVNTCOUNT (0)) & DWC3_GEVNTCOUNT_MASK);
  Dev->EventPos = 0;
  RefSay ("[REF] EventBuf verified ADRLO=0x%08x ADRHI=0x%08x SIZ=0x%08x\n",
          ReadLow, ReadHigh, Register);
  return EFI_SUCCESS;
}

VOID
Dwc3EventBufferCleanup (IN DWC3_DEV *Dev)
{
  MmioWrite32 (Dev->Base + DWC3_GEVNTADRLO (0), 0);
  MmioWrite32 (Dev->Base + DWC3_GEVNTADRHI (0), 0);
  MmioWrite32 (Dev->Base + DWC3_GEVNTSIZ (0), DWC3_GEVNTSIZ_INTMASK);
  MmioWrite32 (Dev->Base + DWC3_GEVNTCOUNT (0), 0);
}

//
// Implemented in PlatformUsbfnLibSm8650.c, which owns the PHY base address.
//
UINT8
UsbfnPhyReadByte (
  IN UINT32  Offset
  );

VOID
UsbfnPhyUpdateByte (
  IN UINT32  Offset,
  IN UINT8   AndMask,
  IN UINT8   OrMask
  );

VOID
UsbfnPhyStateReport (
  VOID
  );

VOID
UsbfnRepeaterReport (
  VOID
  );

VOID
UsbfnRepeaterPrepare (
  VOID
  );

/**
  Name the USB link state reported in DSTS.USBLNKST.

  USB2 names follow the stock kernel's dwc3_gadget_hs_link_string().
  In particular Reset/Resume are 0xE/0xF, not 0x6/0x7.
**/
CONST CHAR8 *
Dwc3LinkName (
  IN UINT32  State
  )
{
  switch (State & 0xF) {
  case 0x0: return "On";
  case 0x2: return "Sleep";
  case 0x3: return "Suspend";
  case 0x4: return "Disconnected";
  case 0x5: return "EarlySuspend";
  case 0x8: return "Recovery";
  case 0xE: return "Reset";
  case 0xF: return "Resume";
  default:  return "unknown";
  }
}

EFI_STATUS
Dwc3RunStop (
  IN DWC3_DEV *Dev,
  IN BOOLEAN   Run)
{
  UINT32 Register;
  UINT32 Control;
  UINT32 Timeout;
  UINT32 SavedDevten;

  SavedDevten = 0;

  Register = MmioRead32 (Dev->Base + DWC3_DCTL);

  if (Run && ((Register & DWC3_DCTL_CSFTRST) != 0)) {
    RefSay ("[REF] RunStop(TRUE) REJECT CSFTRST DCTL=0x%08x DSTS=0x%08x\n",
            Register, MmioRead32 (Dev->Base + DWC3_DSTS));
    return EFI_DEVICE_ERROR;
  }

  if (Run) {
    //
    // Preserve the literal stock post-reset 0x00F00000 write at RVA 386C.
    // The reverse report's TSTCTRL/TRGTULST labels for bits 23:20 are not
    // supported by that instruction. Stock SET_FEATURE(TEST_MODE) uses
    // BFI #1,#4 at 46FC, confirming TSTCTRL[4:1], which is cleared below.
    // No experiment on the high field is justified by the incorrect labels.
    //
    Register |= DWC3_DCTL_RUN_STOP;
  } else {
    Register &= ~DWC3_DCTL_RUN_STOP;
  }

  Register &= ~DWC3_DCTL_TSTCTRL_MASK;

  if (Run) {
    //
    // Everything that could go wrong the moment the pull-up is asserted, sampled first.
    //
    // The capture stops between "calling RunStop(TRUE)" and the DCTL readback below, so
    // the write that sets RUN_STOP is where the machine goes down. That is the instant
    // the device appears on the host, the core starts consuming the event ring, and the
    // first SETUP gets DMAed. Each of those has an address behind it, so print all of
    // them before asking for any of it.
    //
    RefSay ("[REF] pre-runstop DCTL=0x%08x DSTS=0x%08x DALEPENA=0x%08x DEVTEN=0x%08x\n",
            MmioRead32 (Dev->Base + DWC3_DCTL),
            MmioRead32 (Dev->Base + DWC3_DSTS),
            MmioRead32 (Dev->Base + DWC3_DALEPENA),
            MmioRead32 (Dev->Base + DWC3_DEVTEN));
    RefSay ("[REF] pre-runstop DCFG=0x%08x GCTL=0x%08x PHYCFG=0x%08x PIPECTL=0x%08x\n",
            MmioRead32 (Dev->Base + DWC3_DCFG),
            MmioRead32 (Dev->Base + DWC3_GCTL),
            MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0)),
            MmioRead32 (Dev->Base + DWC3_GUSB3PIPECTL (0)));

    //
    // What the core says it can do, decoded, at the moment it matters.
    //
    // The field layout is the one in this kernel's drivers/usb/dwc3/core.h, not the newer
    // upstream one - there is no DMA field in GHWPARAMS0 on this IP:
    //
    //   [1:0] MODE (0 gadget, 1 host, 2 dual role)   [5:3] MBUS_TYPE
    //   [7:6] SBUS_TYPE   [15:8] MDWIDTH   [23:16] SDWIDTH   [31:24] AWIDTH
    //
    // MDWIDTH and AWIDTH together say whether a master exists and how wide it is. A first
    // attempt at this printed "dma=0" from a field that does not exist on DWC_usb31, which
    // read as "the core has no DMA master" - it does not mean that, and the numbers here
    // have to be read off the register instead.
    //
    {
      UINT32 Hw0 = MmioRead32 (Dev->Base + DWC3_GHWPARAMS0);

      RefSay ("[REF] pre-runstop GHWPARAMS0=0x%08x mode=%u mbus=%u sbus=%u mdw=%u sdw=%u aw=%u\n",
              Hw0,
              Hw0 & 0x3,
              (Hw0 >> 3) & 0x7,
              (Hw0 >> 6) & 0x3,
              (Hw0 >> 8) & 0xFF,
              (Hw0 >> 16) & 0xFF,
              (Hw0 >> 24) & 0xFF);
    }
    RefSay ("[REF] evtbuf cpu=0x%016lx phys=0x%016lx adrlo=0x%08x adrhi=0x%08x siz=0x%08x cnt=0x%08x\n",
            (UINT64)(UINTN)Dev->EventBuffer,
            (UINT64)Dev->EventBufferPhys,
            MmioRead32 (Dev->Base + DWC3_GEVNTADRLO (0)),
            MmioRead32 (Dev->Base + DWC3_GEVNTADRHI (0)),
            MmioRead32 (Dev->Base + DWC3_GEVNTSIZ (0)),
            MmioRead32 (Dev->Base + DWC3_GEVNTCOUNT (0)));
    //
    // Verdicts rather than raw digits. Reading a sixteen-digit address off a photograph
    // has already produced one wrong conclusion; the firmware can compare against the
    // DDR base itself and say ok or BAD, which survives any amount of image blur.
    //
    RefSay ("[REF] addr: evt=0x%016lx %a, trb=0x%016lx %a, ep0=0x%016lx %a\n",
            (UINT64)Dev->EventBufferPhys,
            ((UINT64)Dev->EventBufferPhys >= DWC3_DRAM_BASE) ? "ok" : "BAD-below-DDR",
            (UINT64)Dev->Endpoints[DWC3_EP0_OUT].TrbPoolPhys,
            ((UINT64)Dev->Endpoints[DWC3_EP0_OUT].TrbPoolPhys >= DWC3_DRAM_BASE) ? "ok" : "BAD-below-DDR",
            (UINT64)Dev->Ep0BufferPhys,
            ((UINT64)Dev->Ep0BufferPhys >= DWC3_DRAM_BASE) ? "ok" : "BAD-below-DDR");

    if ((Dev->EventBufferPhys < DWC3_DRAM_BASE) ||
        (Dev->Endpoints[DWC3_EP0_OUT].TrbPoolPhys < DWC3_DRAM_BASE) ||
        (Dev->Ep0BufferPhys < DWC3_DRAM_BASE)) {
      //
      // Never hand a buffer outside DRAM to the controller. It cannot be read, and the
      // failure mode is the whole machine going down rather than an error coming back.
      //
      RefSay ("[REF] refusing to start: a DMA buffer is outside DDR\n");
      return EFI_DEVICE_ERROR;
    }

    //
    // Prove the DCTL write path with the exact same value minus RUN_STOP, so a hang on
    // the next write is unambiguously about the bit and not about the register.
    //
    MmioWrite32 (Dev->Base + DWC3_DCTL, Register & ~DWC3_DCTL_RUN_STOP);
    RefSay ("[REF] DCTL write path ok (RUN_STOP clear), reads 0x%08x\n",
            MmioRead32 (Dev->Base + DWC3_DCTL));

    //
    // Does the device controller state machine actually follow DCTL?
    //
    // With RUN_STOP clear the core must report DEVCTRLHLT, DSTS bit 22, set. The capture
    // before this write already showed DSTS=0x00200001, which has that bit clear while
    // RUN_STOP is also clear - a contradiction. If it is still clear after explicitly
    // clearing RUN_STOP, the status register is not tracking the control register, and a
    // core whose status does not move is a core whose datapath is not clocked.
    //
    // SOFFN is the other half of the same question. It comes from the USB2 frame timer, so
    // on a clocked core with a live link it advances between two reads twenty milliseconds
    // apart. A frozen SOFFN next to a connected host says the datapath clock is not running.
    //
    // Both are read here, before the write that starts the core, because that write is the
    // one that does not come back.
    //
    {
      UINT32 DstsA;
      UINT32 DstsB;

      DstsA = MmioRead32 (Dev->Base + DWC3_DSTS);
      gBS->Stall (20000);
      DstsB = MmioRead32 (Dev->Base + DWC3_DSTS);

      RefSay ("[REF] dsts probe a=0x%08x b=0x%08x hlt=%u idle=%u link=%u spd=%u soffn=%u->%u\n",
              DstsA, DstsB,
              (UINT32)((DstsA >> 22) & 1),
              (UINT32)((DstsA >> 23) & 1),
              (UINT32)((DstsA >> 18) & 0xF),
              (UINT32)(DstsA & 0x7),
              (UINT32)((DstsA >> 3) & 0x3FFF),
              (UINT32)((DstsB >> 3) & 0x3FFF));
    }
  }

  if (Run) {
    //
    // Take the event ring out of the picture before starting the core.
    //
    // Setting RUN_STOP makes the controller live, and the first thing a live device
    // controller does that touches memory is write an event into the ring - the pull-up is
    // a PHY action, but the reset the host then sends produces a USB Reset event, and that
    // is a DMA write to GEVNTADR.
    //
    // The write that sets RUN_STOP has never come back. Two explanations fit and they lead
    // to different places: the store itself is stalled by the core, or the store completes
    // and the DMA that follows wedges the memory path so that the very next console write -
    // which goes to a framebuffer in DDR - never returns. Clearing DEVTEN removes the
    // second possibility entirely: with no event types enabled the core has nothing to
    // write, so if the write still does not return, the DMA is not the cause.
    //
    // DEVTEN is restored immediately afterwards so the rest of the bring-up behaves as
    // before.
    //
    SavedDevten = MmioRead32 (Dev->Base + DWC3_DEVTEN);
    MmioWrite32 (Dev->Base + DWC3_DEVTEN, 0);
    RefSay ("[REF] devten masked for start: was 0x%08x now 0x%08x\n",
            SavedDevten, MmioRead32 (Dev->Base + DWC3_DEVTEN));
  }

  if (Run) {
    //
    // UsbfnDwcCoreInitImpl @ 0x37EC clears this bit at connect time, and that is the
    // last vendor code to touch it before RunStop. b204/b205 set it instead, on the
    // strength of SynopsysInitDevice @ 0x8BD0 - a different module that runs earlier.
    // Neither made any difference to the link, so go back to what the connect-time
    // path does and remove the variable.
    //
    {
      UINT32 PhyCfg = MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0));

      MmioWrite32 (Dev->Base + DWC3_GUSB2PHYCFG (0), PhyCfg & ~BIT8);
      RefSay ("[REF] GUSB2PHYCFG 0x%08x -> 0x%08x (ENBLSLPM cleared)\n",
              PhyCfg, MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0)));
    }
  }

  MmioWrite32 (Dev->Base + DWC3_DCTL, Register);

  //
  // Read back before printing anything, so a single line carries the verdict.
  //
  // If this line appears, the store returned and both reads after it completed. If it does
  // not appear, the failure is inside those three accesses and not in the console.
  //
  {
    UINT32 After = MmioRead32 (Dev->Base + DWC3_DCTL);
    UINT32 Dsts  = MmioRead32 (Dev->Base + DWC3_DSTS);

    RefSay ("[REF] RunStop(%d) wrote 0x%08x DCTL=0x%08x DSTS=0x%08x hlt=%u link=%u\n",
            Run, Register, After, Dsts,
            (UINT32)((Dsts >> 22) & 1),
            (UINT32)((Dsts >> 18) & 0xF));

    //
    // Deliberately left masked for the rest of this run.
    //
    // The previous build restored DEVTEN immediately after this print and the machine stopped
    // on the very next register read. With DEVTEN clear the core starts and DSTS shows
    // hlt=0; the moment event types are enabled again the core posts an event, that write
    // goes out on its master, and everything stops - the console write before it had already
    // completed, so memory was still reachable at that instant and the stall is the core's
    // own bus access, not the framebuffer.
    //
    // Keeping DEVTEN at zero holds the event ring out of the way so the rest of the bring-up
    // can be observed: the core stays running, and the next thing to be attempted is the EP0
    // STARTTRANSFER, which is a TRB *read*. That separates the two directions - if the
    // doorbell now completes, only writes are broken; if it still stalls, both are.
    //
    if (Run) {
      RefSay ("[REF] devten left masked at 0x%08x, events off for this run\n",
              MmioRead32 (Dev->Base + DWC3_DEVTEN));
    }
  }

  for (Timeout = 0; Timeout < DWC3_RUNSTOP_TIMEOUT_US; Timeout++) {
    Register = MmioRead32 (Dev->Base + DWC3_DSTS);

    if (Run) {
      // HALTED=0 is not proof of success while a reset is asserted.
      Control = MmioRead32 (Dev->Base + DWC3_DCTL);
      if ((Control & DWC3_DCTL_CSFTRST) != 0) {
        RefSay ("[REF] RunStop(TRUE) REJECT CSFTRST DCTL=0x%08x DSTS=0x%08x\n",
                Control, Register);
        return EFI_DEVICE_ERROR;
      }

      if ((Register & DWC3_DSTS_DEVCTRLHLT) == 0) {
        RefSay ("[REF] RunStop(TRUE) done DSTS=0x%08x speed=%u link=%u DCTL=0x%08x\n",
                Register,
                (UINT32)(Register & 0x7),
                (UINT32)((Register >> 18) & 0xF),
                Control);

        //
        // Watch the link for 300 ms and report only the changes.
        //
        // This is the measurement that separates the two remaining explanations. A device
        // the host has really reset walks 4 (Rx.Detect) -> 6 (Reset) -> 0 (On) within a few
        // milliseconds. A device whose PHY never brought the eUSB2 link up sits still, and
        // the host sees a pull-up it cannot talk to - which is exactly the "unknown device
        // that disappears when unplugged" the PC reports.
        //
        {
          UINT32  Last;
          UINT32  Sample;
          UINTN   Step;

          Last = Register;
          for (Step = 0; Step < 30; Step++) {
            gBS->Stall (10000);
            Sample = MmioRead32 (Dev->Base + DWC3_DSTS);
            if (Sample != Last) {
              RefSay ("[REF] link track t=%ums DSTS=0x%08x link=%u soffn=%u\n",
                      (UINT32)((Step + 1) * 10),
                      Sample,
                      (UINT32)((Sample >> 18) & 0xF),
                      (UINT32)((Sample >> 3) & 0x1FFF));
              Last = Sample;
            }
          }

          Sample = MmioRead32 (Dev->Base + DWC3_DSTS);
          RefSay ("[REF] link track end DSTS=0x%08x link=%u PHYCFG=0x%08x\n",
                  Sample, (UINT32)((Sample >> 18) & 0xF),
                  MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0)));
        }

        //
        // Observe the vendor PHY handoff without forcing SLEEPM or termination.
        // Automatic startup deliberately masks device events; an empty event ring
        // here cannot establish that the PHY or host generated no traffic.
        //
        UsbfnPlatformPhyAttachReport ();

        //
        // The device is live now. If the core has written anything into the event ring,
        // it is visible here - and if the ring address were wrong, this is the read that
        // shows the events landing somewhere else or not at all.
        //
        if (Dev->EventBuffer != NULL) {
          UINT32 *Evt = (UINT32 *)Dev->EventBuffer;

          RefSay ("[REF] post-runstop evtcnt=0x%08x evt=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                  MmioRead32 (Dev->Base + DWC3_GEVNTCOUNT (0)),
                  Evt[0], Evt[1], Evt[2], Evt[3],
                  Evt[4], Evt[5], Evt[6], Evt[7]);
          RefSay ("[REF] post-runstop DSTS=0x%08x DEVTEN=0x%08x DALEPENA=0x%08x\n",
                  MmioRead32 (Dev->Base + DWC3_DSTS),
                  MmioRead32 (Dev->Base + DWC3_DEVTEN),
                  MmioRead32 (Dev->Base + DWC3_DALEPENA));

          //
          // Is the link actually alive, independently of any DMA?
          //
          // The core is running now, with every event type masked, so nothing it does can
          // touch memory. Whatever the two reads below say is therefore about the PHY and
          // the link alone.
          //
          // SOFFN comes from the USB2 frame timer and advances once per frame while the link
          // is up. If it moves over 100 ms, the PHY is transmitting, the host has the device
          // and is sending SOFs, and the only thing broken is the controller's memory access.
          // If it does not move, the link never came up and no amount of DMA work would
          // matter.
          //
          // GFLADJ is printed alongside it because it is the register that sets the frame
          // length from the reference clock, and this driver has never written it. A zero
          // there would explain a frame timer that does not tick.
          //
          {
            UINT32 LinkA = MmioRead32 (Dev->Base + DWC3_DSTS);
            UINT32 LinkB;

            gBS->Stall (100000);
            LinkB = MmioRead32 (Dev->Base + DWC3_DSTS);

            //
            // Words, not digits. Three wrong conclusions in this project have come from
            // reading hex off a photograph - a GHWPARAMS0 field that does not exist, a DSTS
            // value off by one nibble, and a PAR1 that looked shifted but was not. The
            // firmware knows the answer; it should say it.
            //
            RefSay ("[REF] link: %a -> %a | soffn %a | core %a\n",
                    Dwc3LinkName (LinkA >> 18),
                    Dwc3LinkName (LinkB >> 18),
                    (((LinkA >> 3) & 0x3FFF) != ((LinkB >> 3) & 0x3FFF)) ? "MOVED" : "FROZEN",
                    ((LinkB >> 22) & 1) ? "halted" : "running");
            RefSay ("[REF] link detail: DSTS %08x -> %08x, GFLADJ %08x\n",
                    LinkA, LinkB, MmioRead32 (Dev->Base + DWC3_GFLADJ));

            //
            // Everything that decides whether the core can put a pull-up on the wire, in
            // words. If the link reads Disconnected, exactly one of these is the reason, and
            // reading them off the screen is cheaper than another build.
            //
            // GHWPARAMS3 first: it says which PHY interfaces this core was built with. A
            // HSPHY_IFC of "none" would mean there is no high-speed PHY to drive D+ at all,
            // which no register write could fix.
            //
            {
              UINT32 Hw3    = MmioRead32 (Dev->Base + DWC3_GHWPARAMS3);
              UINT32 PhyCfg = MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0));
              UINT32 Ctl    = MmioRead32 (Dev->Base + DWC3_DCTL);
              UINT32 Gctl   = MmioRead32 (Dev->Base + DWC3_GCTL);
              UINT32 SsPhy  = Hw3 & 0x3;
              UINT32 HsPhy  = (Hw3 >> 2) & 0x3;
              UINT32 Cap    = (Gctl >> 12) & 0x3;

              RefSay ("[REF] hw: ssphy=%a hsphy=%a neps=%u\n",
                      (SsPhy == 0) ? "none" : ((SsPhy == 1) ? "gen1" : ((SsPhy == 2) ? "gen2" : "other")),
                      (HsPhy == 0) ? "none" : ((HsPhy == 1) ? "utmi" : ((HsPhy == 2) ? "ulpi" : "utmi+ulpi")),
                      (UINT32)((Hw3 >> 12) & 0x3F));

              RefSay ("[REF] phycfg: suspend %a, phy-soft-reset %a, slpm %a, sel %a, phyif %u, trdtim %u\n",
                      ((PhyCfg >> 6) & 1) ? "ON" : "OFF",
                      ((PhyCfg >> 31) & 1) ? "HELD" : "RELEASED",
                      ((PhyCfg >> 8) & 1) ? "ON" : "OFF",
                      ((PhyCfg >> 4) & 1) ? "ULPI" : "UTMI",
                      (UINT32)((PhyCfg >> 3) & 0x7),
                      (UINT32)((PhyCfg >> 10) & 0xF));

              //
              // The SuperSpeed PIPE control, only to see whether that PHY is parked. It is
              // irrelevant to a USB2 pull-up unless GHWPARAMS3 says there is no separate HS
              // PHY and the core is sharing one.
              //
              RefSay ("[REF] pipe: suspend %a\n",
                      ((MmioRead32 (Dev->Base + DWC3_GUSB3PIPECTL (0)) >> 10) & 1) ?
                      "ON" : "OFF");

              RefSay ("[REF] dctl: runstop %a, csf-rst %a, keep-connect %u, ulstchng %u, trgtulst %u\n",
                      ((Ctl >> 31) & 1) ? "SET" : "CLEAR",
                      ((Ctl >> 30) & 1) ? "SET" : "CLEAR",
                      (UINT32)((Ctl >> 19) & 1),
                      (UINT32)((Ctl >> 5) & 0xF),
                      (UINT32)((Ctl >> 17) & 0xF));

              RefSay ("[REF] gctl: prtcap=%a clock-gating=%a\n",
                      (Cap == 1) ? "host" : ((Cap == 2) ? "device" : ((Cap == 3) ? "otg" : "none")),
                      (Gctl & 1) ? "DISABLED" : "enabled");
            }

            // Read-only snapshots. A frozen SOF counter on a disconnected link says
            // nothing about DMA, and probing must not alter PLL divider registers.
            UsbfnPhyStateReport ();
            UsbfnRepeaterReport ();
          }
        }

        return EFI_SUCCESS;
      }
    } else {
      if ((Register & DWC3_DSTS_DEVCTRLHLT) != 0) {
        return EFI_SUCCESS;
      }
    }

    gBS->Stall (1);
  }

  RefSay ("[REF] RunStop(%d) TIMEOUT DSTS=0x%08x DCTL=0x%08x\n",
          Run, Register, MmioRead32 (Dev->Base + DWC3_DCTL));

  return EFI_DEVICE_ERROR;
}

EFI_STATUS
Dwc3CoreInit (IN DWC3_DEV *Dev)
{
  EFI_STATUS Status;
  UINT32     Register;
  UINT32     HwParams3;
  UINT8      Index;

//[LOGOUT]   RefSay ("[REF] CoreInit enter base=0x%lx\n", (UINT64)Dev->Base);


  Register = MmioRead32 (Dev->Base + DWC3_GSNPSID);
//[LOGOUT]   RefSay ("[REF] GSNPSID=0x%08x\n", Register);
  if ((Register & DWC3_GSNPSID_MASK) != DWC3_GSNPSID_USB3 &&
      (Register & DWC3_GSNPSID_MASK) != DWC3_GSNPSID_USB31)
  {
    DEBUG ((EFI_D_ERROR, "DWC3: Bad Synopsys ID 0x%08x!\n", Register));
    ASSERT (FALSE);
  }

  Dev->IsUsb31 = (BOOLEAN)((Register & DWC3_GSNPSID_MASK) == DWC3_GSNPSID_USB31);

  RefSay ("[REF] HW: GHWPARAMS0=0x%08x GHWPARAMS1=0x%08x GHWPARAMS3=0x%08x GHWPARAMS4=0x%08x\n",
          MmioRead32 (Dev->Base + DWC3_GHWPARAMS0),
          MmioRead32 (Dev->Base + DWC3_GHWPARAMS1),
          MmioRead32 (Dev->Base + DWC3_GHWPARAMS3),
          MmioRead32 (Dev->Base + DWC3_GHWPARAMS4));

  Register  = MmioRead32 (Dev->Base + DWC3_GCTL);
  Register &= ~DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_OTG);
  Register |= DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_DEVICE);
  RefSay ("[REF] step1: writing GCTL prtcap\n");
  MmioWrite32 (Dev->Base + DWC3_GCTL, Register);
  RefSay ("[REF] step1 done: GCTL=0x%08x\n", MmioRead32 (Dev->Base + DWC3_GCTL));

  gBS->Stall (100);

  Status = Dwc3SoftReset (Dev);
  RefSay ("[REF] SoftReset -> %r\n", Status);
  if (EFI_ERROR (Status)) {
    RefSay ("[REF] CoreInit aborted: reset did not complete\n");
    Dwc3DumpState (Dev);
    return Status;
  }

  Dwc3PhyInterfaceSetup (Dev);

  Register  = MmioRead32 (Dev->Base + DWC3_GCTL);
  Register &= ~DWC3_GCTL_SCALEDOWN_MASK;
  Register &= ~DWC3_GCTL_DISSCRAMBLE;
  Register &= ~DWC3_GCTL_GBLHIBERNATIONEN;
  //
  // Disable clock gating rather than enable it.
  //
  // This used to clear the bit, which turns gating on, and GCTL then reads 0x00102000.
  // With gating enabled the endpoint block is clocked down between accesses, and a store
  // to it that cannot complete the wake handshake simply never returns - which is what the
  // STARTTRANSFER doorbell does while DEPCFG on the same register goes through. Synopsys
  // recommends holding gating off while bringing a core up.
  //
  Register |= DWC3_GCTL_DSBLCLKGTNG;
  MmioWrite32 (Dev->Base + DWC3_GCTL, Register);

  Dwc3FrameLengthAdjust (Dev);

  if (Dev->Platform.NumEndpoints != 0) {
    Dev->NumPhysicalEps = Dev->Platform.NumEndpoints;
  } else {
    HwParams3           = MmioRead32 (Dev->Base + DWC3_GHWPARAMS3);
    Dev->NumPhysicalEps = (UINT8)DWC3_GHWPARAMS3_NUM_EPS (HwParams3);
  }

  if (Dev->NumPhysicalEps == 0 || Dev->NumPhysicalEps > DWC3_MAX_PHYSICAL_EP) {
    Dev->NumPhysicalEps = DWC3_MAX_PHYSICAL_EP;
  }

  Status = Dwc3EventBufferSetup (Dev);
  if (EFI_ERROR (Status)) {
    RefSay ("[REF] CoreInit aborted: EventBufferSetup -> %r\n", Status);
    return Status;
  }

  //
  // TX FIFO sizing. The reference driver never programs GTXFIFOSIZ and gives every endpoint
  // FIFO number 0, 1, 2, ... without ever sizing those FIFOs. If they come back zero, EP0
  // has no room to transmit and the host will see the device but never get a descriptor -
  // which is the symptom this port has had from the beginning. The Qualcomm driver sizes
  // them from GHWPARAMS3.DFIFO (bits [22:18]) right after its reset, so read them here
  // before deciding anything.
  //
  RefSay ("[REF] FIFO: GTX0=0x%08x GTX1=0x%08x GTX2=0x%08x GRX0=0x%08x\n",
          MmioRead32 (Dev->Base + DWC3_GTXFIFOSIZ (0)),
          MmioRead32 (Dev->Base + DWC3_GTXFIFOSIZ (1)),
          MmioRead32 (Dev->Base + DWC3_GTXFIFOSIZ (2)),
          MmioRead32 (Dev->Base + DWC3_GRXFIFOSIZ (0)));

  Register  = MmioRead32 (Dev->Base + DWC3_GCTL);
  Register &= ~DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_OTG);
  Register |= DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_DEVICE);
  MmioWrite32 (Dev->Base + DWC3_GCTL, Register);

  gBS->Stall (100);

  Register  = MmioRead32 (Dev->Base + DWC3_DCFG);
  Register &= ~DWC3_DCFG_SPEED_MASK;

  switch (Dev->Platform.MaximumSpeed) {
    case UsbBusSpeedSuper:
      Register |= DWC3_DCFG_SUPERSPEED;
      break;
    case UsbBusSpeedFull:
      Register |= DWC3_DCFG_FULLSPEED;
      break;
    default:
      Register |= DWC3_DCFG_HIGHSPEED;
      break;
  }

  // Start out unaddressed
  Register &= ~DWC3_DCFG_DEVADDR_MASK;

  MmioWrite32 (Dev->Base + DWC3_DCFG, Register);

  RefSay ("[REF] step2 done: DCFG=0x%08x\n", MmioRead32 (Dev->Base + DWC3_DCFG));

  //
  // The vendor writes 0x1277 here, not the five bits this port has been using. The
  // difference is bits 5, 6, 9 and 12 - error, L1 and vendor events. None of them is a
  // link gate, but matching the vendor costs nothing and removes a variable.
  //
  MmioWrite32 (Dev->Base + DWC3_DEVTEN, 0x1277);

  RefSay ("[REF] step3 done: DEVTEN=0x%08x\n", MmioRead32 (Dev->Base + DWC3_DEVTEN));

//[LOGOUT]   RefSay ("[REF] DCFG=0x%08x DEVTEN=0x%08x\n",
//[LOGOUT]           MmioRead32 (Dev->Base + DWC3_DCFG),
//[LOGOUT]           MmioRead32 (Dev->Base + DWC3_DEVTEN));


  MmioWrite32 (Dev->Base + DWC3_DALEPENA, 0);

  RefSay ("[REF] step4 done: DALEPENA=0x%08x\n", MmioRead32 (Dev->Base + DWC3_DALEPENA));

  for (Index = 0; Index < DWC3_MAX_PHYSICAL_EP; Index++) {
    Dev->Endpoints[Index].Enabled         = FALSE;
    Dev->Endpoints[Index].Stalled         = FALSE;
    Dev->Endpoints[Index].TransferStarted = FALSE;
    Dev->Endpoints[Index].PhysEpNum       = Index;
  }

  Dev->ConfigStarted = FALSE;
  Dev->EnumerationPending = FALSE;
  Dev->DmaTestAllowed = FALSE;
  Dev->DmaTestAttempted = FALSE;
  Dev->DmaBlockLogged = FALSE;
  Dev->Speed     = UsbBusSpeedUnknown;
  Dev->Ep0State  = Dwc3Ep0StateSetup;
  Dev->LinkState = DWC3_LINK_STATE_HS_ON;
  Dev->Suspended = FALSE;

  RefSay ("[REF] step5 done: CoreInit returning success\n");

  return EFI_SUCCESS;
}

VOID
Dwc3CoreExit (IN DWC3_DEV *Dev)
{
  UINT32 Register;

  Dev->EnumerationPending = FALSE;
  Dev->DmaTestAllowed = FALSE;

  // Mask every Device Event
  MmioWrite32 (Dev->Base + DWC3_DEVTEN, 0);
  MmioWrite32 (Dev->Base + DWC3_DALEPENA, 0);

  Dwc3EventBufferCleanup (Dev);

  // Park PHYs
  Register  = MmioRead32 (Dev->Base + DWC3_GUSB2PHYCFG (0));
  Register |= DWC3_GUSB2PHYCFG_SUSPHY;
  MmioWrite32 (Dev->Base + DWC3_GUSB2PHYCFG (0), Register);

  Register  = MmioRead32 (Dev->Base + DWC3_GUSB3PIPECTL (0));
  Register |= DWC3_GUSB3PIPECTL_SUSPHY;
  MmioWrite32 (Dev->Base + DWC3_GUSB3PIPECTL (0), Register);
}
