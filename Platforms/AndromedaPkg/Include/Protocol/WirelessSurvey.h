/** Resident wireless survey results. SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef WIRELESS_SURVEY_H_
#define WIRELESS_SURVEY_H_
#include <Uefi.h>
#define WIRELESS_SURVEY_GUID {0x7f9c0bd5,0xe415,0x4bca,{0x91,0xce,0x47,0x80,0xa0,0x46,0xc2,0x17}}
#define WIRELESS_SURVEY_MAX 32
typedef struct {
  UINT8 Ssid[32], SsidLength, Quality;
  CHAR16 Name[64];
} WIRELESS_NETWORK;
typedef struct {
  UINT8 Address[6], AddressType;
  BOOLEAN LowEnergy;
  CHAR16 Name[64];
} WIRELESS_DEVICE;
typedef struct {
  UINT32 Generation;
  EFI_STATUS WifiStatus, BluetoothStatus;
  BOOLEAN WifiPending, WifiTruncated, BluetoothTruncated;
  UINTN WifiProviders, LegacyWifiProviders, BluetoothProviders, BleProviders;
  UINTN NetworkCount, DeviceCount;
  WIRELESS_NETWORK Networks[WIRELESS_SURVEY_MAX];
  WIRELESS_DEVICE Devices[WIRELESS_SURVEY_MAX];
} WIRELESS_SURVEY_RESULT;
typedef struct _WIRELESS_SURVEY_PROTOCOL WIRELESS_SURVEY_PROTOCOL;
struct _WIRELESS_SURVEY_PROTOCOL {
  UINT64 Revision;
  EFI_STATUS (EFIAPI *StartWifi)(WIRELESS_SURVEY_PROTOCOL *This);
  EFI_STATUS (EFIAPI *RefreshBluetooth)(WIRELESS_SURVEY_PROTOCOL *This);
  EFI_STATUS (EFIAPI *GetResult)(WIRELESS_SURVEY_PROTOCOL *This, WIRELESS_SURVEY_RESULT *Result);
};
#endif
