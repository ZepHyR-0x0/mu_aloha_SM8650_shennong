// Copyright (c) 2026, ProjectMu shennong port.
// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/WcdUsbLib.h>
#include <Guid/EventGroup.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PlatformInfoCompat.h>
#include <Protocol/EFIUsbDevice.h>
#include <Protocol/UsbFunctionIo.h>

//
// Build-time recovery switch. Set to 0 for an image that never touches the USB
// controller and therefore always reaches BDS/UI, even while the DWC3 start path is
// still being debugged. No disk or NVRAM is involved.
//
#ifndef USB_BRINGUP_AUTOSTART
//
// Off by default.
//
// Start() runs the whole device bring-up synchronously from this driver's entry point, and
// while the controller cannot complete a memory access the doorbell inside it never returns
// - which takes the machine down before BDS ever gets to draw anything. With this at 0 the
// image never touches the controller, so the GUI comes up and the USB work can be looked at
// separately. Set it to 1 to bring the device up again.
//
#define USB_BRINGUP_AUTOSTART 2
#endif

#pragma pack(1)
typedef struct {
  EFI_USB_CONFIG_DESCRIPTOR     Config;
  EFI_USB_INTERFACE_DESCRIPTOR  Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR   Endpoint;
} PORT_CONFIG_SET;
#pragma pack()

STATIC EFI_USB_DEVICE_DESCRIPTOR mDevice = {
  18, 1, 0x0200, 0, 0, 0, 64, 0x05C6, 0x9008, 0x0100, 0, 0, 0, 1
};
STATIC EFI_USB_DEVICE_QUALIFIER_DESCRIPTOR mQualifier = {
  10, 6, 0x0200, 0, 0, 0, 64, 1, 0
};
STATIC PORT_CONFIG_SET mConfig = {
  { 9, 2, sizeof (PORT_CONFIG_SET), 1, 1, 0, 0x80, 50 },
  { 9, 4, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0 },
  { 7, 5, 0x81, 2, 512, 0 }
};
STATIC UINT8 mBos[] = { 5, 15, 5, 0, 0 };
STATIC VOID *mDescriptors[] = { &mConfig, NULL };
STATIC EFI_USB_DEVICE_PROTOCOL *mUsbDevice;
STATIC EFI_EVENT mPollEvent;
STATIC EFI_EVENT mExitEvent;
STATIC USB_DEVICE_START mStart;
STATIC USB_DEVICE_START_EX mStartEx;
STATIC USB_DEVICE_STOP mStop;
STATIC USB_DEVICE_HANDLE_EVENT mHandleEvent;



STATIC VOID
UsbStartSay (IN CONST CHAR8 *Format, ...)
{
  CHAR8 Buffer[192];
  VA_LIST Marker;
  UINTN Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  // No SerialPortInitialize() per line - see the note in UsbfnDwc3Dxe's RefSay.
  SerialPortWrite ((UINT8 *)Buffer, Length);

}

STATIC VOID
UsbReleaseEventPump (VOID)
{
  if (mPollEvent != NULL) {
    gBS->SetTimer (mPollEvent, TimerCancel, 0);
    gBS->CloseEvent (mPollEvent);
    mPollEvent = NULL;
  }
  if (mExitEvent != NULL) {
    gBS->CloseEvent (mExitEvent);
    mExitEvent = NULL;
  }
  if (mUsbDevice != NULL && mHandleEvent != NULL) {
    mUsbDevice->Start = mStart;
    mUsbDevice->StartEx = mStartEx;
    mUsbDevice->Stop = mStop;
    mUsbDevice->HandleEvent = mHandleEvent;
  }
}

// A later USB client owns its events; surrender before forwarding its first call.
STATIC EFI_STATUS EFIAPI
UsbClientStart (
  IN USB_DEVICE_DESCRIPTOR *Device,
  IN VOID **Descriptors,
  IN USB_DEVICE_QUALIFIER_DESCRIPTOR *Qualifier,
  IN VOID *Bos,
  IN UINT8 StringCount,
  IN USB_STRING_DESCRIPTOR **Strings
  )
{
  UsbReleaseEventPump ();
  return mStart (Device, Descriptors, Qualifier, Bos, StringCount, Strings);
}

STATIC EFI_STATUS EFIAPI
UsbClientStartEx (IN USB_DEVICE_DESCRIPTOR_SET *Descriptors)
{
  UsbReleaseEventPump ();
  return mStartEx (Descriptors);
}

STATIC EFI_STATUS EFIAPI
UsbClientStop (VOID)
{
  UsbReleaseEventPump ();
  return mStop ();
}

STATIC EFI_STATUS EFIAPI
UsbClientHandleEvent (
  OUT USB_DEVICE_EVENT *Event,
  OUT UINTN *DataSize,
  OUT USB_DEVICE_EVENT_DATA *Data
  )
{
  UsbReleaseEventPump ();
  return mHandleEvent (Event, DataSize, Data);
}

STATIC VOID EFIAPI
UsbExitBootServices (IN EFI_EVENT Event, IN VOID *Context)
{
  UsbReleaseEventPump ();
}

