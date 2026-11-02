#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <IndustryStandard/Usb.h>

#include "UsbfnDwc3Dxe.h"

STATIC
VOID
Dwc3PushMessage (
  IN DWC3_DEV                  *Dev,
  IN EFI_USBFN_MESSAGE          Message,
  IN EFI_USBFN_MESSAGE_PAYLOAD *Payload OPTIONAL)
{
  UINT32 Next = (Dev->MessageTail + 1) % DWC3_MESSAGE_QUEUE_DEPTH;

  if (Next == Dev->MessageHead) {
    DEBUG ((EFI_D_ERROR, "DWC3: Message Queue Overflow, dropping Message %u!\n", Message));
    return;
  }

  Dev->Messages[Dev->MessageTail].Message = Message;

  if (Payload != NULL) {
    CopyMem (&Dev->Messages[Dev->MessageTail].Payload, Payload, sizeof (EFI_USBFN_MESSAGE_PAYLOAD));
  } else {
    ZeroMem (&Dev->Messages[Dev->MessageTail].Payload, sizeof (EFI_USBFN_MESSAGE_PAYLOAD));
  }

  Dev->MessageTail = Next;
}

BOOLEAN
Dwc3PopMessage (
  IN  DWC3_DEV     *Dev,
  OUT DWC3_MESSAGE *Message)
{
  if (Dev->MessageHead == Dev->MessageTail) {
    return FALSE;
  }

  CopyMem (Message, &Dev->Messages[Dev->MessageHead], sizeof (DWC3_MESSAGE));

  Dev->MessageHead = (Dev->MessageHead + 1) % DWC3_MESSAGE_QUEUE_DEPTH;

  return TRUE;
}

STATIC
VOID
Dwc3PushTransferResult (
  IN DWC3_DEV                *Dev,
  IN UINT8                    PhysEpNum,
  IN UINTN                    BytesTransferred,
  IN EFI_USBFN_TRANSFER_STATUS Status)
{
  EFI_USBFN_MESSAGE_PAYLOAD Payload;
  DWC3_ENDPOINT            *Endpoint = &Dev->Endpoints[PhysEpNum];
  BOOLEAN                   IsIn     = (PhysEpNum & 1) != 0;

  ZeroMem (&Payload, sizeof (Payload));

  Payload.utr.BytesTransferred = BytesTransferred;
  Payload.utr.TransferStatus   = Status;
  Payload.utr.EndpointIndex    = (UINT8)(PhysEpNum >> 1);
  Payload.utr.Direction        = IsIn ? EfiUsbEndpointDirectionDeviceTx : EfiUsbEndpointDirectionDeviceRx;
  Payload.utr.Buffer           = Endpoint->TransferBuffer;

  Dwc3PushMessage (Dev, IsIn ? EfiUsbMsgEndpointStatusChangedTx : EfiUsbMsgEndpointStatusChangedRx, &Payload);
}

