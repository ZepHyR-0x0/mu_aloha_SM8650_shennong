// SPDX-License-Identifier: BSD-2-Clause-Patent
// Private FAT32 writes are bounded by the separately allocated RAM extent.
// No BlockIo/DiskIo write, native media method, or address from the host is used.
typedef struct {
  UINT8 *Disk, *Entry;
  UINTN Bytes;
  UINT32 FatSectors, Clusters, ClusterBytes, DataSector, Root, Cursor;
  UINT32 Head, Tail, Current, Expected, Received, ExpectedCrc, Crc, OldHead;
  BOOLEAN Active, Existing;
  CHAR8 Name[96];
  UINT8 Alias[11], Slots;
} RAM_FILE_STORE;

STATIC UINT16 Rf16(CONST UINT8 *P){return (UINT16)(P[0]|((UINT16)P[1]<<8));}
STATIC UINT32 Rf32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC VOID RfPut16(UINT8 *P,UINT16 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);}
STATIC VOID RfPut32(UINT8 *P,UINT32 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);P[2]=(UINT8)(V>>16);P[3]=(UINT8)(V>>24);}
STATIC CHAR8 RfUpper(CHAR8 C){return C>='a' && C<='z'?(CHAR8)(C-'a'+'A'):C;}
STATIC BOOLEAN RfEqual(CONST CHAR8 *A,CONST CHAR8 *B){while(*A && RfUpper(*A)==RfUpper(*B)){A++;B++;}return *A==0 && *B==0;}
STATIC UINT32 RfCrc(UINT32 C,CONST UINT8 *P,UINTN N){
  while(N--){C^=*P++;for(UINTN I=0;I<8;I++)C=(C>>1)^(0xEDB88320u & (0u-(C&1)));}return C;
}
STATIC BOOLEAN RfCluster(RAM_FILE_STORE *S,UINT32 C){return C>=2 && C<S->Clusters+2;}
STATIC UINT8 *RfData(RAM_FILE_STORE *S,UINT32 C){
  if(!RfCluster(S,C))return NULL;
  return S->Disk+(UINTN)S->DataSector*512+(UINTN)(C-2)*S->ClusterBytes;
}
STATIC UINT32 RfNext(RAM_FILE_STORE *S,UINT32 C){
  UINTN Off=32*512+(UINTN)C*4;UINT32 A,B;
  if(!RfCluster(S,C))return MAX_UINT32;
  A=Rf32(S->Disk+Off)&0x0FFFFFFF;B=Rf32(S->Disk+Off+(UINTN)S->FatSectors*512)&0x0FFFFFFF;
  return A==B?A:MAX_UINT32;
}
STATIC VOID RfFat(RAM_FILE_STORE *S,UINT32 C,UINT32 V){
  for(UINTN I=0;I<2;I++){
    UINT8 *P=S->Disk+(32+(UINTN)I*S->FatSectors)*512+(UINTN)C*4;
    RfPut32(P,(Rf32(P)&0xF0000000)|(V&0x0FFFFFFF));
  }
  // Allocation hints are optional. Unknown avoids stale free counts after
  // a private upload or abort; consumers may recompute them from the FAT.
  RfPut32(S->Disk+512+488,MAX_UINT32);RfPut32(S->Disk+512+492,MAX_UINT32);
  RfPut32(S->Disk+7*512+488,MAX_UINT32);RfPut32(S->Disk+7*512+492,MAX_UINT32);
}
STATIC EFI_STATUS RfChain(RAM_FILE_STORE *S,UINT32 Head);
STATIC EFI_STATUS RfInit(RAM_FILE_STORE *S,UINT8 *Disk,UINTN Bytes){
  UINT32 Total,Fat=1,Next,Count;UINT8 Spc;
  if(S==NULL || Disk==NULL || Bytes<64*1024*1024ULL || Bytes>8*1024*1024*1024ULL || Bytes%(1024*1024)!=0)return EFI_INVALID_PARAMETER;
  if(S->Active)return EFI_ALREADY_STARTED;
  Total=(UINT32)(Bytes/512);Spc=Bytes>=512*1024*1024ULL?8:Bytes>=256*1024*1024ULL?4:Bytes>=128*1024*1024ULL?2:1;
  for(UINTN I=0;I<16;I++){Count=(Total-32-2*Fat)/Spc;Next=((Count+2)*4+511)/512;if(Next==Fat)break;Fat=I==15?MAX(Fat,Next):Next;}
  Count=(Total-32-2*Fat)/Spc;
  if(Rf16(Disk+11)!=512 || Disk[13]!=Spc || Rf16(Disk+14)!=32 || Disk[16]!=2 ||
     Rf32(Disk+32)!=Total || Rf32(Disk+36)!=Fat || Rf32(Disk+44)!=2 ||
     Disk[510]!=0x55 || Disk[511]!=0xAA || ((UINT64)(Count+2)*4)>((UINT64)Fat*512))return EFI_COMPROMISED_DATA;
  ZeroMem(S,sizeof(*S));S->Disk=Disk;S->Bytes=Bytes;S->FatSectors=Fat;S->Clusters=Count;
  S->ClusterBytes=Spc*512;S->DataSector=32+2*Fat;S->Root=2;S->Cursor=3;
  return RfChain(S,2);
}
STATIC EFI_STATUS RfAllocate(RAM_FILE_STORE *S,UINT32 *Result){
  for(UINT32 I=0;I<S->Clusters;I++){
    UINT32 C=S->Cursor++;if(S->Cursor>=S->Clusters+2)S->Cursor=2;
    if(RfNext(S,C)!=0)continue;
    RfFat(S,C,0x0FFFFFFF);ZeroMem(RfData(S,C),S->ClusterBytes);*Result=C;return EFI_SUCCESS;
  }
  return EFI_VOLUME_FULL;
}
STATIC EFI_STATUS RfChain(RAM_FILE_STORE *S,UINT32 Head){
  UINT32 Slow=Head,Fast=Head;
  if(Head==0)return EFI_SUCCESS;
  for(UINT32 I=0;I<S->Clusters;I++){
    if(!RfCluster(S,Slow))return EFI_COMPROMISED_DATA;
    Slow=RfNext(S,Slow);if(Slow>=0x0FFFFFF8 && Slow!=MAX_UINT32)return EFI_SUCCESS;
    for(UINTN J=0;J<2;J++){
      if(Fast>=0x0FFFFFF8 && Fast!=MAX_UINT32)break;
      if(!RfCluster(S,Fast))return EFI_COMPROMISED_DATA;
      Fast=RfNext(S,Fast);
    }
    if(Slow==Fast)return EFI_COMPROMISED_DATA;
  }
  return EFI_COMPROMISED_DATA;
}
STATIC VOID RfFree(RAM_FILE_STORE *S,UINT32 Head){
  for(UINT32 I=0;I<S->Clusters && RfCluster(S,Head);I++){
    UINT32 Next=RfNext(S,Head);RfFat(S,Head,0);Head=Next;
  }
}
STATIC UINT8 RfChecksum(CONST UINT8 *Alias){UINT8 C=0;for(UINTN I=0;I<11;I++)C=(UINT8)(((C&1)<<7)+(C>>1)+Alias[I]);return C;}
STATIC UINT32 RfHead(CONST UINT8 *Entry){return ((UINT32)Rf16(Entry+20)<<16)|Rf16(Entry+26);}
STATIC EFI_STATUS RfFind(RAM_FILE_STORE *S,UINT32 Dir,CONST CHAR8 *Name,CONST UINT8 *Alias,UINT8 **Entry){
  CHAR8 Long[261];UINT8 Seq=0,Checksum=0;BOOLEAN Valid=FALSE;
  STATIC CONST UINT8 Positions[]={1,3,5,7,9,14,16,18,20,22,24,28,30};
  if(!RfCluster(S,Dir))return EFI_COMPROMISED_DATA;
  EFI_STATUS Status=RfChain(S,Dir);if(EFI_ERROR(Status))return Status;
  for(UINT32 Steps=0;Steps<S->Clusters && RfCluster(S,Dir);Steps++,Dir=RfNext(S,Dir)){
    UINT8 *Base=RfData(S,Dir);
    for(UINTN Off=0;Off<S->ClusterBytes;Off+=32){
      UINT8 *P=Base+Off;
      if(P[0]==0)return EFI_NOT_FOUND;
      if(P[0]==0xE5){Valid=FALSE;continue;}
      if(P[11]==0x0F){
        UINT8 Ord=P[0]&31;
        if(P[0]&0x40){ZeroMem(Long,sizeof(Long));Seq=Ord;Checksum=P[13];Valid=Ord>0 && Ord<=20;}
        if(!Valid || Ord!=Seq || P[13]!=Checksum || P[12]!=0 || Rf16(P+26)!=0){Valid=FALSE;continue;}
        for(UINTN J=0;J<13;J++){
          UINT16 C=Rf16(P+Positions[J]);UINTN At=(Ord-1)*13+J;
          if(C!=0 && C!=0xFFFF && C>127){Valid=FALSE;break;}
          Long[At]=C==0 || C==0xFFFF?0:(CHAR8)C;
        }
        Seq--;continue;
      }
      if((P[11]&8)==0){
        CHAR8 Short[13];UINTN N=0;
        for(UINTN J=0;J<8 && P[J]!=' ';J++)Short[N++]=(CHAR8)P[J];
        if(P[8]!=' '){Short[N++]='.';for(UINTN J=8;J<11 && P[J]!=' ';J++)Short[N++]=(CHAR8)P[J];}Short[N]=0;
        if((Alias!=NULL && CompareMem(P,Alias,11)==0) ||
           (Name!=NULL && (RfEqual(Short,Name) || (Valid && Seq==0 && RfChecksum(P)==Checksum && RfEqual(Long,Name))))){*Entry=P;return EFI_SUCCESS;}
      }
      Valid=FALSE;
    }
  }
  return EFI_NOT_FOUND;
}
STATIC EFI_STATUS RfSlots(RAM_FILE_STORE *S,UINT32 Dir,UINT8 Count,UINT8 **Entry){
  UINT32 Last=Dir;EFI_STATUS Status=RfChain(S,Dir);if(EFI_ERROR(Status))return Status;
  for(UINT32 Steps=0;Steps<S->Clusters && RfCluster(S,Dir);Steps++,Dir=RfNext(S,Dir)){
    UINT8 *P=RfData(S,Dir);UINTN Run=0,EndAt=S->ClusterBytes;BOOLEAN End=FALSE;Last=Dir;
    for(UINTN Off=0;Off<S->ClusterBytes;Off+=32){
      if(P[Off]==0 && !End){End=TRUE;EndAt=Off;}
      if(End || P[Off]==0xE5)Run++;else Run=0;
      if(Run==Count){
        if(End && Off+32<S->ClusterBytes)P[Off+32]=0;
        *Entry=P+Off-(Count-1)*32;return EFI_SUCCESS;
      }
    }
    // A zero terminator in the old tail would hide the newly appended
    // cluster. Turn unused tail slots into deleted entries before extending.
    for(UINTN Off=EndAt;Off<S->ClusterBytes;Off+=32){ZeroMem(P+Off,32);P[Off]=0xE5;}
  }
  UINT32 Added;Status=RfAllocate(S,&Added);if(EFI_ERROR(Status))return Status;
  RfFat(S,Last,Added);*Entry=RfData(S,Added);return EFI_SUCCESS;
}
STATIC EFI_STATUS RfAlias(RAM_FILE_STORE *S,UINT32 Dir,CONST CHAR8 *Name,UINT8 *Alias,UINT8 *Slots){
  UINTN N=AsciiStrLen(Name),Dot=N,Stem,Ext;BOOLEAN Short=TRUE;UINT8 *Found=NULL;
  for(UINTN I=0;I<N;I++)if(Name[I]=='.')Dot=I;
  Stem=Dot;Ext=Dot<N?N-Dot-1:0;
  if(Stem==0 || Stem>8 || Ext>3)Short=FALSE;
  for(UINTN I=0;I<N;I++)if(Name[I]!='.' && !((Name[I]>='a' && Name[I]<='z') || (Name[I]>='A' && Name[I]<='Z') || (Name[I]>='0' && Name[I]<='9') || Name[I]=='_' || Name[I]=='-' || Name[I]=='~'))Short=FALSE;
  for(UINTN I=0;I<Stem;I++)if(Name[I]=='.')Short=FALSE;
  for(UINTN Try=0;Try<99;Try++){
    for(UINTN I=0;I<11;I++)Alias[I]=' ';
    UINTN Keep=Short && Try==0?Stem:MIN(Stem,(UINTN)(Try<10?6:5));
    for(UINTN I=0;I<Keep;I++){
      CHAR8 C=RfUpper(Name[I]);Alias[I]=(UINT8)((C>='A'&&C<='Z')||(C>='0'&&C<='9')||C=='_'||C=='-'||C=='~'?C:'_');
    }
    if(!Short || Try!=0){UINTN V=Short?Try:Try+1;Alias[Keep++]='~';if(V>=10)Alias[Keep++]=(UINT8)('0'+V/10);Alias[Keep]=(UINT8)('0'+V%10);}
    for(UINTN I=0;I<MIN(Ext,(UINTN)3);I++){
      CHAR8 C=RfUpper(Name[Dot+1+I]);Alias[8+I]=(UINT8)((C>='A'&&C<='Z')||(C>='0'&&C<='9')||C=='_'||C=='-'||C=='~'?C:'_');
    }
    EFI_STATUS Status=RfFind(S,Dir,NULL,Alias,&Found);
    if(Status==EFI_NOT_FOUND){*Slots=(UINT8)(1+((Short && Try==0)?0:(N+12)/13));return EFI_SUCCESS;}
    if(EFI_ERROR(Status))return Status;
  }
  return EFI_VOLUME_FULL;
}
STATIC VOID RfEntry(UINT8 *P,CONST CHAR8 *Name,CONST UINT8 *Alias,UINT8 Slots,UINT8 Attr,UINT32 Head,UINT32 Bytes){
  STATIC CONST UINT8 Positions[]={1,3,5,7,9,14,16,18,20,22,24,28,30};
  UINTN Length=AsciiStrLen(Name);UINT8 Checksum=RfChecksum(Alias);
  for(UINTN I=0;I<Slots-1;I++){
    UINT8 *L=P+I*32;UINTN Ord=Slots-1-I;ZeroMem(L,32);L[0]=(UINT8)(Ord|(I==0?0x40:0));L[11]=0x0F;L[13]=Checksum;
    for(UINTN J=0;J<13;J++){UINTN At=(Ord-1)*13+J;RfPut16(L+Positions[J],At<Length?(UINT8)Name[At]:At==Length?0:0xFFFF);}
  }
  P+=(Slots-1)*32;ZeroMem(P,32);CopyMem(P,Alias,11);P[11]=Attr;RfPut16(P+20,(UINT16)(Head>>16));RfPut16(P+26,(UINT16)Head);RfPut32(P+28,Bytes);
}
STATIC EFI_STATUS RfDirectory(RAM_FILE_STORE *S,UINT32 Parent,CONST CHAR8 *Name,UINT32 *Dir){
  UINT8 *Entry,Alias[11],Slots;EFI_STATUS Status=RfFind(S,Parent,Name,NULL,&Entry);
  if(!EFI_ERROR(Status)){if(!(Entry[11]&16))return EFI_ACCESS_DENIED;*Dir=RfHead(Entry);return RfCluster(S,*Dir)?RfChain(S,*Dir):EFI_COMPROMISED_DATA;}
  if(Status!=EFI_NOT_FOUND)return Status;
  Status=RfAlias(S,Parent,Name,Alias,&Slots);if(EFI_ERROR(Status))return Status;
  Status=RfSlots(S,Parent,Slots,&Entry);if(EFI_ERROR(Status))return Status;
  Status=RfAllocate(S,Dir);if(EFI_ERROR(Status))return Status;
  UINT8 *Data=RfData(S,*Dir);STATIC CONST UINT8 Dot[11]={'.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
  STATIC CONST UINT8 DotDot[11]={'.','.',' ',' ',' ',' ',' ',' ',' ',' ',' '};
  RfEntry(Data,".",Dot,1,16,*Dir,0);RfEntry(Data+32,"..",DotDot,1,16,Parent==S->Root?0:Parent,0);
  RfEntry(Entry,Name,Alias,Slots,16,*Dir,0);return EFI_SUCCESS;
}
STATIC EFI_STATUS RfBegin(RAM_FILE_STORE *S,CONST CHAR8 *Path,UINT32 Bytes,UINT32 Crc){
  CHAR8 Parts[96];UINTN N,Start=0;UINT32 Dir=S->Root;EFI_STATUS Status;
  if(S->Active)return EFI_ALREADY_STARTED;
  if(Path==NULL || (N=AsciiStrLen(Path))==0 || N>=sizeof(Parts) || Bytes>S->Bytes)return EFI_INVALID_PARAMETER;
  CopyMem(Parts,Path,N+1);
  if(Parts[0]=='\\' || Parts[0]=='/')Start=1;
  // Validate every component before creating any directory.
  for(UINTN I=Start,Part=Start;I<=N;I++){
    CHAR8 C=Parts[I];
    if(C=='\\'||C=='/'||C==0){
      if(I==Part || (I-Part==1&&Parts[Part]=='.') || (I-Part==2&&Parts[Part]=='.'&&Parts[Part+1]=='.') || Parts[I-1]==' ' || Parts[I-1]=='.')return EFI_INVALID_PARAMETER;
      Part=I+1;
    }else if((UINT8)C<32 || (UINT8)C>126 || C==':' || C=='*' || C=='?' || C=='"' || C=='<' || C=='>' || C=='|')return EFI_INVALID_PARAMETER;
  }
  for(UINTN I=Start;I<=N;I++){
    CHAR8 C=Parts[I];
    if(C=='\\' || C=='/' || C==0){
      Parts[I]=0;
      if(I==Start || RfEqual(Parts+Start,".") || RfEqual(Parts+Start,"..") || Parts[I-1]==' ' || Parts[I-1]=='.')return EFI_INVALID_PARAMETER;
      if(C!=0){Status=RfDirectory(S,Dir,Parts+Start,&Dir);if(EFI_ERROR(Status))return Status;Start=I+1;}
    } else if((UINT8)C<32 || (UINT8)C>126 || C==':' || C=='*' || C=='?' || C=='"' || C=='<' || C=='>' || C=='|')return EFI_INVALID_PARAMETER;
  }
  CopyMem(S->Name,Parts+Start,N-Start+1);S->Entry=NULL;S->OldHead=0;S->Existing=FALSE;
  Status=RfFind(S,Dir,S->Name,NULL,&S->Entry);
  if(!EFI_ERROR(Status)){
    if(S->Entry[11]&(16|1))return EFI_ACCESS_DENIED;
    S->OldHead=RfHead(S->Entry);Status=RfChain(S,S->OldHead);if(EFI_ERROR(Status))return Status;S->Existing=TRUE;
  } else {
    if(Status!=EFI_NOT_FOUND)return Status;
    Status=RfAlias(S,Dir,S->Name,S->Alias,&S->Slots);if(EFI_ERROR(Status))return Status;
    Status=RfSlots(S,Dir,S->Slots,&S->Entry);if(EFI_ERROR(Status))return Status;
  }
  S->Head=S->Tail=S->Current=0;S->Expected=Bytes;S->Received=0;S->ExpectedCrc=Crc;S->Crc=MAX_UINT32;
  for(UINT64 I=0;I<((UINT64)Bytes+S->ClusterBytes-1)/S->ClusterBytes;I++){
    UINT32 C;Status=RfAllocate(S,&C);
    if(EFI_ERROR(Status)){RfFree(S,S->Head);S->Head=0;return Status;}
    if(S->Tail!=0)RfFat(S,S->Tail,C);else S->Head=C;S->Tail=C;
  }
  S->Current=S->Head;S->Active=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS RfWrite(RAM_FILE_STORE *S,CONST UINT8 *Data,UINTN Bytes){
  if(!S->Active)return EFI_NOT_READY;
  if(Data==NULL || Bytes==0 || Bytes>S->Expected-S->Received)return EFI_INVALID_PARAMETER;
  while(Bytes){
    UINTN Offset=S->Received%S->ClusterBytes,N=MIN(Bytes,S->ClusterBytes-Offset);
    UINT8 *Target=RfData(S,S->Current);if(Target==NULL)return EFI_COMPROMISED_DATA;
    CopyMem(Target+Offset,Data,N);S->Crc=RfCrc(S->Crc,Data,N);S->Received+=(UINT32)N;Data+=N;Bytes-=N;
    if(S->Received%S->ClusterBytes==0 && S->Received<S->Expected)S->Current=RfNext(S,S->Current);
  }
  return EFI_SUCCESS;
}
STATIC VOID RfAbort(RAM_FILE_STORE *S){if(S->Active)RfFree(S,S->Head);S->Active=FALSE;S->Head=S->Tail=S->Current=0;}
STATIC EFI_STATUS RfFinish(RAM_FILE_STORE *S){
  if(!S->Active)return EFI_NOT_READY;
  if(S->Received!=S->Expected)return EFI_NOT_READY;
  if((~S->Crc)!=S->ExpectedCrc){RfAbort(S);return EFI_CRC_ERROR;}
  if(S->Existing){RfPut16(S->Entry+20,(UINT16)(S->Head>>16));RfPut16(S->Entry+26,(UINT16)S->Head);RfPut32(S->Entry+28,S->Expected);}
  else RfEntry(S->Entry,S->Name,S->Alias,S->Slots,32,S->Head,S->Expected);
  RfFree(S,S->OldHead);S->Active=FALSE;return EFI_SUCCESS;
}