//
// Direct-drive mode keeps the protocol pointer and a timer, so the event handler can be
// pumped without the vendor stack. UsbDirectSetup answers descriptors and
// configuration; the lower EP0 state machine handles SET_ADDRESS internally.
//
STATIC EFI_USBFN_IO_PROTOCOL  *mDirectFnIo;
STATIC EFI_EVENT               mDirectEvent;
STATIC UINT32                  mDirectTicks;
STATIC VOID                   *mDirectBuffer;
STATIC BOOLEAN                 mDirectDataPending;
STATIC BOOLEAN                 mDirectStatusPending;
STATIC UINT8                   mDirectConfiguration;
STATIC UINT8                   mDirectPendingConfiguration;
STATIC EFI_USB_BUS_SPEED       mDirectSpeed = UsbBusSpeedHigh;

// The direct path bypasses UsbDeviceDxe, so it must answer descriptor requests
// itself. Logging SETUP packets alone cannot complete host enumeration.
STATIC EFI_STATUS
UsbDirectSetup (IN EFI_USB_DEVICE_REQUEST *Request)
{
  CONST VOID *Descriptor = NULL;
  UINTN Length = 0;
  PORT_CONFIG_SET Config;
  EFI_STATUS Status;
  UINT16 LangId[] = { 0x0304, 0x0409 };
  UINT16 ZeroStatus = 0;

  if (mDirectBuffer == NULL) return EFI_NOT_READY;
  // A valid SET_CONFIGURATION has no DATA stage, only an IN status ZLP.
  // Publish the new value only after its successful completion, and never
  // submit a second OUT status stage as for a descriptor DATA completion.
  if (Request->RequestType == 0 && Request->Request == 9 &&
      Request->Value <= 1 && Request->Index == 0 && Request->Length == 0) {
    Status = mDirectFnIo->Transfer (mDirectFnIo, 0, EfiUsbEndpointDirectionDeviceTx,
                                   &Length, mDirectBuffer);
    mDirectDataPending = FALSE;
    mDirectStatusPending = (BOOLEAN)!EFI_ERROR (Status);
    mDirectPendingConfiguration = (UINT8)Request->Value;
    UsbStartSay ("[USBSTART b215] configuration=%u status queued -> %r\r\n",
                 (UINT32)Request->Value, Status);
    return Status;
  }

  if (Request->RequestType != 0x80 ||
      Request->Request != 6) {
    // Qualcomm's slot uses scalar BOOLEAN; see Dwc3InstallQualcommStallSlot.
    typedef EFI_STATUS (EFIAPI *SET_STALL)(EFI_USBFN_IO_PROTOCOL *, UINT8,
      EFI_USBFN_ENDPOINT_DIRECTION, BOOLEAN);
    if (Request->RequestType == 0x80 && Request->Request == 0 &&
        Request->Value == 0 && Request->Index == 0 && Request->Length == 2) {
      Descriptor = &ZeroStatus;
      Length = sizeof (ZeroStatus);
    } else if (Request->RequestType == 0x80 && Request->Request == 8 &&
               Request->Value == 0 && Request->Index == 0 && Request->Length == 1) {
      Descriptor = &mDirectConfiguration;
      Length = sizeof (mDirectConfiguration);
    } else {
      return ((SET_STALL)mDirectFnIo->SetEndpointStallState)
        (mDirectFnIo, 0, EfiUsbEndpointDirectionDeviceTx, TRUE);
    }
  } else {
    switch (Request->Value >> 8) {
      case 1:
        if ((Request->Value & 0xFF) != 0 || Request->Index != 0) break;
        Descriptor = &mDevice;
        Length = sizeof (mDevice);
        break;
      case 2:
      case 7:
        if ((Request->Value & 0xFF) != 0 || Request->Index != 0) break;
        Config = mConfig;
        Config.Config.DescriptorType = (UINT8)(Request->Value >> 8);
        Config.Endpoint.MaxPacketSize =
          ((mDirectSpeed == UsbBusSpeedHigh) == ((Request->Value >> 8) == 2)) ? 512 : 64;
        Descriptor = &Config;
        Length = sizeof (Config);
        break;
      case 3:
        if ((Request->Value & 0xFF) != 0 || Request->Index != 0) break;
        Descriptor = LangId;
        Length = sizeof (LangId);
        break;
      case 6:
        if ((Request->Value & 0xFF) != 0 || Request->Index != 0) break;
        Descriptor = &mQualifier;
        Length = sizeof (mQualifier);
        break;
      default:
        break;
    }
    if (Descriptor == NULL) {
      typedef EFI_STATUS (EFIAPI *SET_STALL)(EFI_USBFN_IO_PROTOCOL *, UINT8,
        EFI_USBFN_ENDPOINT_DIRECTION, BOOLEAN);
      return ((SET_STALL)mDirectFnIo->SetEndpointStallState)
        (mDirectFnIo, 0, EfiUsbEndpointDirectionDeviceTx, TRUE);
    }
  }

  Length = MIN (Length, Request->Length);
  CopyMem (mDirectBuffer, Descriptor, Length);
  Status = mDirectFnIo->Transfer (mDirectFnIo, 0, EfiUsbEndpointDirectionDeviceTx,
                                 &Length, mDirectBuffer);
  mDirectDataPending = (BOOLEAN)!EFI_ERROR (Status);
  UsbStartSay ("[USBSTART] descriptor type=%u length=%u -> %r\r\n",
               Request->Value >> 8, (UINT32)Length, Status);
  return Status;
}

