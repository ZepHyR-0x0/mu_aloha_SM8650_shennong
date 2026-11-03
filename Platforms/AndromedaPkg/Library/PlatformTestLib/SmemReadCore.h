// SPDX-License-Identifier: BSD-2-Clause-Patent
// SMEM v12 private uncached partition decoder. Numeric wire layout from
// Linux drivers/soc/qcom/smem.c; all input is an immutable, bounded copy.
// No allocation in shared memory, FIFO consumption or remote state changes.
#ifndef SMEM_READ_CORE_H_
#define SMEM_READ_CORE_H_
STATIC UINT16 Sr16(CONST UINT8 *P){return P[0]|((UINT16)P[1]<<8);}
STATIC UINT32 Sr32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC BOOLEAN SrRange(UINTN Total,UINTN At,UINTN Bytes){return At<=Total && Bytes<=Total-At;}
STATIC EFI_STATUS SrItem(CONST UINT8 *Data,UINTN Bytes,UINT16 Host,UINT16 Item,
                         CONST UINT8 **Value,UINTN *Size)
{
  UINTN Toc,Count,I;CONST UINT8 *Part=NULL;UINT32 Length=0,Free;
  if(Data==NULL || Value==NULL || Size==NULL || Bytes<4096)return EFI_INVALID_PARAMETER;
  *Value=NULL;*Size=0;Toc=Bytes-4096;
  if(Sr32(Data+Toc)!=0x434f5424)return EFI_NOT_FOUND; // $TOC
  if(Sr32(Data+Toc+4)!=1)return EFI_UNSUPPORTED;
  Count=Sr32(Data+Toc+8);if(Count>64 || !SrRange(4096,32,Count*48))return EFI_COMPROMISED_DATA;
  for(I=0;I<Count;I++){
    CONST UINT8 *E=Data+Toc+32+48*I;UINT16 A=Sr16(E+12),B=Sr16(E+14);
    UINT32 At=Sr32(E),N=Sr32(E+4);
    if(!((A==0 && B==Host)||(B==0 && A==Host)))continue;
    if(Part!=NULL || N<32 || At<8192 || !SrRange(Toc,At,N))return EFI_COMPROMISED_DATA;
    Part=Data+At;Length=N;
  }
  if(Part==NULL)return EFI_NOT_FOUND;
  if(Sr32(Part)!=0x54525024 || Sr32(Part+8)!=Length ||
     !((Sr16(Part+4)==0 && Sr16(Part+6)==Host)||(Sr16(Part+6)==0 && Sr16(Part+4)==Host)))return EFI_COMPROMISED_DATA;
  Free=Sr32(Part+12);UINT32 Cached=Sr32(Part+16);
  if(Free<32 || Free>Length || Cached<Free || Cached>Length)return EFI_COMPROMISED_DATA;
  UINTN At=32;BOOLEAN Found=FALSE;
  while(At<Free){
    if(!SrRange(Free,At,16))return EFI_COMPROMISED_DATA;
    CONST UINT8 *E=Part+At;UINT32 N=Sr32(E+4);UINT16 Pad=Sr16(E+8),Hdr=Sr16(E+10);
    if(Sr16(E)!=0xa5a5 || Pad>N || N==0 || !SrRange(Free,At+16,Hdr) ||
       !SrRange(Free,At+16+Hdr,N))return EFI_COMPROMISED_DATA;
    if(Sr16(E+2)==Item){
      if(Found)return EFI_COMPROMISED_DATA;
      Found=TRUE;*Value=E+16+Hdr;*Size=N-Pad;
    }
    At+=16+Hdr+N;
  }
  return Found?EFI_SUCCESS:EFI_NOT_FOUND;
}
STATIC EFI_STATUS SrRing(CONST UINT8 *Descriptor,UINTN DescriptorBytes,UINTN FifoBytes,
                         BOOLEAN Rx,UINT32 *Tail,UINT32 *Head,UINT32 *Available)
{
  if(Descriptor==NULL || DescriptorBytes!=32 || FifoBytes<16 || FifoBytes>65536 ||
     (FifoBytes&7)!=0 || Tail==NULL || Head==NULL || Available==NULL)return EFI_COMPROMISED_DATA;
  *Tail=Sr32(Descriptor+(Rx?8:0));*Head=Sr32(Descriptor+(Rx?12:4));
  if(*Tail>=FifoBytes || *Head>=FifoBytes || ((*Tail|*Head)&7)!=0)return EFI_COMPROMISED_DATA;
  *Available=*Head>=*Tail?*Head-*Tail:(UINT32)FifoBytes-*Tail+*Head;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS SrPeek(CONST UINT8 *Fifo,UINTN Bytes,UINT32 Tail,UINT32 Available,
                         UINT8 *Output,UINTN Count)
{
  if(Fifo==NULL || Output==NULL || Tail>=Bytes || Available>=Bytes || Count>Available)return EFI_NOT_READY;
  for(UINTN I=0;I<Count;I++)Output[I]=Fifo[((UINTN)Tail+I)%Bytes];
  return EFI_SUCCESS;
}
#endif
