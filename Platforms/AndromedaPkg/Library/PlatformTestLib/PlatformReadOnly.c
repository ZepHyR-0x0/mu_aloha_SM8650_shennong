/** Native RPMB read client. SPDX-License-Identifier: BSD-2-Clause-Patent
    ABI: EFIRpmb.h revision 10001, checked against linked Shennong UFSDxe.
    Requests are created here, never supplied by the caller. Only 0004 (read)
    is submitted to ReadBlocks. Reserved write/key slots have no callable type.
    SECURITY PROTOCOL OUT transports the read request; it does not write media.
**/
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformTestLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

typedef struct RPMB_READ_PROTOCOL RPMB_READ_PROTOCOL;
struct RPMB_READ_PROTOCOL {
  UINT64 Revision;
  UINT32 ReliableWriteCount, PartitionBytes;
  INT32 (EFIAPI *ReadCounter)(RPMB_READ_PROTOCOL *, UINT32 *);
  VOID *ReservedKey;
  INT32 (EFIAPI *ReadBlocks)(RPMB_READ_PROTOCOL *, UINT32 *, UINT32, UINT32 *);
  VOID *ReservedWrite;
  UINT32 MaxTransfer;
};
STATIC_ASSERT(OFFSET_OF(RPMB_READ_PROTOCOL,ReadCounter)==0x10,"RPMB counter ABI");
STATIC_ASSERT(OFFSET_OF(RPMB_READ_PROTOCOL,ReadBlocks)==0x20,"RPMB read ABI");
STATIC_ASSERT(OFFSET_OF(RPMB_READ_PROTOCOL,MaxTransfer)==0x30,"RPMB size ABI");
STATIC EFI_GUID mRpmbGuid={0xEF6FD01D,0x8861,0x4132,{0x83,0x4E,0xDE,0xCC,0x0F,0xF1,0xAA,0xFA}};
STATIC BOOLEAN mRpmbBusy;
#include "RpmbNativeLayout.h"