STATIC VOID EFIAPI
UsbDirectPoll (IN EFI_EVENT Timer, IN VOID *Context)
{
  EFI_USBFN_MESSAGE          Message;
  EFI_USBFN_MESSAGE_PAYLOAD  Payload;
  UINTN                      PayloadSize;
  EFI_STATUS                 Status;

  (VOID)Timer;
  (VOID)Context;

  if (mDirectFnIo == NULL) {
    return;
  }

  mDirectTicks++;

  ZeroMem (&Message, sizeof (Message));
  ZeroMem (&Payload, sizeof (Payload));
  PayloadSize = sizeof (Payload);

  Status = mDirectFnIo->EventHandler (mDirectFnIo, &Message, &PayloadSize, &Payload);
  if (!EFI_ERROR (Status) && (Message != UsbDeviceEventNoEvent)) {
    UsbStartSay ("[USBSTART] direct event=%u size=%u t=%u\r\n",
                 (UINT32)Message, (UINT32)PayloadSize, mDirectTicks);
    if (Message == EfiUsbMsgBusEventSpeed) {
      mDirectSpeed = Payload.ubs;
    } else if (Message == EfiUsbMsgBusEventReset || Message == EfiUsbMsgBusEventDetach) {
      mDirectDataPending = FALSE;
      mDirectStatusPending = FALSE;
      mDirectConfiguration = 0;
    } else if (Message == EfiUsbMsgSetupPacket) {
      mDirectDataPending = FALSE;
      mDirectStatusPending = FALSE;
      UsbDirectSetup (&Payload.udr);
    } else if (Message == EfiUsbMsgEndpointStatusChangedTx &&
               Payload.utr.EndpointIndex == 0 && mDirectStatusPending) {
      mDirectStatusPending = FALSE;
      if (Payload.utr.TransferStatus == UsbTransferStatusComplete) {
        mDirectConfiguration = mDirectPendingConfiguration;
        UsbStartSay ("[USBSTART b215] configuration=%u status complete\r\n",
                     (UINT32)mDirectConfiguration);
      }
    } else if (Message == EfiUsbMsgEndpointStatusChangedTx &&
               Payload.utr.EndpointIndex == 0 && mDirectDataPending) {
      UINTN StatusLength = 0;
      mDirectDataPending = FALSE;
      if (Payload.utr.TransferStatus == UsbTransferStatusComplete) {
        Status = mDirectFnIo->Transfer (mDirectFnIo, 0, EfiUsbEndpointDirectionDeviceRx,
                                       &StatusLength, mDirectBuffer);
        UsbStartSay ("[USBSTART] descriptor status -> %r\r\n", Status);
      }
    }
  }

}

STATIC VOID EFIAPI
UsbPoll (IN EFI_EVENT Timer, IN VOID *Context)
{
  EFI_STATUS             Status;
  USB_DEVICE_EVENT       Event;
  USB_DEVICE_EVENT_DATA  Data;
  UINTN                  DataSize;
  STATIC UINT32          Tick;
  STATIC UINT32          Errors;
  UINTN                  Base;

  (VOID)Timer;
  (VOID)Context;

  Base = 0x0A600000;

  if (mUsbDevice == NULL || mHandleEvent == NULL) {
    return;
  }

  Tick++;

  //
  // Vendor-stack pump used by AUTOSTART=1. The active AUTOSTART=2 path instead
  // uses UsbDirectPoll/UsbDirectSetup and the lower EP0 state machine; the stock
  // UsbConfig/UsbDevice drivers are omitted from the current FV.
  //
  // The previous build turned the pump off after the machine reset during
  // enumeration. That reset was later located in the first STARTTRANSFER, before any
  // event was processed, so the pump was not the cause. It is on again now that the
  // DMA buffers are constrained to DDR and forced uncached.
  //
  Event    = UsbDeviceEventNoEvent;
  DataSize = sizeof (Data);
  ZeroMem (&Data, sizeof (Data));
  Status = mHandleEvent (&Event, &DataSize, &Data);
  if (EFI_ERROR (Status)) {
    //
    // EFI_NOT_READY is normal when no message is pending; anything else repeated
    // forever means the upper stack is gone. Do not keep calling into it.
    //
    Errors++;
    if ((Errors == 1) || ((Errors % 500) == 0)) {
      UsbStartSay ("[USBSTART] HandleEvent -> %r (errors=%u)\r\n", Status, Errors);
    }
    if (Errors >= 1000) {
      UsbStartSay ("[USBSTART] pump disabled after %u errors\r\n", Errors);
      UsbReleaseEventPump ();
      return;
    }
  } else {
    Errors = 0;
  }

  if ((Event != UsbDeviceEventNoEvent) && (Tick <= 200)) {
    UsbStartSay ("[USBSTART] event=%u size=%u t=%u\r\n",
                 (UINT32)Event, (UINT32)DataSize, Tick);
  }

  if ((Tick != 1) && ((Tick % 500) != 0)) {
    return;
  }

  UsbStartSay ("[USBSTART] alive t=%u ms DCTL=0x%08x DSTS=0x%08x DEVTEN=0x%08x\r\n",
               Tick,
               MmioRead32 (Base + 0x0C704),
               MmioRead32 (Base + 0x0C70C),
               MmioRead32 (Base + 0x0C708));
}

