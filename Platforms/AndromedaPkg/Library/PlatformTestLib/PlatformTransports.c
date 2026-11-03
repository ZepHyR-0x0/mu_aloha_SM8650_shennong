// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shennong subsystem transport discovery. Only read existing SMEM contents;
// do not allocate items, acknowledge IRQs, advance rings or boot DSPs.
#include <Uefi.h>
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/PlatformTestLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PciIo.h>
#include "SmemReadCore.h"

STATIC VOID TrReport(PLATFORM_TEST_OUTPUT Output,VOID *Context,CONST CHAR8 *Format,...)
{
  CHAR8 Text[256];VA_LIST Args;VA_START(Args,Format);
  UINTN N=AsciiVSPrint(Text,sizeof(Text),Format,Args);VA_END(Args);
  SerialPortWrite((UINT8 *)Text,N);if(Output!=NULL)Output(Text,Context);
}
STATIC VOID TrReadWords(UINT8 *Destination,UINTN Source,UINTN Bytes)
{
  // SMEM is mapped Device/uncached. Use aligned scalar accesses, never the
  // generic CopyMem implementation's vector or paired loads across padding.
  for(UINTN I=0;I<Bytes;I+=4){
    UINT32 V=*(volatile UINT32 *)(Source+I);
    CopyMem(Destination+I,&V,sizeof(V));
  }
}
STATIC EFI_STATUS TrSmemCopy(UINT8 **Copy,PLATFORM_TEST_OUTPUT Output,VOID *Context)
{
  // Exact no-map extent from the captured Shennong device tree.
  CONST UINTN Base=0x81d00000,Bytes=0x200000;UINTN At=Base;
  if(AsciiStrCmp((CONST CHAR8 *)PcdGetPtr(PcdSmbiosSystemRetailModel),"shennong")!=0)return EFI_UNSUPPORTED;
  while(At<Base+Bytes){
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR M;EFI_STATUS S=gDS->GetMemorySpaceDescriptor(At,&M);
    if(EFI_ERROR(S))return S;
    if(M.GcdMemoryType==EfiGcdMemoryTypeNonExistent || M.BaseAddress>At || M.Length==0 ||
       M.BaseAddress>MAX_UINT64-M.Length || M.BaseAddress+M.Length<=At)return EFI_NOT_READY;
    At=(UINTN)MIN(M.BaseAddress+M.Length,Base+Bytes);
  }
  *Copy=AllocateZeroPool(Bytes);if(*Copy==NULL)return EFI_OUT_OF_RESOURCES;
  TrReport(Output,Context,"[TRANSPORT] SMEM TOC read begin @%lx\n",(UINT64)(Base+Bytes-4096));
  MemoryFence();TrReadWords(*Copy+Bytes-4096,Base+Bytes-4096,4096);MemoryFence();
  CONST UINT8 *Toc=*Copy+Bytes-4096;UINT32 Count=Sr32(Toc+8);
  if(Sr32(Toc)!=0x434f5424 || Sr32(Toc+4)!=1 || Count>64){
    FreePool(*Copy);*Copy=NULL;return EFI_COMPROMISED_DATA;
  }
  // Read only AP/DSP private partitions listed by the validated TOC. Skip
  // global heaps and unused gaps; the observer does not own those regions.
  for(UINTN I=0;I<Count;I++){
    CONST UINT8 *E=Toc+32+48*I;UINT16 A=Sr16(E+12),B=Sr16(E+14);
    if(!((A==0 && (B==1 || B==2 || B==5)) || (B==0 && (A==1 || A==2 || A==5))))continue;
    UINT32 At=Sr32(E),N=Sr32(E+4);
    if(At<8192 || N<32 || ((At|N)&3)!=0 || !SrRange(Bytes-4096,At,N)){
      FreePool(*Copy);*Copy=NULL;return EFI_COMPROMISED_DATA;
    }
    TrReport(Output,Context,"[TRANSPORT] SMEM partition read hosts=%u/%u offset=%x bytes=%u\n",A,B,At,N);
    TrReadWords(*Copy+At,Base+At,N);
  }
  MemoryFence();
  // Remote firmware may change the partition table during the copy. No
  // shared lock is acquired by this read-only observer; retry next invocation.
  for(UINTN I=0;I<4096;I+=4){
    if(Sr32(*Copy+Bytes-4096+I)!=*(volatile UINT32 *)(Base+Bytes-4096+I)){
      FreePool(*Copy);*Copy=NULL;return EFI_NOT_READY;
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS TrGlink(UINT8 *Copy,UINT16 Host,CONST CHAR8 *Name,PLATFORM_TEST_OUTPUT Output,VOID *Context)
{
  CONST UINT8 *D,*F;UINTN Dn,Fn;EFI_STATUS S=SrItem(Copy,0x200000,Host,478,&D,&Dn);
  TrReport(Output,Context,"[GLINK] %a host=%u descriptor=%r bytes=%u\n",Name,Host,S,(UINT32)Dn);
  if(EFI_ERROR(S))return S;
  for(UINTN Rx=0;Rx<2;Rx++){
    UINT32 Tail=0,Head=0,Available=0;
    S=SrItem(Copy,0x200000,Host,(UINT16)(479+Rx),&F,&Fn);
    if(!EFI_ERROR(S))S=SrRing(D,Dn,Fn,(BOOLEAN)Rx,&Tail,&Head,&Available);
    TrReport(Output,Context,"[GLINK] %a %a status=%r fifo=%u head=%u tail=%u available=%u\n",
      Name,Rx?"RX":"TX",S,(UINT32)Fn,Head,Tail,Available);
    if(EFI_ERROR(S) || !Rx || Available<8)continue;
    UINT8 Header[8];S=SrPeek(F,Fn,Tail,Available,Header,sizeof(Header));
    if(EFI_ERROR(S))continue;
    UINT16 Cmd=Sr16(Header),Channel=Sr16(Header+2);UINT32 Param=Sr32(Header+4);
    TrReport(Output,Context,"[GLINK] %a first RX cmd=%u channel=%u param=%u (snapshot; not consumed)\n",Name,Cmd,Channel,Param);
    if(Cmd==2){
      UINT32 N=Param&0xffff; // OPEN name length; high bits carry request flags.
      UINT8 Packet[72];CHAR8 Label[65];
      if(N==0 || N>64 || EFI_ERROR(SrPeek(F,Fn,Tail,Available,Packet,8+N)))continue;
      if(Packet[8+N-1]!=0)continue;
      for(UINTN I=0;I<N-1;I++)Label[I]=Packet[8+I]>=32 && Packet[8+I]<127?(CHAR8)Packet[8+I]:'?';
      Label[N-1]=0;
      TrReport(Output,Context,"[GLINK] %a advertised channel=%a; no OPEN_ACK sent\n",Name,Label);
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS TrMhi(PLATFORM_TEST_OUTPUT Output,VOID *Context)
{
  EFI_HANDLE *Handles=NULL;UINTN Count=0;EFI_STATUS Status;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiPciIoProtocolGuid,NULL,&Count,&Handles);
  TrReport(Output,Context,"[MHI] PCI_IO discovery=%r handles=%u\n",Status,(UINT32)Count);
  if(EFI_ERROR(Status))return Status;
  Status=EFI_NOT_FOUND;
  for(UINTN I=0;I<Count;I++){
    EFI_PCI_IO_PROTOCOL *P=NULL;UINT32 Id=0,Command=0;
    if(EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiPciIoProtocolGuid,(VOID **)&P)) || P==NULL)continue;
    if(EFI_ERROR(P->Pci.Read(P,EfiPciIoWidthUint32,0,1,&Id)) || Id!=0x110717cb)continue;
    Status=P->Pci.Read(P,EfiPciIoWidthUint32,4,1,&Command);
    if(EFI_ERROR(Status))break;
    // Respect the enumerator's BAR and power ownership. Never enable memory
    // decoding or bus mastering here; no firmware DMA is initiated.
    if(!(Command&2)){Status=EFI_NOT_READY;break;}
    CONST UINT32 Off[]={0,8,0x28,0x2c,0x38,0x48};
    CONST CHAR8 *Names[]={"MHIREGLEN","MHIVER","BHIOFF","BHIEOFF","MHICTRL","MHISTATUS"};
    UINT32 Values[6]={0};
    TrReport(Output,Context,"[MHI] BAR0 read begin command=%08x\n",Command);
    Status=P->Mem.Read(P,EfiPciIoWidthUint32,0,0,1,&Values[0]);
    if(EFI_ERROR(Status))break;
    if(Values[0]<0x4c || Values[0]>0x100000){Status=EFI_COMPROMISED_DATA;break;}
    for(UINTN J=1;J<6;J++){
      Status=P->Mem.Read(P,EfiPciIoWidthUint32,0,Off[J],1,&Values[J]);if(EFI_ERROR(Status))break;
    }
    if(EFI_ERROR(Status))break;
    for(UINTN J=0;J<6;J++)TrReport(Output,Context,"[MHI] %a=%08x\n",Names[J],Values[J]);
    TrReport(Output,Context,"[MHI] state=%u ready=%u syserr=%u; no reset/download/doorbell writes\n",
      (Values[5]>>8)&255,Values[5]&1,(Values[5]>>2)&1);
    break;
  }
  if(Handles!=NULL)FreePool(Handles);
  TrReport(Output,Context,"[MHI] 17cb:1107 provider read=%r; QMI/WiFi scan still requires firmware transport\n",Status);
  return Status;
}
EFI_STATUS PlatformTransportTestSelected(PLATFORM_TEST_OUTPUT Output,VOID *Context,PLATFORM_TRANSPORT_RESULT *Result,UINT32 Mask)
{
  UINT8 *Copy=NULL;EFI_STATUS S=EFI_UNSUPPORTED;
  if(Result==NULL || Mask==0 || Mask>3)return EFI_INVALID_PARAMETER;ZeroMem(Result,sizeof(*Result));
  Result->Adsp=Result->Modem=Result->Cdsp=Result->Mhi=EFI_UNSUPPORTED;
  TrReport(Output,Context,"[TRANSPORT] discovery begin mask=%u\n",Mask);
  if(Mask&1)S=TrSmemCopy(&Copy,Output,Context);Result->Smem=S;
  TrReport(Output,Context,"[TRANSPORT] SMEM bounded snapshot=%r; live state/freshness not established\n",S);
  if(!EFI_ERROR(S)){
    Result->Adsp=TrGlink(Copy,2,"ADSP/SSC",Output,Context);
    Result->Modem=TrGlink(Copy,1,"MODEM/GNSS",Output,Context);
    Result->Cdsp=TrGlink(Copy,5,"CDSP",Output,Context);FreePool(Copy);
  }
  if(Mask&2)Result->Mhi=TrMhi(Output,Context);
  TrReport(Output,Context,"[TRANSPORT] Bluetooth needs GENI UART@898000 + WCN7850 firmware; no HCI provider yet\n");
  TrReport(Output,Context,"[TRANSPORT] GLINK visibility is a prerequisite, not a gyro sample or GPS fix\n");
  return Mask==2?Result->Mhi:S;
}
EFI_STATUS PlatformTransportTest(PLATFORM_TEST_OUTPUT Output,VOID *Context,PLATFORM_TRANSPORT_RESULT *Result)
{
  return PlatformTransportTestSelected(Output,Context,Result,3);
}
