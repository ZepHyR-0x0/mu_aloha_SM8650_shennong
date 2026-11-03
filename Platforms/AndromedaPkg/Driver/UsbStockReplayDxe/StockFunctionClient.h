// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../ReadOnlyStorageDxe/ReadOnlyStorage.h"
#include "UsbFunctionDescriptors.h"
STATIC USB_FUNCTIONS mFunctions;
STATIC UINT32 mActiveMode, mDesiredMode=USB_MODE_NATIVE;
STATIC UINT64 mCdcReturnAt;
STATIC BOOLEAN mUsbConfigured, mUsbSavePending, mStopFailed;
STATIC BOOLEAN mResetSeen;
STATIC UINT8 mClearSeen;
STATIC UINT32 mSpeedSeen;
STATIC UINTN mClassMessages;
STATIC UINTN mBotCommands;
STATIC UINT32 mMscSaves;
STATIC UINT64 mMscSaveAt;
STATIC BOOLEAN mMscSavePending, mMscFaultSaved;
STATIC UINT8 mMscLastOp;
STATIC BOOLEAN mRawCompletionValid;
STATIC USB_DEVICE_TRANSFER_OUTCOME mRawCompletion;
STATIC BOOLEAN mRawSetupValid;
STATIC EFI_USB_DEVICE_REQUEST mRawSetup;

// Snapshot only outside HandleEvent and native trace hooks. Keep the first
// command outcomes, then sparse progress; reserve a separate save for failure.
STATIC VOID StockMscSaveService (EFI_STATUS Status)
{
  UINT64 Now;
  BOOLEAN Fault;
  if (mActiveMode!=USB_MODE_RAMDISK && mActiveMode!=USB_MODE_STORAGE) return;
  if (mPolling || StockCurrentTpl()!=TPL_APPLICATION) return;
  Now=StockTraceNowMs();
  Fault=EFI_ERROR(Status) && Status!=EFI_NOT_READY;
  if (Fault && !mMscFaultSaved) {
    mMscFaultSaved=TRUE;
    StockSay("[USBBOT] fault op=%02x status=%r state=%u ctrl=%u rx=%u tx=%u residue=%u reset=%u\r\n",
      mMscLastOp,Status,mFunctions.State,mFunctions.ControlState,mFunctions.RxBusy,mFunctions.TxBusy,
      mFunctions.Residue,mFunctions.NeedReset);
    (VOID)BootLogSave("usb-msc-fault");
  } else if (mMscSavePending && mMscSaves<16 && Now>=mMscSaveAt) {
    mMscSavePending=FALSE;mMscSaves++;
    mMscSaveAt=Now+(mMscSaves<8?250:2000);
    StockSay("[USBBOT] progress cbw=%u op=%02x state=%u ctrl=%u rx=%u tx=%u residue=%u reset=%u\r\n",
      (UINT32)mBotCommands,mMscLastOp,mFunctions.State,mFunctions.ControlState,mFunctions.RxBusy,
      mFunctions.TxBusy,mFunctions.Residue,mFunctions.NeedReset);
    (VOID)BootLogSave("usb-msc-progress");
  }
}