STATIC EFI_STATUS
UsbConfigureSwitch (VOID)
{
  EFI_GUID I2cGuid = {
    0xB27AE8B1, 0x3E10, 0x4D07, { 0xAB, 0x5C, 0xEB, 0x9A, 0x6D, 0xC6, 0xFA, 0x8F }
  };
  typedef UINT32 (EFIAPI *I2C_OPEN)(UINT32, UINT64 *);
  typedef UINT32 (EFIAPI *I2C_CLOSE)(UINT64);
  UINT8 *Protocol = NULL;
  I2C_OPEN Open;
  I2C_CLOSE Close;
  UINT32 Config[6] = { 100, 0x0E, 0, 500, 0, 0 };
  WCD_USB_IO Io;
  UINT32 Rc;
  EFI_STATUS Status;
  UINTN Index;
  STATIC CONST UINT16 Registers[] = { 0x403,0x404,0x406,0x407,0x413,0x414,0x415,0x417,0x419,0x41B,0x41C };

  Status = gBS->LocateProtocol (&I2cGuid, NULL, (VOID **)&Protocol);
  if (EFI_ERROR (Status) || Protocol == NULL) return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  Open = (I2C_OPEN)*(UINT64 *)(Protocol + 8);
  // QC I2C: +20 is the multi-descriptor Transfer method; +28 is Close.
  // Stock I2C.efi table 90D8 has Transfer=2C70 and Close=30E0. Calling
  // Transfer with one argument can take its invalid-parameter return after
  // RaiseTPL(8), before RestoreTPL, leaking TPL into the DXE dispatcher.
  Close = (I2C_CLOSE)*(UINT64 *)(Protocol + 0x28);
  ZeroMem (&Io, sizeof (Io));
  Io.Config = Config;
  Io.Read = (WCD_I2C_TRANSFER)*(UINT64 *)(Protocol + 0x10);
  Io.Write = (WCD_I2C_TRANSFER)*(UINT64 *)(Protocol + 0x18);
  if (Open == NULL || Io.Read == NULL || Io.Write == NULL || Close == NULL) return EFI_UNSUPPORTED;
  Rc = Open (0x0E, &Io.Handle);
  UsbStartSay ("[WCD b200] open rc=%u handle=%lx\r\n", Rc, Io.Handle);
  if (Rc != 0 || Io.Handle == 0) return EFI_DEVICE_ERROR;
  Status = WcdResetUsb (&Io);
  if (!EFI_ERROR (Status)) Status = WcdConfigureUsb (&Io);
  UsbStartSay ("[WCD b200] USB DP/DM routing -> %r\r\n", Status);
  for (Index = 0; Index < ARRAY_SIZE (Registers); Index++) {
    UINT8 Value;
    EFI_STATUS ReadStatus = WcdReadRegister (&Io, Registers[Index], &Value);
    UsbStartSay ("[WCD b200] page-aware %03x=%02x -> %r\r\n", Registers[Index], Value, ReadStatus);
  }
  Rc = Close (Io.Handle);
  UsbStartSay ("[WCD b224] close rc=%u\r\n", Rc);
  if (Rc != 0 && !EFI_ERROR (Status)) Status = EFI_DEVICE_ERROR;
  return Status;
}

