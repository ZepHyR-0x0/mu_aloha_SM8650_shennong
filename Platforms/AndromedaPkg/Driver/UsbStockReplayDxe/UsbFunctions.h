// SPDX-License-Identifier: BSD-2-Clause-Patent
// Asynchronous MSC BOT / CDC ACM clients of the stock UsbDevice protocol.
// logfs is an immutable snapshot. Only the separately allocated RAM disk has a
// writable pointer; this client has no UFS writer, erase or firmware capability.
#define USB_FUNCTION_BUFFER_BYTES 65536U
typedef EFI_STATUS (*USB_FUNCTION_SEND)(UINT8, UINTN, VOID *);
typedef EFI_STATUS (*USB_FUNCTION_STALL)(UINT8, BOOLEAN);
typedef EFI_STATUS (*USB_FUNCTION_ABORT)(UINT8);
typedef EFI_STATUS (*USB_FUNCTION_LOG)(CHAR8 *, UINTN, UINTN *);
typedef VOID (*USB_FUNCTION_INPUT)(CONST UINT8 *, UINTN);
typedef VOID (*USB_FUNCTION_COMMAND)(CONST CHAR8 *, UINTN);
typedef VOID (*USB_FUNCTION_RESET)(VOID);
typedef BOOLEAN (*USB_FUNCTION_BLOB)(CONST UINT8 *, UINTN);
typedef UINT32 (*USB_FUNCTION_BLOB_RECEIVE)(VOID);
typedef BOOLEAN (*USB_FUNCTION_DESCRIPTOR)(UINT16, UINT16, CONST UINT8 **, UINTN *);
typedef struct {
  UINT32 Mode;
  BOOLEAN Connected, Failed, RxBusy, TxBusy, NotifyBusy, NotifyPending, Quiescing;
  UINT8 *Rx, *Tx, *Control, *Notify;
  CONST UINT8 *Disk;
  UINT8 *WritableDisk, *WriteData;
  UINTN DiskBytes;
  CHAR8 *Log;
  UINTN LogCapacity, LogBytes, LogAt;
  USB_FUNCTION_SEND Send;
  USB_FUNCTION_STALL Stall;
  USB_FUNCTION_ABORT Abort;
  USB_FUNCTION_LOG ReadLog;
  USB_FUNCTION_LOG ReadConsole;
  USB_FUNCTION_INPUT ConsoleInput;
  USB_FUNCTION_COMMAND CommandInput;
  USB_FUNCTION_LOG ReadReply;
  USB_FUNCTION_RESET ResetReply;
  USB_FUNCTION_BLOB BlobInput;
  USB_FUNCTION_BLOB_RECEIVE BlobReceiveBytes;
  USB_FUNCTION_DESCRIPTOR Descriptor;
  BOOLEAN Console, CdcTxConsole, CdcZlpPending;
  UINT32 State, ControlState, Tag, Residue, HostLength;
  UINT32 DataBytes, DataAt, Queued, RxQueued, Drain;
  UINT32 SectorBytes, ControlQueued;
  UINT8 ControlEp, ResetClears, ClearAfterStatus;
  UINT8 Status, SenseKey, SenseAsc, SenseAscq;
  BOOLEAN NeedReset, SendZlp, LogPending, HaltIn, HaltOut, WriteExcess;
  CONST UINT8 *ReadData;
  UINT8 Small[96], LineCoding[7];
  UINT16 LineState, PacketSize;
  CHAR8 Command[128], Commands[8][128];
  UINTN CommandRead, CommandWrite, CommandCount;
  BOOLEAN CommandOverflow, CommandBusy, ReplyActive;
  UINTN CommandBytes;
} USB_FUNCTIONS;
enum { UfIdle, UfAwaitCbw, UfDataIn, UfDataOut, UfZlp, UfSendingCsw, UfClearIn, UfRecovery };
enum { UfControlIdle, UfControlRead, UfControlWrite, UfControlStatusIn, UfControlStatusOut };
STATIC UINT32 UfBe32 (CONST UINT8 *P) { return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3]; }
STATIC UINT16 UfBe16 (CONST UINT8 *P) { return (UINT16)(((UINT16)P[0]<<8)|P[1]); }
STATIC UINT32 UfLe32 (CONST UINT8 *P) { return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24); }
STATIC VOID UfPutBe32 (UINT8 *P, UINT32 V) { P[0]=(UINT8)(V>>24);P[1]=(UINT8)(V>>16);P[2]=(UINT8)(V>>8);P[3]=(UINT8)V; }
STATIC VOID UfPutLe32 (UINT8 *P, UINT32 V) { P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);P[2]=(UINT8)(V>>16);P[3]=(UINT8)(V>>24); }
STATIC EFI_STATUS UfQueue (USB_FUNCTIONS *F, UINT8 Ep, UINTN Bytes, VOID *Buffer)
{
  EFI_STATUS S = F->Send (Ep, Bytes, Buffer);
  if (EFI_ERROR (S)) F->Failed = TRUE;
  return S;
}
STATIC EFI_STATUS UfStall (USB_FUNCTIONS *F, UINT8 Ep, BOOLEAN Halt)
{
  EFI_STATUS S=F->Stall(Ep,Halt);
  if (EFI_ERROR(S)) F->Failed=TRUE;
  else if (Ep==1) F->HaltOut=Halt;
  else if (Ep==0x81) F->HaltIn=Halt;
  return S;
}
STATIC EFI_STATUS UfRx (USB_FUNCTIONS *F, UINT32 Bytes)
{
  EFI_STATUS S;
  if (F->RxBusy || Bytes==0 || Bytes>USB_FUNCTION_BUFFER_BYTES) return EFI_NOT_READY;
  S=UfQueue (F,0x01,Bytes,F->Rx);
  if (!EFI_ERROR(S)) { F->RxBusy=TRUE;F->RxQueued=Bytes; }
  return S;
}
STATIC EFI_STATUS UfTx (USB_FUNCTIONS *F, UINT32 Bytes)
{
  EFI_STATUS S;
  if (F->TxBusy || Bytes>USB_FUNCTION_BUFFER_BYTES) return EFI_NOT_READY;
  S=UfQueue (F,0x81,Bytes,F->Tx);
  if (!EFI_ERROR(S)) { F->TxBusy=TRUE;F->Queued=Bytes; }
  return S;
}
STATIC EFI_STATUS UfCbw (USB_FUNCTIONS *F) {
  if(F->Quiescing){F->State=UfIdle;return EFI_SUCCESS;}
  if (F->HaltOut || F->HaltIn || F->ResetClears || F->NeedReset) { F->State=UfIdle;return EFI_SUCCESS; }
  F->State=UfAwaitCbw;return UfRx(F,31);
}
STATIC BOOLEAN UfMscQuiescent(CONST USB_FUNCTIONS *F)
{
  return F->ControlState==UfControlIdle && !F->TxBusy &&
    (F->State==UfIdle || F->State==UfAwaitCbw || F->State==UfRecovery);
}
STATIC EFI_STATUS UfCsw (USB_FUNCTIONS *F)
{
  UfPutLe32(F->Tx,0x53425355);UfPutLe32(F->Tx+4,F->Tag);
  UfPutLe32(F->Tx+8,F->Residue);F->Tx[12]=F->Status;F->State=UfSendingCsw;
  return UfTx(F,13);
}
STATIC VOID UfSense (USB_FUNCTIONS *F, UINT8 Key, UINT8 Asc, UINT8 Ascq)
{ F->SenseKey=Key;F->SenseAsc=Asc;F->SenseAscq=Ascq;F->Status=1; }
STATIC EFI_STATUS UfReadNext (USB_FUNCTIONS *F)
{
  UINT32 N;
  if (F->DataAt==F->DataBytes) {
    if (F->SendZlp) { F->SendZlp=FALSE;F->State=UfZlp;return UfTx(F,0); }
    return UfCsw(F);
  }
  N=MIN (F->DataBytes-F->DataAt,USB_FUNCTION_BUFFER_BYTES);
  CopyMem(F->Tx,F->ReadData+F->DataAt,N);F->State=UfDataIn;
  return UfTx(F,N);
}
STATIC EFI_STATUS UfScsi (USB_FUNCTIONS *F)
{
  CONST UINT8 *C=F->Rx+15;
  UINT8 Op=C[0], Length=F->Rx[14];
  UINT32 N=0, Allocation=MAX_UINT32, Blocks=0, MinLength=6;
  UINT64 Lba=0;
  BOOLEAN In=TRUE;
  F->Tag=UfLe32(F->Rx+4);F->HostLength=UfLe32(F->Rx+8);F->Residue=F->HostLength;
  F->DataAt=0;F->Status=0;F->NeedReset=FALSE;F->ReadData=F->Small;
  F->SendZlp=FALSE;F->WriteData=NULL;F->WriteExcess=FALSE;ZeroMem(F->Small,sizeof(F->Small));
  switch(Op) {
    case 0x00: case 0x1B: case 0x1E: case 0x35: case 0x2F:
      In=FALSE;MinLength=(Op==0x35 || Op==0x2F)?10:6;break;
    case 0x03:
      N=18;Allocation=C[4];F->Small[0]=0x70;F->Small[2]=F->SenseKey;F->Small[7]=10;
      F->Small[12]=F->SenseAsc;F->Small[13]=F->SenseAscq;
      F->SenseKey=F->SenseAsc=F->SenseAscq=0;break;
    case 0x12:
      Allocation=C[4];
      if ((C[1]&~1U)!=0) { UfSense(F,5,0x24,0);break; }
      if ((C[1]&1)!=0) {
        F->Small[1]=C[2];
        if (C[2]==0) { F->Small[3]=2;F->Small[4]=0;F->Small[5]=0x80;N=6; }
        else if (C[2]==0x80) { F->Small[3]=8;CopyMem(F->Small+4,F->WritableDisk?"MU-RAM  ":"MU-LOGFS",8);N=12; }
        else UfSense(F,5,0x24,0);
      } else if (C[2]!=0) UfSense(F,5,0x24,0);
      else {
        N=36;F->Small[1]=0x80;F->Small[2]=4;F->Small[3]=2;F->Small[4]=31;
        CopyMem(F->Small+8,"ProjectM",8);CopyMem(F->Small+16,F->WritableDisk?"RAM DISK        ":"LOGFS READ ONLY ",16);CopyMem(F->Small+32,"0327",4);
      }
      break;
    case 0x25:
      MinLength=10;N=8;UfPutBe32(F->Small,(UINT32)(F->DiskBytes/F->SectorBytes-1));UfPutBe32(F->Small+4,F->SectorBytes);break;
    case 0x23:
      MinLength=10;Allocation=UfBe16(C+7);N=12;F->Small[3]=8;
      UfPutBe32(F->Small+4,(UINT32)(F->DiskBytes/F->SectorBytes));F->Small[8]=2;
      F->Small[9]=(UINT8)(F->SectorBytes>>16);F->Small[10]=(UINT8)(F->SectorBytes>>8);F->Small[11]=(UINT8)F->SectorBytes;break;
    case 0x9E:
      MinLength=16;Allocation=UfBe32(C+10);
      if ((C[1]&31)!=0x10) { UfSense(F,5,0x20,0);break; }
      N=32;UfPutBe32(F->Small+4,(UINT32)(F->DiskBytes/F->SectorBytes-1));UfPutBe32(F->Small+8,F->SectorBytes);break;
    case 0x1A: case 0x5A: {
      UINT8 Page=C[2]&0x3F, Pc=C[2]>>6;
      MinLength=Op==0x5A?10:6;Allocation=Op==0x5A?UfBe16(C+7):C[4];
      if ((C[1]&~8U)!=0 || C[3]!=0 || (Page!=0x3F && Page!=8 && Page!=0x1C && Page!=0)) { UfSense(F,5,0x24,0);break; }
      // PC=1 asks for changeable fields, not saved values. All parameters are
      // fixed; PC=2 defaults equal current. PC=3 requires persistence we lack.
      if (Pc==3) { UfSense(F,5,0x39,0);break; }
      N=Op==0x5A?8:4;F->Small[Op==0x5A?3:2]=F->WritableDisk?0:0x80;
      // Writes finish in RAM before CSW; WCE stays zero. No MODE SELECT or
      // persistent setting is offered. Exception reporting is fixed/disabled.
      if (Page==8 || Page==0x3F) { F->Small[N]=8;F->Small[N+1]=18;N+=20; }
      if (Page==0x1C || Page==0x3F) {
        F->Small[N]=0x1C;F->Small[N+1]=10;if(Pc!=1)F->Small[N+2]=8;N+=12;
      }
      if (Op==0x5A) { F->Small[1]=(UINT8)(N-2); } else F->Small[0]=(UINT8)(N-1);
      break;
    }
    case 0x08: case 0x28: case 0xA8: case 0x88:
      if (Op==0x08) { Lba=((UINT32)(C[1]&31)<<16)|((UINT32)C[2]<<8)|C[3];Blocks=C[4]?C[4]:256; }
      else if (Op==0x88) { MinLength=16;Lba=((UINT64)UfBe32(C+2)<<32)|UfBe32(C+6);Blocks=UfBe32(C+10); }
      else { MinLength=Op==0xA8?12:10;Lba=UfBe32(C+2);Blocks=Op==0xA8?UfBe32(C+6):UfBe16(C+7); }
      if (Lba>F->DiskBytes/F->SectorBytes || Blocks>F->DiskBytes/F->SectorBytes-Lba || Blocks>MAX_UINT32/F->SectorBytes) { UfSense(F,5,0x21,0);break; }
      N=Blocks*F->SectorBytes;F->ReadData=F->Disk+(UINTN)Lba*F->SectorBytes;break;
    case 0x0A: case 0x2A: case 0xAA: case 0x8A:
      MinLength=Op==0x0A?6:Op==0xAA?12:Op==0x8A?16:10;In=FALSE;
      if (F->WritableDisk==NULL || F->WritableDisk!=F->Disk || F->Mode!=USB_MODE_RAMDISK) { UfSense(F,7,0x27,0);break; }
      if (Op==0x0A) { Lba=((UINT32)(C[1]&31)<<16)|((UINT32)C[2]<<8)|C[3];Blocks=C[4]?C[4]:256; }
      else if (Op==0x8A) { Lba=((UINT64)UfBe32(C+2)<<32)|UfBe32(C+6);Blocks=UfBe32(C+10); }
      else { Lba=UfBe32(C+2);Blocks=Op==0xAA?UfBe32(C+6):UfBe16(C+7); }
      if (Lba>F->DiskBytes/F->SectorBytes || Blocks>F->DiskBytes/F->SectorBytes-Lba || Blocks>MAX_UINT32/F->SectorBytes) {
        UfSense(F,5,0x21,0);break;
      }
      N=Blocks*F->SectorBytes;F->WriteData=F->WritableDisk+(UINTN)Lba*F->SectorBytes;break;
    case 0x04: case 0x15: case 0x55: case 0x42:
      MinLength=(Op==0x55 || Op==0x42)?10:6;In=FALSE;UfSense(F,F->WritableDisk?5:7,F->WritableDisk?0x20:0x27,0);break;
    case 0xA0:
      MinLength=12;Allocation=UfBe32(C+6);N=16;F->Small[3]=8;break;
    default: UfSense(F,5,0x20,0);In=FALSE;break;
  }
  if (Length<MinLength) { UfSense(F,5,0x24,0);N=0; }
  if (F->Status!=0) N=0;
  N=MIN(N,Allocation);
  // A rejected logfs write has no device data phase (Dn). BOT case 9 halts OUT
  // and returns failed CSW with full residue, without accepting any payload.
  if (F->HostLength==0) {
    if (N!=0) { F->Status=2;F->NeedReset=TRUE; }
    return UfCsw(F);
  }
  if ((F->Rx[12]&0x80)==0) {
    if (N!=0 && !In && F->WriteData!=NULL && F->Status==0) {
      // Accept only whole, bounded RAM writes. A too-short host data phase
      // never starts a RAM update and requires BOT reset recovery.
      if (F->HostLength<N) { F->Status=2;F->NeedReset=TRUE;(VOID)UfStall(F,1,TRUE);return UfCsw(F); }
      F->DataBytes=N;F->Drain=N;F->WriteExcess=F->HostLength>N;F->State=UfDataOut;
      return UfRx(F,MIN(N,USB_FUNCTION_BUFFER_BYTES));
    }
    if (N!=0 && In) { F->Status=2;F->NeedReset=TRUE; }
    (VOID)UfStall(F,0x01,TRUE);return UfCsw(F);
  }
  if (!In || N==0) {
    if (!In && N!=0) { F->Status=2;F->NeedReset=TRUE; }
    (VOID)UfStall(F,0x81,TRUE);F->State=UfClearIn;return EFI_SUCCESS;
  }
  if (F->HostLength<N) { F->Status=2;F->NeedReset=TRUE; }
  F->DataBytes=MIN(N,F->HostLength);
  F->SendZlp=(BOOLEAN)(F->DataBytes<F->HostLength && F->DataBytes%F->PacketSize==0);
  return UfReadNext(F);
}
STATIC EFI_STATUS UfCdcNext (USB_FUNCTIONS *F)
{
  UINTN N;
  EFI_STATUS S;
  if (F->Failed || !F->Connected || (F->LineState&1)==0 || F->TxBusy) return EFI_SUCCESS;
  // Complete a host read even when this reply ends on a full USB packet.
  // A ZLP belongs to the preceding transfer, before console/reply interleaving.
  if (F->CdcZlpPending) { F->CdcZlpPending=FALSE;F->CdcTxConsole=TRUE;return UfTx(F,0); }
  if (F->Console && F->ReadConsole!=NULL) {
    N=0;S=F->ReadConsole((CHAR8 *)F->Tx,USB_FUNCTION_BUFFER_BYTES,&N);
    if (EFI_ERROR(S) || N>USB_FUNCTION_BUFFER_BYTES) return EFI_ERROR(S)?S:EFI_BAD_BUFFER_SIZE;
    if (N==0) return EFI_SUCCESS;
    F->CdcTxConsole=TRUE;return UfTx(F,(UINT32)N);
  }
  if (F->CommandInput!=NULL && F->ReadReply!=NULL) {
    if (!F->ReplyActive && (F->CommandCount!=0 || F->CommandBusy)) {
      if (F->CommandCount!=0) {
        CHAR8 *Command=F->Commands[F->CommandRead];
        UINTN Length=0;while (Command[Length]!=0) Length++;
        F->CommandInput(Command,Length);
        F->CommandRead=(F->CommandRead+1)%ARRAY_SIZE(F->Commands);F->CommandCount--;
      } else { F->CommandInput("!busy",5);F->CommandBusy=FALSE; }
      F->ReplyActive=TRUE;
    }
    if (!F->ReplyActive) return EFI_SUCCESS;
    N=0;S=F->ReadReply((CHAR8 *)F->Tx,USB_FUNCTION_BUFFER_BYTES,&N);
    if (EFI_ERROR(S) || N>USB_FUNCTION_BUFFER_BYTES) return EFI_ERROR(S)?S:EFI_BAD_BUFFER_SIZE;
    if (N==0) { F->ReplyActive=FALSE;return EFI_SUCCESS; }
    F->CdcTxConsole=TRUE;return UfTx(F,(UINT32)N);
  }
  if (F->LogPending) {
    F->LogPending=FALSE;F->LogAt=F->LogBytes=0;
    S=F->ReadLog(F->Log,F->LogCapacity,&F->LogBytes);
    if (EFI_ERROR(S) || F->LogBytes>F->LogCapacity) { F->LogBytes=0;return EFI_ERROR(S)?S:EFI_BAD_BUFFER_SIZE; }
  }
  if (F->LogAt==F->LogBytes) return EFI_SUCCESS;
  N=MIN(F->LogBytes-F->LogAt,USB_FUNCTION_BUFFER_BYTES);CopyMem(F->Tx,F->Log+F->LogAt,N);
  F->CdcTxConsole=FALSE;
  return UfTx(F,(UINT32)N);
}
STATIC EFI_STATUS UfControl (USB_FUNCTIONS *F, UINT8 Ep, UINT32 Bytes, UINT32 State)
{
  F->ControlState=State;F->ControlEp=Ep;F->ControlQueued=Bytes;
  return UfQueue(F,Ep,Bytes,F->Control);
}
STATIC EFI_STATUS UfCdcNotify (USB_FUNCTIONS *F)
{
  STATIC CONST UINT8 Header[8]={0xA1,0x20,0,0,0,0,2,0};
  EFI_STATUS S;
  if (!F->Connected || !F->NotifyPending || F->NotifyBusy) return EFI_SUCCESS;
  F->NotifyPending=FALSE;
  CopyMem(F->Notify,Header,8);F->Notify[8]=(F->LineState&1)!=0?3:0;F->Notify[9]=0;
  S=F->Send(0x82,10,F->Notify);
  if (!EFI_ERROR(S)) F->NotifyBusy=TRUE;
  // Interrupt notifications are advisory. A host that cancels or does not
  // poll them must not lose the independent EP0 and bulk command paths.
  if (S==EFI_SECURITY_VIOLATION) { F->Failed=TRUE;return S; }
  return EFI_SUCCESS;
}
STATIC VOID UfClearHalt (USB_FUNCTIONS *F, UINT8 Ep);
STATIC EFI_STATUS UfSetup (USB_FUNCTIONS *F, CONST EFI_USB_DEVICE_REQUEST *R)
{
  EFI_STATUS S;
  if (F->ControlState!=UfControlIdle) {
    S=F->Abort(0);if (EFI_ERROR(S)) return S;
    S=F->Abort(0x80);if (EFI_ERROR(S)) return S;
    F->ControlState=UfControlIdle;
  }
  F->ClearAfterStatus=0;
  // The native layer can delegate a repeated descriptor request after
  // configuration. Reply from the same active descriptor set used by StartEx.
  if(R->RequestType==0x80 && R->Request==6 && R->Length!=0 && F->Descriptor!=NULL){
    CONST UINT8 *Data=NULL;UINTN Bytes=0;
    if(F->Descriptor(R->Value,R->Index,&Data,&Bytes) && Data!=NULL && Bytes<=512){
      Bytes=MIN(Bytes,R->Length);CopyMem(F->Control,Data,Bytes);
      return UfControl(F,0x80,(UINT32)Bytes,UfControlRead);
    }
  }
  // The native device layer delegates endpoint CLEAR_FEATURE as an OEM
  // request. ACK it on EP0 and resume BOT only after that status completes.
  if ((F->Mode==USB_MODE_STORAGE || F->Mode==USB_MODE_RAMDISK) &&
      R->RequestType==2 && R->Request==1 && R->Value==0 && R->Length==0 &&
      (R->Index==1 || R->Index==0x81)) {
    S=UfStall(F,(UINT8)R->Index,FALSE);if(EFI_ERROR(S))return S;
    F->ClearAfterStatus=(UINT8)R->Index;return UfControl(F,0x80,0,UfControlStatusIn);
  }
  if (R->Index!=0) goto Stall;
  if (F->Mode==USB_MODE_STORAGE || F->Mode==USB_MODE_RAMDISK) {
    if (R->RequestType==0xA1 && R->Request==0xFE && R->Value==0 && R->Length==1) {
      F->Control[0]=0;return UfControl(F,0x80,1,UfControlRead);
    }
    if (R->RequestType==0x21 && R->Request==0xFF && R->Value==0 && R->Length==0) {
      // BOT reset cancels commands; endpoint halts remain until CLEAR_FEATURE.
      S=F->Abort(0x01);if (EFI_ERROR(S)) return S;
      S=F->Abort(0x81);if (EFI_ERROR(S)) return S;
      F->ResetClears=(F->NeedReset || F->HaltOut || F->HaltIn)?3:0;
      F->RxBusy=F->TxBusy=FALSE;F->Failed=FALSE;F->NeedReset=FALSE;
      F->State=UfIdle;return UfControl(F,0x80,0,UfControlStatusIn);
    }
  } else if (F->Mode==USB_MODE_CDC) {
    if (R->RequestType==0x21 && R->Request==0x20 && R->Value==0 && R->Length==7)
      return UfControl(F,0,7,UfControlWrite);
    if (R->RequestType==0xA1 && R->Request==0x21 && R->Value==0 && R->Length==7) {
      CopyMem(F->Control,F->LineCoding,7);return UfControl(F,0x80,7,UfControlRead);
    }
    if (R->RequestType==0x21 && R->Request==0x22 && (R->Value&~3U)==0 && R->Length==0) {
      if (F->LineState!=R->Value) F->NotifyPending=TRUE;
      F->LineState=R->Value;if ((R->Value&1)!=0 && !F->Console && F->CommandInput==NULL) F->LogPending=TRUE;
      if ((R->Value&1)==0) {
        F->CommandBytes=F->CommandRead=F->CommandWrite=F->CommandCount=0;
        F->CommandOverflow=F->CommandBusy=F->ReplyActive=FALSE;
        if (F->ResetReply!=NULL) F->ResetReply();
      }
      return UfControl(F,0x80,0,UfControlStatusIn);
    }
    if (R->RequestType==0x21 && R->Request==0x23 && R->Length==0)
      return UfControl(F,0x80,0,UfControlStatusIn);
  }
Stall:
  F->Stall((R->RequestType&0x80)!=0?0x80:0,TRUE);return EFI_SUCCESS;
}
STATIC VOID UfClearHalt (USB_FUNCTIONS *F, UINT8 Ep)
{
  if ((F->Mode!=USB_MODE_STORAGE && F->Mode!=USB_MODE_RAMDISK) || !F->Connected) return;
  if (Ep==1) F->HaltOut=FALSE;
  if (Ep==0x81) F->HaltIn=FALSE;
  if (Ep==1) F->ResetClears&=~1U;
  if (Ep==0x81) F->ResetClears&=~2U;
  if (Ep==0x81 && F->State==UfClearIn && !F->TxBusy) (VOID)UfCsw(F);
  else if (F->State==UfIdle && !F->RxBusy && !F->NeedReset) (VOID)UfCbw(F);
}
STATIC EFI_STATUS UfComplete (USB_FUNCTIONS *F, CONST USB_DEVICE_TRANSFER_OUTCOME *O)
{
  EFI_STATUS S;
  UINTN I;
  if (O->Status==UsbDeviceTransferStatusActive) return EFI_SUCCESS;
  // A reset/stop may have already invalidated old work. Ignore only outcomes
  // for buffers that are no longer busy; cancellation of current work fails
  // closed and must be recovered by BOT reset or reconnect.
  if (O->EndpointIndex==0 || O->EndpointIndex==0x80) {
    if (F->ControlState==UfControlIdle || O->DataBuffer!=F->Control || O->EndpointIndex!=F->ControlEp) return EFI_SUCCESS;
    if (O->Status!=UsbDeviceTransferStatusCompleteOK || O->BytesCompleted!=F->ControlQueued) {
      F->ControlState=UfControlIdle;F->Failed=TRUE;return EFI_DEVICE_ERROR;
    }
    if (F->ControlState==UfControlRead && O->EndpointIndex==0x80)
      return UfControl(F,0,0,UfControlStatusOut);
    if (F->ControlState==UfControlWrite && O->EndpointIndex==0) {
      if (O->BytesCompleted!=7 || UfLe32(F->Control)==0 || F->Control[4]>2 || F->Control[5]>4 || F->Control[6]<5 || F->Control[6]>8) {
        F->ControlState=UfControlIdle;F->Stall(0x80,TRUE);return EFI_SUCCESS;
      }
      CopyMem(F->LineCoding,F->Control,7);return UfControl(F,0x80,0,UfControlStatusIn);
    }
    if ((F->ControlState==UfControlStatusIn && O->EndpointIndex==0x80) ||
        (F->ControlState==UfControlStatusOut && O->EndpointIndex==0)) {
      F->ControlState=UfControlIdle;
      if(F->ClearAfterStatus!=0){UINT8 Ep=F->ClearAfterStatus;F->ClearAfterStatus=0;UfClearHalt(F,Ep);}
      if (F->Mode==USB_MODE_CDC) { S=UfCdcNotify(F);if (EFI_ERROR(S)) return S; }
      if ((F->Mode==USB_MODE_STORAGE || F->Mode==USB_MODE_RAMDISK) && F->State==UfIdle && F->Connected && !F->RxBusy) return UfCbw(F);
      if (F->Mode==USB_MODE_CDC) return UfCdcNext(F);
    }
    return EFI_SUCCESS;
  }
  if (O->EndpointIndex==0x82 && O->DataBuffer==F->Notify && F->NotifyBusy) {
    F->NotifyBusy=FALSE;
    return UfCdcNotify(F);
  }
  if (O->EndpointIndex==0x01 && O->DataBuffer==F->Rx && F->RxBusy) {
    F->RxBusy=FALSE;
    if (O->Status!=UsbDeviceTransferStatusCompleteOK || O->BytesCompleted>F->RxQueued) { F->Failed=TRUE;return EFI_DEVICE_ERROR; }
    if (F->Mode==USB_MODE_CDC) {
      // Consume only completed RX storage. No abort, buffer replacement, or
      // changed DMA ownership is needed when entering/leaving raw upload mode.
      if ((F->LineState&1)!=0 && F->BlobInput!=NULL && F->BlobInput(F->Rx,O->BytesCompleted)) {
        S=UfRx(F,F->BlobReceiveBytes!=NULL?F->BlobReceiveBytes():1024);return EFI_ERROR(S)?S:UfCdcNext(F);
      }
      if (F->Console && F->ConsoleInput!=NULL) {
        if ((F->LineState&1)!=0) F->ConsoleInput(F->Rx,O->BytesCompleted);
        S=UfRx(F,1024);return EFI_ERROR(S)?S:UfCdcNext(F);
      }
      for (I=0;I<O->BytesCompleted;I++) {
        CHAR8 C=(CHAR8)F->Rx[I];
        if ((F->LineState&1)==0) continue;
        if (C=='\r' || C=='\n') {
          if (F->CommandInput!=NULL && (F->CommandBytes!=0 || F->CommandOverflow)) {
            if (F->CommandCount<ARRAY_SIZE(F->Commands)) {
              F->Command[F->CommandBytes]=0;
              if (F->CommandOverflow) CopyMem(F->Commands[F->CommandWrite],"!long",6);
              else CopyMem(F->Commands[F->CommandWrite],F->Command,F->CommandBytes+1);
              F->CommandWrite=(F->CommandWrite+1)%ARRAY_SIZE(F->Commands);F->CommandCount++;
            } else F->CommandBusy=TRUE;
          } else if (F->CommandBytes==3 && CompareMem(F->Command,"LOG",3)==0) F->LogPending=TRUE;
          F->CommandBytes=0;F->CommandOverflow=FALSE;
        } else if (C=='\b' || C==127) {
          if (!F->CommandOverflow && F->CommandBytes!=0) F->CommandBytes--;
        } else if (F->CommandBytes<sizeof(F->Command)-1) F->Command[F->CommandBytes++]=C;
        else F->CommandOverflow=TRUE;
      }
      S=UfRx(F,1024);return EFI_ERROR(S)?S:UfCdcNext(F);
    }
    if (F->State==UfAwaitCbw) {
      if (O->BytesCompleted!=31 || UfLe32(F->Rx)!=0x43425355 || F->Rx[13]!=0 ||
          (F->Rx[12]&0x7F)!=0 || F->Rx[14]==0 || F->Rx[14]>16) {
        F->NeedReset=TRUE;F->State=UfRecovery;(VOID)UfStall(F,1,TRUE);(VOID)UfStall(F,0x81,TRUE);return EFI_SUCCESS;
      }
      return UfScsi(F);
    }
    if (F->State==UfDataOut) {
      if (O->BytesCompleted==0 || O->BytesCompleted!=F->RxQueued || O->BytesCompleted>F->Drain) {
        F->Status=2;F->NeedReset=TRUE;(VOID)UfStall(F,1,TRUE);return UfCsw(F);
      }
      if (F->Mode!=USB_MODE_RAMDISK || F->WritableDisk==NULL || F->WritableDisk!=F->Disk ||
          F->WriteData==NULL || F->DataAt>F->DataBytes || O->BytesCompleted>F->DataBytes-F->DataAt) {
        F->Failed=TRUE;return EFI_SECURITY_VIOLATION;
      }
      CopyMem(F->WriteData+F->DataAt,F->Rx,O->BytesCompleted);
      F->DataAt+=(UINT32)O->BytesCompleted;F->Residue-=(UINT32)O->BytesCompleted;
      F->Drain-=(UINT32)O->BytesCompleted;
      if (F->Drain==0 && F->WriteExcess) (VOID)UfStall(F,1,TRUE);
      return F->Drain?UfRx(F,MIN(F->Drain,USB_FUNCTION_BUFFER_BYTES)):UfCsw(F);
    }
  }
  if (O->EndpointIndex==0x81 && O->DataBuffer==F->Tx && F->TxBusy) {
    F->TxBusy=FALSE;
    if (O->Status!=UsbDeviceTransferStatusCompleteOK || O->BytesCompleted!=F->Queued) { F->Failed=TRUE;return EFI_DEVICE_ERROR; }
    if (F->Mode==USB_MODE_CDC) {
      if (!F->CdcTxConsole) F->LogAt+=O->BytesCompleted;
      if (F->Queued!=0 && F->PacketSize!=0 && F->Queued%F->PacketSize==0) F->CdcZlpPending=TRUE;
      return UfCdcNext(F);
    }
    if (F->State==UfDataIn) { F->DataAt+=(UINT32)O->BytesCompleted;F->Residue-=(UINT32)O->BytesCompleted;return UfReadNext(F); }
    if (F->State==UfZlp) return UfCsw(F);
    if (F->State==UfSendingCsw) {
      if (F->NeedReset) { F->State=UfRecovery;return EFI_SUCCESS; }
      return UfCbw(F);
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS UfConnected (USB_FUNCTIONS *F, BOOLEAN Connected)
{
  if (Connected && F->Connected) return EFI_SUCCESS;
  F->Connected=Connected;F->RxBusy=F->TxBusy=F->NotifyBusy=FALSE;
  F->CdcZlpPending=FALSE;
  F->NotifyPending=FALSE;
  F->ControlState=UfControlIdle;F->State=UfIdle;F->Failed=FALSE;F->NeedReset=FALSE;
  F->HaltIn=F->HaltOut=FALSE;
  F->ResetClears=0;
  F->ClearAfterStatus=0;
  F->CommandBytes=0;F->LogAt=F->LogBytes=0;F->LineState=0;
  F->CommandRead=F->CommandWrite=F->CommandCount=0;
  F->CommandOverflow=F->CommandBusy=F->ReplyActive=FALSE;
  if (F->ResetReply!=NULL) F->ResetReply();
  if (!Connected) return EFI_SUCCESS;
  if (F->Mode==USB_MODE_STORAGE || F->Mode==USB_MODE_RAMDISK) return UfCbw(F);
  if (F->Mode==USB_MODE_CDC) return UfRx(F,1024);
  return EFI_SUCCESS;
}
