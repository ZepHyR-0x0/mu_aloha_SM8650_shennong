// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only Windows ARM64 prerequisites; successful checks are not proof that
// a particular Windows build supports SM8650 or that its drivers are present.
#include <Uefi.h>
#include <Library/PlatformTestLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/Rng.h>
#include <Protocol/MemoryAttribute.h>

STATIC VOID WbReport(PLATFORM_TEST_OUTPUT Out,VOID *Context,CONST CHAR8 *Format,...)
{
  CHAR8 Text[256];VA_LIST Args;VA_START(Args,Format);
  UINTN N=AsciiVSPrint(Text,sizeof(Text),Format,Args);VA_END(Args);
  SerialPortWrite((UINT8 *)Text,N);if(Out!=NULL)Out(Text,Context);
}
EFI_STATUS PlatformWinBootCheck(PLATFORM_TEST_OUTPUT Out,VOID *Context)
{
  EFI_ACPI_SDT_PROTOCOL *Sdt=NULL;EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;
  VOID *Provider=NULL;EFI_STATUS Status;UINT32 Found=0;BOOLEAN FadtOk=FALSE;
  CONST UINT32 Required[]={SIGNATURE_32('F','A','C','P'),SIGNATURE_32('A','P','I','C'),
    SIGNATURE_32('G','T','D','T'),SIGNATURE_32('D','S','D','T')};
  Status=gBS->LocateProtocol(&gEfiAcpiSdtProtocolGuid,NULL,(VOID **)&Sdt);
  WbReport(Out,Context,"[WINBOOT] ACPI provider=%r\n",Status);
  if(!EFI_ERROR(Status) && Sdt!=NULL){
    for(UINTN I=0;I<64;I++){
      EFI_ACPI_SDT_HEADER *H=NULL;EFI_ACPI_TABLE_VERSION Version;UINTN Key;
      Status=Sdt->GetAcpiTable(I,&H,&Version,&Key);
      if(EFI_ERROR(Status))break;
      if(H==NULL || H->Length<sizeof(*H) || H->Length>1024*1024)return EFI_COMPROMISED_DATA;
      CHAR8 Sig[5];CopyMem(Sig,&H->Signature,4);Sig[4]=0;
      if(H->Signature==SIGNATURE_32('F','A','C','S')){
        WbReport(Out,Context,"[WINBOOT] table=FACS length=%u checksum=n/a\n",H->Length);continue;
      }
      UINT8 Sum=0;for(UINTN J=0;J<H->Length;J++)Sum=(UINT8)(Sum+((UINT8 *)H)[J]);
      WbReport(Out,Context,"[WINBOOT] table=%a length=%u revision=%u checksum=%u\n",Sig,H->Length,H->Revision,Sum);
      for(UINTN J=0;J<ARRAY_SIZE(Required);J++)if(H->Signature==Required[J] && Sum==0)Found|=1u<<J;
      if(H->Signature==Required[0] && H->Length>=132){
        UINT32 Flags=ReadUnaligned32((UINT32 *)((UINT8 *)H+112));
        UINT16 Arm=ReadUnaligned16((UINT16 *)((UINT8 *)H+129));
        FadtOk=Sum==0 && (Flags&(1u<<20))!=0 && (Arm&1)!=0;
        WbReport(Out,Context,"[WINBOOT] FADT reduced=%u PSCI=%u conduit=%a\n",(Flags>>20)&1,Arm&1,(Arm&2)?"HVC":"SMC");
      }
      if(H->Signature==Required[1]){
        UINTN At=44,Enabled=0;CONST UINT8 *B=(CONST UINT8 *)H;
        while(At<H->Length){
          if(H->Length-At<2 || B[At+1]<2 || B[At+1]>H->Length-At)return EFI_COMPROMISED_DATA;
          if(B[At]==11){
            if(B[At+1]<80)return EFI_COMPROMISED_DATA;
            UINT32 Flags=ReadUnaligned32((CONST UINT32 *)(B+At+12));
            WbReport(Out,Context,"[WINBOOT] GICC len=%u uid=%u enabled=%u GICR=%lx MPIDR=%lx\n",
              B[At+1],ReadUnaligned32((CONST UINT32 *)(B+At+8)),Flags&1,
              ReadUnaligned64((CONST UINT64 *)(B+At+60)),ReadUnaligned64((CONST UINT64 *)(B+At+68)));
            if(Flags&1)Enabled++;
          }
          At+=B[At+1];
        }
        WbReport(Out,Context,"[WINBOOT] MADT enabled CPUs=%u\n",(UINT32)Enabled);
      }
      if(H->Signature==Required[2] && H->Length>=80){
        CONST UINT8 *B=(CONST UINT8 *)H;
        for(UINTN J=0;J<4;J++)WbReport(Out,Context,"[WINBOOT] GTDT timer=%u interrupt=%u flags=%x\n",(UINT32)J,
          ReadUnaligned32((CONST UINT32 *)(B+48+8*J)),ReadUnaligned32((CONST UINT32 *)(B+52+8*J)));
      }
    }
  }
  Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  BOOLEAN Graphics=FALSE;
  if(!EFI_ERROR(Status) && Gop!=NULL && Gop->Mode!=NULL && Gop->Mode->Info!=NULL){
    Graphics=Gop->Mode->Info->PixelFormat!=PixelBltOnly && Gop->Mode->FrameBufferBase!=0 && Gop->Mode->FrameBufferSize!=0;
    WbReport(Out,Context,"[WINBOOT] GOP=%ux%u format=%u physical_fb=%lx bytes=%lu\n",
      Gop->Mode->Info->HorizontalResolution,Gop->Mode->Info->VerticalResolution,
      Gop->Mode->Info->PixelFormat,Gop->Mode->FrameBufferBase,(UINT64)Gop->Mode->FrameBufferSize);
  }
  WbReport(Out,Context,"[WINBOOT] RNG provider=%r (no entropy requested)\n",
    gBS->LocateProtocol(&gEfiRngProtocolGuid,NULL,&Provider));
  WbReport(Out,Context,"[WINBOOT] memory attributes provider=%r\n",
    gBS->LocateProtocol(&gEfiMemoryAttributeProtocolGuid,NULL,&Provider));
  WbReport(Out,Context,"[WINBOOT] required ACPI mask=%x/0f FADT=%u GOP=%u; verify PSCI/timers and Windows drivers separately\n",Found,FadtOk,Graphics);
  WbReport(Out,Context,"[WINBOOT] FAT32 supports this sub-4GiB WIM; NTFS is not required for the current boot directory\n");
  if(AsciiStrCmp((CONST CHAR8 *)PcdGetPtr(PcdSmbiosSystemRetailModel),"shennong")==0){
    UINT64 El,Actlr;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(El));
    __asm__ volatile("mrs %0, actlr_el1" : "=r"(Actlr));
    WbReport(Out,Context,"[WINBOOT] CurrentEL=%u ACTLR_EL1=%lx (read-only; inherited ACTLR patch disabled)\n",(UINT32)(El>>2),Actlr);
  }
  return Found==15 && FadtOk && Graphics?EFI_SUCCESS:EFI_NOT_READY;
}