EFI_STATUS
Dwc3SendEpCommand (
  IN  DWC3_DEV *Dev,
  IN  UINT8     PhysEpNum,
  IN  UINT32    Command,
  IN  UINT32    Param0,
  IN  UINT32    Param1,
  IN  UINT32    Param2,
  OUT UINT32   *CommandRegister OPTIONAL)
{
  UINTN  DepBase = DWC3_DEP_BASE (PhysEpNum);
  UINT32 Register;
  UINT32 Timeout;
  UINT32 CommandStatus;

  // All clients, including UsbDeviceDxe and late protocol callbacks, pass this
  // boundary. Do not allow a boot-time client to trigger the known DMA stall.
  if (DWC3_DEPCMD_CMD (Command) == DWC3_DEPCMD_STARTTRANSFER && !Dev->DmaTestAllowed) {
    if (!Dev->DmaBlockLogged) {
      RefSay ("[USBSAFE b200] blocked STARTTRANSFER ep%u caller=0x%lx; use USB TEST\n",
              (UINT32)PhysEpNum, (UINT64)(UINTN)__builtin_return_address (0));
      Dev->DmaBlockLogged = TRUE;
    }
    return EFI_NOT_READY;
  }

  MmioWrite32 (Dev->Base + DepBase + DWC3_DEPCMDPAR0, Param0);
  MmioWrite32 (Dev->Base + DepBase + DWC3_DEPCMDPAR1, Param1);
  MmioWrite32 (Dev->Base + DepBase + DWC3_DEPCMDPAR2, Param2);

  // Order UC DMA TRBs and command parameters before the AArch64 doorbell.
  MemoryFence ();
  //
  // Three markers, because "the write hung" and "the read hung" are different faults and
  // the last few captures could not tell them apart.
  //
  //   A - before the store
  //   B - after the store returns (posted, so the core may not have taken it yet)
  //   C - after reading the register back
  //
  // A only            : the store itself never returned.
  // A then B          : the store returned, the read did not.
  // A, B then C       : the register is reachable; CMDACT not clearing is then about the
  //                     command, not the bus.
  //
  {
    UINT32 Par0 = MmioRead32 (Dev->Base + DepBase + DWC3_DEPCMDPAR0);
    UINT32 Par1 = MmioRead32 (Dev->Base + DepBase + DWC3_DEPCMDPAR1);
    UINT32 Par2 = MmioRead32 (Dev->Base + DepBase + DWC3_DEPCMDPAR2);

    RefSay ("[REF] par readback: %08x %08x %08x  depcmd=%08x\n",
            Par0, Par1, Par2, MmioRead32 (Dev->Base + DepBase + DWC3_DEPCMD));

    //
    // A verdict, not a hex string.
    //
    // STARTTRANSFER carries the TRB address in PAR1. Reading sixteen digits off a photograph
    // has already produced two wrong conclusions in this project - a GHWPARAMS0 field that
    // does not exist, and a DSTS value misread by one nibble - so the firmware compares what
    // it wrote against what the register gives back and names the outcome:
    //
    //   MATCH              the register holds exactly the address that was written
    //   SHIFTED-right-4    it holds the address divided by sixteen, which is the [35:4]
    //                      encoding some cores use; harmless, the hardware normalises
    //   DIFFERENT          neither, and the core would fetch from somewhere else entirely
    //
    if (Command == DWC3_DEPCMD_STARTTRANSFER) {
      RefSay ("[REF] par1 check: wrote %08x read %08x -> %a\n",
              Param1, Par1,
              (Param1 == Par1) ? "EXACT" :
              ((Par1 == (Param1 >> 4)) ? "SHIFTED-4" : "BROKEN"));
    }
  }

  //
  // Four stages, because MmioWrite32 is not a bare store.
  //
  // BaseIoLibIntrinsic on AArch64 is "dmb st ; str ; dmb" - there is a barrier on the way
  // in and another on the way out, both inside the call. So "the line after the write never
  // printed" cannot distinguish a store that did not retire from a barrier that did not
  // complete. This does the store with no wrapper at all, then puts the barrier on its own
  // line, then reads the register.
  //
  //   A  before anything
  //   B  after the raw store          - the store itself returned
  //   C  after an explicit MemoryFence - the barrier returned
  //   D  after reading DEPCMD back
  //
  RefSay ("[REF b200] doorbell A ep%u cmd=0x%08x par1=0x%08x caller=0x%lx\n",
          (UINT32)PhysEpNum, Command, Param1, (UINT64)(UINTN)__builtin_return_address (0));

  *(volatile UINT32 *)(UINTN)(Dev->Base + DepBase + DWC3_DEPCMD) =
    Command | DWC3_DEPCMD_CMDACT;

  RefSay ("[REF] doorbell B ep%u raw store returned\n", (UINT32)PhysEpNum);

  MemoryFence ();

  RefSay ("[REF] doorbell C ep%u barrier returned\n", (UINT32)PhysEpNum);

  Register = MmioRead32 (Dev->Base + DepBase + DWC3_DEPCMD);
  RefSay ("[REF] doorbell D ep%u now=0x%08x\n", (UINT32)PhysEpNum, Register);

  for (Timeout = 0; Timeout < DWC3_CMD_TIMEOUT_US; Timeout++) {
    Register = MmioRead32 (Dev->Base + DepBase + DWC3_DEPCMD);

    if ((Register & DWC3_DEPCMD_CMDACT) == 0) {
      CommandStatus = DWC3_DEPCMD_STATUS (Register);

      if (CommandRegister != NULL) {
        *CommandRegister = Register;
      }

      if (CommandStatus != 0) {
        DEBUG ((EFI_D_ERROR, "DWC3: Endpoint Command 0x%x on Physical EP %u failed with Status %u!\n", DWC3_DEPCMD_CMD (Command), PhysEpNum, CommandStatus));
        return EFI_DEVICE_ERROR;
      }

      return EFI_SUCCESS;
    }

    gBS->Stall (1);
  }

  DEBUG ((EFI_D_ERROR, "DWC3: Endpoint Command 0x%x on Physical EP %u timed out!\n", DWC3_DEPCMD_CMD (Command), PhysEpNum));

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
Dwc3StartConfig (IN DWC3_DEV *Dev)
{
  EFI_STATUS Status;
  UINT8      Index;
  UINT32     Register;

  Status = Dwc3SendEpCommand (Dev, DWC3_EP0_OUT, DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Dev->NumPhysicalEps; Index++) {
    //
    // Keep the resource index the core hands back. It arrives in bits 19:16 of DEPCMD -
    // the capture showed 0x00010002, 0x00020002, 0x00030002 - and the old code passed
    // NULL here, threw them away, and left every endpoint claiming resource 0 while the
    // core had assigned 1, 2, 3.
    //
    Status = Dwc3SendEpCommand (Dev, Index, DWC3_DEPCMD_SETTRANSFRESOURCE,
                                DWC3_DEPXFERCFG_NUM_XFER_RES (1), 0, 0, &Register);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Dev->Endpoints[Index].ResourceIndex = (UINT8)DWC3_DEPCMD_GET_RSC_IDX (Register);
    if (Index < 4) {
      RefSay ("[REF] resource ep%u: DEPCMD=0x%08x rsc=%u\n",
              (UINT32)Index, Register, (UINT32)Dev->Endpoints[Index].ResourceIndex);
    }
  }

  return EFI_SUCCESS;
}

STATIC
VOID
Dwc3InitTrbRing (IN DWC3_ENDPOINT *Endpoint)
{
  DWC3_TRB *Link;

  ZeroMem (Endpoint->TrbPool, sizeof (DWC3_TRB) * DWC3_TRB_PER_EP);

  Link = &Endpoint->TrbPool[DWC3_TRB_PER_EP - 1];

  Link->BufferPtrLow  = (UINT32)(Endpoint->TrbPoolPhys & 0xFFFFFFFF);
  Link->BufferPtrHigh = (UINT32)(Endpoint->TrbPoolPhys >> 32);
  Link->Size          = 0;
  Link->Control       = DWC3_TRBCTL_LINK_TRB | DWC3_TRB_CTRL_HWO;

  //
  // The core walks the ring over AXI. Push the link TRB out with the rest of the ring;
  // a link TRB left in cache makes the core stop at the end of the first pass.
  //
  WriteBackDataCacheRange (Endpoint->TrbPool, sizeof (DWC3_TRB) * DWC3_TRB_PER_EP);
}

EFI_STATUS
Dwc3EnableEndpoint (
  IN DWC3_DEV *Dev,
  IN UINT8     PhysEpNum,
  IN UINT8     EpType,
  IN UINT16    MaxPacketSize,
  IN BOOLEAN   Modify)
{
  DWC3_ENDPOINT *Endpoint = &Dev->Endpoints[PhysEpNum];
  EFI_STATUS     Status;
  UINT32         Param0;
  UINT32         Param1;
  UINT32         Register;

  if (PhysEpNum >= Dev->NumPhysicalEps) {
    return EFI_INVALID_PARAMETER;
  }

  if (PhysEpNum == DWC3_EP0_OUT && !Modify && !Dev->ConfigStarted) {
    RefSay ("[REF] StartConfig (DEPSTARTCFG + resources)\n");
    Status = Dwc3StartConfig (Dev);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Dev->ConfigStarted = TRUE;
  }

  Param0 = DWC3_DEPCFG_EP_TYPE (EpType) | DWC3_DEPCFG_MAX_PACKET_SIZE (MaxPacketSize);
  Param0 |= Modify ? DWC3_DEPCFG_ACTION_MODIFY : DWC3_DEPCFG_ACTION_INIT;

  if ((PhysEpNum & 1) != 0) {
    Param0 |= DWC3_DEPCFG_FIFO_NUMBER (PhysEpNum >> 1);
  }

  Param1 = DWC3_DEPCFG_EP_NUMBER (PhysEpNum);

  if (EpType == DWC3_DEPCMD_TYPE_CONTROL) {
    Param1 |= DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_NOT_READY_EN;
  } else {
    Param1 |= DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_IN_PROGRESS_EN;
  }

  Status = Dwc3SendEpCommand (Dev, PhysEpNum, DWC3_DEPCMD_SETEPCONFIG, Param0, Param1, 0, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Endpoint->PhysEpNum     = PhysEpNum;
  Endpoint->EpType        = EpType;
  Endpoint->MaxPacketSize = MaxPacketSize;

  if (Modify) {
    return EFI_SUCCESS;
  }

  Register  = MmioRead32 (Dev->Base + DWC3_DALEPENA);
  Register |= DWC3_DALEPENA_EP (PhysEpNum);
  MmioWrite32 (Dev->Base + DWC3_DALEPENA, Register);
  RefSay ("[REF] DALEPENA ep%u: wrote 0x%08x, reads 0x%08x\n",
          (UINT32)PhysEpNum, Register,
          MmioRead32 (Dev->Base + DWC3_DALEPENA));

  Endpoint->Enabled         = TRUE;
  Endpoint->Stalled         = FALSE;
  Endpoint->TransferStarted = FALSE;
  //
  // ResourceIndex is deliberately NOT cleared here. Dwc3StartConfig has just recorded
  // what the core assigned, and zeroing it made every later ENDTRANSFER believe the
  // endpoint owned no resource at all.
  //

  if (EpType != DWC3_DEPCMD_TYPE_CONTROL) {
    Dwc3InitTrbRing (Endpoint);
  }

  return EFI_SUCCESS;
}

VOID
Dwc3DisableEndpoint (
  IN DWC3_DEV *Dev,
  IN UINT8     PhysEpNum)
{
  DWC3_ENDPOINT *Endpoint = &Dev->Endpoints[PhysEpNum];
  UINT32         Register;

  if (!Endpoint->Enabled) {
    return;
  }

  Dwc3EndTransfer (Dev, PhysEpNum);

  Register  = MmioRead32 (Dev->Base + DWC3_DALEPENA);
  Register &= ~DWC3_DALEPENA_EP (PhysEpNum);
  MmioWrite32 (Dev->Base + DWC3_DALEPENA, Register);

  Endpoint->Enabled         = FALSE;
  Endpoint->TransferStarted = FALSE;
  Endpoint->TransferBuffer  = NULL;
}

EFI_STATUS
Dwc3StartTransfer (
  IN DWC3_DEV *Dev,
  IN UINT8     PhysEpNum,
  IN UINT64    BufferPhys,
  IN UINT32    Length,
  IN UINT32    TrbType)
{
  DWC3_ENDPOINT *Endpoint = &Dev->Endpoints[PhysEpNum];
  DWC3_TRB      *Trb      = &Endpoint->TrbPool[0];
  DWC3_TRB      *Zlp      = &Endpoint->TrbPool[1];
  EFI_STATUS     Status   = EFI_DEVICE_ERROR;
  UINT32         CommandRegister;
  BOOLEAN        NeedZlp;
  UINT32         Attempt;

  if (PhysEpNum <= DWC3_EP0_IN && Dev->Ep0DmaUnsafe) {
    return EFI_DEVICE_ERROR;
  }

  if (Endpoint->TransferStarted ||
      (PhysEpNum <= DWC3_EP0_IN && Dev->Endpoints[PhysEpNum ^ 1].TransferStarted)) {
    return EFI_NOT_READY;
  }

  NeedZlp = (BOOLEAN)(Endpoint->ZeroLengthTermination &&
                      (PhysEpNum & 1) != 0 &&
                      TrbType == DWC3_TRBCTL_NORMAL &&
                      Length != 0 &&
                      Endpoint->MaxPacketSize != 0 &&
                      (Length % Endpoint->MaxPacketSize) == 0);

  Trb->BufferPtrLow  = (UINT32)(BufferPhys & 0xFFFFFFFF);
  Trb->BufferPtrHigh = (UINT32)(BufferPhys >> 32);
  Trb->Size          = DWC3_TRB_SIZE_LENGTH (Length);
  Trb->Control       = TrbType | DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_ISP_IMI;

  if (NeedZlp) {
    Trb->Control |= DWC3_TRB_CTRL_CHN;

    Zlp->BufferPtrLow  = (UINT32)(BufferPhys & 0xFFFFFFFF);
    Zlp->BufferPtrHigh = (UINT32)(BufferPhys >> 32);
    Zlp->Size          = 0;
    Zlp->Control       = TrbType | DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_ISP_IMI |
                         DWC3_TRB_CTRL_IOC | DWC3_TRB_CTRL_LST;
  } else {
    Trb->Control |= DWC3_TRB_CTRL_IOC | DWC3_TRB_CTRL_LST;
  }

  for (Attempt = 0; Attempt < DWC3_START_XFER_ATTEMPTS; Attempt++) {
    if (Attempt != 0) {
      gBS->Stall (200);

      Dwc3EndTransfer (Dev, PhysEpNum);

      Trb->BufferPtrLow  = (UINT32)(BufferPhys & 0xFFFFFFFF);
      Trb->BufferPtrHigh = (UINT32)(BufferPhys >> 32);
      Trb->Size          = DWC3_TRB_SIZE_LENGTH (Length);
      Trb->Control       = TrbType | DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_ISP_IMI;

      if (NeedZlp) {
        Trb->Control |= DWC3_TRB_CTRL_CHN;

        Zlp->BufferPtrLow  = (UINT32)(BufferPhys & 0xFFFFFFFF);
        Zlp->BufferPtrHigh = (UINT32)(BufferPhys >> 32);
        Zlp->Size          = 0;
        Zlp->Control       = TrbType | DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_ISP_IMI |
                             DWC3_TRB_CTRL_IOC | DWC3_TRB_CTRL_LST;
      } else {
        Trb->Control |= DWC3_TRB_CTRL_IOC | DWC3_TRB_CTRL_LST;
      }
    }

    //
    // The core fetches the TRB over AXI and never looks at our cache. MemoryFence() is a
    // dmb: it orders the stores, it does not write them back. If the pool is cacheable at
    // all, the core reads a zeroed TRB, sees HWO clear, and simply does not start the
    // transfer - which is a STARTTRANSFER the core accepts and never completes, exactly
    // what has been happening. Push the line out first.
    //
    WriteBackDataCacheRange (Trb, sizeof (DWC3_TRB));
    if (NeedZlp) {
      WriteBackDataCacheRange (Zlp, sizeof (DWC3_TRB));
    }

    MemoryFence ();

    {
      UINT8  *Raw;
      UINT32  ByteIndex;

      Raw = (UINT8 *)Trb;
      RefSay ("[REF] trb raw:");
      for (ByteIndex = 0; ByteIndex < sizeof (DWC3_TRB); ByteIndex++) {
        RefSay (" %02x", Raw[ByteIndex]);
      }
      RefSay ("\n");
    }

    RefSay ("[REF] trb[0] ep%u buf=0x%08x%08x size=0x%08x ctrl=0x%08x dalepena=0x%08x rsc=%u\n",
            (UINT32)PhysEpNum, Trb->BufferPtrHigh, Trb->BufferPtrLow,
            Trb->Size, Trb->Control,
            MmioRead32 (Dev->Base + DWC3_DALEPENA),
            (UINT32)Endpoint->ResourceIndex);

    //
    // The endpoint has to be active for STARTTRANSFER to mean anything. The TRB dump
    // showed DALEPENA reading zero at this point, so whatever cleared it, put the bit
    // back and say that it had to be done - a doorbell to an endpoint that is not in
    // DALEPENA is not a request the core can honour.
    //
    {
      UINT32  Dali;

      Dali = MmioRead32 (Dev->Base + DWC3_DALEPENA);
      if ((Dali & DWC3_DALEPENA_EP (PhysEpNum)) == 0) {
        MmioWrite32 (Dev->Base + DWC3_DALEPENA, Dali | DWC3_DALEPENA_EP (PhysEpNum));
        RefSay ("[REF] DALEPENA was 0x%08x, re-armed ep%u -> 0x%08x\n",
                Dali, (UINT32)PhysEpNum, MmioRead32 (Dev->Base + DWC3_DALEPENA));
      }
    }

    //
    // Read DEPCMD and the core state before the doorbell. If the line does not print,
    // the register access itself is the problem; if it prints and the command still
    // never completes, the command is being accepted and not executed.
    //
    //
    // The clock chain, not just the branch.
    //
    // gcc_aggre_usb3_prim_axi_clk sits at CBCR 0x3908C and its parent is
    // gcc_usb30_prim_master_clk_src, whose root control is 0x3902C / 0x39030. A branch that
    // reads halt=0 only says its own gate is open; it says nothing about the RCG above it.
    // GCC base is 0x00100000.
    //
    RefSay ("[REF] gcc: master_rcg cmd=0x%08x cfg=0x%08x m=0x%08x\n",
            MmioRead32 (0x0013902C), MmioRead32 (0x00139030), MmioRead32 (0x00139034));
    RefSay ("[REF] gcc: cbcr 18=%08x 24=%08x 28=%08x 8c=%08x\n",
            MmioRead32 (0x00139018), MmioRead32 (0x00139024),
            MmioRead32 (0x00139028), MmioRead32 (0x0013908C));
    RefSay ("[REF] gcc: cbcr 60=%08x 64=%08x 68=%08x 88=%08x\n",
            MmioRead32 (0x00139060), MmioRead32 (0x00139064),
            MmioRead32 (0x00139068), MmioRead32 (0x00139088));

    RefSay ("[REF] pre-doorbell DCFG=0x%08x DCTL=0x%08x DSTS=0x%08x dalepena=0x%08x\n",
            MmioRead32 (Dev->Base + DWC3_DCFG),
            MmioRead32 (Dev->Base + DWC3_DCTL),
            MmioRead32 (Dev->Base + DWC3_DSTS),
            MmioRead32 (Dev->Base + DWC3_DALEPENA));

    Status = Dwc3SendEpCommand (Dev, PhysEpNum, DWC3_DEPCMD_STARTTRANSFER,
                                (UINT32)(Endpoint->TrbPoolPhys >> 32),
                                (UINT32)(Endpoint->TrbPoolPhys & 0xFFFFFFFF),
                                0, &CommandRegister);
    if (!EFI_ERROR (Status)) {
      break;
    }

    if (Status == EFI_TIMEOUT) {
      // The command may still execute; do not rewrite its TRB or retry it.
      Endpoint->TransferStarted = TRUE;
      if (PhysEpNum <= DWC3_EP0_IN) {
        Dev->Ep0DmaUnsafe = TRUE;
      }
      return Status;
    }
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "DWC3: STARTTRANSFER failed on Physical EP %u, TrbType 0x%x Len %u RscIdx %u Ep0State %u\n",
            PhysEpNum, TrbType, Length, Endpoint->ResourceIndex, Dev->Ep0State));
    Trb->Control = 0;
    return Status;
  }

  Endpoint->ResourceIndex   = (UINT8)DWC3_DEPCMD_GET_RSC_IDX (CommandRegister);
  Endpoint->TransferStarted = TRUE;

  return EFI_SUCCESS;
}