// Select peripheral mode and release HIZ here. Powered reset/tune/device-force
// preparation is owned by PlatformUsbfnLib, after the RPMh supply votes.
STATIC EFI_STATUS
UsbConfigureRepeater (VOID)
{
  EFI_GUID SpmiGuid = {
    0xFA5F306B, 0xF47D, 0x4AC4, { 0xA4, 0x7D, 0x88, 0x2F, 0x82, 0x04, 0xEC, 0x30 }
  };
  typedef UINT32 (EFIAPI *SPMI_READ)(VOID *, UINT32, UINT32, UINT32,
    UINT32, UINT8 *, UINT32, UINT32 *);
  typedef UINT32 (EFIAPI *SPMI_WRITE)(VOID *, UINT32, UINT32, UINT32,
    UINT32, UINT8 *, UINT32);
  STATIC CONST UINT32 Registers[] = { 0xFD40, 0xFD41 };
  VOID *Protocol;
  SPMI_READ Read;
  SPMI_WRITE Write;
  EFI_STATUS Status;
  UINT32 Rc;
  UINT32 Done;
  UINT8 Value;
  UINT8 Wanted;
  UINT8 Verify;
  UINTN Index;

  Protocol = NULL;
  Status = gBS->LocateProtocol (&SpmiGuid, NULL, &Protocol);
  if (EFI_ERROR (Status) || (Protocol == NULL)) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }
  Read = (SPMI_READ)*(UINT64 *)((UINT8 *)Protocol + 0x08);
  Write = (SPMI_WRITE)*(UINT64 *)((UINT8 *)Protocol + 0x10);
  if ((Read == NULL) || (Write == NULL)) {
    return EFI_UNSUPPORTED;
  }
  for (Index = 0; Index < ARRAY_SIZE (Registers); Index++) {
    Value = 0;
    Done = 0;
    Rc = Read (Protocol, 0, 7, 0, Registers[Index], &Value, 1, &Done);
    if ((Rc != 0) || (Done != 1)) {
      UsbStartSay ("[EUSB2 b223] route %04x unavailable rc=%u bytes=%u/1\r\n", Registers[Index], Rc, Done);
      return EFI_DEVICE_ERROR;
    }
    Wanted = (Index == 0) ? (UINT8)((Value & ~3U) | BIT0) : (UINT8)(Value & ~BIT7);
    if (Wanted != Value) {
      Rc = Write (Protocol, 0, 7, 0, Registers[Index], &Wanted, 1);
      if (Rc != 0) {
        return EFI_DEVICE_ERROR;
      }
    }
    Verify = 0;
    Done = 0;
    Rc = Read (Protocol, 0, 7, 0, Registers[Index], &Verify, 1, &Done);
    UsbStartSay ("[EUSB2 b223] route %04x %02x->%02x rc=%u bytes=%u/1\r\n",
                 Registers[Index], (UINT32)Value, (UINT32)Verify, Rc, Done);
    if ((Rc != 0) || (Done != 1) || (Verify != Wanted)) {
      return EFI_DEVICE_ERROR;
    }
  }
  return EFI_SUCCESS;
}

//
// "Power Control USB", published by UsbPwrCtrlDxe (GUID at RVA 0x8068, interface
// struct at RVA 0x80E8). Every method is gated by a single byte at RVA 0x8378
// which only becomes 1 when the driver's PMIC backend init at 0x4038 returns
// EFI_SUCCESS. When it does not, all methods return EFI_UNSUPPORTED and
// UsbConfigDxe can never learn that a cable is attached - so the redriver is
// never switched to device mode and the host never sees a D+ pull-up.
//
// vtable offsets used here:
//   +0x10  GetVbusStatus        (UINT8 Port, OUT VOID *Status)
//   +0x30  GetTypeCPortStatus   (UINT8 Port, OUT VOID *Status)
//
STATIC EFI_GUID mPowerControlUsbGuid = {
  0xE07DF17E, 0xE79E, 0x4150, { 0x93, 0x78, 0x50, 0x62, 0x3A, 0x14, 0x99, 0x4A }
};
STATIC EFI_GUID mPmicSchgGuid = {
  0xAE6AE96E, 0x483F, 0x42AE, { 0x9C, 0xC1, 0x9F, 0xAC, 0x1B, 0x58, 0x47, 0x28 }
};
STATIC EFI_GUID mAdcGuid = {
  0x61740CC4, 0x916F, 0x4471, { 0xB0, 0xDC, 0x00, 0xC6, 0xF4, 0x3A, 0x7F, 0xC6 }
};
STATIC EFI_GUID mPmicVersionGuid = {
  0x4684800A, 0x2755, 0x4EDC, { 0xB4, 0x43, 0x7F, 0x8C, 0xEB, 0x32, 0x39, 0xD3 }
};
STATIC EFI_GUID mPlatformTypeGuid = {
  0x157A5C45, 0x21B2, 0x43C5, { 0xBA, 0x7C, 0x82, 0x2F, 0xEE, 0x5F, 0xE5, 0x99 }
};

typedef EFI_STATUS (EFIAPI *PWR_PORT_GET_STATUS) (IN UINT8 Port, OUT VOID *Status);

STATIC VOID
UsbProbePowerControl (
  IN CONST CHAR8 *Tag
  )
{
  EFI_STATUS          Status;
  VOID                *Iface;
  PWR_PORT_GET_STATUS GetTypeCPortStatus;
  PWR_PORT_GET_STATUS GetVbusStatus;
  UINT8               Raw[32];
  UINTN               Index;

  Iface  = NULL;
  Status = gBS->LocateProtocol (&mPowerControlUsbGuid, NULL, &Iface);
  UsbStartSay ("[PWR %a] locate PowerCtrl -> %r iface=0x%lx\r\n",
               Tag, Status, (UINT64)(UINTN)Iface);
  if (EFI_ERROR (Status) || (Iface == NULL)) {
    return;
  }

  //
  // Print raw bytes only. The struct layout below 0x565C is partly decoded but
  // not proven, so the probe reports what the firmware produced rather than an
  // interpretation of it.
  //
  GetTypeCPortStatus = (PWR_PORT_GET_STATUS)(UINTN)(((UINT64 *)Iface)[6]);
  GetVbusStatus      = (PWR_PORT_GET_STATUS)(UINTN)(((UINT64 *)Iface)[2]);

  ZeroMem (Raw, sizeof (Raw));
  Status = GetTypeCPortStatus (0, Raw);
  UsbStartSay ("[PWR %a] GetTypeCPortStatus(0) -> %r raw:", Tag, Status);
  for (Index = 0; Index < sizeof (Raw); Index++) {
    UsbStartSay (" %02x", Raw[Index]);
  }
  UsbStartSay ("\r\n");

  ZeroMem (Raw, sizeof (Raw));
  Status = GetVbusStatus (0, Raw);
  UsbStartSay ("[PWR %a] GetVbusStatus(0) -> %r raw=%08x\r\n",
               Tag, Status, *(UINT32 *)Raw);
}

