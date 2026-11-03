/** Platform dependency checks. SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PLATFORM_TEST_LIB_H_
#define PLATFORM_TEST_LIB_H_
#include <Uefi.h>
#include <Protocol/WirelessSurvey.h>
typedef VOID (EFIAPI *PLATFORM_TEST_OUTPUT)(CONST CHAR8 *Line, VOID *Context);
typedef struct {
  EFI_STATUS AcpiStatus;
  EFI_STATUS ScmStatus;
  EFI_STATUS PilStatus;
  EFI_STATUS AdcStatus;
  EFI_STATUS RtcStatus;
  UINTN ShmHandles;
  UINTN UsbHandles;
  UINTN TouchHandles;
  UINTN DisplayHandles;
  UINTN BlockHandles;
  UINTN FileSystemHandles;
  UINTN AcpiTables, AcpiEnabledCpus;
  EFI_STATUS AcpiLinksStatus;
} PLATFORM_TEST_RESULT;
EFI_STATUS PlatformHardwareTest (IN PLATFORM_TEST_OUTPUT Output OPTIONAL, IN VOID *Context OPTIONAL,
                                OUT PLATFORM_TEST_RESULT *Result);
typedef struct {
  EFI_STATUS HostStatus;
  EFI_STATUS StorageStatus;
  UINTN HostHandles;
  UINTN DeviceHandles;
  UINTN BlockHandles;
  UINTN FileSystemHandles;
  UINTN ReadableVolumes;
} USB_HOST_TEST_RESULT;
typedef struct {
  EFI_STATUS PciStatus;
  EFI_STATUS WifiStatus;
  UINTN RootBridges;
  UINTN PciHandles;
  UINTN WlanEndpoints;
  UINTN WifiHandles;
  UINTN NetworkHandles;
} WLAN_TEST_RESULT;
EFI_STATUS PlatformUsbHostTest (IN PLATFORM_TEST_OUTPUT Output OPTIONAL, IN VOID *Context OPTIONAL,
                               OUT USB_HOST_TEST_RESULT *Result);
EFI_STATUS PlatformWlanTest (IN PLATFORM_TEST_OUTPUT Output OPTIONAL, IN VOID *Context OPTIONAL,
                            OUT WLAN_TEST_RESULT *Result);
// Physical, manual PCIe test. Endpoint identification does not imply WiFi service.
typedef struct {
  UINT32 Stage, Detail, Error, Device;
  EFI_STATUS FirmwareStatus, CleanupStatus;
  UINT32 Rails, Clocks, Phy, Link, Elbi, RetainedRails, PipeHz;
} WLAN_PCIE_TEST_RESULT;
EFI_STATUS PlatformWlanPcieTest (IN PLATFORM_TEST_OUTPUT Output OPTIONAL, IN VOID *Context OPTIONAL,
                                OUT WLAN_PCIE_TEST_RESULT *Result);
EFI_STATUS PlatformWirelessGetResult (OUT WIRELESS_SURVEY_RESULT *Result);
EFI_STATUS PlatformWirelessScan (VOID);
EFI_STATUS PlatformBluetoothRefresh (VOID);
EFI_STATUS PlatformWirelessTest (IN PLATFORM_TEST_OUTPUT Output OPTIONAL, IN VOID *Context OPTIONAL,
                                OUT WIRELESS_SURVEY_RESULT *Result);
typedef struct {
  EFI_STATUS AdspStatus, SensorConfigStatus, ModemStatus, GyroStatus, GpsStatus;
  UINTN FirmwareVolumes;
  UINT32 AdspBytes, AdspCrc, SensorConfigBytes, SensorConfigCrc, ModemBytes, ModemCrc;
} PLATFORM_SENSOR_RESULT;
EFI_STATUS PlatformSensorTest (IN PLATFORM_TEST_OUTPUT Output OPTIONAL, IN VOID *Context OPTIONAL,
                              OUT PLATFORM_SENSOR_RESULT *Result);
typedef struct { EFI_STATUS Smem, Adsp, Modem, Cdsp, Mhi; } PLATFORM_TRANSPORT_RESULT;
EFI_STATUS PlatformTransportTest(PLATFORM_TEST_OUTPUT Output,VOID *Context,PLATFORM_TRANSPORT_RESULT *Result);
EFI_STATUS PlatformTransportTestSelected(PLATFORM_TEST_OUTPUT Output,VOID *Context,PLATFORM_TRANSPORT_RESULT *Result,UINT32 Mask);
EFI_STATUS PlatformWinBootCheck(PLATFORM_TEST_OUTPUT Output,VOID *Context);
typedef struct {
  EFI_STATUS Status;
  UINT32 PartitionBytes, MaxTransfer, Frames, CounterBefore, CounterAfter;
} PLATFORM_RPMB_RESULT;
// Blocks=0 reports the provider. Otherwise read 1..4 frames, with no write API.
EFI_STATUS PlatformRpmbRead (PLATFORM_TEST_OUTPUT Output, VOID *Context, UINT32 Address, UINT32 Blocks,
                             PLATFORM_RPMB_RESULT *Result);
EFI_STATUS PlatformAcpiRead (PLATFORM_TEST_OUTPUT Output, VOID *Context, UINT32 Index, UINT32 Offset, UINT32 Bytes);
typedef struct {
  EFI_STATUS Status;
  UINT8 Type, Subtype, Status1, Status2;
  BOOLEAN Started, Restored;
} PLATFORM_FLASH_RESULT;
EFI_STATUS PlatformFlashTest (PLATFORM_TEST_OUTPUT Output, VOID *Context, UINT32 Channel, UINT32 Ms, UINT32 Ma,
                              PLATFORM_FLASH_RESULT *Result);
EFI_STATUS PlatformBrightnessStatus (PLATFORM_TEST_OUTPUT Output, VOID *Context);
#endif
