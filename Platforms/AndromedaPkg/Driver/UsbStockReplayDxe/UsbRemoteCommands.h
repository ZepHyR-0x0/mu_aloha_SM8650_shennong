// SPDX-License-Identifier: BSD-2-Clause-Patent
// Request/response framing is byte counted. A screenshot is immutable until
// its reply ends; base64 is generated directly into the next transfer buffer.
STATIC struct {
  UINT8 *Data;
  UINTN Bytes,At,Payload;
  BOOLEAN Base64;
  CHAR8 Header[256],Footer[32],Action[128];
  UINTN HeaderBytes,FooterBytes;
  BOOLEAN Pending,Running,Cancelled,Reboot,AckSubmitted;
} mRemote;
STATIC USB_REMOTE_EXECUTOR mRemoteExecute;
STATIC VOID *mRemoteContext;
STATIC VOID StockCommandReset (VOID)
{
  // A Shell may pump USB while executing. A disconnect cancels its reply,
  // but the buffer remains valid until its application callback returns.
  if (mRemote.Running) { mRemote.Cancelled=TRUE;return; }
  if (mRemote.Data!=NULL) FreePool(mRemote.Data);
  ZeroMem(&mRemote,sizeof(mRemote));
}
STATIC VOID StockCommandText (CONST CHAR8 *Text)
{
  mRemote.HeaderBytes=AsciiSPrint(mRemote.Header,sizeof(mRemote.Header),"%a\r\n",Text);
}
STATIC EFI_STATUS EFIAPI StockSetRemoteExecutor (USB_BRINGUP_PROTOCOL *This, USB_REMOTE_EXECUTOR Execute, VOID *Context)
{
  (VOID)This;
  if (mRemote.Running) return EFI_NOT_READY;
  StockCommandReset();mRemoteExecute=Execute;mRemoteContext=Context;return EFI_SUCCESS;
}
STATIC VOID StockCommandService (BOOLEAN Connected, BOOLEAN TxBusy)
{
  EFI_STATUS Status;UINTN Bytes=0;
  BOOLEAN LastLog,LogSync;
  if (mRemote.Running) return;
  if (!Connected) { StockCommandReset();return; }
  if (!mRemote.Pending && !(mRemote.Reboot && mRemote.AckSubmitted && !TxBusy)) return;
  LastLog=AsciiStrCmp(mRemote.Action,"logcat-last")==0;
  LogSync=AsciiStrCmp(mRemote.Action,"logsync")==0;
  mRemote.Running=TRUE;
  if (LogSync) {
    Status=BootLogSave("cdc-logsync");
  } else if (mRemote.Pending) {
    mRemote.Data=AllocatePool(BOOT_LOG_MAX_BYTES);
    if (LastLog) {
      Bytes=BOOT_LOG_MAX_BYTES;
      Status=mRemote.Data==NULL?EFI_OUT_OF_RESOURCES:BootLogReadStored((CHAR8 *)mRemote.Data,&Bytes,TRUE);
    } else Status=mRemote.Data==NULL?EFI_OUT_OF_RESOURCES:mRemoteExecute==NULL?EFI_NOT_READY:
        mRemoteExecute(mRemoteContext,mRemote.Action,(CHAR8 *)mRemote.Data,BOOT_LOG_MAX_BYTES,&Bytes);
  } else {
    Status=mRemoteExecute==NULL?EFI_NOT_READY:mRemoteExecute(mRemoteContext,mRemote.Action,NULL,0,&Bytes);
  }
  mRemote.Running=FALSE;
  if (mRemote.Cancelled) { StockCommandReset();return; }
  if (LogSync) {
    StockCommandReset();
    if (EFI_ERROR(Status)) mRemote.HeaderBytes=AsciiSPrint(mRemote.Header,sizeof(mRemote.Header),"ERR logsync %r\r\n",Status);
    else StockCommandText("OK logsync");
    return;
  }
  if (mRemote.Reboot) {
    // A successful reset never returns. Report any verification/reset failure.
    StockCommandReset();mRemote.HeaderBytes=AsciiSPrint(mRemote.Header,sizeof(mRemote.Header),"ERR reboot %r\r\n",Status);
    return;
  }
  if (Bytes>BOOT_LOG_MAX_BYTES) { Bytes=0;Status=EFI_BAD_BUFFER_SIZE; }
  if (LastLog && EFI_ERROR(Status)) {
    StockCommandReset();mRemote.HeaderBytes=AsciiSPrint(mRemote.Header,sizeof(mRemote.Header),"ERR logcat-last %r\r\n",Status);return;
  }
  mRemote.Pending=FALSE;mRemote.Bytes=mRemote.Payload=Bytes;
  if (LastLog) {
    mRemote.HeaderBytes=AsciiSPrint(mRemote.Header,sizeof(mRemote.Header),"BEGIN logcat-last %u\r\n",(UINT32)Bytes);
    mRemote.FooterBytes=AsciiSPrint(mRemote.Footer,sizeof(mRemote.Footer),"\r\nEND logcat-last\r\n");return;
  }
  mRemote.HeaderBytes=AsciiSPrint(mRemote.Header,sizeof(mRemote.Header),"BEGIN shell %u status=0x%Lx\r\n",(UINT32)Bytes,(UINT64)Status);
  mRemote.FooterBytes=AsciiSPrint(mRemote.Footer,sizeof(mRemote.Footer),"\r\nEND shell\r\n");
}
STATIC BOOLEAN StockNumber (CONST CHAR8 *Text, UINT32 *Number)
{
  UINT32 V=0,Base=10,D;
  if (Text[0]=='0' && Text[1]=='x') { Base=16;Text+=2; }
  if (*Text==0) return FALSE;
  for (;*Text!=0;Text++) {
    D=*Text>='0' && *Text<='9'?*Text-'0':*Text>='a' && *Text<='f'?*Text-'a'+10:MAX_UINT32;
    if (D>=Base || V>(MAX_UINT32-D)/Base) return FALSE;
    V=V*Base+D;
  }
  *Number=V;return TRUE;
}
STATIC EFI_STATUS StockPress (CONST CHAR8 *Argument)
{
  EFI_GUID Guid=FIRMWARE_INPUT_PROTOCOL_GUID;
  FIRMWARE_INPUT_PROTOCOL *Input=NULL;EFI_KEY_DATA Key;UINT32 Code;
  ZeroMem(&Key,sizeof(Key));
  if (AsciiStrCmp(Argument,"power")==0 || AsciiStrCmp(Argument,"enter")==0) Key.Key.UnicodeChar=L'\r';
  else if (AsciiStrCmp(Argument,"up")==0) Key.Key.ScanCode=SCAN_UP;
  else if (AsciiStrCmp(Argument,"down")==0) Key.Key.ScanCode=SCAN_DOWN;
  else if (AsciiStrCmp(Argument,"left")==0) Key.Key.ScanCode=SCAN_LEFT;
  else if (AsciiStrCmp(Argument,"right")==0) Key.Key.ScanCode=SCAN_RIGHT;
  else if (AsciiStrCmp(Argument,"esc")==0) Key.Key.ScanCode=SCAN_ESC;
  else if (AsciiStrnCmp(Argument,"scan ",5)==0) {
    if (!StockNumber(Argument+5,&Code) || Code==0 || Code>0xffff) return EFI_INVALID_PARAMETER;
    Key.Key.ScanCode=(UINT16)Code;
  } else if (AsciiStrnCmp(Argument,"unicode ",8)==0) {
    if (!StockNumber(Argument+8,&Code) || Code==0 || Code>0xffff || (Code>=0xd800 && Code<=0xdfff)) return EFI_INVALID_PARAMETER;
    Key.Key.UnicodeChar=(CHAR16)Code;
  } else {
    if (!StockNumber(Argument,&Code)) return EFI_INVALID_PARAMETER;
    switch (Code) {
      case 19:case 24:Key.Key.ScanCode=SCAN_UP;break;
      case 20:case 25:Key.Key.ScanCode=SCAN_DOWN;break;
      case 21:Key.Key.ScanCode=SCAN_LEFT;break;
      case 22:Key.Key.ScanCode=SCAN_RIGHT;break;
      case 23:case 26:case 66:Key.Key.UnicodeChar=L'\r';break;
      case 67:Key.Key.UnicodeChar=L'\b';break;
      case 4:case 111:Key.Key.ScanCode=SCAN_ESC;break;
      default:return EFI_UNSUPPORTED;
    }
  }
  EFI_STATUS Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Input);
  return EFI_ERROR(Status)?Status:Input==NULL?EFI_NOT_READY:Input->Inject(Input,&Key);
}
STATIC EFI_STATUS StockScreenCapture (VOID)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;EFI_STATUS Status;
  UINTN Width,Height,Pixels;
  Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if (EFI_ERROR(Status)) return Status;
  if (Gop==NULL || Gop->Mode==NULL || Gop->Mode->Info==NULL) return EFI_NOT_READY;
  Width=Gop->Mode->Info->HorizontalResolution;Height=Gop->Mode->Info->VerticalResolution;
  if (Width==0 || Height==0 || Width>16384 || Height>16384 || Width>32*1024*1024/Height) return EFI_BAD_BUFFER_SIZE;
  Pixels=Width*Height*4;mRemote.Bytes=56+Pixels;
  mRemote.Data=AllocateZeroPool(mRemote.Bytes);if (mRemote.Data==NULL) return EFI_OUT_OF_RESOURCES;
  mRemote.Data[0]='B';mRemote.Data[1]='M';
  UfPutLe32(mRemote.Data+2,(UINT32)mRemote.Bytes);UfPutLe32(mRemote.Data+10,56);
  UfPutLe32(mRemote.Data+14,40);UfPutLe32(mRemote.Data+18,(UINT32)Width);
  UfPutLe32(mRemote.Data+22,0-(UINT32)Height);mRemote.Data[26]=1;mRemote.Data[28]=32;
  UfPutLe32(mRemote.Data+34,(UINT32)Pixels);
  Status=Gop->Blt(Gop,(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(mRemote.Data+56),EfiBltVideoToBltBuffer,
    0,0,0,0,Width,Height,Width*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  if (EFI_ERROR(Status)) return Status;
  mRemote.Base64=TRUE;return EFI_SUCCESS;
}
STATIC VOID StockCommandInput (CONST CHAR8 *Command, UINTN Length)
{
  CHAR8 Line[128];UINTN I,Start=0;EFI_STATUS Status=EFI_SUCCESS;CONST CHAR8 *Name;
  StockCommandReset();
  if (Length>=sizeof(Line)) { StockCommandText("ERR command too long");return; }
  CopyMem(Line,Command,Length);Line[Length]=0;
  while (Line[Start]==' ' || Line[Start]=='\t') Start++;
  while (Length>Start && (Line[Length-1]==' ' || Line[Length-1]=='\t')) Line[--Length]=0;
  Name=Line+Start;
  // Only normalize the command verb: Shell paths and arguments are preserved.
  for (I=Start;Line[I]!=0 && Line[I]!=' ' && Line[I]!='\t';I++)
    if (Line[I]>='A' && Line[I]<='Z') Line[I]+='a'-'A';
  if (AsciiStrCmp(Name,"!long")==0) { StockCommandText("ERR command too long");return; }
  if (AsciiStrCmp(Name,"!busy")==0) { StockCommandText("ERR command queue full");return; }
  if (AsciiStrCmp(Name,"help")==0) { StockCommandText("OK logcat | logcat-last | logsync | scrcat | press <key> | press scan <n> | press unicode <n> | reboot [system|bootloader|recovery] | shell <command>");return; }
  if (AsciiStrCmp(Name,"logcat-last")==0 || AsciiStrCmp(Name,"logsync")==0) {
    CopyMem(mRemote.Action,Name,AsciiStrLen(Name)+1);mRemote.Pending=TRUE;return;
  }
  if (AsciiStrnCmp(Name,"shell ",6)==0 && Name[6]!=0) {
    if (mRemoteExecute==NULL) { StockCommandText("ERR application executor unavailable");return; }
    CopyMem(mRemote.Action,Name,AsciiStrLen(Name)+1);mRemote.Pending=TRUE;return;
  }
  if (AsciiStrCmp(Name,"reboot")==0 || AsciiStrnCmp(Name,"reboot ",7)==0) {
    CONST CHAR8 *Target=Name[6]==0?"system":Name+7;
    for (I=Start+6;Line[I]!=0;I++) if (Line[I]>='A' && Line[I]<='Z') Line[I]+='a'-'A';
    if (AsciiStrCmp(Target,"system")!=0 && AsciiStrCmp(Target,"bootloader")!=0 && AsciiStrCmp(Target,"recovery")!=0) {
      StockCommandText("ERR reboot target: system|bootloader|recovery");return;
    }
    if (mRemoteExecute==NULL) { StockCommandText("ERR application executor unavailable");return; }
    AsciiSPrint(mRemote.Action,sizeof(mRemote.Action),"reboot %a",Target);mRemote.Reboot=TRUE;
    mRemote.HeaderBytes=AsciiSPrint(mRemote.Header,sizeof(mRemote.Header),"OK reboot %a\r\n",Target);return;
  }
  if (AsciiStrnCmp(Name,"press ",6)==0) {
    for (I=Start+6;Line[I]!=0;I++) if (Line[I]>='A' && Line[I]<='Z') Line[I]+='a'-'A';
    Status=StockPress(Name+6);
    if (EFI_ERROR(Status)) mRemote.HeaderBytes=AsciiSPrint(mRemote.Header,sizeof(mRemote.Header),"ERR press %r\r\n",Status);
    else StockCommandText("OK press");
    return;
  }
  if (AsciiStrCmp(Name,"logcat")==0 || AsciiStrCmp(Name,"log")==0) {
    Name="logcat";mRemote.Data=AllocatePool(BOOT_LOG_MAX_BYTES);
    Status=mRemote.Data==NULL?EFI_OUT_OF_RESOURCES:BootLogRead((CHAR8 *)mRemote.Data,BOOT_LOG_MAX_BYTES,&mRemote.Bytes);
  } else if (AsciiStrCmp(Name,"scrcat")==0) { Name="scrcat";Status=StockScreenCapture(); }
  else { StockCommandText("ERR unknown command; type help");return; }
  if (EFI_ERROR(Status)) {
    StockCommandReset();mRemote.HeaderBytes=AsciiSPrint(mRemote.Header,sizeof(mRemote.Header),"ERR %a %r\r\n",Name,Status);return;
  }
  mRemote.Payload=mRemote.Base64?((mRemote.Bytes+2)/3)*4:mRemote.Bytes;
  mRemote.HeaderBytes=AsciiSPrint(mRemote.Header,sizeof(mRemote.Header),"BEGIN %a %u%a\r\n",Name,
    (UINT32)mRemote.Payload,mRemote.Base64?" image/bmp base64":"");
  mRemote.FooterBytes=AsciiSPrint(mRemote.Footer,sizeof(mRemote.Footer),"\r\nEND %a\r\n",Name);
}
STATIC EFI_STATUS StockCommandRead (CHAR8 *Data, UINTN Capacity, UINTN *Bytes)
{
  STATIC CONST CHAR8 Alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  UINTN Total=mRemote.HeaderBytes+mRemote.Payload+mRemote.FooterBytes,I,At,Raw;
  UINT32 Value;
  if (Data==NULL || Bytes==NULL) return EFI_INVALID_PARAMETER;
  *Bytes=0;if (mRemote.Pending || mRemote.Running) return EFI_NOT_READY;
  *Bytes=MIN(Capacity,Total-mRemote.At);
  for (I=0;I<*Bytes;I++) {
    At=mRemote.At+I;
    if (At<mRemote.HeaderBytes) Data[I]=mRemote.Header[At];
    else if ((At-=mRemote.HeaderBytes)<mRemote.Payload) {
      if (!mRemote.Base64) Data[I]=(CHAR8)mRemote.Data[At];
      else {
        Raw=(At/4)*3;
        Value=(UINT32)mRemote.Data[Raw]<<16;
        if (Raw+1<mRemote.Bytes) Value|=(UINT32)mRemote.Data[Raw+1]<<8;
        if (Raw+2<mRemote.Bytes) Value|=mRemote.Data[Raw+2];
        Data[I]=((At%4==2 && Raw+1>=mRemote.Bytes) || (At%4==3 && Raw+2>=mRemote.Bytes))?'=':Alphabet[(Value>>(18-6*(At%4)))&63];
      }
    } else Data[I]=mRemote.Footer[At-mRemote.Payload];
  }
  mRemote.At+=*Bytes;
  if (Total!=0 && mRemote.At==Total) {
    if (mRemote.Reboot) {
      mRemote.HeaderBytes=mRemote.FooterBytes=mRemote.Payload=mRemote.At=0;
      mRemote.AckSubmitted=TRUE;
    } else StockCommandReset();
  }
  return EFI_SUCCESS;
}
