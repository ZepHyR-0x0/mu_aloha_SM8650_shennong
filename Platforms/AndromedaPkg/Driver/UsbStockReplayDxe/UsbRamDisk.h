// SPDX-License-Identifier: BSD-2-Clause-Patent
// Private, volatile FAT32 superfloppy. USB is the only writable view. Local
// BlockIo is read-only and removed before USB takes ownership of the volume.
// No storage-guard exemption and no UFS/DiskIo writer are involved.
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
STATIC UINT32 mRamRequestedMiB=512;
STATIC UINTN mRamBytes;
#define USB_RAM_DISK_BYTES mRamBytes
#define USB_RAM_DISK_PAGES EFI_SIZE_TO_PAGES(mRamBytes)
STATIC UINT8 *mRamDisk;
STATIC EFI_HANDLE mRamHandle;
STATIC BOOLEAN mRamUsbOwned;
STATIC EFI_BLOCK_IO_MEDIA mRamMedia;
STATIC EFI_BLOCK_IO_PROTOCOL mRamBlock;
#pragma pack(push,1)
STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} mRamPath = {
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},
   USB_RAM_DISK_DEVICE_GUID},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}}
};
#pragma pack(pop)
STATIC VOID RamPut16 (UINT8 *P, UINT16 N) { P[0]=(UINT8)N;P[1]=(UINT8)(N>>8); }
STATIC VOID RamPut32 (UINT8 *P, UINT32 N) {
  P[0]=(UINT8)N;P[1]=(UINT8)(N>>8);P[2]=(UINT8)(N>>16);P[3]=(UINT8)(N>>24);
}
STATIC VOID RamFormat (UINT8 *Disk)
{
  UINT8 *B=Disk,*Info=Disk+512,*Fat,*Root;
  UINTN I;
  UINT32 Total=(UINT32)(mRamBytes/512), FatSectors=1, Clusters, Next;
  UINT8 Spc=mRamRequestedMiB>=512?8:mRamRequestedMiB>=256?4:mRamRequestedMiB>=128?2:1;
  // Geometry is derived from the allocated extent, never from a host-edited BPB.
  for (I=0;I<16;I++) {
    Clusters=(Total-32-2*FatSectors)/Spc;
    Next=((Clusters+2)*4+511)/512;
    if (Next==FatSectors) break;
    // A one-sector oscillation is harmless if the larger FAT is retained.
    if (I==15) { FatSectors=MAX(FatSectors,Next);break; }
    FatSectors=Next;
  }
  Clusters=(Total-32-2*FatSectors)/Spc;
  ZeroMem(Disk,USB_RAM_DISK_BYTES);
  B[0]=0xEB;B[1]=0x58;B[2]=0x90;CopyMem(B+3,"MSWIN4.1",8);
  RamPut16(B+11,512);B[13]=Spc;RamPut16(B+14,32);B[16]=2;B[21]=0xF8;
  RamPut16(B+24,63);RamPut16(B+26,255);RamPut32(B+32,Total);
  RamPut32(B+36,FatSectors);RamPut32(B+44,2);RamPut16(B+48,1);RamPut16(B+50,6);
  B[64]=0x80;B[66]=0x29;RamPut32(B+67,0xB3275120);
  CopyMem(B+71,"MU_RAMDISK ",11);CopyMem(B+82,"FAT32   ",8);B[510]=0x55;B[511]=0xAA;
  RamPut32(Info,0x41615252);RamPut32(Info+484,0x61417272);
  RamPut32(Info+488,Clusters-1);RamPut32(Info+492,3);RamPut32(Info+508,0xAA550000);
  CopyMem(Disk+6*512,B,512);CopyMem(Disk+7*512,Info,512);
  for (I=0;I<2;I++) {
    Fat=Disk+(32+I*FatSectors)*512;
    RamPut32(Fat,0x0FFFFFF8);RamPut32(Fat+4,0x0FFFFFFF);RamPut32(Fat+8,0x0FFFFFFF);
  }
  Root=Disk+(32+2*FatSectors)*512;CopyMem(Root,"MU_RAMDISK ",11);Root[11]=8;
}
STATIC EFI_STATUS EFIAPI RamReset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Extended)
{ (VOID)Extended;return This==&mRamBlock && mRamDisk!=NULL?EFI_SUCCESS:EFI_NO_MEDIA; }
STATIC EFI_STATUS EFIAPI RamRead (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id,
                                  EFI_LBA Lba, UINTN Bytes, VOID *Buffer)
{
  if (This!=&mRamBlock || mRamDisk==NULL || mRamUsbOwned) return EFI_NO_MEDIA;
  if (Id!=mRamMedia.MediaId) return EFI_MEDIA_CHANGED;
  if (Bytes%512!=0) return EFI_BAD_BUFFER_SIZE;
  if (Bytes==0) return EFI_SUCCESS;
  if (Buffer==NULL || Lba>=USB_RAM_DISK_BYTES/512 || Bytes>USB_RAM_DISK_BYTES-(UINTN)Lba*512)
    return EFI_INVALID_PARAMETER;
  CopyMem(Buffer,mRamDisk+(UINTN)Lba*512,Bytes);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI RamWrite (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id,
                                   EFI_LBA Lba, UINTN Bytes, VOID *Buffer)
{ (VOID)This;(VOID)Id;(VOID)Lba;(VOID)Bytes;(VOID)Buffer;return EFI_WRITE_PROTECTED; }
STATIC EFI_STATUS EFIAPI RamFlush (EFI_BLOCK_IO_PROTOCOL *This)
{ (VOID)This;return EFI_SUCCESS; }
STATIC EFI_STATUS RamUnpublish (VOID)
{
  EFI_STATUS S;
  if (mRamHandle==NULL) return EFI_SUCCESS;
  S=gBS->DisconnectController(mRamHandle,NULL,NULL);
  if (EFI_ERROR(S)) return S;
  S=gBS->UninstallMultipleProtocolInterfaces(mRamHandle,&gEfiBlockIoProtocolGuid,&mRamBlock,
      &gEfiDevicePathProtocolGuid,&mRamPath,NULL);
  if (!EFI_ERROR(S)) mRamHandle=NULL;
  return S;
}
STATIC EFI_STATUS RamPublish (VOID)
{
  EFI_STATUS S;
  if (mRamDisk==NULL || mRamUsbOwned || mRamHandle!=NULL) return EFI_SUCCESS;
  ZeroMem(&mRamMedia,sizeof(mRamMedia));
  mRamMedia.MediaId=0xB327;mRamMedia.MediaPresent=TRUE;mRamMedia.ReadOnly=TRUE;
  mRamMedia.LogicalPartition=TRUE;
  mRamMedia.BlockSize=512;mRamMedia.LastBlock=USB_RAM_DISK_BYTES/512-1;
  mRamBlock=(EFI_BLOCK_IO_PROTOCOL){EFI_BLOCK_IO_PROTOCOL_REVISION,&mRamMedia,RamReset,RamRead,RamWrite,RamFlush};
  S=gBS->InstallMultipleProtocolInterfaces(&mRamHandle,&gEfiBlockIoProtocolGuid,&mRamBlock,
      &gEfiDevicePathProtocolGuid,&mRamPath,NULL);
  if (!EFI_ERROR(S)) (VOID)gBS->ConnectController(mRamHandle,NULL,NULL,TRUE);
  return S;
}
STATIC EFI_STATUS RamAllocate (VOID)
{
  EFI_PHYSICAL_ADDRESS Address=0;
  EFI_STATUS S;
  if (mRamDisk!=NULL) return EFI_SUCCESS;
  UINTN Bytes=(UINTN)mRamRequestedMiB*1024*1024;
  S=gBS->AllocatePages(AllocateAnyPages,EfiBootServicesData,EFI_SIZE_TO_PAGES(Bytes),&Address);
  if (EFI_ERROR(S)) return S;
  mRamBytes=Bytes;
  mRamDisk=(UINT8 *)(UINTN)Address;RamFormat(mRamDisk);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS RamRelease (VOID)
{
  EFI_STATUS S;
  if (mRamUsbOwned) return EFI_ACCESS_DENIED;
  S=RamUnpublish();if (EFI_ERROR(S)) return S;
  if (mRamDisk==NULL) return EFI_SUCCESS;
  S=gBS->FreePages((UINTN)mRamDisk,USB_RAM_DISK_PAGES);
  if (!EFI_ERROR(S)) { mRamDisk=NULL;mRamBytes=0; }
  return S;
}
STATIC EFI_STATUS EFIAPI StockSetRamSize (USB_BRINGUP_PROTOCOL *This, UINT32 MiB)
{
  (VOID)This;
  if (StockCurrentTpl()!=TPL_APPLICATION || mPolling) return EFI_NOT_READY;
  if (MiB<64 || MiB>2048) return EFI_INVALID_PARAMETER;
  if (mRamDisk!=NULL && MiB!=mRamBytes/(1024*1024)) return EFI_ACCESS_DENIED;
  mRamRequestedMiB=MiB;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI StockGetRamSize (USB_BRINGUP_PROTOCOL *This, UINT32 *MiB, BOOLEAN *Allocated)
{
  (VOID)This;if (MiB==NULL || Allocated==NULL) return EFI_INVALID_PARAMETER;
  *MiB=mRamRequestedMiB;*Allocated=mRamDisk!=NULL;return EFI_SUCCESS;
}
