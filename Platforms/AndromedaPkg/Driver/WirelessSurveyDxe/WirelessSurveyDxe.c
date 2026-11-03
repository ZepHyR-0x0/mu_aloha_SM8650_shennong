/**
  Asynchronous WiFi tokens and callbacks belong to this resident driver, never
  to a transient Shell application or GUI page. Returning from a client during
  a scan does not release driver-owned input, event or result storage.
  Bluetooth lists are provider caches; no pairing or connection is requested.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/WiFi.h>
#include <Protocol/WiFi2.h>
#include <Protocol/BluetoothConfig.h>
#include <Protocol/BluetoothLeConfig.h>
#include <Protocol/WirelessSurvey.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

STATIC EFI_GUID mGuid = WIRELESS_SURVEY_GUID;
STATIC WIRELESS_SURVEY_PROTOCOL mProtocol;
STATIC WIRELESS_SURVEY_RESULT mResult;
STATIC EFI_80211_GET_NETWORKS_TOKEN mToken;
STATIC EFI_80211_GET_NETWORKS_DATA mInput;
STATIC BOOLEAN mBluetoothBusy;

STATIC VOID Say (CONST CHAR8 *Text, EFI_STATUS Status, UINTN Count)
{
  CHAR8 Line[160];
  UINTN Bytes = AsciiSPrint (Line,sizeof(Line),"[WIRELESS] %a -> %r count=%u\r\n",Text,Status,Count);
  SerialPortWrite ((UINT8 *)Line,Bytes);
}

STATIC UINTN Count (EFI_GUID *Guid)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Number = 0;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol,Guid,NULL,&Number,&Handles);
  if (Handles != NULL) FreePool (Handles);
  return EFI_ERROR (Status) ? 0 : Number;
}

STATIC VOID Printable (CHAR16 *Name, UINTN Capacity, CONST UINT8 *Bytes, UINTN Length)
{
  UINTN I;
  for (I=0;I<Length && I+1<Capacity;I++) Name[I] = Bytes[I]>=32 && Bytes[I]<=126 ? Bytes[I] : L'?';
  Name[I]=0;
}

STATIC VOID EFIAPI WifiDone (EFI_EVENT Event, VOID *Context)
{
  UINTN I;
  (VOID)Context;
  mResult.WifiStatus=mToken.Status;
  mResult.NetworkCount=0;
  mResult.WifiTruncated=FALSE;
  if (!EFI_ERROR(mToken.Status) && mToken.Result != NULL) {
    mResult.WifiTruncated=mToken.Result->NumOfNetworkDesc>WIRELESS_SURVEY_MAX;
    for (I=0;I<MIN(mToken.Result->NumOfNetworkDesc,WIRELESS_SURVEY_MAX);I++) {
      EFI_80211_NETWORK_DESCRIPTION *Source=&mToken.Result->NetworkDesc[I];
      WIRELESS_NETWORK *Target=&mResult.Networks[mResult.NetworkCount];
      if (Source->Network.SSId.SSIdLen>32 || Source->NetworkQuality>100) {
        mResult.WifiStatus=EFI_COMPROMISED_DATA;
        continue;
      }
      ZeroMem(Target,sizeof(*Target));
      Target->SsidLength=Source->Network.SSId.SSIdLen;
      Target->Quality=Source->NetworkQuality;
      CopyMem(Target->Ssid,Source->Network.SSId.SSId,Target->SsidLength);
      Printable(Target->Name,ARRAY_SIZE(Target->Name),Target->Ssid,Target->SsidLength);
      mResult.NetworkCount++;
    }
  } else if (!EFI_ERROR(mToken.Status)) mResult.WifiStatus=EFI_COMPROMISED_DATA;
  if (mToken.Result != NULL) FreePool(mToken.Result);
  mToken.Result=NULL;
  mResult.WifiPending=FALSE;
  mResult.Generation++;
  gBS->CloseEvent(Event);
  mToken.Event=NULL;
  Say("WiFi scan complete",mResult.WifiStatus,mResult.NetworkCount);
}

STATIC EFI_STATUS EFIAPI StartWifi (WIRELESS_SURVEY_PROTOCOL *This)
{
  EFI_STATUS Status;
  EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL *Wifi=NULL;
  if (This!=&mProtocol) return EFI_INVALID_PARAMETER;
  if (mResult.WifiPending) return EFI_ALREADY_STARTED;
  mResult.WifiProviders=Count(&gEfiWiFi2ProtocolGuid);
  mResult.LegacyWifiProviders=Count(&gEfiWiFiProtocolGuid);
  mResult.NetworkCount=0;
  mResult.WifiTruncated=FALSE;
  Status=gBS->LocateProtocol(&gEfiWiFi2ProtocolGuid,NULL,(VOID **)&Wifi);
  if (EFI_ERROR(Status) || Wifi==NULL || Wifi->GetNetworks==NULL) {
    mResult.WifiStatus=mResult.LegacyWifiProviders ? EFI_UNSUPPORTED : EFI_NOT_FOUND;
    mResult.Generation++;
    Say("WiFi scan provider",mResult.WifiStatus,mResult.WifiProviders);
    return mResult.WifiStatus;
  }
  ZeroMem(&mToken,sizeof(mToken));
  ZeroMem(&mInput,sizeof(mInput));
  mToken.Data=&mInput;
  mToken.Status=EFI_NOT_READY;
  Status=gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,WifiDone,NULL,&mToken.Event);
  if (EFI_ERROR(Status)) {mResult.WifiStatus=Status;return Status;}
  mResult.WifiPending=TRUE;
  mResult.WifiStatus=EFI_NOT_READY;
  mResult.Generation++;
  Status=Wifi->GetNetworks(Wifi,&mToken);
  if (EFI_ERROR(Status)) {
    gBS->CloseEvent(mToken.Event);
    mToken.Event=NULL;
    mResult.WifiPending=FALSE;
    mResult.WifiStatus=Status;
    mResult.Generation++;
  }
  Say("WiFi scan start",Status,mResult.WifiProviders);
  return Status;
}

STATIC VOID AddDevice (CONST UINT8 *Address, UINT8 Type, BOOLEAN Le, CONST UINT8 *Name, UINTN Length)
{
  WIRELESS_DEVICE *Device;
  UINTN I;
  for(I=0;I<mResult.DeviceCount;I++) {
    Device=&mResult.Devices[I];
    if(Device->LowEnergy==Le && Device->AddressType==Type && CompareMem(Device->Address,Address,6)==0) return;
  }
  if(mResult.DeviceCount==WIRELESS_SURVEY_MAX) {mResult.BluetoothTruncated=TRUE;return;}
  Device=&mResult.Devices[mResult.DeviceCount++];
  ZeroMem(Device,sizeof(*Device));
  CopyMem(Device->Address,Address,6);
  Device->AddressType=Type;
  Device->LowEnergy=Le;
  for(I=0;I<Length && Name[I]!=0;I++);
  Printable(Device->Name,ARRAY_SIZE(Device->Name),Name,I);
}

STATIC EFI_STATUS EFIAPI RefreshBluetooth (WIRELESS_SURVEY_PROTOCOL *This)
{
  EFI_HANDLE *Handles=NULL;
  UINTN CountHandles=0,I,J,Size,NameSize;
  EFI_STATUS Status,Overall=EFI_NOT_FOUND;
  BOOLEAN Le;
  UINT8 Name[248];
  if(This!=&mProtocol) return EFI_INVALID_PARAMETER;
  if(mBluetoothBusy) return EFI_ALREADY_STARTED;
  mBluetoothBusy=TRUE;
  mResult.DeviceCount=0;
  mResult.BluetoothTruncated=FALSE;
  mResult.BluetoothProviders=Count(&gEfiBluetoothConfigProtocolGuid);
  mResult.BleProviders=Count(&gEfiBluetoothLeConfigProtocolGuid);
  for(Le=FALSE;;Le=TRUE) {
    EFI_GUID *Guid=Le ? &gEfiBluetoothLeConfigProtocolGuid : &gEfiBluetoothConfigProtocolGuid;
    Handles=NULL;CountHandles=0;
    Status=gBS->LocateHandleBuffer(ByProtocol,Guid,NULL,&CountHandles,&Handles);
    if(!EFI_ERROR(Status)) for(I=0;I<CountHandles;I++) {
      EFI_BLUETOOTH_CONFIG_PROTOCOL *Classic=NULL;
      EFI_BLUETOOTH_LE_CONFIG_PROTOCOL *Ble=NULL;
      union {BLUETOOTH_ADDRESS Classic[128];BLUETOOTH_LE_ADDRESS Ble[128];} Addresses;
      UINTN Stride=Le ? sizeof(BLUETOOTH_LE_ADDRESS) : sizeof(BLUETOOTH_ADDRESS);
      Size=Stride*128;
      if(Le) {
        Status=gBS->HandleProtocol(Handles[I],Guid,(VOID **)&Ble);
        if(EFI_ERROR(Status) || Ble==NULL || Ble->GetData==NULL) continue;
        Status=Ble->GetData(Ble,EfiBluetoothConfigDataTypeAvailableDeviceList,&Size,&Addresses);
      } else {
        Status=gBS->HandleProtocol(Handles[I],Guid,(VOID **)&Classic);
        if(EFI_ERROR(Status) || Classic==NULL || Classic->GetData==NULL) continue;
        Status=Classic->GetData(Classic,EfiBluetoothConfigDataTypeAvailableDeviceList,&Size,&Addresses);
      }
      if(Status==EFI_BUFFER_TOO_SMALL) mResult.BluetoothTruncated=TRUE;
      if(!EFI_ERROR(Status) && (Size>Stride*128 || Size%Stride!=0)) Status=EFI_COMPROMISED_DATA;
      Say(Le?"BLE cached list":"Bluetooth cached list",Status,EFI_ERROR(Status)?0:Size/Stride);
      if(EFI_ERROR(Status)) {if(Overall!=EFI_SUCCESS) Overall=Status;continue;}
      Overall=EFI_SUCCESS;
      for(J=0;J<Size/Stride;J++) {
        ZeroMem(Name,sizeof(Name));NameSize=sizeof(Name);
        if(Le && Ble->GetRemoteData!=NULL)
          Status=Ble->GetRemoteData(Ble,EfiBluetoothConfigDataTypeDeviceName,&Addresses.Ble[J],&NameSize,Name);
        else if(!Le && Classic->GetRemoteData!=NULL)
          Status=Classic->GetRemoteData(Classic,EfiBluetoothConfigDataTypeDeviceName,&Addresses.Classic[J],&NameSize,Name);
        else Status=EFI_UNSUPPORTED;
        if(EFI_ERROR(Status) || NameSize>sizeof(Name)) NameSize=0;
        AddDevice(Le?Addresses.Ble[J].Address:Addresses.Classic[J].Address,
          Le?Addresses.Ble[J].Type:0,Le,Name,NameSize);
      }
    }
    if(Handles!=NULL) FreePool(Handles);
    if(Le) break;
  }
  mResult.BluetoothStatus=Overall;
  mResult.Generation++;
  mBluetoothBusy=FALSE;
  return Overall;
}

STATIC EFI_STATUS EFIAPI GetResult (WIRELESS_SURVEY_PROTOCOL *This, WIRELESS_SURVEY_RESULT *Result)
{
  EFI_TPL Old;
  if(This!=&mProtocol || Result==NULL) return EFI_INVALID_PARAMETER;
  Old=gBS->RaiseTPL(TPL_CALLBACK);
  CopyMem(Result,&mResult,sizeof(*Result));
  gBS->RestoreTPL(Old);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI WirelessSurveyEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_HANDLE Handle=NULL;
  EFI_STATUS Status;
  (VOID)ImageHandle;(VOID)SystemTable;
  mResult.WifiStatus=mResult.BluetoothStatus=EFI_NOT_READY;
  mProtocol=(WIRELESS_SURVEY_PROTOCOL){1,StartWifi,RefreshBluetooth,GetResult};
  Status=gBS->InstallProtocolInterface(&Handle,&mGuid,EFI_NATIVE_INTERFACE,&mProtocol);
  Say("Resident survey installed",Status,0);
  return Status;
}