STATIC VOID
UsbProbePmicProtocols (VOID)
{
  STATIC CONST struct {
    EFI_GUID    *Guid;
    CONST CHAR8 *Name;
  } Table[] = {
    { &mPmicSchgGuid,     "PmicSchg"    },
    { &mAdcGuid,          "Adc"         },
    { &mPmicVersionGuid,  "PmicVersion" },
    { &mPlatformTypeGuid, "PlatType"    },
  };
  EFI_STATUS  Status;
  VOID        *Iface;
  UINTN       Index;
  EFI_PLATFORMINFO_PLATFORM_INFO_TYPE Info;
  EFI_PLATFORMINFO_PROTOCOL *Platform;
  UINT32 Extension;

  for (Index = 0; Index < ARRAY_SIZE (Table); Index++) {
    Iface  = NULL;
    Status = gBS->LocateProtocol (Table[Index].Guid, NULL, &Iface);
    UsbStartSay ("[PWR] locate %a -> %r iface=0x%lx\r\n",
                 Table[Index].Name, Status, (UINT64)(UINTN)Iface);
    if (Table[Index].Guid == &mPlatformTypeGuid && !EFI_ERROR (Status) && Iface != NULL) {
      Platform = (EFI_PLATFORMINFO_PROTOCOL *)Iface;
      ZeroMem (&Info, sizeof (Info));
      Extension = 0;
      Status = PlatformInfoGetSafe (Platform, &Info, &Extension);
      UsbStartSay ("[USBPLAT b215] rev=%lx GetPlatformInfo -> %r\r\n",
                   Platform->Version, Status);
      if (!EFI_ERROR (Status)) {
        UsbStartSay ("[USBPLAT b215] type=%x version=%x subtype=%x fusion=%u extension=%08x QRD-redriver=%u\r\n",
                     (UINT32)Info.platform, Info.version, Info.subtype,
                     (UINT32)Info.fusion, Extension,
                     (UINT32)(Info.platform == EFI_PLATFORMINFO_TYPE_QRD));
      }
    }
  }
}

// Observe stage boundaries without repairing or hiding a leaked TPL.
STATIC VOID
UsbStartTplReport (IN CONST CHAR8 *Stage)
{
  EFI_TPL Current;

  Current = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Current);
  UsbStartSay ("[USBSTART b224] tpl %a=%u\r\n", Stage, (UINT32)Current);
}

EFI_STATUS EFIAPI
UsbDevStartEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  UsbStartSay ("[USBSTART] stock-chain-i2c-b224 %a %a\r\n", __DATE__, __TIME__);
  UsbStartTplReport ("entry");
  UsbProbePowerControl ("early");
  UsbStartTplReport ("power-early");
#if USB_BRINGUP_AUTOSTART != 2
  Status = gBS->LocateProtocol (&gEfiUsbDeviceProtocolGuid, NULL, (VOID **)&mUsbDevice);
  if (EFI_ERROR (Status)) {
    return Status;
  }
#endif
  Status = UsbConfigureSwitch ();
  UsbStartTplReport ("switch");
  UsbStartSay ("[USBSTART] WCD switch -> %r\r\n", Status);
  if (EFI_ERROR (Status)) return Status;
  Status = UsbConfigureRepeater ();
  UsbStartTplReport ("repeater");
  UsbStartSay ("[USBSTART] eUSB2 repeater -> %r\r\n", Status);

  //
  // Second sample after local switch/repeater setup. Protocol absence describes
  // this point in dispatch only; dependencies can still publish later.
  //
  UsbProbePmicProtocols ();
  UsbProbePowerControl ("late");
  UsbStartTplReport ("power-late");

  //
  // Log HAL IOMMU presence. The active source driver separately creates and
  // attaches its persistent USB domain; a successful Locate alone is not DMA
  // validation. The stock UsbConfig owner is absent from this build.
  //
  {
    EFI_GUID HalIommuGuid = {
      0x54B6D3B4, 0x5D33, 0x4F91, { 0x86, 0x00, 0x6C, 0x41, 0xD5, 0xDE, 0xB1, 0x9A }
    };
    VOID       *HalIommu;
    EFI_STATUS  IommuStatus;

    HalIommu    = NULL;
    IommuStatus = gBS->LocateProtocol (&HalIommuGuid, NULL, &HalIommu);
    UsbStartSay ("[USBSTART] HAL IOMMU -> %r handle=0x%lx\r\n",
                 IommuStatus, (UINT64)(UINTN)HalIommu);
  }

  //
  // UsbDeviceDxe opens UsbFnIo during its own start and caches a failure from the
  // first dispatch, which happened before UsbfnIo existed. Disconnect/connect
  // re-runs that start now that both protocols are published.
  //
