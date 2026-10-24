/** @file
  UsbDevStartDxe - start the DWC3 peripheral controller ourselves.

  Why this exists
  ---------------
  Everything above the hardware reports success:

    EFI_USBFN_IO_PROTOCOL   present
    EFI_USB_DEVICE_PROTOCOL present
    UsbMsdDxe dispatched (and ordered after UsbDeviceDxe)

  yet plugging the phone into a host does nothing, and reading the controller
  directly says why:

    DCFG = 0x00080805   DCTL = 0x00F00000   DSTS = 0x00000000
    RUN_STOP (DCTL bit30) = 0

  RUN_STOP = 0 means the controller never left reset/idle, so it never asserts
  the D+ pull-up and no host can possibly see it. Nothing in the stack called
  StartController / StartDevice.

  So call it here. The descriptor set below is deliberately minimal: one
  configuration, one vendor-specific interface, zero endpoints. That is enough
  for a host to enumerate the device and report "unknown device" - which is
  exactly the proof we need that the PHY, the pull-up and the packet path work.
  Once that is confirmed, the descriptors can be replaced with a real MSD
  function.

  Copyright (c) 2026, ProjectMu shennong port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Protocol/EFIUsbDevice.h>

#pragma pack(1)

typedef struct {
  UINT8     bLength;
  UINT8     bDescriptorType;
  UINT16    bcdUSB;
  UINT8     bDeviceClass;
  UINT8     bDeviceSubClass;
  UINT8     bDeviceProtocol;
  UINT8     bMaxPacketSize0;
  UINT16    idVendor;
  UINT16    idProduct;
  UINT16    bcdDevice;
  UINT8     iManufacturer;
  UINT8     iProduct;
  UINT8     iSerialNumber;
  UINT8     bNumConfigurations;
} PORT_DEVICE_DESCRIPTOR;

typedef struct {
  UINT8     bLength;
  UINT8     bDescriptorType;
  UINT16    wTotalLength;
  UINT8     bNumInterfaces;
  UINT8     bConfigurationValue;
  UINT8     iConfiguration;
  UINT8     bmAttributes;
  UINT8     bMaxPower;
} PORT_CONFIG_DESCRIPTOR;

typedef struct {
  UINT8     bLength;
  UINT8     bDescriptorType;
  UINT8     bInterfaceNumber;
  UINT8     bAlternateSetting;
  UINT8     bNumEndpoints;
  UINT8     bInterfaceClass;
  UINT8     bInterfaceSubClass;
  UINT8     bInterfaceProtocol;
  UINT8     iInterface;
} PORT_INTERFACE_DESCRIPTOR;

typedef struct {
  PORT_CONFIG_DESCRIPTOR     Config;
  PORT_INTERFACE_DESCRIPTOR  Interface;
} PORT_CONFIG_SET;

typedef struct {
  UINT8     bLength;
  UINT8     bDescriptorType;
  UINT16    bcdUSB;
  UINT8     bDeviceClass;
  UINT8     bDeviceSubClass;
  UINT8     bDeviceProtocol;
  UINT8     bMaxPacketSize0;
  UINT8     bNumConfigurations;
  UINT8     bReserved;
} PORT_QUALIFIER_DESCRIPTOR;

typedef struct {
  UINT8     bLength;
  UINT8     bDescriptorType;
  UINT16    wTotalLength;
  UINT8     bNumDeviceCaps;
} PORT_BOS_DESCRIPTOR;

#pragma pack()

STATIC
VOID
UsbStartSay (
  IN CONST CHAR8  *Text
  )
{
  SerialPortInitialize ();
  SerialPortWrite ((UINT8 *)Text, AsciiStrLen (Text));
}

STATIC
EFI_STATUS
UsbStartTry (
  IN EFI_USB_DEVICE_PROTOCOL  *UsbDevice,
  IN VOID                     *DevDesc,
  IN VOID                     **Descriptors,
  IN VOID                     *QualDesc,
  IN VOID                     *Bos
  )
{
  return UsbDevice->Start (
                      (USB_DEVICE_DESCRIPTOR *)DevDesc,
                      Descriptors,
                      (USB_DEVICE_QUALIFIER_DESCRIPTOR *)QualDesc,
                      Bos,
                      0,
                      NULL
                      );
}

EFI_STATUS
EFIAPI
UsbDevStartEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_USB_DEVICE_PROTOCOL    *UsbDevice;
  PORT_DEVICE_DESCRIPTOR     DevDesc;
  PORT_CONFIG_SET            CfgSet;
  PORT_QUALIFIER_DESCRIPTOR  QualDesc;
  PORT_BOS_DESCRIPTOR        BosDesc;
  USB_DEVICE_DESCRIPTOR_SET  DescSet;
  VOID                       *Descriptors[1];
  EFI_HANDLE                 *Handles;
  UINTN                      HandleCount;
  CHAR8                      Buf[112];
  UINTN                      Len;

  Status = gBS->LocateProtocol (&gEfiUsbDeviceProtocolGuid, NULL, (VOID **)&UsbDevice);
  if (EFI_ERROR (Status)) {
    UsbStartSay ("[USBSTART] no UsbDevice protocol\n");
    return EFI_SUCCESS;
  }

  ZeroMem (&DevDesc, sizeof (DevDesc));
  DevDesc.bLength            = sizeof (DevDesc);
  DevDesc.bDescriptorType    = 0x01;      /* DEVICE */
  DevDesc.bcdUSB             = 0x0200;
  DevDesc.bDeviceClass       = 0x00;      /* per interface */
  DevDesc.bMaxPacketSize0    = 64;
  DevDesc.idVendor           = 0x05C6;
  DevDesc.idProduct          = 0x9008;
  DevDesc.bcdDevice          = 0x0100;
  DevDesc.bNumConfigurations = 1;

  ZeroMem (&QualDesc, sizeof (QualDesc));
  QualDesc.bLength            = sizeof (QualDesc);
  QualDesc.bDescriptorType    = 0x06;     /* DEVICE_QUALIFIER */
  QualDesc.bcdUSB             = 0x0200;
  QualDesc.bMaxPacketSize0    = 64;
  QualDesc.bNumConfigurations = 1;

  ZeroMem (&BosDesc, sizeof (BosDesc));
  BosDesc.bLength         = sizeof (BosDesc);
  BosDesc.bDescriptorType = 0x0F;         /* BOS */
  BosDesc.wTotalLength    = sizeof (BosDesc);
  BosDesc.bNumDeviceCaps  = 0;

  ZeroMem (&CfgSet, sizeof (CfgSet));
  CfgSet.Config.bLength             = sizeof (PORT_CONFIG_DESCRIPTOR);
  CfgSet.Config.bDescriptorType     = 0x02;
  CfgSet.Config.wTotalLength        = sizeof (CfgSet);
  CfgSet.Config.bNumInterfaces      = 1;
  CfgSet.Config.bConfigurationValue = 1;
  CfgSet.Config.bmAttributes        = 0x80;
  CfgSet.Config.bMaxPower           = 50;

  CfgSet.Interface.bLength            = sizeof (PORT_INTERFACE_DESCRIPTOR);
  CfgSet.Interface.bDescriptorType    = 0x04;
  CfgSet.Interface.bInterfaceNumber   = 0;
  CfgSet.Interface.bNumEndpoints      = 0;
  CfgSet.Interface.bInterfaceClass    = 0xFF;
  CfgSet.Interface.bInterfaceSubClass = 0xFF;
  CfgSet.Interface.bInterfaceProtocol = 0xFF;

  Descriptors[0] = &CfgSet;

  ZeroMem (&DescSet, sizeof (DescSet));
  DescSet.DeviceDescriptor          = (EFI_USB_DEVICE_DESCRIPTOR *)&DevDesc;
  DescSet.Descriptors               = Descriptors;
  DescSet.SSDeviceDescriptor        = NULL;
  DescSet.SSDescriptors             = NULL;
  DescSet.DeviceQualifierDescriptor = (EFI_USB_DEVICE_QUALIFIER_DESCRIPTOR *)&QualDesc;
  DescSet.BinaryDeviceOjectStore    = &BosDesc;
  DescSet.StringDescriptorCount     = 0;
  DescSet.StringDescritors          = NULL;

  Status = UsbDevice->StartEx (&DescSet);
  Len    = AsciiSPrint (Buf, sizeof (Buf), "[USBSTART] StartEx -> %r\n", Status);
  SerialPortWrite ((UINT8 *)Buf, Len);

  Status = UsbStartTry (UsbDevice, &DevDesc, Descriptors, &QualDesc, &BosDesc);
  Len    = AsciiSPrint (Buf, sizeof (Buf), "[USBSTART] Start   -> %r\n", Status);
  SerialPortWrite ((UINT8 *)Buf, Len);

  if (Status != EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }

  //
  // EFI_NOT_FOUND here does not mean 'no such protocol' - UsbDeviceDxe reports
  // it from its own cached state. On this platform it ran BEFORE
  // SecurityStubDxe published gEfiSecurityArchProtocolGuid, so UsbfnDwc3Dxe
  // could not dispatch yet and UsbDeviceOpenProtocols failed:
  //
  //   UsbDeviceOpenProtocols: Failed to locate USB FN IO protocol: Not Found
  //
  // It never retries. Force it to: disconnect the controller and reconnect it,
  // which re-runs UsbDeviceDxe's init now that EFI_USBFN_IO_PROTOCOL exists.
  //
  UsbStartSay ("[USBSTART] reconnecting the USB device controller\n");

  Handles     = NULL;
  HandleCount = 0;
  Status      = gBS->LocateHandleBuffer (
                       ByProtocol,
                       &gEfiUsbDeviceProtocolGuid,
                       NULL,
                       &HandleCount,
                       &Handles
                       );
  if (EFI_ERROR (Status) || (HandleCount == 0)) {
    UsbStartSay ("[USBSTART] no controller handle\n");
    return EFI_SUCCESS;
  }

  gBS->DisconnectController (Handles[0], NULL, NULL);
  gBS->ConnectController (Handles[0], NULL, NULL, TRUE);
  gBS->FreePool (Handles);

  UsbDevice = NULL;
  Status    = gBS->LocateProtocol (&gEfiUsbDeviceProtocolGuid, NULL, (VOID **)&UsbDevice);
  if (EFI_ERROR (Status)) {
    UsbStartSay ("[USBSTART] UsbDevice gone after reconnect\n");
    return EFI_SUCCESS;
  }

  Status = UsbDevice->StartEx (&DescSet);
  Len    = AsciiSPrint (Buf, sizeof (Buf), "[USBSTART] retry StartEx -> %r\n", Status);
  SerialPortWrite ((UINT8 *)Buf, Len);

  Status = UsbStartTry (UsbDevice, &DevDesc, Descriptors, &QualDesc, &BosDesc);
  Len    = AsciiSPrint (Buf, sizeof (Buf), "[USBSTART] retry Start   -> %r\n", Status);
  SerialPortWrite ((UINT8 *)Buf, Len);

  return EFI_SUCCESS;
}
