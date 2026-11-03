// SPDX-License-Identifier: BSD-2-Clause-Patent
// Windowed CDC stream into the private RAM FAT. RX only copies completed
// bytes; allocation, mount changes and commit run at application TPL.
#include "RamFileCore.h"
#define RAM_UPLOAD_WINDOW 65536U
STATIC RAM_FILE_STORE mRamFile;
STATIC struct {
  BOOLEAN Cancelled, ReplyReady;
  UINT32 WindowEnd;
  UINT64 LastAt;
  EFI_STATUS Error;
  CHAR8 Reply[128];
} mRamUpload;

STATIC EFI_STATUS RamUploadBegin(CONST CHAR8 *Path,UINT32 Bytes,UINT32 Crc)
{
  EFI_STATUS S;
  if(StockCurrentTpl()!=TPL_APPLICATION || mPolling)return EFI_NOT_READY;
  if(mUsbModeSwitchBusy || mUsbModeSwitchQueued)return EFI_NOT_READY;
  if(mRamDisk==NULL)return EFI_NO_MEDIA;
  if(mRamUsbOwned || mRamMutating)return EFI_ACCESS_DENIED;
  S=RfInit(&mRamFile,mRamDisk,mRamBytes);if(EFI_ERROR(S))return S;
  S=RamUnpublish();if(EFI_ERROR(S))return S;
  mRamMutating=TRUE;ZeroMem(&mRamUpload,sizeof(mRamUpload));
  S=RfBegin(&mRamFile,Path,Bytes,Crc);
  if(EFI_ERROR(S)){mRamMutating=FALSE;(VOID)RamPublish();return S;}
  mRamUpload.WindowEnd=MIN(Bytes,RAM_UPLOAD_WINDOW);mRamUpload.LastAt=StockTraceNowMs();
  return EFI_SUCCESS;
}
STATIC VOID RamUploadCancel(VOID)
{
  // Disconnect may arrive inside the native event pump; defer remount/free.
  if(mRamMutating)mRamUpload.Cancelled=TRUE;
}
STATIC UINT32 StockRamReceiveBytes(VOID)
{
  if(!mRamMutating || mRamUpload.Cancelled || EFI_ERROR(mRamUpload.Error) ||
     mRamFile.Received>=mRamUpload.WindowEnd)return 1024;
  UINT64 End=mRamUpload.WindowEnd;
  // The host pads only the final window. Round its total end, not the
  // remaining length, so a partial completion cannot overrun the window.
  if(End==mRamFile.Expected)End=(End+1023U)&~1023ULL;
  return (UINT32)MIN(End-mRamFile.Received,RAM_UPLOAD_WINDOW);
}
STATIC BOOLEAN StockRamInput(CONST UINT8 *Data,UINTN Bytes)
{
  if(!mRamMutating)return FALSE;
  if(mRamUpload.Cancelled || EFI_ERROR(mRamUpload.Error))return TRUE;
  if(Bytes==0)return TRUE;
  UINTN Left=mRamUpload.WindowEnd-mRamFile.Received;
  if(Bytes>Left){
    // usbser does not guarantee an OUT ZLP. The advertised protocol pads the
    // last window to 1024 bytes so the already queued receive can complete.
    if(mRamUpload.WindowEnd!=mRamFile.Expected || Left==0 || Bytes>RAM_UPLOAD_WINDOW || Bytes-Left>1023){
      mRamUpload.Error=EFI_BAD_BUFFER_SIZE;return TRUE;
    }
    for(UINTN I=Left;I<Bytes;I++)if(Data[I]!=0){mRamUpload.Error=EFI_BAD_BUFFER_SIZE;return TRUE;}
    Bytes=Left;
  }
  mRamUpload.Error=RfWrite(&mRamFile,Data,Bytes);mRamUpload.LastAt=StockTraceNowMs();
  return TRUE;
}
STATIC VOID RamUploadService(BOOLEAN Connected,BOOLEAN CanReply)
{
  EFI_STATUS S;
  if(!mRamMutating)return;
  if(!Connected || StockTraceNowMs()-mRamUpload.LastAt>30000)mRamUpload.Cancelled=TRUE;
  if(mRamUpload.Cancelled || EFI_ERROR(mRamUpload.Error)){
    S=mRamUpload.Error;RfAbort(&mRamFile);mRamMutating=FALSE;
    EFI_STATUS Mount=RamPublish();if(!EFI_ERROR(S))S=EFI_ERROR(Mount)?Mount:EFI_ABORTED;
    if(Connected && !mRamUpload.Cancelled){
      AsciiSPrint(mRamUpload.Reply,sizeof(mRamUpload.Reply),"ERR put %r",S);mRamUpload.ReplyReady=TRUE;
    }
    StockSay("[CDCRAM] upload aborted received=%u status=%r remount=%r\r\n",mRamFile.Received,S,Mount);return;
  }
  if(!CanReply || mRamUpload.ReplyReady || mRamFile.Received!=mRamUpload.WindowEnd)return;
  if(mRamFile.Received==mRamFile.Expected){
    S=RfFinish(&mRamFile);mRamMutating=FALSE;
    EFI_STATUS Mount=RamPublish();if(!EFI_ERROR(S))S=Mount;
    if(EFI_ERROR(S))AsciiSPrint(mRamUpload.Reply,sizeof(mRamUpload.Reply),"ERR put %r",S);
    else AsciiSPrint(mRamUpload.Reply,sizeof(mRamUpload.Reply),"OK put done %u crc=0x%08x",mRamFile.Received,mRamFile.ExpectedCrc);
    StockSay("[CDCRAM] commit bytes=%u crc=%08x status=%r remount=%r\r\n",mRamFile.Received,mRamFile.ExpectedCrc,S,Mount);
  }else{
    AsciiSPrint(mRamUpload.Reply,sizeof(mRamUpload.Reply),"OK put progress %u",mRamFile.Received);
    mRamUpload.WindowEnd=mRamFile.Received+MIN(mRamFile.Expected-mRamFile.Received,RAM_UPLOAD_WINDOW);
  }
  mRamUpload.ReplyReady=TRUE;
}
STATIC EFI_STATUS RamFileHash(CONST CHAR8 *Path,UINT32 *Bytes,UINT32 *Crc)
{
  CHAR8 Parts[96];UINTN N,Start=0;UINT32 Dir=2;UINT8 *Entry=NULL;EFI_STATUS S;
  if(mRamDisk==NULL)return EFI_NO_MEDIA;
  if(mRamUsbOwned || mRamMutating)return EFI_ACCESS_DENIED;
  if(Path==NULL || (N=AsciiStrLen(Path))==0 || N>=sizeof(Parts))return EFI_INVALID_PARAMETER;
  S=RfInit(&mRamFile,mRamDisk,mRamBytes);if(EFI_ERROR(S))return S;
  CopyMem(Parts,Path,N+1);if(Parts[0]=='\\' || Parts[0]=='/')Start=1;
  for(UINTN I=Start;I<=N;I++){
    CHAR8 C=Parts[I];if(C!='\\' && C!='/' && C!=0)continue;
    Parts[I]=0;if(I==Start || RfEqual(Parts+Start,".") || RfEqual(Parts+Start,".."))return EFI_INVALID_PARAMETER;
    S=RfFind(&mRamFile,Dir,Parts+Start,NULL,&Entry);if(EFI_ERROR(S))return S;
    if(C!=0){if(!(Entry[11]&16))return EFI_NOT_FOUND;Dir=RfHead(Entry);Start=I+1;}
  }
  if(Entry==NULL || (Entry[11]&16))return EFI_INVALID_PARAMETER;
  UINT32 Head=RfHead(Entry),Size=Rf32(Entry+28),Left=Size,Value=MAX_UINT32;
  if(Size>mRamBytes)return EFI_COMPROMISED_DATA;
  S=RfChain(&mRamFile,Head);if(EFI_ERROR(S))return S;
  while(Left){
    UINT8 *Data=RfData(&mRamFile,Head);if(Data==NULL)return EFI_COMPROMISED_DATA;
    UINT32 Count=MIN(Left,mRamFile.ClusterBytes);Value=RfCrc(Value,Data,Count);Left-=Count;Head=RfNext(&mRamFile,Head);
  }
  *Bytes=Size;*Crc=~Value;return EFI_SUCCESS;
}