#if USB_BRINGUP_AUTOSTART != 2
  {
    EFI_HANDLE *Handles;
    UINTN      Count;

    Handles = NULL;
    Count   = 0;
    if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbDeviceProtocolGuid,
                                            NULL, &Count, &Handles)) && (Count > 0)) {
      gBS->DisconnectController (Handles[0], NULL, NULL);
      gBS->ConnectController (Handles[0], NULL, NULL, TRUE);
      gBS->FreePool (Handles);
      UsbStartSay ("[USBSTART] UsbDevice reconnected\r\n");
      gBS->Stall (20000);
      mUsbDevice = NULL;
      Status = gBS->LocateProtocol (&gEfiUsbDeviceProtocolGuid, NULL, (VOID **)&mUsbDevice);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }
#endif

#if USB_BRINGUP_AUTOSTART == 2
  //
  // Drive this driver directly, with the vendor stack out of the way.
  //
  // With the stack in the path the machine dies somewhere between StartController returning
  // and any UsbfnIo entry point being called, and none of the eighteen entry markers fire -
  // so the fault is inside vendor code that this image cannot see into. Calling the two
  // protocol functions here answers the question that actually matters: does this driver's
  // controller start and EP0 arming work at all, on its own.
  //
  {
    EFI_USBFN_IO_PROTOCOL        *FnIo;
    EFI_USB_DEVICE_INFO           DevInfo;
    EFI_STATUS                    ProbeStatus;
    EFI_USB_ENDPOINT_DESCRIPTOR  *EpTable[2];
    EFI_USB_INTERFACE_INFO        IfaceInfo;
    EFI_USB_INTERFACE_INFO       *IfaceTable[2];
    EFI_USB_CONFIG_INFO           ConfigInfo;
    EFI_USB_CONFIG_INFO          *ConfigTable[2];

    FnIo = NULL;
    ProbeStatus = gBS->LocateProtocol (&gEfiUsbFunctionIoProtocolGuid, NULL, (VOID **)&FnIo);
    UsbStartSay ("[USBSTART] direct: UsbfnIo -> %r\r\n", ProbeStatus);
    if (!EFI_ERROR (ProbeStatus) && (FnIo != NULL)) {
      //
      // ConfigureEnableEndpoints validates four things before it touches hardware: a config
      // table, an interface table inside it, an interface descriptor and an endpoint table.
      // A zeroed structure fails the first of those and returns EFI_INVALID_PARAMETER, which
      // is what the previous build reported - the endpoint path was never reached at all.
      //
      EpTable[0]    = &mConfig.Endpoint;
      EpTable[1]    = NULL;

      IfaceInfo.InterfaceDescriptor     = &mConfig.Interface;
      IfaceInfo.EndpointDescriptorTable = EpTable;
      IfaceTable[0]                     = &IfaceInfo;
      IfaceTable[1]                     = NULL;

      ConfigInfo.ConfigDescriptor   = &mConfig.Config;
      ConfigInfo.InterfaceInfoTable = IfaceTable;
      ConfigTable[0]                = &ConfigInfo;
      ConfigTable[1]                = NULL;

      ZeroMem (&DevInfo, sizeof (DevInfo));
      DevInfo.DeviceDescriptor = &mDevice;
      DevInfo.ConfigInfoTable  = ConfigTable;

      UsbStartSay ("[USBSTART] direct: StartController\r\n");
      ProbeStatus = FnIo->StartController (FnIo);
      UsbStartTplReport ("controller");
      UsbStartSay ("[USBSTART] direct: StartController -> %r\r\n", ProbeStatus);

      if (!EFI_ERROR (ProbeStatus)) {
        UsbStartSay ("[USBSTART] direct: ConfigureEnableEndpoints\r\n");
        ProbeStatus = FnIo->ConfigureEnableEndpoints (FnIo, &DevInfo);
        UsbStartTplReport ("endpoints");
        UsbStartSay ("[USBSTART] direct: ConfigureEnableEndpoints -> %r\r\n", ProbeStatus);
        if (ProbeStatus == EFI_NOT_READY) {
          UsbStartSay ("[USBSAFE b200] EP0/event DMA disabled; open GUI USB TEST to run once\r\n");
        }

        //
        // EP0 is armed. Pump the handler so anything the host sends is at least seen.
        //
        if (!EFI_ERROR (ProbeStatus) || ProbeStatus == EFI_NOT_READY) {
          mDirectFnIo = FnIo;
          ProbeStatus = FnIo->AllocateTransferBuffer (FnIo, 4096, &mDirectBuffer);
          UsbStartTplReport ("buffer");
          UsbStartSay ("[USBSTART] descriptor buffer -> %r\r\n", ProbeStatus);
          if (EFI_ERROR (ProbeStatus)) {
            return ProbeStatus;
          }
          ProbeStatus = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                          UsbDirectPoll, NULL, &mDirectEvent);
          UsbStartSay ("[USBSTART] direct: timer create -> %r\r\n", ProbeStatus);
          if (!EFI_ERROR (ProbeStatus)) {
            ProbeStatus = gBS->SetTimer (mDirectEvent, TimerPeriodic, 10000);
            UsbStartSay ("[USBSTART] direct: timer arm -> %r\r\n", ProbeStatus);
          }
          UsbStartTplReport ("timer");
        }
      }
    }
  }

  UsbStartTplReport ("exit");
  return EFI_SUCCESS;