STATIC VOID RpmbReport(PLATFORM_TEST_OUTPUT Output, VOID *Context, CONST CHAR8 *Format, ...)
{
  CHAR8 Line[256]; VA_LIST Args;
  VA_START(Args,Format); AsciiVSPrint(Line,sizeof(Line),Format,Args); VA_END(Args);
  SerialPortWrite((UINT8 *)Line,AsciiStrLen(Line));
  if(Output!=NULL) Output(Line,Context);
}
STATIC UINT16 Be16(CONST UINT8 *P) { return (UINT16)((P[0]<<8)|P[1]); }
STATIC UINT32 Be32(CONST UINT8 *P) { return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|(P[2]<<8)|P[3]; }
STATIC VOID Put16(UINT8 *P, UINT16 V) {P[0]=(UINT8)(V>>8);P[1]=(UINT8)V;}
STATIC VOID RpmbHex(PLATFORM_TEST_OUTPUT Output,VOID *Context,CONST UINT8 *Frame,UINT32 Block)
{
  UINTN I,J;CHAR8 Hex[49];
  for(I=0;I<512;I+=16) {
    for(J=0;J<16;J++) AsciiSPrint(Hex+J*3,sizeof(Hex)-J*3,"%02x ",Frame[I+J]);
    RpmbReport(Output,Context,"[RPMB RAW] block=%u off=%03x %a\n",Block,(UINT32)I,Hex);
  }
}
STATIC EFI_STATUS RpmbProvider(RPMB_READ_PROTOCOL **Protocol, PLATFORM_TEST_OUTPUT Output, VOID *Context)
{
  EFI_HANDLE *Images=NULL; UINTN Count=0,I; EFI_STATUS Status;
  Status=gBS->LocateProtocol(&mRpmbGuid,NULL,(VOID **)Protocol);
  if(EFI_ERROR(Status)) return Status;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Count,&Images);
  if(EFI_ERROR(Status)) return Status;
  Status=EFI_UNSUPPORTED;
  for(I=0;I<Count;I++) {
    EFI_LOADED_IMAGE_PROTOCOL *Image=NULL;UINTN Base,J;
    if(EFI_ERROR(gBS->HandleProtocol(Images[I],&gEfiLoadedImageProtocolGuid,(VOID **)&Image)) || Image==NULL) continue;
    Base=(UINTN)Image->ImageBase;
    if(Image->ImageSize<0x22000 || (UINTN)*Protocol!=Base+0x1F890) continue;
    if((*Protocol)->Revision!=0x10001 || (UINTN)(*Protocol)->ReadCounter!=Base+0x4BD0 ||
       (UINTN)(*Protocol)->ReadBlocks!=Base+0x4D3C) continue;
    // Linked native provider installs this table from its constant template.
    if(ReadUnaligned64((UINT64 *)(Base+0x1A528))!=0x10001 ||
       ReadUnaligned64((UINT64 *)(Base+0x1A538))!=Base+0x4BD0 ||
       ReadUnaligned64((UINT64 *)(Base+0x1A548))!=Base+0x4D3C) continue;
    // Pin the linked DEBUG driver's read implementation and B5/A2 CDBs.
    // All fingerprint spans contain position-independent code, no PE relocations.
    for(J=0;J<ARRAY_SIZE(mRpmbReadCode);J++) {
      if(CalculateCrc32((VOID *)(Base+mRpmbReadCode[J].Rva),mRpmbReadCode[J].Bytes)!=mRpmbReadCode[J].Crc) break;
    }
    if(J!=ARRAY_SIZE(mRpmbReadCode)) continue;
    RpmbReport(Output,Context,"[RPMB] owner=%p interface_rva=1f890 counter_rva=4bd0 read_rva=4d3c read_code=verified\n",Image->ImageBase);
    Status=EFI_SUCCESS;break;
  }
  FreePool(Images);return Status;
}
STATIC EFI_STATUS RpmbCounter(RPMB_READ_PROTOCOL *Protocol,UINT32 *Value,
                             PLATFORM_TEST_OUTPUT Output,VOID *Context,CONST CHAR8 *Stage)
{
  UINT32 Storage[128];UINT8 *Frame=(UINT8 *)Storage;INT32 Rc;
  ZeroMem(Storage,sizeof(Storage));Rc=Protocol->ReadCounter(Protocol,Storage);
  RpmbReport(Output,Context,"[RPMB] counter_%a rc=%d type=%04x result=%04x value=%u\n",
    Stage,Rc,Be16(Frame+510),Be16(Frame+508),Be32(Frame+500));
  if(Rc!=0) return EFI_DEVICE_ERROR;
  if(Be16(Frame+510)!=0x0200 || Be16(Frame+508)!=0) return EFI_COMPROMISED_DATA;
  *Value=Be32(Frame+500);return EFI_SUCCESS;
}
EFI_STATUS PlatformRpmbRead(PLATFORM_TEST_OUTPUT Output,VOID *Context,UINT32 Address,UINT32 Blocks,
                            PLATFORM_RPMB_RESULT *Result)
{
  RPMB_READ_PROTOCOL *Protocol=NULL;EFI_STATUS Status,AfterStatus;
  UINT32 Request[128],Response[128],I;UINT64 Nonce;UINT8 *Req=(UINT8 *)Request,*Resp=(UINT8 *)Response;
  if(Result==NULL || Address>65535 || Blocks>4 || (Blocks!=0 && Blocks>65536-Address)) return EFI_INVALID_PARAMETER;
  ZeroMem(Result,sizeof(*Result));
  if(mRpmbBusy) return EFI_ALREADY_STARTED;
  mRpmbBusy=TRUE;
  Status=RpmbProvider(&Protocol,Output,Context);
  RpmbReport(Output,Context,"[RPMB] read-only provider -> %r\n",Status);
  if(EFI_ERROR(Status)) goto Done;
  Result->PartitionBytes=Protocol->PartitionBytes;Result->MaxTransfer=Protocol->MaxTransfer;
  RpmbReport(Output,Context,"[RPMB] revision=%lx bytes=%u max_transfer=%u\n",
    Protocol->Revision,Protocol->PartitionBytes,Protocol->MaxTransfer);
  if(Blocks==0) {Status=EFI_SUCCESS;goto Done;}
  if(Protocol->PartitionBytes<256 || Protocol->MaxTransfer==0 ||
     Address>=Protocol->PartitionBytes/256 || Blocks>Protocol->PartitionBytes/256-Address) {
    Status=EFI_INVALID_PARAMETER;goto Done;
  }
  Status=RpmbCounter(Protocol,&Result->CounterBefore,Output,Context,"before");
  if(EFI_ERROR(Status)) goto Done;
  for(I=0;I<Blocks;I++) {
    INT32 Rc;ZeroMem(Request,sizeof(Request));ZeroMem(Response,sizeof(Response));
    Nonce=GetPerformanceCounter();CopyMem(Req+484,&Nonce,8);
    Nonce=~Nonce ^ ((UINT64)(Address+I)<<32);CopyMem(Req+492,&Nonce,8);
    Put16(Req+504,(UINT16)(Address+I));Put16(Req+506,1);Put16(Req+510,4);
    // The only request accepted by this client is a zero-data, zero-MAC read.
    Rc=Protocol->ReadBlocks(Protocol,Request,1,Response);
    RpmbReport(Output,Context,"[RPMB] read address=%u request=0004 frames=1 rc=%d type=%04x result=%04x returned_addr=%u\n",
      Address+I,Rc,Be16(Resp+510),Be16(Resp+508),Be16(Resp+504));
    if(Rc!=0) {Status=EFI_DEVICE_ERROR;break;}
    if(Be16(Resp+510)!=0x0400 || Be16(Resp+508)!=0 || Be16(Resp+504)!=Address+I ||
       CompareMem(Req+484,Resp+484,16)!=0) {Status=EFI_COMPROMISED_DATA;break;}
    RpmbHex(Output,Context,Resp,Address+I);Result->Frames++;
  }
  AfterStatus=RpmbCounter(Protocol,&Result->CounterAfter,Output,Context,"after");
  if(EFI_ERROR(AfterStatus)) Status=AfterStatus;
  else if(Result->CounterBefore!=Result->CounterAfter) Status=EFI_COMPROMISED_DATA;
  RpmbReport(Output,Context,"[RPMB] frames=%u counter_unchanged=%u MAC_authentication=unavailable -> %r\n",
    Result->Frames,!EFI_ERROR(AfterStatus)&&Result->CounterBefore==Result->CounterAfter,Status);
Done:
  Result->Status=Status;mRpmbBusy=FALSE;return Status;
}