STATIC BOOLEAN StockFunctionMessage (EFI_USBFN_MESSAGE Message, UINTN Size, EFI_USBFN_MESSAGE_PAYLOAD *P)
{
  if (Message==EfiUsbMsgBusEventReset || Message==EfiUsbMsgBusEventDetach) mResetSeen=TRUE;
  if (Message==EfiUsbMsgBusEventSpeed && Size>=sizeof(P->ubs)) {mSpeedSeen=P->ubs;mNegotiatedSpeed=P->ubs;}
  if (Message==EfiUsbMsgSetupPacket && Size>=sizeof(P->udr) &&
      P->udr.RequestType==2 && P->udr.Request==1 && P->udr.Value==0 && P->udr.Length==0)
    mClearSeen=(UINT8)P->udr.Index;
  // After configuration the native standard descriptor handler can both
  // reply and delegate the same request. Claim supported repeated descriptors
  // after Usbfn establishes EP0 ownership, before that duplicate upper reply.
  if(Message==EfiUsbMsgSetupPacket && Size>=sizeof(P->udr) && mUsbConfigured &&
     mFunctions.Mode!=0 && P->udr.RequestType==0x80 && P->udr.Request==6 && P->udr.Length!=0 &&
     mFunctions.Descriptor!=NULL){
    CONST UINT8 *Descriptor=NULL;UINTN DescriptorBytes=0;
    if(mFunctions.Descriptor(P->udr.Value,P->udr.Index,&Descriptor,&DescriptorBytes) && Descriptor!=NULL && DescriptorBytes<=512){
      mRawSetup=P->udr;mRawSetupValid=TRUE;return TRUE;
    }
  }
  if ((Message==EfiUsbMsgEndpointStatusChangedRx || Message==EfiUsbMsgEndpointStatusChangedTx) &&
      Size>=sizeof(P->utr) && mFunctions.Mode!=0 && P->utr.Buffer!=NULL &&
      ((P->utr.Buffer==mFunctions.Rx && P->utr.EndpointIndex==1 && P->utr.Direction==EfiUsbEndpointDirectionDeviceRx) ||
       (P->utr.Buffer==mFunctions.Tx && P->utr.EndpointIndex==1 && P->utr.Direction==EfiUsbEndpointDirectionDeviceTx) ||
       (P->utr.Buffer==mFunctions.Control && P->utr.EndpointIndex==0) ||
       (P->utr.Buffer==mFunctions.Notify && P->utr.EndpointIndex==2 && P->utr.Direction==EfiUsbEndpointDirectionDeviceTx))) {
    // Usbfn already processed the hardware completion. Claim our buffers at
    // this boundary, before UsbDevice's upper state machine treats them as its
    // standard control transfers. Its single b3d8 flag is cleared by unrelated
    // bulk/interrupt events; a later EP0 event then submits an extra status
    // with its own buffer. A fallback after HandleEvent is too late to undo it.
    mRawCompletion.EndpointIndex=(UINT8)(P->utr.EndpointIndex|((P->utr.Direction==EfiUsbEndpointDirectionDeviceTx)?0x80:0));
    mRawCompletion.BytesCompleted=P->utr.BytesTransferred;mRawCompletion.DataBuffer=P->utr.Buffer;
    mRawCompletion.Status=P->utr.TransferStatus==UsbTransferStatusComplete?UsbDeviceTransferStatusCompleteOK:
      P->utr.TransferStatus==UsbTransferStatusAborted?UsbDeviceTransferStatusCancelled:
      P->utr.TransferStatus==UsbTransferStatusActive?UsbDeviceTransferStatusActive:UsbDeviceTransferStatusCompleteError;
    mRawCompletionValid=TRUE;
    return TRUE;
  }
  return FALSE;
}
STATIC EFI_STATUS StockFunctionSend (UINT8 Ep, UINTN Bytes, VOID *Buffer)
{
  EFI_STATUS Status;
  UINTN Capacity=Buffer==mFunctions.Rx || Buffer==mFunctions.Tx?USB_FUNCTION_BUFFER_BYTES:
    Buffer==mFunctions.Control?512:Buffer==mFunctions.Notify?16:0;
  if (Capacity==0 || Bytes>Capacity || mDevice==NULL || mDevice->Send==NULL || !StockTraceWritable(Buffer,Bytes==0?1:Bytes) ||
      !StockDmaPreflight()) return EFI_SECURITY_VIOLATION;
  // All class buffers come from native AllocateTransferBuffer; native DMA
  // primitives remain responsible for mapping/cache maintenance and doorbells.
  StockTraceBegin();Status=mDevice->Send(Ep,Bytes,Buffer);StockTraceEnd();
  return Status;
}
STATIC EFI_STATUS StockFunctionStall (UINT8 Ep, BOOLEAN Halt)
{ return mDevice->SetEndpointStallState(Ep,Halt); }
STATIC EFI_STATUS StockFunctionAbort (UINT8 Ep)
{ return mDevice->AbortXfer(Ep); }
STATIC BOOLEAN StockFunctionDescriptor(UINT16 Value,UINT16 Index,CONST UINT8 **Data,UINTN *Bytes)
{
  BOOLEAN Super=mFunctions.PacketSize==1024;
  if(Index!=0 || (Value&255)!=0)return FALSE;
  switch(Value>>8){
    case 1:*Data=(CONST UINT8 *)(Super?&mSsDevice:&mHsDevice);*Bytes=sizeof(mHsDevice);break;
    case 2:*Data=(CONST UINT8 *)(Super?mSsDescriptors[0]:mHsDescriptors[0]);*Bytes=(*Data)[2]|((UINTN)(*Data)[3]<<8);break;
    case 3:*Data=mLanguage;*Bytes=sizeof(mLanguage);break;
    case 6:*Data=(CONST UINT8 *)&mQualifier;*Bytes=sizeof(mQualifier);break;
    case 15:if(!Super)return FALSE;*Data=mBos;*Bytes=sizeof(mBos);break;
    default:return FALSE;
  }
  return TRUE;
}
STATIC EFI_STATUS StockFunctionCreate (VOID)
{
  EFI_STATUS Status;
  VOID **Buffers[4]={(VOID **)&mFunctions.Rx,(VOID **)&mFunctions.Tx,(VOID **)&mFunctions.Control,(VOID **)&mFunctions.Notify};
  UINTN Sizes[4]={USB_FUNCTION_BUFFER_BYTES,USB_FUNCTION_BUFFER_BYTES,512,16},I;
  if (mDesiredMode!=USB_MODE_STORAGE && mDesiredMode!=USB_MODE_RAMDISK && mDesiredMode!=USB_MODE_CDC) return EFI_SUCCESS;
  if (mDevice->Send==NULL || mDevice->AbortXfer==NULL || mDevice->SetEndpointStallState==NULL ||
      mDevice->AllocateTransferBuffer==NULL || mDevice->FreeTransferBuffer==NULL) return EFI_UNSUPPORTED;
  for (I=0;I<ARRAY_SIZE(Buffers);I++) {
    Status=mDevice->AllocateTransferBuffer(Sizes[I],Buffers[I]);
    if (EFI_ERROR(Status)) return Status;
    if (*Buffers[I]==NULL) return EFI_OUT_OF_RESOURCES;
  }
  mFunctions.Mode=mDesiredMode;mFunctions.PacketSize=512;
  if (mDesiredMode==USB_MODE_STORAGE || mDesiredMode==USB_MODE_RAMDISK) {
    // RAM's logical block size is fixed. A host may edit/format its BPB; that
    // must not change the device's block geometry when it is reconnected.
    mFunctions.SectorBytes=mDesiredMode==USB_MODE_RAMDISK?512:
      mFunctions.Disk[11]|((UINT32)mFunctions.Disk[12]<<8);
    if ((mFunctions.SectorBytes!=512 && mFunctions.SectorBytes!=4096) || mFunctions.DiskBytes%mFunctions.SectorBytes!=0)
      return EFI_SECURITY_VIOLATION;
  }
  mFunctions.Send=StockFunctionSend;mFunctions.Stall=StockFunctionStall;mFunctions.Abort=StockFunctionAbort;
  mFunctions.ReadLog=BootLogRead;
  mFunctions.ReadConsole=StockConsoleRead;mFunctions.ConsoleInput=StockConsoleInput;
  mFunctions.CommandInput=StockCommandInput;mFunctions.ReadReply=StockCommandRead;mFunctions.ResetReply=StockSessionReset;
  mFunctions.BlobInput=StockRamInput;mFunctions.BlobReceiveBytes=StockRamReceiveBytes;
  mFunctions.Descriptor=StockFunctionDescriptor;
  if (mDesiredMode==USB_MODE_RAMDISK && mFunctions.Disk==mRamDisk && mRamUsbOwned) mFunctions.WritableDisk=mRamDisk;
  mFunctions.LineCoding[0]=0xC0;mFunctions.LineCoding[1]=0xC6;mFunctions.LineCoding[2]=0x2D;
  mFunctions.LineCoding[3]=mFunctions.LineCoding[4]=mFunctions.LineCoding[5]=0;mFunctions.LineCoding[6]=8;
  if (mDesiredMode==USB_MODE_CDC) {
    mFunctions.LogCapacity=BOOT_LOG_MAX_BYTES;mFunctions.Log=AllocatePool(BOOT_LOG_MAX_BYTES);
    if (mFunctions.Log==NULL) return EFI_OUT_OF_RESOURCES;
  }
  return EFI_SUCCESS;
}
STATIC VOID StockFunctionFree (VOID)
{
  UINT8 *Buffers[4]={mFunctions.Rx,mFunctions.Tx,mFunctions.Control,mFunctions.Notify};UINTN I;
  // Only called after native Stop plus common-init/halt verification.
  StockSessionReset();
  for (I=0;I<ARRAY_SIZE(Buffers);I++) if (Buffers[I]!=NULL) (VOID)mDevice->FreeTransferBuffer(Buffers[I]);
  if (mFunctions.Disk!=NULL && mFunctions.Disk!=mRamDisk) FreePool((VOID *)mFunctions.Disk);
  if (mFunctions.Log!=NULL) FreePool(mFunctions.Log);
  ZeroMem(&mFunctions,sizeof(mFunctions));
}
STATIC EFI_STATUS StockFunctionEvent (USB_DEVICE_EVENT Event, UINTN Bytes, USB_DEVICE_EVENT_DATA *Data)
{
  STATIC EFI_GUID SetupGuid={0x11418746,0xCA3B,0x4106,{0xBB,0x5A,0x7C,0xC9,0xD8,0x2D,0xE6,0x91}};
  EFI_STATUS Status=EFI_SUCCESS;
  UINT16 Packet;
  if (mResetSeen) {
    mResetSeen=FALSE;mUsbConfigured=FALSE;
    mRawSetupValid=FALSE;
    if(mActiveMode==USB_MODE_RAMDISK || mActiveMode==USB_MODE_STORAGE){
      StockSay("[USBBOT] bus reset/detach cbw=%u op=%02x state=%u\r\n",(UINT32)mBotCommands,mMscLastOp,mFunctions.State);
      mMscSavePending=TRUE;
    }
    if (mFunctions.Mode!=0) (VOID)UfConnected(&mFunctions,FALSE);
  }
  if (mSpeedSeen!=0 && mTraceFn!=NULL) {
    Packet=0;
    Status=mTraceFn->GetEndpointMaxPacketSize(mTraceFn,UsbEndpointBulk,(EFI_USB_BUS_SPEED)mSpeedSeen,&Packet);
    if (!EFI_ERROR(Status) && (Packet==64 || Packet==512 || Packet==1024)) mFunctions.PacketSize=mNegotiatedPacket=Packet;
    mSpeedSeen=0;Status=EFI_SUCCESS;
  }
  if (Event==UsbDeviceEventDeviceStateChange && Bytes>=sizeof(Data->DeviceState)) {
    mUsbConfigured=Data->DeviceState==UsbDeviceStateConnected;mUsbSavePending=TRUE;
    if (mFunctions.Mode!=0) Status=UfConnected(&mFunctions,mUsbConfigured);
    StockSay("[USBMODE b327] mode=%u configured=%u\r\n",mActiveMode,mUsbConfigured);
  }
  if (mFunctions.Mode!=0 && !EFI_ERROR(Status)) {
    if(mRawSetupValid){
      EFI_USB_DEVICE_REQUEST Request=mRawSetup;mRawSetupValid=FALSE;
      StockSay("[USBCLASS] owned descriptor value=%04x length=%u\r\n",Request.Value,Request.Length);
      Status=UfSetup(&mFunctions,&Request);
      if(mActiveMode==USB_MODE_RAMDISK || mActiveMode==USB_MODE_STORAGE)mMscSavePending=TRUE;
    }
    if (Event==UsbDeviceEventOemEvent && Bytes>=sizeof(Data->OemData) &&
        CompareGuid(&Data->OemData.DataGuid,&SetupGuid) && Data->OemData.Size==sizeof(EFI_USB_DEVICE_REQUEST) &&
        StockTraceReadable(Data->OemData.Context,sizeof(EFI_USB_DEVICE_REQUEST))) {
      EFI_USB_DEVICE_REQUEST Request;
      CopyMem(&Request,Data->OemData.Context,sizeof(Request));
      if (mClassMessages++<24) StockSay("[USBCLASS b327] mode=%u type=%02x req=%02x length=%u\r\n",
        mActiveMode,Request.RequestType,Request.Request,Request.Length);
      Status=UfSetup(&mFunctions,&Request);
      if(mActiveMode==USB_MODE_RAMDISK || mActiveMode==USB_MODE_STORAGE){
        StockSay("[USBBOT] setup type=%02x req=%02x value=%04x index=%04x length=%u -> %r\r\n",
          Request.RequestType,Request.Request,Request.Value,Request.Index,Request.Length,Status);
        mMscSavePending=TRUE;
      }
      if(!EFI_ERROR(Status) && Request.RequestType==2 && Request.Request==1 && Request.Value==0 && Request.Length==0 &&
         (Request.Index==1 || Request.Index==0x81))mClearSeen=0;
    } else if (Event==UsbDeviceEventTransferNotification && Bytes>=sizeof(Data->TransferOutcome))
      Status=UfComplete(&mFunctions,&Data->TransferOutcome);
    if(mRawCompletionValid && (mFunctions.Mode==USB_MODE_RAMDISK || mFunctions.Mode==USB_MODE_STORAGE)){
      if(mRawCompletion.EndpointIndex==1 && mFunctions.State==UfAwaitCbw &&
         mRawCompletion.Status==UsbDeviceTransferStatusCompleteOK && mRawCompletion.BytesCompleted==31){
        mBotCommands++;mMscLastOp=mFunctions.Rx[15];mMscSavePending=TRUE;
        if(mBotCommands<=64)StockSay("[USBBOT] cbw=%u sig=%08x tag=%08x op=%02x cdb_len=%u host=%u flags=%02x\r\n",
          (UINT32)mBotCommands,UfLe32(mFunctions.Rx),UfLe32(mFunctions.Rx+4),mMscLastOp,
          mFunctions.Rx[14],UfLe32(mFunctions.Rx+8),mFunctions.Rx[12]);
        if(mBotCommands<=64)StockSay("[USBBOT] cdb=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
          mFunctions.Rx[15],mFunctions.Rx[16],mFunctions.Rx[17],mFunctions.Rx[18],mFunctions.Rx[19],
          mFunctions.Rx[20],mFunctions.Rx[21],mFunctions.Rx[22],mFunctions.Rx[23],mFunctions.Rx[24]);
      }
      if(mRawCompletion.Status!=UsbDeviceTransferStatusCompleteOK && mRawCompletion.Status!=UsbDeviceTransferStatusActive){
        StockSay("[USBBOT] completion ep=%02x status=%u bytes=%u state=%u queued=%u/%u\r\n",
          mRawCompletion.EndpointIndex,(UINT32)mRawCompletion.Status,(UINT32)mRawCompletion.BytesCompleted,
          mFunctions.State,mFunctions.RxQueued,mFunctions.Queued);mMscSavePending=TRUE;
      }
    }
    if (!EFI_ERROR(Status) && mRawCompletionValid &&
        !(Event==UsbDeviceEventTransferNotification && Bytes>=sizeof(Data->TransferOutcome) &&
          Data->TransferOutcome.EndpointIndex==mRawCompletion.EndpointIndex && Data->TransferOutcome.DataBuffer==mRawCompletion.DataBuffer))
      Status=UfComplete(&mFunctions,&mRawCompletion);
    mRawCompletionValid=FALSE;
    if (mClearSeen!=0) { UfClearHalt(&mFunctions,mClearSeen);mClearSeen=0; }
    // Preserve a DMA ownership/bounds violation so the pump can stop at once.
    // Ordinary class errors remain recoverable through the host's bus reset.
    if (mFunctions.Failed) return Status==EFI_SECURITY_VIOLATION?Status:EFI_DEVICE_ERROR;
  } else { mClearSeen=0;mRawCompletionValid=FALSE; }
  return Status;
}
STATIC EFI_STATUS EFIAPI StockGetMode (USB_BRINGUP_PROTOCOL *This, UINT32 *Mode, BOOLEAN *Configured)
{
  (VOID)This;if (Mode==NULL || Configured==NULL) return EFI_INVALID_PARAMETER;
  *Mode=mActiveMode;*Configured=mUsbConfigured;return EFI_SUCCESS;
}
STATIC BOOLEAN EFIAPI StockGetSwitchBusy(USB_BRINGUP_PROTOCOL *This)
{ (VOID)This;return mUsbModeSwitchBusy || mUsbModeSwitchQueued; }
STATIC EFI_STATUS EFIAPI StockTest (USB_BRINGUP_PROTOCOL *This);
STATIC EFI_STATUS EFIAPI StockSelectModeInternal (USB_BRINGUP_PROTOCOL *This, UINT32 Mode)
{
  EFI_STATUS Status;
  EFI_GUID PolicyGuid=READ_ONLY_STORAGE_GUID;
  READ_ONLY_STORAGE_POLICY *Policy=NULL;
  VOID *Snapshot=NULL;UINTN Bytes=0;
  BOOLEAN Refresh=Mode==USB_MODE_REFRESH, Release=Mode==USB_MODE_RAM_RELEASE;
  // Split the guards so a rejected request says why. As one combined test this
  // returned EFI_INVALID_PARAMETER with no trace, which is indistinguishable
  // from "the GUI did nothing" on the screen.
  if (Mode>USB_MODE_RAM_RELEASE) {
    StockSay ("[USBMODE b337] reject: mode %u out of range\r\n", Mode);
    return EFI_INVALID_PARAMETER;
  }
  if (StockCurrentTpl()!=TPL_APPLICATION) {
    StockSay ("[USBMODE b337] reject: tpl=%u, needs TPL_APPLICATION\r\n", (UINT32)StockCurrentTpl());
    return EFI_INVALID_PARAMETER;
  }
  if (mPolling) {
    StockSay ("[USBMODE b337] reject: event pump still running\r\n");
    return EFI_INVALID_PARAMETER;
  }
  if(mRamMutating){StockSay("[CDCRAM] mode change refused during upload\r\n");return EFI_ACCESS_DENIED;}
  mCdcReturnAt=0;
  if (mConsoleAttached) {
    // The CDC Shell merges its console with gST->ConIn/ConOut. When the GUI
    // asks for a different mode the old CDC console must be detached first,
    // otherwise the new function would leave a live console protocol pointed at
    // a controller that is about to Stop/StartEx. ConsoleRestore() only unpins
    // the system console and is safe at this application TPL.
    ConsoleRestore ();
  }
  if (mStopFailed) {
    //
    // A previous Stop() failed and left this flag set, which used to lock out
    // every later selection with EFI_DEVICE_ERROR - exactly the "first mode
    // works, nothing switches afterwards" symptom. The controller may well have
    // settled by now, so retry the stop once instead of refusing forever.
    //
    StockSay ("[USBMODE b337] previous stop failed, retrying\r\n");
    if ((mDevice != NULL) && (mDevice->Stop != NULL)) {
      EFI_STATUS  RetryStatus = mDevice->Stop ();
      if(!EFI_ERROR(RetryStatus))RetryStatus=StockModeReset();
      StockSay ("[USBMODE b337] stop retry -> %r\r\n", RetryStatus);
      if (EFI_ERROR (RetryStatus)) {
        return EFI_DEVICE_ERROR;
      }

      StockFunctionFree ();
      gBS->Stall(500000);
      mAttempted  = FALSE;
      mDevice     = NULL;
      mStopFailed = FALSE;
    } else {
      // Nothing left to stop; clear the flag so the new mode can be armed.
      mStopFailed = FALSE;
    }
  }
  if (Release && !mRamUsbOwned) {
    // The RAM disk was allocated but is not the active USB function. Free it
    // without touching the running CDC/logfs device on the host link.
    Status = RamRelease ();
    StockSay ("[USBRAM b330] release-only (not active) -> %r\r\n", Status);
    return Status;
  }
  if (Release) Mode=USB_MODE_DISABLED;
  if (Refresh) {
    if (mActiveMode!=USB_MODE_STORAGE) return EFI_NOT_READY;
    Mode=USB_MODE_STORAGE;
  }
  StockSay ("[USBMODE b336] request mode=%u active=%u poll=%lu refresh=%u release=%u\r\n",
    (UINT32)Mode, mActiveMode, (UINT64)(UINTN)mPoll, (UINT32)Refresh, (UINT32)Release);
  (VOID)BootLogSave("usb-mode-change");
  if (Mode==USB_MODE_STORAGE) {
    Status=gBS->LocateProtocol(&PolicyGuid,NULL,(VOID **)&Policy);
    if (EFI_ERROR(Status)) return Status;
    if (Policy==NULL || Policy->Revision<3 || Policy->SnapshotLogfs==NULL) return EFI_UNSUPPORTED;
    Status=Policy->SnapshotLogfs(&Snapshot,&Bytes);
    if (EFI_ERROR(Status)) return Status;
    if (Snapshot==NULL || Bytes==0 || Bytes>8*1024*1024 || Bytes%512!=0) {
      if (Snapshot!=NULL) FreePool(Snapshot);return EFI_SECURITY_VIOLATION;
    }
  }
  if (Mode==USB_MODE_RAMDISK) {
    Status=RamAllocate();if (EFI_ERROR(Status)) return Status;
    Status=RamUnpublish();if (EFI_ERROR(Status)) return Status;
  }
  // Record what the switch is actually working from. Without this the two
  // branches below look identical from the outside: one stops the controller,
  // the other just drops the state, and only the log can tell them apart.
  StockSay ("[USBMODE b337] state attempted=%u device=%lx active=%u\r\n",
    (UINT32)mAttempted, (UINT64)(UINTN)mDevice, mActiveMode);

  if (mAttempted && mDevice!=NULL) {
    StockModeDrain();
    StockReleasePump();
    // Abort client-owned transfers before stopping the native controller.
    if (mFunctions.Mode!=0) {
      if(mFunctions.ControlState!=UfControlIdle)(VOID)mDevice->AbortXfer(mFunctions.ControlEp);
      if(mFunctions.RxBusy)(VOID)mDevice->AbortXfer(1);
      if(mFunctions.TxBusy)(VOID)mDevice->AbortXfer(0x81);
      if(mFunctions.NotifyBusy)(VOID)mDevice->AbortXfer(0x82);
    }
    Status=mDevice->Stop();
    StockSay("[USBMODE b327] native stop -> %r\r\n",Status);
    StockSay("[USBMODE] after Stop dctl=%08x dsts=%08x dale=%08x\r\n",
      MmioRead32(0xA60C704),MmioRead32(0xA60C70C),MmioRead32(0xA60C720));
    (VOID)BootLogSave("usb-native-stop-result");
    if (EFI_ERROR(Status)) {
      // Do not free a buffer if the device might still DMA into it.
      mStopFailed=TRUE;if (Snapshot!=NULL) FreePool(Snapshot);return Status;
    }
    Status=StockModeReset();
    if(EFI_ERROR(Status)){
      StockSay("[USBRESET] DMA buffers retained; reinitialization/halt unverified -> %r\r\n",Status);
      mStopFailed=TRUE;if(Snapshot!=NULL)FreePool(Snapshot);return Status;
    }
    StockFunctionFree();
    // Native Stop clears RunStop and the cached cable state. Keep that
    // disconnect visible long enough for the host to discard the old device
    // and its EP0 requests before presenting a different PID/class.
    StockSay("[USBMODE b338] detached; waiting 500 ms before re-enumeration\r\n");
    gBS->Stall(500000);
    // Board switch/repeater setup belongs to StockBoardCoreInit. Replaying it
    // here, between Stop and native PHY initialization, can break the route.
  } else {
    // A failed protocol lookup may leave only the independently allocated
    // snapshot, never a submitted DMA buffer. Release it before replacing it.
    if (mFunctions.Disk!=NULL && mFunctions.Disk!=mRamDisk) FreePool((VOID *)mFunctions.Disk);
    ZeroMem(&mFunctions,sizeof(mFunctions));
  }
  // Same-mode selections are now always re-armed: Stop ran above, so do not
  // return EFI_ALREADY_STARTED. This is the path that previously made disable
  // and repeated selections appear to be no-ops while the link stayed stale.
  mRamUsbOwned=FALSE;
  mAttempted=FALSE;mDevice=NULL;mActiveMode=USB_MODE_DISABLED;mUsbConfigured=FALSE;
  mResetSeen=FALSE;mClearSeen=0;mSpeedSeen=0;mUsbSavePending=FALSE;mRawCompletionValid=FALSE;
  mRawSetupValid=FALSE;
  mErrors=mEvents=mPollCalls=0;mClassMessages=mBotCommands=0;
  mMscSaves=0;mMscSaveAt=0;mMscSavePending=mMscFaultSaved=FALSE;mMscLastOp=0;
  if (Mode==USB_MODE_DISABLED) {
    if (Snapshot!=NULL) FreePool(Snapshot);
    if (Release) {
      Status=RamRelease();StockSay("[USBRAM b327] release -> %r\r\n",Status);
      if (EFI_ERROR(Status)) return Status;
    } else (VOID)RamPublish();
    StockSay("[USBMODE b327] disabled\r\n");(VOID)BootLogSave("usb-disabled");return EFI_SUCCESS;
  }
  if (Mode==USB_MODE_RAMDISK) {
    mRamUsbOwned=TRUE;Snapshot=mRamDisk;Bytes=USB_RAM_DISK_BYTES;
  } else (VOID)RamPublish();
  mDesiredMode=Mode;mFunctions.Disk=Snapshot;mFunctions.DiskBytes=Bytes;
  StockFunctionDescriptors(Mode);
  Status=StockTest(This);
  StockSay("[USBMODE b327] selected mode=%u snapshot=%lu -> %r\r\n",Mode,(UINT64)Bytes,Status);
  if (EFI_ERROR(Status)) {
    StockReleasePump();
    if (mDevice!=NULL && StockCurrentTpl()==TPL_APPLICATION && mDevice->Stop!=NULL) {
      EFI_STATUS StopStatus=mDevice->Stop();
      if(!EFI_ERROR(StopStatus))StopStatus=StockModeReset();
      if (!EFI_ERROR(StopStatus)) { StockFunctionFree();mAttempted=FALSE;mDevice=NULL; }
      else mStopFailed=TRUE;
    } else if (mDevice==NULL) {
      if (mFunctions.Disk!=NULL && mFunctions.Disk!=mRamDisk) FreePool((VOID *)mFunctions.Disk);
      ZeroMem(&mFunctions,sizeof(mFunctions));mAttempted=FALSE;
    } else mStopFailed=TRUE;
    mActiveMode=USB_MODE_DISABLED;mUsbConfigured=FALSE;
    if (!mStopFailed) { mRamUsbOwned=FALSE;(VOID)RamPublish(); }
  }
  (VOID)BootLogSave("usb-mode-result");
  return Status;
}
STATIC EFI_STATUS EFIAPI StockSelectMode (USB_BRINGUP_PROTOCOL *This, UINT32 Mode)
{
  EFI_STATUS Status;
  if(StockGetSwitchBusy(This)){
    StockSay("[USBMODE] reject overlapping switch -> Already Started\r\n");return EFI_ALREADY_STARTED;
  }
  if(StockCurrentTpl()!=TPL_APPLICATION || mPolling)return EFI_NOT_READY;
  // All mode changes run on the application event loop. Guard re-entry until
  // the inner function has completed every success, timeout or error exit.
  mUsbModeSwitchBusy=TRUE;
  Status=StockSelectModeInternal(This,Mode);
  mUsbModeSwitchBusy=FALSE;
  // Stop resets the native cached cable state. Do not interpret that
  // software transition as another physical unplug on the next poll.
  mCdcRecovery.CableKnown=FALSE;mCdcRecovery.Detached=FALSE;
  StockSay("[USBMODE] switch lock released status=%r stop_failed=%u\r\n",Status,mStopFailed);
  return Status;
}
STATIC EFI_STATUS EFIAPI StockEnableNative (USB_BRINGUP_PROTOCOL *This)
{ return StockSelectMode(This,USB_MODE_NATIVE); }