VOID
Dwc3EndTransfer (
  IN DWC3_DEV *Dev,
  IN UINT8     PhysEpNum)
{
  DWC3_ENDPOINT *Endpoint = &Dev->Endpoints[PhysEpNum];
  UINT32         Command;
  EFI_STATUS     Status;

  if (PhysEpNum <= DWC3_EP0_IN && Dev->Ep0DmaUnsafe) return;

  //
  // Only end a transfer that is actually in flight.
  //
  // This used to test ResourceIndex == 0, which happened to skip the call every time
  // because the index was never captured. Now that it is, that test would send
  // ENDTRANSFER to endpoints that were never started - and the capture shows what the
  // core does with that: "depcmd ep1 cmd=0x00020908" is the last line before the machine
  // goes down. TransferStarted is the honest question to ask.
  //
  if (!Endpoint->TransferStarted) {
    return;
  }

  Command = DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_HIPRI_FORCERM;
  if (!Dev->IsUsb31) Command |= DWC3_DEPCMD_CMDIOC;
  Command |= DWC3_DEPCMD_PARAM (Endpoint->ResourceIndex);

  Status = Dwc3SendEpCommand (Dev, PhysEpNum, Command, 0, 0, 0, NULL);
  if (EFI_ERROR (Status)) {
    if (PhysEpNum <= DWC3_EP0_IN) {
      Dev->Ep0DmaUnsafe = TRUE;
    }
    return;
  }

  // DWC_usb31 cannot use GUCTL2[14] to make ENDTRANSFER's CMDACT a
  // completion guarantee. The kernel requires 1ms for a non-IOC stop before
  // reusing/unmapping a TRB (gadget.c dwc3_stop_active_transfer).
  gBS->Stall (Dev->IsUsb31 ? 1000 : 100);

  Endpoint->ResourceIndex   = 0;
  Endpoint->TransferStarted = FALSE;
  // Do not touch the shared EP0 TRB here: its other direction may own it.
}

