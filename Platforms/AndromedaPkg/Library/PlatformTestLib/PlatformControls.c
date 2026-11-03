/** Bounded camera LED test using the board's qti-flash register map.
    SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PlatformTestLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
typedef struct FLASH_SPMI FLASH_SPMI;
struct FLASH_SPMI {
  UINT64 Revision;
  UINT32 (EFIAPI *Read)(VOID *,UINT32,UINT32,UINT32,UINT32,UINT8 *,UINT32,UINT32 *);
  UINT32 (EFIAPI *Write)(VOID *,UINT32,UINT32,UINT32,UINT32,UINT8 *,UINT32);
};
STATIC EFI_GUID mFlashSpmiGuid={0xFA5F306B,0xF47D,0x4AC4,{0xA4,0x7D,0x88,0x2F,0x82,0x04,0xEC,0x30}};
STATIC EFI_GUID mBrightnessGuid={0x7BFA4293,0x7AA4,0x4375,{0xB6,0x3C,0xB6,0xAA,0xB7,0x86,0xC4,0x3C}};
STATIC BOOLEAN mFlashBusy;
STATIC VOID ControlReport(PLATFORM_TEST_OUTPUT Output,VOID *Context,CONST CHAR8 *Format,...)
{
  CHAR8 Line[256];VA_LIST Args;VA_START(Args,Format);AsciiVSPrint(Line,sizeof(Line),Format,Args);VA_END(Args);
  SerialPortWrite((UINT8 *)Line,AsciiStrLen(Line));if(Output!=NULL) Output(Line,Context);
}
STATIC EFI_STATUS FlashProvider(FLASH_SPMI **Spmi)
{
  EFI_HANDLE *Images=NULL;UINTN Count=0,I;EFI_STATUS Status;
  if(AsciiStrCmp((CONST CHAR8 *)PcdGetPtr(PcdSmbiosSystemRetailModel),"shennong")!=0) return EFI_UNSUPPORTED;
  Status=gBS->LocateProtocol(&mFlashSpmiGuid,NULL,(VOID **)Spmi);if(EFI_ERROR(Status)) return Status;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Count,&Images);if(EFI_ERROR(Status)) return Status;
  Status=EFI_UNSUPPORTED;
  for(I=0;I<Count;I++) {
    EFI_LOADED_IMAGE_PROTOCOL *Image=NULL;UINTN Base;
    if(EFI_ERROR(gBS->HandleProtocol(Images[I],&gEfiLoadedImageProtocolGuid,(VOID **)&Image)) || Image==NULL) continue;
    Base=(UINTN)Image->ImageBase;
    if(Image->ImageSize>=0x70b0 && (UINTN)*Spmi==Base+0x7098 && (*Spmi)->Revision==0x10003 &&
       (UINTN)(*Spmi)->Read==Base+0x13ec && (UINTN)(*Spmi)->Write==Base+0x14ac) {Status=EFI_SUCCESS;break;}
  }
  FreePool(Images);return Status;
}
STATIC EFI_STATUS FlashRead(FLASH_SPMI *Spmi,UINT16 Reg,UINT8 *Value)
{
  UINT32 Done=0,Rc=Spmi->Read(Spmi,0,1,0,Reg,Value,1,&Done);
  return Rc==0 && Done==1?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS FlashWrite(FLASH_SPMI *Spmi,UINT16 Reg,UINT8 Value)
{
  UINT8 Actual=0;UINT32 Rc;
  // This is the complete physical write allowlist: channels 0/1 plus common controls.
  if(Reg!=0xee3e && Reg!=0xee3f && Reg!=0xee42 && Reg!=0xee43 && Reg!=0xee49 &&
     Reg!=0xee4a && Reg!=0xee4b && Reg!=0xee46 && Reg!=0xee4e) return EFI_ACCESS_DENIED;
  Rc=Spmi->Write(Spmi,0,1,0,Reg,&Value,1);
  if(Rc!=0) return EFI_DEVICE_ERROR;
  return !EFI_ERROR(FlashRead(Spmi,Reg,&Actual)) && Actual==Value?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
EFI_STATUS PlatformBrightnessStatus(PLATFORM_TEST_OUTPUT Output,VOID *Context)
{
  VOID *Protocol=NULL;EFI_STATUS Status=gBS->LocateProtocol(&mBrightnessGuid,NULL,&Protocol);
  ControlReport(Output,Context,"[BRIGHTNESS] DCS provider -> %r interface=%p; SimpleFb retains ABL panel state\n",Status,Protocol);
  return EFI_ERROR(Status)?Status:EFI_NOT_READY;
}
EFI_STATUS PlatformFlashTest(PLATFORM_TEST_OUTPUT Output,VOID *Context,UINT32 Channel,UINT32 Ms,UINT32 Ma,
                             PLATFORM_FLASH_RESULT *Result)
{
  FLASH_SPMI *Spmi=NULL;UINT8 Id[4],State[4],Old[6],Value[6],Actual=0;
  UINT16 Reg[6];UINTN I,Changed=0,Attempt;EFI_STATUS Status,Cleanup=EFI_SUCCESS;
  BOOLEAN Activated=FALSE;
  if(Result==NULL || Channel>1 || (Ms!=0 && (Ms<50 || Ms>300 || Ms%10!=0 || Ma<25 || Ma>100 || Ma%25!=0))) return EFI_INVALID_PARAMETER;
  ZeroMem(Result,sizeof(*Result));if(mFlashBusy) return EFI_ALREADY_STARTED;mFlashBusy=TRUE;
  Status=FlashProvider(&Spmi);if(EFI_ERROR(Status)) goto Done;
  for(I=0;I<4;I++) {Status=FlashRead(Spmi,(UINT16)(0xee02+I),&Id[I]);if(EFI_ERROR(Status)) goto Done;}
  Status=FlashRead(Spmi,0xee06,&State[0]);if(EFI_ERROR(Status)) goto Done;
  Status=FlashRead(Spmi,0xee07,&State[1]);if(EFI_ERROR(Status)) goto Done;
  Status=FlashRead(Spmi,0xee46,&State[2]);if(EFI_ERROR(Status)) goto Done;
  Status=FlashRead(Spmi,0xee4e,&State[3]);if(EFI_ERROR(Status)) goto Done;
  Result->Type=Id[2];Result->Subtype=Id[3];Result->Status1=State[0];Result->Status2=State[1];
  ControlReport(Output,Context,"[FLASH] sid=1 base=ee00 rev=%02x/%02x type=%02x subtype=%02x status=%02x/%02x module=%02x channels=%02x\n",
    Id[0],Id[1],Id[2],Id[3],State[0],State[1],State[2],State[3]);
  if(Ms==0) {Status=EFI_SUCCESS;goto Done;}
  // Kernel map: 4-channel subtype 07. Refuse an active module or fault/thermal/low-VPH state.
  if(Id[2]!=0x18 || Id[3]!=7) {Status=EFI_UNSUPPORTED;goto Done;}
  if((State[2]&BIT7)!=0 || (State[3]&15)!=0) {Status=EFI_ALREADY_STARTED;goto Done;}
  if(State[0]!=0 || (State[1]&(BIT5|BIT4|BIT0))!=0) {Status=EFI_NOT_READY;goto Done;}
  Reg[0]=(UINT16)(0xee3e + Channel);Reg[1]=(UINT16)(0xee42+Channel);Reg[2]=0xee49;
  Reg[3]=(UINT16)(0xee4a+Channel);Reg[4]=0xee46;Reg[5]=0xee4e;
  for(I=0;I<6;I++) {Status=FlashRead(Spmi,Reg[I],&Old[I]);if(EFI_ERROR(Status)) goto Done;}
  // 12.5 mA/code, hardware safety timeout armed before any enable.
  Value[0]=(UINT8)(BIT7+Ms/10-1);Value[1]=(UINT8)((Old[1]&BIT7)|(Ma*1000/12500-1));
  Value[2]=(UINT8)(Old[2]&~(1u<<Channel));Value[3]=(UINT8)(Old[3]&~0x74u);
  Value[4]=(UINT8)(Old[4]|BIT7);Value[5]=(UINT8)(Old[5]|(1u<<Channel));
  for(I=0;I<6;I++) {
    Changed=I+1;if(I>=4) Activated=TRUE;
    Status=FlashWrite(Spmi,Reg[I],Value[I]);
    ControlReport(Output,Context,"[FLASH IO] set reg=%04x value=%02x -> %r\n",Reg[I],Value[I],Status);
    if(EFI_ERROR(Status)) break;
  }
  if(!EFI_ERROR(Status)) {
    Result->Started=TRUE;
    for(I=0;I<Ms/10;I++) {
      gBS->Stall(10000);Status=FlashRead(Spmi,0xee06,&Actual);
      if(EFI_ERROR(Status) || Actual!=0) {Status=EFI_DEVICE_ERROR;break;}
    }
  }
  // Stop and verify first; only then restore persistent configuration.
  if(Activated) {
    for(Attempt=0;Attempt<3;Attempt++) {
      Cleanup=FlashWrite(Spmi,0xee4e,Old[5]);
      if(!EFI_ERROR(Cleanup)) Cleanup=FlashWrite(Spmi,0xee46,Old[4]);
      if(!EFI_ERROR(Cleanup)) break;
      gBS->Stall(10000);
    }
  }
  if(!EFI_ERROR(Cleanup)) {
    while(Changed>0) {
      I=--Changed;
      for(Attempt=0;Attempt<3;Attempt++) {
        Cleanup=FlashWrite(Spmi,Reg[I],Old[I]);if(!EFI_ERROR(Cleanup)) break;gBS->Stall(10000);
      }
      if(EFI_ERROR(Cleanup)) break;
    }
  }
  Result->Restored=!EFI_ERROR(Cleanup);
  ControlReport(Output,Context,"[FLASH] channel=%u duration_ms=%u current_ma=%u started=%u restored=%u cleanup=%r -> %r\n",
    Channel,Ms,Ma,Result->Started,Result->Restored,Cleanup,Status);
  if(EFI_ERROR(Cleanup)) Status=Cleanup;
Done:
  Result->Status=Status;mFlashBusy=FALSE;return Status;
}