STATIC EFI_STATUS EFIAPI StockGetSpeed(USB_BRINGUP_PROTOCOL *This,USB_SPEED_INFO *Info)
{
  (VOID)This;if(Info==NULL)return EFI_INVALID_PARAMETER;
  Info->Requested=mRequestedSpeed;Info->Negotiated=mNegotiatedSpeed;
  Info->BulkPacketBytes=mFunctions.PacketSize;
  Info->FallbackArmed=mSpeedFallbackAt!=0;Info->SwitchBusy=StockGetSwitchBusy(This);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI StockSelectSpeed(USB_BRINGUP_PROTOCOL *This,UINT32 Speed)
{
  EFI_STATUS S;UINT32 Mode=mActiveMode,Old=mRequestedSpeed;
  if(Speed!=USB_SPEED_HIGH && Speed!=USB_SPEED_SUPER_EXPERIMENTAL)return EFI_INVALID_PARAMETER;
  if(StockGetSwitchBusy(This))return EFI_ALREADY_STARTED;
  if(StockCurrentTpl()!=TPL_APPLICATION || mPolling)return EFI_NOT_READY;
  if(mRamMutating || mConsoleAttached)return EFI_ACCESS_DENIED;
  if(Mode==USB_MODE_DISABLED)Mode=USB_MODE_CDC;
  mRequestedSpeed=Speed;mNegotiatedSpeed=mNegotiatedPacket=0;
  S=StockSelectMode(This,Mode);
  if(EFI_ERROR(S)){
    mRequestedSpeed=USB_SPEED_HIGH;mSpeedFallbackAt=0;
    // Startup failures may leave no pump. Restore synchronously through the
    // same DMA-safe stop/reset path; never free retained live DMA buffers.
    if(Speed==USB_SPEED_SUPER_EXPERIMENTAL){
      EFI_STATUS Restore=StockSelectMode(This,USB_MODE_CDC);
      StockSay("[USBSPEED] startup failure; HS CDC restore=%r\r\n",Restore);
    }else mRequestedSpeed=Old;
  }else mSpeedFallbackAt=Speed==USB_SPEED_SUPER_EXPERIMENTAL?StockTraceNowMs()+15000:0;
  StockSay("[USBSPEED] requested=%u native-restart=%r fallback_ms=%u\r\n",Speed,S,mSpeedFallbackAt?15000:0);
  return S;
}