EFI_STATUS
Dwc3SetHalt (
  IN DWC3_DEV *Dev,
  IN UINT8     PhysEpNum,
  IN BOOLEAN   Stall)
{
  DWC3_ENDPOINT *Endpoint = &Dev->Endpoints[PhysEpNum];
  EFI_STATUS     Status;

  if (Stall) {
    Status = Dwc3SendEpCommand (Dev, PhysEpNum, DWC3_DEPCMD_SETSTALL, 0, 0, 0, NULL);
  } else {
    Status = Dwc3SendEpCommand (Dev, PhysEpNum, DWC3_DEPCMD_CLEARSTALL, 0, 0, 0, NULL);
  }

  if (!EFI_ERROR (Status)) {
    Endpoint->Stalled = Stall;
  }

  return Status;
}

EFI_STATUS
Dwc3Ep0ResetState (IN DWC3_DEV *Dev)
{
  Dev->Ep0State           = Dwc3Ep0StateSetup;
  Dev->ThreeStageSetup    = FALSE;
  Dev->Ep0ExpectIn        = FALSE;
  Dev->Ep0StatusStarted   = FALSE;
  Dev->Ep0StatusRequested = FALSE;
  Dev->Ep0StatusReady     = FALSE;
  Dev->Ep0StatusInternal  = FALSE;
  Dev->Ep0StatusBuffer    = NULL;
  Dev->Ep0Bounced         = FALSE;
  Dev->Ep0BounceTarget    = NULL;
  Dev->Ep0BounceLength    = 0;

  Dwc3EndTransfer (Dev, DWC3_EP0_IN);
  Dwc3EndTransfer (Dev, DWC3_EP0_OUT);
  Dev->Endpoints[DWC3_EP0_IN].TransferBuffer     = NULL;
  Dev->Endpoints[DWC3_EP0_OUT].TransferBuffer    = NULL;
  Dev->Endpoints[DWC3_EP0_IN].TransferTrbLength  = 0;
  Dev->Endpoints[DWC3_EP0_OUT].TransferTrbLength = 0;

  if (Dev->Ep0DmaUnsafe || Dev->Endpoints[DWC3_EP0_IN].TransferStarted ||
      Dev->Endpoints[DWC3_EP0_OUT].TransferStarted) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
Dwc3Ep0OutStart (IN DWC3_DEV *Dev)
{
  EFI_STATUS Status;

  RefSay ("[REF] ep0out: reset state\n");
  Status = Dwc3Ep0ResetState (Dev);
  if (EFI_ERROR (Status)) {
    RefSay ("[REF] ep0out: reset state -> %r\n", Status);
    return Status;
  }

  RefSay ("[REF] ep0out: dram trb=%a buf=%a uc trb=%a buf=%a\n",
          (Dev->Endpoints[DWC3_EP0_OUT].TrbPoolPhys >= 0x80000000ULL) ? "ok" : "BAD",
          (Dev->Ep0BufferPhys >= 0x80000000ULL) ? "ok" : "BAD",
          Dwc3DmaBufferIsUc (Dev->Endpoints[DWC3_EP0_OUT].TrbPool) ? "yes" : "NO-WB",
          Dwc3DmaBufferIsUc (Dev->Ep0Buffer) ? "yes" : "NO-WB");

  RefSay ("[REF] ep0out: start transfer\n");
  Status = Dwc3StartTransfer (Dev, DWC3_EP0_OUT, Dev->Ep0BufferPhys, 8, DWC3_TRBCTL_CONTROL_SETUP);
  if (EFI_ERROR (Status)) {
    RefSay ("[REF] EP0 SETUP arm failed: %r\n", Status);
  }
  return Status;
}

EFI_STATUS
Dwc3Ep0StallAndRestart (IN DWC3_DEV *Dev)
{
  EFI_STATUS Status;

  Status = Dwc3Ep0ResetState (Dev);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Dwc3SetHalt (Dev, DWC3_EP0_OUT, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Dev->Endpoints[DWC3_EP0_OUT].Stalled = FALSE;
  Dev->Endpoints[DWC3_EP0_IN].Stalled  = FALSE;

  return Dwc3Ep0OutStart (Dev);
}

STATIC
VOID
Dwc3ResetNonControlEndpoints (IN DWC3_DEV *Dev)
{
  DWC3_ENDPOINT *Endpoint;
  UINT8          Index;

  for (Index = DWC3_EP0_IN + 1; Index < Dev->NumPhysicalEps; Index++) {
    Endpoint = &Dev->Endpoints[Index];

    if (!Endpoint->Enabled) {
      continue;
    }

    if (Endpoint->TransferStarted) {
      Dwc3EndTransfer (Dev, Index);
      Dwc3PushTransferResult (Dev, Index, 0, UsbTransferStatusAborted);
      Endpoint->TransferBuffer = NULL;
    }

    Dwc3EnableEndpoint (Dev, Index, Endpoint->EpType, Endpoint->MaxPacketSize, FALSE);
  }
}

STATIC
BOOLEAN
Dwc3Ep0HandleStandardRequest (IN DWC3_DEV *Dev)
{
  EFI_USB_DEVICE_REQUEST *Request = &Dev->SetupPacket;
  UINT32                  Register;
  UINT8                   Address;

  if ((Request->RequestType & (0x03 << 5)) != USB_REQ_TYPE_STANDARD) {
    return FALSE;
  }

  switch (Request->Request) {
    case USB_REQ_SET_ADDRESS:
      if (Request->RequestType != 0 || Request->Length != 0 ||
          Request->Index != 0 || Request->Value > 127) {
        return FALSE;
      }
      Address = (UINT8)Request->Value;

      Register  = MmioRead32 (Dev->Base + DWC3_DCFG);
      Register &= ~DWC3_DCFG_DEVADDR_MASK;
      Register |= DWC3_DCFG_DEVADDR (Address);
      MmioWrite32 (Dev->Base + DWC3_DCFG, Register);

      return TRUE;

    case USB_REQ_SET_CONFIG:
    case USB_REQ_SET_INTERFACE:
      Dwc3ResetNonControlEndpoints (Dev);

      return FALSE;

    default:
      return FALSE;
  }
}

STATIC
VOID
Dwc3Ep0HandleSetup (IN DWC3_DEV *Dev)
{
  EFI_USBFN_MESSAGE_PAYLOAD Payload;

  //
  // The controller wrote this over AXI; drop any line the CPU still holds.
  //
  WriteBackInvalidateDataCacheRange (Dev->Ep0Buffer, sizeof (EFI_USB_DEVICE_REQUEST));
  MemoryFence ();

  CopyMem (&Dev->SetupPacket, Dev->Ep0Buffer, sizeof (EFI_USB_DEVICE_REQUEST));

  Dev->ThreeStageSetup = (BOOLEAN)(Dev->SetupPacket.Length != 0);
  Dev->Ep0ExpectIn     = (BOOLEAN)((Dev->SetupPacket.RequestType & USB_ENDPOINT_DIR_IN) != 0);

  if (Dev->EventTraceBudget != 0) {
    RefSay ("[REF] EP0 SETUP type=%02x req=%02x value=%04x index=%04x len=%u\n",
            Dev->SetupPacket.RequestType, Dev->SetupPacket.Request,
            Dev->SetupPacket.Value, Dev->SetupPacket.Index, Dev->SetupPacket.Length);
    Dev->EventTraceBudget--;
  }

  if (Dwc3Ep0HandleStandardRequest (Dev)) {
    Dev->Ep0State           = Dwc3Ep0StateStatus;
    Dev->Ep0StatusInternal  = TRUE;
    Dev->Ep0StatusRequested = TRUE;
    return;
  }

  Dev->Ep0State = Dev->ThreeStageSetup ? Dwc3Ep0StateData : Dwc3Ep0StateStatus;

  ZeroMem (&Payload, sizeof (Payload));
  CopyMem (&Payload.udr, &Dev->SetupPacket, sizeof (EFI_USB_DEVICE_REQUEST));

  Dwc3PushMessage (Dev, EfiUsbMsgSetupPacket, &Payload);
}

STATIC
UINT8
Dwc3Ep0StatusEndpoint (IN DWC3_DEV *Dev)
{
  // A two-stage request always has IN status, regardless of bmRequestType.
  return (Dev->ThreeStageSetup && Dev->Ep0ExpectIn) ? DWC3_EP0_OUT : DWC3_EP0_IN;
}

STATIC
EFI_STATUS
Dwc3Ep0StartStatus (IN DWC3_DEV *Dev)
{
  UINT8      PhysEpNum = Dwc3Ep0StatusEndpoint (Dev);
  UINT32     TrbType = Dev->ThreeStageSetup ? DWC3_TRBCTL_CONTROL_STATUS3 : DWC3_TRBCTL_CONTROL_STATUS2;
  EFI_STATUS Status;

  if (Dev->Ep0State != Dwc3Ep0StateStatus || Dev->Ep0StatusStarted ||
      !Dev->Ep0StatusRequested || !Dev->Ep0StatusReady) {
    return EFI_SUCCESS;
  }

  Dev->Endpoints[PhysEpNum].TransferBuffer    = Dev->Ep0StatusBuffer;
  Dev->Endpoints[PhysEpNum].TransferTrbLength = 0;
  Status = Dwc3StartTransfer (Dev, PhysEpNum, Dev->Ep0BufferPhys, 0, TrbType);
  if (EFI_ERROR (Status)) {
    // A request accepted before NR must also receive an asynchronous abort.
    if (!Dev->Ep0StatusInternal) {
      Dwc3PushTransferResult (Dev, PhysEpNum, 0, UsbTransferStatusAborted);
    }
    Dwc3Ep0StallAndRestart (Dev);
    return Status;
  }

  Dev->Ep0StatusStarted = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
Dwc3Ep0RequestStatus (
  IN DWC3_DEV *Dev,
  IN UINT8     PhysEpNum,
  IN VOID     *Buffer OPTIONAL)
{
  if (Dev->Ep0DmaUnsafe) {
    return EFI_DEVICE_ERROR;
  }

  if (Dev->Ep0State != Dwc3Ep0StateStatus || Dev->Ep0StatusRequested) {
    return EFI_NOT_READY;
  }

  if (PhysEpNum != Dwc3Ep0StatusEndpoint (Dev)) {
    return EFI_INVALID_PARAMETER;
  }

  Dev->Ep0StatusRequested = TRUE;
  Dev->Ep0StatusBuffer    = Buffer;
  return Dwc3Ep0StartStatus (Dev);
}

STATIC
UINTN
Dwc3TrbBytesTransferred (IN DWC3_ENDPOINT *Endpoint)
{
  UINT32 Remaining = DWC3_TRB_SIZE_LENGTH (Endpoint->TrbPool[0].Size);

  if (Remaining > Endpoint->TransferTrbLength) {
    return 0;
  }

  return Endpoint->TransferTrbLength - Remaining;
}

STATIC
VOID
Dwc3HandleEp0Event (
  IN DWC3_DEV *Dev,
  IN UINT8     PhysEpNum,
  IN UINT8     EventType,
  IN UINT8     EventStatus)
{
  DWC3_ENDPOINT *Endpoint = &Dev->Endpoints[PhysEpNum];
  UINTN          Transferred;

  switch (EventType) {
    case DWC3_DEPEVT_XFERCOMPLETE:
      // Ignore stale/duplicate events, including a previous OUT status after
      // the shared TRB has already been handed back to SETUP reception.
      if (Dev->Ep0DmaUnsafe || !Endpoint->TransferStarted ||
          (Endpoint->TrbPool[0].Control & DWC3_TRB_CTRL_HWO) != 0) {
        break;
      }

      Endpoint->TransferStarted = FALSE;
      Endpoint->ResourceIndex   = 0;

      if (Dev->EventTraceBudget != 0) {
        RefSay ("[REF] EP0 complete ep=%u phase=%u event=%x trb=%08x internal=%u\n",
                PhysEpNum, Dev->Ep0State, EventStatus, Endpoint->TrbPool[0].Size,
                Dev->Ep0StatusInternal);
        Dev->EventTraceBudget--;
      }

      if ((EventStatus & DEPEVT_STATUS_BUSERR) != 0 ||
          DWC3_TRB_SIZE_TRBSTS (Endpoint->TrbPool[0].Size) != DWC3_TRBSTS_OK) {
        if (Dev->Ep0State == Dwc3Ep0StateData ||
            (Dev->Ep0State == Dwc3Ep0StateStatus && !Dev->Ep0StatusInternal)) {
          Dwc3PushTransferResult (Dev, PhysEpNum, 0, UsbTransferStatusAborted);
        }
        Dwc3Ep0StallAndRestart (Dev);
        break;
      }

      switch (Dev->Ep0State) {
        case Dwc3Ep0StateSetup:
          Dwc3Ep0HandleSetup (Dev);
          break;

        case Dwc3Ep0StateData:
          Transferred = Dwc3TrbBytesTransferred (Endpoint);

          if (Dev->Ep0Bounced) {
            if (Transferred > Dev->Ep0BounceLength) {
              Transferred = Dev->Ep0BounceLength;
            }

            if (Dev->Ep0BounceTarget != NULL && Transferred != 0) {
              CopyMem (Dev->Ep0BounceTarget, Dev->Ep0Buffer, Transferred);
            }
          }
          Dev->Ep0Bounced      = FALSE;
          Dev->Ep0BounceTarget = NULL;
          Dev->Ep0BounceLength = 0;

          // Publish Status before the result can cause the upper reverse-direction
          // Transfer(0). NR alone must never take ownership of an external status.
          Dev->Ep0State = Dwc3Ep0StateStatus;
          if (Transferred == 0) {
            // Qualcomm Rx/Tx handlers (reverse 0x2c70/0x2d84) treat any zero-byte
            // completion as final. Only for a successful zero-byte DATA stage of
            // wLength != 0, finish status internally; do not invent data bytes or
            // deliver a second completion for a status the upper never requested.
            Dev->Ep0StatusInternal  = TRUE;
            Dev->Ep0StatusRequested = TRUE;
          }
          Dwc3PushTransferResult (Dev, PhysEpNum, Transferred, UsbTransferStatusComplete);
          Endpoint->TransferBuffer = NULL;
          Dwc3Ep0StartStatus (Dev);
          break;

        case Dwc3Ep0StateStatus:
          if (Dev->Ep0StatusStarted && !Dev->Ep0StatusInternal) {
            Dwc3PushTransferResult (Dev, PhysEpNum, 0, UsbTransferStatusComplete);
          }
          Dwc3Ep0OutStart (Dev);
          break;

        default:
          break;
      }

      break;

    case DWC3_DEPEVT_XFERNOTREADY:
      switch (DEPEVT_STATUS_CONTROL_PHASE (EventStatus)) {
        case DEPEVT_STATUS_CONTROL_DATA:
          if (Dev->Ep0State == Dwc3Ep0StateData &&
              (PhysEpNum == DWC3_EP0_IN) != Dev->Ep0ExpectIn) {
            DEBUG ((EFI_D_ERROR, "DWC3: Unexpected Direction for the EP0 Data Stage!\n"));
            if (Dev->Endpoints[Dev->Ep0ExpectIn ? DWC3_EP0_IN : DWC3_EP0_OUT].TransferStarted) {
              Dwc3PushTransferResult (Dev, Dev->Ep0ExpectIn ? DWC3_EP0_IN : DWC3_EP0_OUT,
                                      0, UsbTransferStatusAborted);
            }
            Dwc3Ep0StallAndRestart (Dev);
          }

          break;

        case DEPEVT_STATUS_CONTROL_STATUS:
          if (Dev->Ep0State != Dwc3Ep0StateSetup &&
              PhysEpNum == Dwc3Ep0StatusEndpoint (Dev) &&
              !Dev->Ep0StatusStarted && !Dev->Ep0DmaUnsafe) {
            Dev->Ep0StatusReady = TRUE;
            Dwc3Ep0StartStatus (Dev);
          }
          break;

        default:
          break;
      }

      break;

    default:
      break;
  }
}

STATIC
VOID
Dwc3HandleEpEvent (
  IN DWC3_DEV *Dev,
  IN UINT32    Event)
{
  UINT8          PhysEpNum   = (UINT8)DWC3_DEPEVT_EPNUMBER (Event);
  UINT8          EventType   = (UINT8)DWC3_DEPEVT_TYPE (Event);
  UINT8          EventStatus = (UINT8)DWC3_DEPEVT_STATUS (Event);
  DWC3_ENDPOINT *Endpoint;
  UINTN          Transferred;

  if (PhysEpNum >= DWC3_MAX_PHYSICAL_EP) {
    return;
  }

  Endpoint = &Dev->Endpoints[PhysEpNum];

  if (PhysEpNum <= DWC3_EP0_IN) {
    Dwc3HandleEp0Event (Dev, PhysEpNum, EventType, EventStatus);
    return;
  }

  switch (EventType) {
    case DWC3_DEPEVT_XFERCOMPLETE:
    case DWC3_DEPEVT_XFERINPROGRESS:
      if (!Endpoint->TransferStarted) {
        break;
      }

      if ((Endpoint->TrbPool[0].Control & DWC3_TRB_CTRL_HWO) != 0) {
        break;
      }

      Endpoint->TransferStarted = FALSE;
      Endpoint->ResourceIndex   = 0;

      Transferred = Dwc3TrbBytesTransferred (Endpoint);

      if ((PhysEpNum & 1) == 0 && Endpoint->TransferBuffer != NULL && Endpoint->TransferTrbLength != 0) {
        InvalidateDataCacheRange (Endpoint->TransferBuffer, Endpoint->TransferTrbLength);
      }

      Dwc3PushTransferResult (Dev, PhysEpNum, Transferred,
                              (EventStatus & DEPEVT_STATUS_BUSERR) ? UsbTransferStatusAborted : UsbTransferStatusComplete);

      Endpoint->TransferBuffer = NULL;
      break;

    case DWC3_DEPEVT_XFERNOTREADY:
      break;

    default:
      break;
  }
}

STATIC
VOID
Dwc3HandleDeviceEvent (
  IN DWC3_DEV *Dev,
  IN UINT32    Event)
{
  UINT8                     EventType = (UINT8)DWC3_DEVT_TYPE (Event);
  UINT16                    EventInfo = (UINT16)DWC3_DEVT_INFO (Event);
  EFI_USBFN_MESSAGE_PAYLOAD Payload;
  UINT32                    Register;
  UINT16                    Ep0MaxPacket;
  UINT16                    BulkMaxPacket;
  UINT8                     Index;
  BOOLEAN                   Suspended;
  BOOLEAN                   Ep0Rearm;

  ZeroMem (&Payload, sizeof (Payload));

  // The reverse UsbDevice state machine accepts Reset only after Attach.
  // A host reset is attachment evidence; CONNECT_DONE is the fallback.
  if (!Dev->Attached && (EventType == DWC3_DEVICE_EVENT_RESET ||
                        EventType == DWC3_DEVICE_EVENT_CONNECT_DONE)) {
    Dev->Attached = TRUE;
    Dwc3PushMessage (Dev, EfiUsbMsgBusEventAttach, NULL);
  }

  switch (EventType) {
    case DWC3_DEVICE_EVENT_DISCONNECT:
      Dev->Speed    = UsbBusSpeedUnknown;
      Dev->Attached = FALSE;
      Dwc3Ep0ResetState (Dev);

      Dwc3PushMessage (Dev, EfiUsbMsgBusEventDetach, NULL);
      break;

    case DWC3_DEVICE_EVENT_RESET:
      Register  = MmioRead32 (Dev->Base + DWC3_DCTL);
      Register &= ~DWC3_DCTL_TSTCTRL_MASK;
      MmioWrite32 (Dev->Base + DWC3_DCTL, Register);

      for (Index = 0; Index < Dev->NumPhysicalEps; Index++) {
        if (Dev->Endpoints[Index].Enabled && Dev->Endpoints[Index].Stalled) {
          Dwc3SetHalt (Dev, Index, FALSE);
        }
      }

      Register  = MmioRead32 (Dev->Base + DWC3_DCFG);
      Register &= ~DWC3_DCFG_DEVADDR_MASK;
      MmioWrite32 (Dev->Base + DWC3_DCFG, Register);

      for (Index = DWC3_EP0_IN + 1; Index < Dev->NumPhysicalEps; Index++) {
        if (Dev->Endpoints[Index].Enabled) {
          Dwc3EndTransfer (Dev, Index);
          Dev->Endpoints[Index].TransferBuffer = NULL;
        }
      }

      Dev->LinkState = DWC3_LINK_STATE_HS_ON;
      Dev->Suspended = FALSE;

      Dwc3Ep0OutStart (Dev);

      Dwc3PushMessage (Dev, EfiUsbMsgBusEventReset, NULL);
      break;

    case DWC3_DEVICE_EVENT_CONNECT_DONE:
      Register = MmioRead32 (Dev->Base + DWC3_DSTS) & DWC3_DSTS_CONNECTSPD;

      switch (Register) {
        case DWC3_DSTS_SUPERSPEED_PLUS:
        case DWC3_DSTS_SUPERSPEED:
          Dev->Speed   = UsbBusSpeedSuper;
          Ep0MaxPacket = 512;
          break;
        case DWC3_DSTS_HIGHSPEED:
          Dev->Speed   = UsbBusSpeedHigh;
          Ep0MaxPacket = 64;
          break;
        case DWC3_DSTS_FULLSPEED:
          Dev->Speed   = UsbBusSpeedFull;
          Ep0MaxPacket = 64;
          break;
        default:
          Dev->Speed   = UsbBusSpeedLow;
          Ep0MaxPacket = 8;
          break;
      }

      Ep0Rearm = (BOOLEAN)(Dev->Ep0State == Dwc3Ep0StateSetup &&
                            !Dev->Endpoints[DWC3_EP0_OUT].TransferStarted &&
                            !Dev->Endpoints[DWC3_EP0_IN].TransferStarted);

      // MODIFY must preserve ownership even when EP0 IN (not OUT) is active.
      Dwc3EnableEndpoint (Dev, DWC3_EP0_OUT, DWC3_DEPCMD_TYPE_CONTROL, Ep0MaxPacket,
                          Dev->Endpoints[DWC3_EP0_OUT].Enabled);
      Dwc3EnableEndpoint (Dev, DWC3_EP0_IN, DWC3_DEPCMD_TYPE_CONTROL, Ep0MaxPacket,
                          Dev->Endpoints[DWC3_EP0_IN].Enabled);

      BulkMaxPacket = (UINT16)((Dev->Speed >= UsbBusSpeedHigh) ? 512 : 64);

      for (Index = DWC3_EP0_IN + 1; Index < Dev->NumPhysicalEps; Index++) {
        if (!Dev->Endpoints[Index].Enabled) {
          continue;
        }

        Dwc3EnableEndpoint (Dev, Index, Dev->Endpoints[Index].EpType, BulkMaxPacket, FALSE);
      }

      if (Ep0Rearm) {
        Dwc3Ep0OutStart (Dev);
      }

      Payload.ubs = Dev->Speed;
      Dwc3PushMessage (Dev, EfiUsbMsgBusEventSpeed, &Payload);
      break;

    case DWC3_DEVICE_EVENT_LINK_STATUS_CHANGE:
      EventInfo &= DWC3_LINK_STATE_MASK;

      if (EventInfo == Dev->LinkState) {
        break;
      }

      Dev->LinkState = (UINT8)EventInfo;

      if (Dev->Speed == UsbBusSpeedSuper) {
        Suspended = (BOOLEAN)(EventInfo == DWC3_LINK_STATE_U3);
      } else {
        Suspended = (BOOLEAN)(EventInfo == DWC3_LINK_STATE_HS_SUSPEND ||
                              EventInfo == DWC3_LINK_STATE_HS_EARLY_SUSPEND);
      }

      if (Suspended != Dev->Suspended) {
        Dev->Suspended = Suspended;
        Dwc3PushMessage (Dev, Suspended ? EfiUsbMsgBusEventSuspend : EfiUsbMsgBusEventResume, NULL);
      }

      break;

    case DWC3_DEVICE_EVENT_WAKEUP:
      Dwc3PushMessage (Dev, EfiUsbMsgBusEventResume, NULL);
      break;

    case DWC3_DEVICE_EVENT_OVERFLOW:
      DEBUG ((EFI_D_ERROR, "DWC3: Event Buffer Overflow!\n"));
      break;

    case DWC3_DEVICE_EVENT_ERRATIC_ERROR:
      // Observe the error without automatically resetting the core or retrying DMA.
      if (Dev->EventTraceBudget != 0) {
        RefSay ("[USBERR b213] ERRATIC_ERROR info=%u DSTS=%08x\n",
                EventInfo, MmioRead32 (Dev->Base + DWC3_DSTS));
        Dev->EventTraceBudget--;
      }
      break;

    default:
      break;
  }
}

EFI_STATUS
Dwc3ProcessEvents (IN DWC3_DEV *Dev)
{
  UINT32 Count;
  UINT32 Event;

  // The count field is 20 bits wide. Mask only EHB so the low two bits still
  // expose misalignment and bits [19:16] cannot be truncated into a small,
  // apparently valid count by GEVNTCOUNT_MASK (0xFFFC).
  Count = MmioRead32 (Dev->Base + DWC3_GEVNTCOUNT (0)) & ~DWC3_GEVNTCOUNT_EHB;
  if (Count > DWC3_EVENT_BUFFER_SIZE || (Count & 3U) != 0 ||
      Dev->EventPos >= DWC3_EVENT_BUFFER_SIZE || (Dev->EventPos & 3U) != 0) {
    if (Dev->EventTraceBudget != 0) {
      RefSay ("[REF] invalid event ring count=%u pos=%u\n", Count, Dev->EventPos);
      Dev->EventTraceBudget--;
    }
    return EFI_DEVICE_ERROR;
  }
  if (Count == 0) {
    return EFI_NOT_READY;
  }

  // Count publishes DMA writes into the uncached event/TRB/bounce buffers.
  MemoryFence ();
  while (Count >= sizeof (UINT32)) {
    WriteBackInvalidateDataCacheRange (Dev->EventBuffer + Dev->EventPos, sizeof (UINT32));
    MemoryFence ();

    CopyMem (&Event, Dev->EventBuffer + Dev->EventPos, sizeof (UINT32));

    Dev->EventPos = (Dev->EventPos + sizeof (UINT32)) % DWC3_EVENT_BUFFER_SIZE;
    Count        -= sizeof (UINT32);

    MemoryFence ();
    MmioWrite32 (Dev->Base + DWC3_GEVNTCOUNT (0), sizeof (UINT32));

    if (Dev->EventTraceBudget != 0) {
      RefSay ("[REF] event=%08x ep0=%u requested=%u ready=%u started=%u\n",
              Event, Dev->Ep0State, Dev->Ep0StatusRequested,
              Dev->Ep0StatusReady, Dev->Ep0StatusStarted);
      Dev->EventTraceBudget--;
    }

    if (DWC3_EVENT_IS_ENDPOINT (Event)) {
      Dwc3HandleEpEvent (Dev, Event);
    } else if (DWC3_DEVT_CATEGORY (Event) == DWC3_EVENT_TYPE_DEV) {
      if (Dev->DmaFaultTraceBudget != 0) {
        UsbfnPlatformDmaSnapshot ("device-event", FALSE);
        Dev->DmaFaultTraceBudget--;
      }
      Dwc3HandleDeviceEvent (Dev, Event);
    } else {
      DEBUG ((EFI_D_ERROR, "DWC3: Unknown Event 0x%08x\n", Event));
    }

    if (Dev->MessageHead != Dev->MessageTail) {
      break;
    }
  }

  return EFI_SUCCESS;
}
