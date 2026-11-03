/** Device list clients and read-only sensor/GNSS firmware prerequisites.
    SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformTestLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_GUID mSurveyGuid=WIRELESS_SURVEY_GUID;
STATIC VOID DeviceReport (PLATFORM_TEST_OUTPUT Output, VOID *Context, CONST CHAR16 *Format, ...)
{
  CHAR16 Wide[256];CHAR8 Line[256];VA_LIST Args;
  VA_START(Args,Format);UnicodeVSPrint(Wide,sizeof(Wide),Format,Args);VA_END(Args);
  if(EFI_ERROR(UnicodeStrToAsciiStrS(Wide,Line,sizeof(Line)))) return;
  SerialPortWrite((UINT8 *)Line,AsciiStrLen(Line));
  if(Output!=NULL) Output(Line,Context);
}
STATIC EFI_STATUS Survey (WIRELESS_SURVEY_PROTOCOL **Protocol)
{
  EFI_STATUS Status=gBS->LocateProtocol(&mSurveyGuid,NULL,(VOID **)Protocol);
  if(EFI_ERROR(Status)) return Status;
  if(*Protocol==NULL || (*Protocol)->Revision!=1 || (*Protocol)->StartWifi==NULL ||
     (*Protocol)->RefreshBluetooth==NULL || (*Protocol)->GetResult==NULL) return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
EFI_STATUS PlatformWirelessGetResult (WIRELESS_SURVEY_RESULT *Result)
{
  WIRELESS_SURVEY_PROTOCOL *Protocol=NULL;EFI_STATUS Status;
  if(Result==NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(Result,sizeof(*Result));Result->WifiStatus=Result->BluetoothStatus=EFI_NOT_FOUND;
  Status=Survey(&Protocol);
  return EFI_ERROR(Status)?Status:Protocol->GetResult(Protocol,Result);
}
EFI_STATUS PlatformWirelessScan (VOID)
{
  WIRELESS_SURVEY_PROTOCOL *Protocol=NULL;EFI_STATUS Status=Survey(&Protocol);
  return EFI_ERROR(Status)?Status:Protocol->StartWifi(Protocol);
}
EFI_STATUS PlatformBluetoothRefresh (VOID)
{
  WIRELESS_SURVEY_PROTOCOL *Protocol=NULL;EFI_STATUS Status=Survey(&Protocol);
  return EFI_ERROR(Status)?Status:Protocol->RefreshBluetooth(Protocol);
}
EFI_STATUS PlatformWirelessTest (PLATFORM_TEST_OUTPUT Output, VOID *Context, WIRELESS_SURVEY_RESULT *Result)
{
  EFI_STATUS Status;UINTN I;
  if(Result==NULL) return EFI_INVALID_PARAMETER;
  Status=PlatformWirelessGetResult(Result);
  DeviceReport(Output,Context,L"[WIRELESS] snapshot -> %r WiFi=%r pending=%u providers=%u legacy=%u networks=%u\n",
    Status,Result->WifiStatus,Result->WifiPending,Result->WifiProviders,Result->LegacyWifiProviders,Result->NetworkCount);
  if(EFI_ERROR(Status)) return Status;
  for(I=0;I<MIN(Result->NetworkCount,WIRELESS_SURVEY_MAX);I++)
    DeviceReport(Output,Context,L"[WIRELESS] network %u SSID=%s quality=%u bytes=%u\n",I,
      Result->Networks[I].Name,Result->Networks[I].Quality,Result->Networks[I].SsidLength);
  DeviceReport(Output,Context,L"[WIRELESS] Bluetooth cache=%r classic=%u BLE=%u devices=%u truncated=%u\n",
    Result->BluetoothStatus,Result->BluetoothProviders,Result->BleProviders,Result->DeviceCount,Result->BluetoothTruncated);
  for(I=0;I<MIN(Result->DeviceCount,WIRELESS_SURVEY_MAX);I++) {
    WIRELESS_DEVICE *D=&Result->Devices[I];
    DeviceReport(Output,Context,L"[WIRELESS] device %u LE=%u type=%u addr=%02x:%02x:%02x:%02x:%02x:%02x name=%s\n",
      I,D->LowEnergy,D->AddressType,D->Address[5],D->Address[4],D->Address[3],D->Address[2],D->Address[1],D->Address[0],D->Name);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ReadFirmware (EFI_FILE_PROTOCOL *Root, CONST CHAR16 *Name, BOOLEAN Elf,
                               UINT32 *Bytes, UINT32 *Crc)
{
  EFI_FILE_PROTOCOL *File=NULL;
  EFI_FILE_INFO *Info=NULL;
  UINT8 *Data=NULL;
  UINTN Size=0,ReadSize;
  EFI_STATUS Status;
  *Bytes=*Crc=0;
  Status=Root->Open(Root,&File,(CHAR16 *)Name,EFI_FILE_MODE_READ,0);
  if(EFI_ERROR(Status)) return Status;
  if(File==NULL) return EFI_COMPROMISED_DATA;
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&Size,NULL);
  if(Status!=EFI_BUFFER_TOO_SMALL || Size<SIZE_OF_EFI_FILE_INFO || Size>4096) {Status=EFI_COMPROMISED_DATA;goto Done;}
  Info=AllocatePool(Size);
  if(Info==NULL) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&Size,Info);
  if(EFI_ERROR(Status)) goto Done;
  if(Info->FileSize==0 || Info->FileSize>256*1024 || (Info->Attribute&EFI_FILE_DIRECTORY)!=0) {Status=EFI_BAD_BUFFER_SIZE;goto Done;}
  ReadSize=(UINTN)Info->FileSize;
  Data=AllocatePool(ReadSize);
  if(Data==NULL) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  Status=File->Read(File,&ReadSize,Data);
  if(EFI_ERROR(Status)) goto Done;
  if(ReadSize!=Info->FileSize || (Elf && (ReadSize<64 || ReadUnaligned32((UINT32 *)Data)!=0x464c457f ||
      (Data[4]!=1 && Data[4]!=2) || Data[5]!=1))) {Status=EFI_COMPROMISED_DATA;goto Done;}
  Status=gBS->CalculateCrc32(Data,ReadSize,Crc);
  if(!EFI_ERROR(Status)) *Bytes=(UINT32)ReadSize;
Done:
  if(Data!=NULL) FreePool(Data);
  if(Info!=NULL) FreePool(Info);
  File->Close(File);
  return Status;
}

EFI_STATUS PlatformSensorTest (PLATFORM_TEST_OUTPUT Output, VOID *Context, PLATFORM_SENSOR_RESULT *Result)
{
  EFI_HANDLE *Handles=NULL;UINTN Count=0,I;EFI_STATUS Status;
  if(Result==NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(Result,sizeof(*Result));
  Result->AdspStatus=Result->SensorConfigStatus=Result->ModemStatus=EFI_NOT_FOUND;
  Result->GyroStatus=Result->GpsStatus=EFI_NOT_READY;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&Count,&Handles);
  if(EFI_ERROR(Status)) return Status;
  for(I=0;I<Count;I++) {
    EFI_PARTITION_INFO_PROTOCOL *Partition=NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs=NULL;
    EFI_FILE_PROTOCOL *Root=NULL;
    CHAR16 Label[37];
    if(EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiPartitionInfoProtocolGuid,(VOID **)&Partition)) ||
      Partition==NULL || Partition->Type!=PARTITION_TYPE_GPT) continue;
    CopyMem(Label,Partition->Info.Gpt.PartitionName,72);Label[36]=0;
    if(StrCmp(Label,L"modem_a")!=0) continue;
    if(EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs)) ||
      Fs==NULL || Fs->OpenVolume==NULL) continue;
    Status=Fs->OpenVolume(Fs,&Root);
    if(EFI_ERROR(Status) || Root==NULL) continue;
    Result->FirmwareVolumes++;
    Result->AdspStatus=ReadFirmware(Root,L"\\image\\adsp.mdt",TRUE,&Result->AdspBytes,&Result->AdspCrc);
    Result->SensorConfigStatus=ReadFirmware(Root,L"\\image\\adsps.jsn",FALSE,&Result->SensorConfigBytes,&Result->SensorConfigCrc);
    Result->ModemStatus=ReadFirmware(Root,L"\\image\\modem.mdt",TRUE,&Result->ModemBytes,&Result->ModemCrc);
    Root->Close(Root);
    DeviceReport(Output,Context,L"[SENSORS] modem_a ADSP MDT=%r bytes=%u crc=%08x\n",Result->AdspStatus,Result->AdspBytes,Result->AdspCrc);
    DeviceReport(Output,Context,L"[SENSORS] modem_a adsps.jsn=%r bytes=%u crc=%08x\n",Result->SensorConfigStatus,Result->SensorConfigBytes,Result->SensorConfigCrc);
    DeviceReport(Output,Context,L"[SENSORS] modem_a MODEM MDT=%r bytes=%u crc=%08x\n",Result->ModemStatus,Result->ModemBytes,Result->ModemCrc);
    break;
  }
  if(Handles!=NULL) FreePool(Handles);
  DeviceReport(Output,Context,L"[SENSORS] Gyroscope=%r GPS=%r; firmware reads only, SSC/QMI samples and GNSS fixes unavailable\n",
    Result->GyroStatus,Result->GpsStatus);
  return Result->FirmwareVolumes?EFI_SUCCESS:EFI_NOT_FOUND;
}