#endif

#if USB_BRINGUP_AUTOSTART == 0
  //
  // Leave the controller alone, but say what the stack looks like.
  //
  // Two protocols answer the question without touching hardware. UsbfnIo present means this
  // driver's controller object exists and is ready to be started. UsbDevice present means the
  // vendor device stack loaded. What neither shows is whether UsbConfigDxe, which is the
  // component that sets up the IOMMU domain, ever completed its own init - it publishes
  // QcomUsbConfig, and UsbDeviceDxe connects to that.
  //
  {
    VOID       *Probe;
    EFI_STATUS  ProbeStatus;
    UINTN       HandleCount;
    EFI_HANDLE  *HandleList;

    Probe = NULL;
    ProbeStatus = gBS->LocateProtocol (&gEfiUsbFunctionIoProtocolGuid, NULL, &Probe);
    UsbStartSay ("[USBSTART] probe UsbfnIo   -> %r handle=0x%lx\r\n",
                 ProbeStatus, (UINT64)(UINTN)Probe);

    Probe = NULL;
    ProbeStatus = gBS->LocateProtocol (&gEfiUsbDeviceProtocolGuid, NULL, &Probe);
    UsbStartSay ("[USBSTART] probe UsbDevice -> %r handle=0x%lx\r\n",
                 ProbeStatus, (UINT64)(UINTN)Probe);

    HandleList  = NULL;
    HandleCount = 0;
    ProbeStatus = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbFunctionIoProtocolGuid,
                                           NULL, &HandleCount, &HandleList);
    UsbStartSay ("[USBSTART] probe UsbfnIo handles -> %r count=%u\r\n",
                 ProbeStatus, (UINT32)HandleCount);
    if (HandleList != NULL) {
      gBS->FreePool (HandleList);
    }

    //
    // Native PmicGlinkDxe +1728 installs GUID +A068 and table +A0B8.
    // The previously probed B5062BE7 is a shared kernel interface.
    //
    {
      EFI_GUID  PmicGlink = {
        0x7429EB07, 0xF2E3, 0x4A6B, { 0xAA, 0x96, 0xC9, 0x09, 0xCD, 0x95, 0x6E, 0xF7 }
      };

      Probe = NULL;
      ProbeStatus = gBS->LocateProtocol (&PmicGlink, NULL, &Probe);
      UsbStartSay ("[USBSTART b320] probe PmicGlink 7429eb07 -> %r handle=0x%lx\r\n",
                   ProbeStatus, (UINT64)(UINTN)Probe);
    }
  }

  UsbStartSay ("[USBSTART] USB_BRINGUP_AUTOSTART=0, controller left alone\r\n");
  return EFI_SUCCESS;
#endif

  Status = mUsbDevice->Start (&mDevice, mDescriptors, &mQualifier, mBos, 0, NULL);
  UsbStartSay ("[USBSTART] Start -> %r\r\n", Status);
  UsbStartSay ("[USBSTART] post-start DCTL=0x%08x DSTS=0x%08x\r\n",
               MmioRead32 (0x0A600000 + 0x0C704),
               MmioRead32 (0x0A600000 + 0x0C70C));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UsbStartSay ("[USBSTART] mk1 creating poll event\r\n");
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, UsbPoll, NULL, &mPollEvent);
  if (EFI_ERROR (Status)) {
    mUsbDevice->Stop ();
    return Status;
  }
  UsbStartSay ("[USBSTART] mk2 creating exit-boot event\r\n");
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, UsbExitBootServices,
                              NULL, &gEfiEventExitBootServicesGuid, &mExitEvent);
  if (EFI_ERROR (Status)) {
    UsbReleaseEventPump ();
    mUsbDevice->Stop ();
    return Status;
  }
  UsbStartSay ("[USBSTART] mk3 overriding entry points\r\n");
  mStart = mUsbDevice->Start;
  mStartEx = mUsbDevice->StartEx;
  mStop = mUsbDevice->Stop;
  mHandleEvent = mUsbDevice->HandleEvent;
  mUsbDevice->Start = UsbClientStart;
  mUsbDevice->StartEx = UsbClientStartEx;
  mUsbDevice->Stop = UsbClientStop;
  mUsbDevice->HandleEvent = UsbClientHandleEvent;

  //
  // The pump is on. UsbConfigDxe is the component that would normally drive
  // UsbDevice->HandleEvent, and its start path is blocked by the missing GLINK/QMI
  // dependency, so this driver takes that job. Start() above has already run the
  // state-machine initialisation, which is the part that used to be missing.
  //
  UsbStartSay ("[USBSTART] mk4 arming pump\r\n");
  Status = gBS->SetTimer (mPollEvent, TimerPeriodic, 10000);
  if (EFI_ERROR (Status)) {
    UsbReleaseEventPump ();
    mStop ();
    return Status;
  }

  UsbStartSay ("[USBSTART] pump active, 1 ms\r\n");
  return EFI_SUCCESS;
}
