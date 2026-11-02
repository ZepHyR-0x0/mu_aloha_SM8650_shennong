#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <IndustryStandard/Usb.h>

//
// Defined in Dwc3Core.c: names a DSTS.USBLNKST value.
//
CONST CHAR8 *
Dwc3LinkName (
  IN UINT32  State
  );


#include "UsbfnDwc3Dxe.h"

STATIC CHAR16 mManufacturerName[] = L"Mu-Silicium";
STATIC CHAR16 mProductName[]      = L"Samsung Exynos Device";
STATIC CHAR16 mSerialNumber[]     = L"0123456789ABCDEF";

STATIC
VOID
Dwc3BuildSerialNumber (VOID)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3BuildSerialNumber\n");
    }
    Seen++;
  }
  //
  // The reference implementation reads the SoC serial from the Qualcomm ChipInfo
  // protocol. That header in this tree spells the type EFI_CHIPINFO_PROTOCOL and names
  // its methods differently, and the serial number has no bearing on whether the host
  // enumerates - it only fills iSerialNumber. The static value from mSerialNumber is
  // kept instead of pulling in another protocol dependency.
  //
  return;
}

STATIC
EFI_STATUS
Dwc3ValidateEndpoint (
  IN  DWC3_DEV                     *Dev,
  IN  UINT8                         EndpointIndex,
  IN  EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  OUT UINT8                        *PhysEpNum)
{
  UINT8 Phys;

  if (Direction != EfiUsbEndpointDirectionDeviceRx && Direction != EfiUsbEndpointDirectionDeviceTx) {
    return EFI_INVALID_PARAMETER;
  }

  if (EndpointIndex >= DWC3_MAX_LOGICAL_EP) {
    return EFI_INVALID_PARAMETER;
  }

  Phys = DWC3_PHYS_EP (EndpointIndex, Direction);

  if (Phys >= Dev->NumPhysicalEps) {
    return EFI_INVALID_PARAMETER;
  }

  *PhysEpNum = Phys;

  return EFI_SUCCESS;
}

STATIC
UINT16
Dwc3MaxPacketSizeForSpeed (
  IN EFI_USB_ENDPOINT_TYPE EndpointType,
  IN EFI_USB_BUS_SPEED     BusSpeed)
{
  if (EndpointType == UsbEndpointControl) {
    switch (BusSpeed) {
      case UsbBusSpeedLow:
        return 8;
      case UsbBusSpeedSuper:
        return 512;
      default:
        return 64;
    }
  }

  switch (BusSpeed) {
    case UsbBusSpeedSuper:
      return 1024;
    case UsbBusSpeedHigh:
      return 512;
    default:
      return 64;
  }
}

STATIC
BOOLEAN
Dwc3IsLinkDisconnected (IN DWC3_DEV *Dev)
{
  if (!Dev->Started) {
    return TRUE;
  }

  // Hacky, ideally we talk to whatever PD controller is here to check VBUS and cable state.
  return (BOOLEAN)(DWC3_DSTS_USBLNKST (MmioRead32 (Dev->Base + DWC3_DSTS)) == DWC3_LINK_STATE_SS_DIS);
}

STATIC
EFI_STATUS
EFIAPI
Dwc3DetectPort (
  IN  EFI_USBFN_IO_PROTOCOL *This,
  OUT EFI_USBFN_PORT_TYPE   *PortType)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3DetectPort\n");
    }
    Seen++;
  }
  DWC3_DEV *Dev;

  if (This == NULL || PortType == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  if (!UsbfnPlatformIsVbusPresent () || Dwc3IsLinkDisconnected (Dev)) {
    *PortType = EfiUsbUnknownPort;
    return EFI_SUCCESS;
  }

  if (Dev->Started && Dev->Speed != UsbBusSpeedUnknown) {
    *PortType = EfiUsbStandardDownstreamPort;
  } else {
    *PortType = EfiUsbUnknownPort;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3ConfigureEnableEndpoints (
  IN  EFI_USBFN_IO_PROTOCOL *This,
  OUT EFI_USB_DEVICE_INFO   *DeviceInfo)
{
  DWC3_DEV                    *Dev;
  EFI_STATUS                   Status;
  EFI_USB_CONFIG_INFO         *ConfigInfo;
  EFI_USB_INTERFACE_INFO      *InterfaceInfo;
  EFI_USB_ENDPOINT_DESCRIPTOR *EpDesc;
  UINT8                        Index;
  UINT8                        PhysEpNum;
  UINT8                        EpType;
  UINT16                       MaxPacketSize;

  RefSay ("[REF] ConfigureEnableEndpoints ENTER\n");


  if (This == NULL || DeviceInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  if (!Dev->Started) {
    return EFI_NOT_READY;
  }

  if (DeviceInfo->ConfigInfoTable == NULL || DeviceInfo->ConfigInfoTable[0] == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ConfigInfo = DeviceInfo->ConfigInfoTable[0];

  if (ConfigInfo->InterfaceInfoTable == NULL || ConfigInfo->InterfaceInfoTable[0] == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  InterfaceInfo = ConfigInfo->InterfaceInfoTable[0];

  if (InterfaceInfo->InterfaceDescriptor == NULL || InterfaceInfo->EndpointDescriptorTable == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  MaxPacketSize = Dwc3MaxPacketSizeForSpeed (UsbEndpointControl, Dev->Platform.MaximumSpeed);

  RefSay ("[REF] StartDevice: base=0x%lx eps=%u maxspeed=%u mps=%u\n",
          (UINT64)Dev->Base, (UINT32)Dev->NumPhysicalEps,
          (UINT32)Dev->Platform.MaximumSpeed, MaxPacketSize);
  //
  // The CPU driver is certainly up by now, so make sure every DMA buffer is really mapped
  // uncached before any of them is handed to the controller.
  //
  Dwc3ForceAllBuffersUncached (Dev);

  //
  // The core fetches every TRB and writes every event over AXI. A buffer below DDR
  // Bank 0 is not memory the controller can reach, and the failure is not an error
  // return: the endpoint command that names it never completes. Refuse here, before
  // the first STARTTRANSFER, instead of discovering it in RunStop afterwards.
  //
  Status = Dwc3ValidateDmaBuffers (Dev);
  if (EFI_ERROR (Status)) {
    RefSay ("[REF] StartDevice: a DMA buffer is outside DDR, refusing to arm endpoints\n");
    return Status;
  }

  RefSay ("[REF] StartDevice: EP0 OUT enable\n");
  Status = Dwc3EnableEndpoint (Dev, DWC3_EP0_OUT, DWC3_DEPCMD_TYPE_CONTROL, MaxPacketSize, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "DWC3: Failed to configure EP0 OUT! Status = %r\n", Status));
    return Status;
  }
  RefSay ("[REF] StartDevice: EP0 OUT done\n");

  RefSay ("[REF] StartDevice: EP0 IN enable\n");
  Status = Dwc3EnableEndpoint (Dev, DWC3_EP0_IN, DWC3_DEPCMD_TYPE_CONTROL, MaxPacketSize, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "DWC3: Failed to configure EP0 IN! Status = %r\n", Status));
    return Status;
  }
  RefSay ("[REF] StartDevice: EP0 IN done\n");

  for (Index = 0; Index < InterfaceInfo->InterfaceDescriptor->NumEndpoints; Index++) {
    EpDesc = InterfaceInfo->EndpointDescriptorTable[Index];

    if (EpDesc == NULL) {
      continue;
    }

    if ((EpDesc->Attributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_BULK) {
      DEBUG ((EFI_D_ERROR, "DWC3: Endpoint 0x%02x is not Bulk!\n", EpDesc->EndpointAddress));
      return EFI_UNSUPPORTED;
    }

    EpType    = DWC3_DEPCMD_TYPE_BULK;
    PhysEpNum = (UINT8)(((EpDesc->EndpointAddress & 0x0F) << 1) |
                        ((EpDesc->EndpointAddress & USB_ENDPOINT_DIR_IN) ? 1 : 0));

    if (PhysEpNum >= Dev->NumPhysicalEps) {
      return EFI_INVALID_PARAMETER;
    }

    MaxPacketSize = Dwc3MaxPacketSizeForSpeed (UsbEndpointBulk, Dev->Platform.MaximumSpeed);

    RefSay ("[REF] StartDevice: bulk ep %u (phys %u) enable\n",
            (UINT32)Index, (UINT32)PhysEpNum);
    Status = Dwc3EnableEndpoint (Dev, PhysEpNum, EpType, MaxPacketSize, FALSE);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "DWC3: Failed to configure Physical EP %u! Status = %r\n", PhysEpNum, Status));
      return Status;
    }
  }
  //
  // Let the endpoint configuration settle before the first STARTTRANSFER.
  //
  // On DWC_usb31 an endpoint command is asynchronous: the U-Boot port for the same IP
  // says so in as many words - the GUCTL2[14] "poll CMDACT" mode is not available there,
  // so CMDACT clearing does not mean the command has reached the endpoint state machine.
  // Restarting a transfer before that has happened desyncs the endpoint, and the symptom
  // it produces is exactly what this port sees: the doorbell is accepted and never
  // completes.
  //
  // This driver issues DEPSTARTCFG, sixteen SETTRANSFRESOURCE, three DEPCFG and then the
  // first STARTTRANSFER back to back, with no gap at all. Linux and U-Boot each spend a
  // function call and a poll per command, which is the same thing more slowly. Give the
  // state machine a moment here.
  //
  RefSay ("[REF] StartDevice: settling endpoint state\n");
  gBS->Stall (2000);

  //
  // Run the core first, then arm EP0.
  //
  // This is the order Linux uses - dwc3_gadget_start() configures EP0, then
  // dwc3_gadget_run_stop(true), and only later does anything queue a transfer on EP0 - and
  // it is the order this driver had backwards.
  //
  // Two earlier observations argued for the old order and neither survives scrutiny. b140
  // put RUN_STOP first and died in the write that sets the bit, but b140 predates the SMMU
  // bypass: at that time ConfigureUsbDomain was being called with argument 1, which turns
  // translation on with both table bases zero, so every DMA the core issued faulted. b162
  // repeated the experiment and reached the same conclusion for the same contaminated
  // reason. A core that faults on its first memory access will hang the APB write that
  // started it, which looks exactly like "the RUN_STOP write hung".
  //
  // With the context bank in bypass (b172) the experiment has never actually been run.
  // It is worth running now because it answers the question that matters more than the
  // doorbell: can this core reach memory at all?
  //
  // RUN_STOP makes the core live, and a live device controller writes events into the ring
  // by DMA. Dwc3RunStop already samples the ring right after the bit takes effect. Events
  // there mean the master works and only the TRB read is broken; an empty ring means the
  // master is dead in both directions and no amount of EP0 work will help.
  //
  RefSay ("[REF] StartDevice: RunStop(TRUE) before arming EP0\n");
  Status = Dwc3RunStop (Dev, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Dev->EnumerationPending = TRUE;
  return Dwc3ActivateEnumeration (Dev);
}

// The bring-up image keeps DMA disabled until the GUI requests a test. Once
// unlocked, EP0 setup and event DMA use the normal controller/event pump.
EFI_STATUS
Dwc3ActivateEnumeration (IN DWC3_DEV *Dev)
{
  UINT32 Dsts;
  UINT32 Link;
  EFI_STATUS Status;

  if (!Dev->EnumerationPending) {
    return EFI_SUCCESS;
  }

  if (!Dev->DmaTestAllowed) {
    return EFI_NOT_READY;
  }

  Dsts = MmioRead32 (Dev->Base + DWC3_DSTS);
  Link = DWC3_DSTS_USBLNKST (Dsts);
  if ((Dsts & DWC3_DSTS_DEVCTRLHLT) != 0) {
    return EFI_NOT_READY;
  }

  // One attempt per controller initialization. A timeout leaves DMA ownership
  // uncertain and must not trigger another doorbell from the periodic callback.
  Dev->EnumerationPending = FALSE;
  Dev->DmaTestAttempted = TRUE;
  RefSay ("[REF] activate: link=%a DSTS=%08x, arming EP0\n", Dwc3LinkName (Link), Dsts);
  Status = Dwc3Ep0OutStart (Dev);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RefSay ("[REF] activate: EP0 armed, enabling device event DMA\n");
  MmioWrite32 (Dev->Base + DWC3_DEVTEN,
               DWC3_DEVTEN_DISCONNEVTEN | DWC3_DEVTEN_USBRSTEN |
               DWC3_DEVTEN_CONNECTDONEEN | DWC3_DEVTEN_ULSTCNGEN |
               DWC3_DEVTEN_WKUPEVTEN | DWC3_DEVTEN_ERRTICERREN |
               DWC3_DEVTEN_EVNTOVERFLOWEN);
  RefSay ("[REF] activate: DEVTEN=%08x eventcount=%08x\n",
          MmioRead32 (Dev->Base + DWC3_DEVTEN),
          MmioRead32 (Dev->Base + DWC3_GEVNTCOUNT (0)));
  return Dwc3ReconnectEnumeration (Dev);
}

// The diagnostic arms EP0 long after RUN_STOP first exposed the device. The
// host may have abandoned that connection; give it a fresh connect only after
// the setup TRB and event service are ready. Never retry a failed command.
EFI_STATUS
Dwc3ReconnectEnumeration (IN DWC3_DEV *Dev)
{
  UINT32 Events;
  UINT32 Control;
  UINT32 Timeout;
  EFI_STATUS Status;

  if (!Dev->DmaTestAllowed || Dev->Ep0DmaUnsafe) return EFI_NOT_READY;
  if ((MmioRead32 (Dev->Base + DWC3_DCTL) & DWC3_DCTL_CSFTRST) != 0) return EFI_DEVICE_ERROR;
  Events = MmioRead32 (Dev->Base + DWC3_DEVTEN);
  MmioWrite32 (Dev->Base + DWC3_DEVTEN, 0);
  Status = Dwc3Ep0ResetState (Dev);
  if (EFI_ERROR (Status)) return Status;
  Status = Dwc3RunStop (Dev, FALSE);
  if (EFI_ERROR (Status)) return Status;
  gBS->Stall (250000);
  RefSay ("[ENUM b200] disconnected; queue fresh SETUP before pull-up\n");
  Status = Dwc3Ep0OutStart (Dev);
  if (EFI_ERROR (Status)) return Status;
  Control = MmioRead32 (Dev->Base + DWC3_DCTL);
  if ((Control & DWC3_DCTL_CSFTRST) != 0) return EFI_DEVICE_ERROR;

  MmioWrite32 (Dev->Base + DWC3_DEVTEN, Events);
  // Dwc3RunStop(TRUE) is a diagnostic start path which masks DEVTEN. Here we
  // must keep event DMA enabled for the host's new RESET/CONNECT_DONE/SETUP.
  MmioWrite32 (Dev->Base + DWC3_DCTL, Control | DWC3_DCTL_RUN_STOP);
  for (Timeout = 0; Timeout < 50000; Timeout++) {
    if ((MmioRead32 (Dev->Base + DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT) == 0) {
      RefSay ("[ENUM b200] reconnected DCTL=%08x DSTS=%08x DEVTEN=%08x count=%08x\n",
              MmioRead32 (Dev->Base + DWC3_DCTL), MmioRead32 (Dev->Base + DWC3_DSTS),
              MmioRead32 (Dev->Base + DWC3_DEVTEN), MmioRead32 (Dev->Base + DWC3_GEVNTCOUNT (0)));
      return EFI_SUCCESS;
    }
    gBS->Stall (1);
  }
  MmioWrite32 (Dev->Base + DWC3_DEVTEN, 0);
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS EFIAPI
Dwc3ManualTestMode (IN USB_BRINGUP_PROTOCOL *This, IN BOOLEAN FullSpeed)
{
  DWC3_DEV *Dev;
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  UINT32 Config;

  if (This == NULL || This->Context == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Dev = This->Context;
  if (!Dev->Started || !Dev->ConfigStarted) {
    return EFI_NOT_READY;
  }
  if (Dev->DmaTestAttempted) {
    return EFI_ALREADY_STARTED;
  }

  // UsbDirectPoll also runs at TPL_CALLBACK, so this excludes its event pump.
  // Platform SPMI methods take an EFI_LOCK at TPL_CALLBACK: TPL_NOTIFY here
  // made those nested RaiseTPL calls lower the current TPL (b213 assertions).
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  RefSay ("[USBTEST b214] critical TPL=8; platform locks and direct timer share CALLBACK\n");
  RefSay ("[USBTEST b200] before: GCTL=%08x DCTL=%08x DSTS=%08x DEVTEN=%08x\n",
          MmioRead32 (Dev->Base + DWC3_GCTL), MmioRead32 (Dev->Base + DWC3_DCTL),
          MmioRead32 (Dev->Base + DWC3_DSTS), MmioRead32 (Dev->Base + DWC3_DEVTEN));
  if (((MmioRead32 (Dev->Base + DWC3_GCTL) >> 12) & 3) != DWC3_GCTL_PRTCAP_DEVICE) {
    RefSay ("[USBTEST b200] controller is not in device mode; no DMA attempted\n");
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  UsbfnPlatformDmaReport ();
  Status = UsbfnEnsureUsbStream ();
  if (EFI_ERROR (Status)) {
    RefSay ("[USBTEST b200] USB stream validation failed %r; no DMA attempted\n", Status);
    gBS->RestoreTPL (OldTpl);
    return Status;
  }
  Status = RpmhUsbNocOn ();
  if (EFI_ERROR (Status)) {
    RefSay ("[USBTEST b200] NoC vote failed %r; DMA test cancelled\n", Status);
    gBS->RestoreTPL (OldTpl);
    return Status;
  }
  Status = UsbfnPlatformDmaSnapshot ("before-manual", TRUE);
  if (EFI_ERROR (Status)) {
    gBS->RestoreTPL (OldTpl);
    return Status;
  }
  Dev->DmaFaultTraceBudget = 4;
  // DCFG.DEVSPD=1 is full speed in this IP and the stock kernel header.
  // Change it only while halted, before any SETUP TRB has been submitted.
  // CONNECT_DONE already reconfigures EP0 and bulk MPS for the actual speed.
  Config = MmioRead32 (Dev->Base + DWC3_DCFG);
  if (FullSpeed || (Config & DWC3_DCFG_SPEED_MASK) != DWC3_DCFG_HIGHSPEED) {
    Status = Dwc3RunStop (Dev, FALSE);
    if (EFI_ERROR (Status)) {
      gBS->RestoreTPL (OldTpl);
      return Status;
    }
    Config = MmioRead32 (Dev->Base + DWC3_DCFG);
    Config = (Config & ~DWC3_DCFG_SPEED_MASK) |
             (FullSpeed ? DWC3_DCFG_FULLSPEED : DWC3_DCFG_HIGHSPEED);
    MmioWrite32 (Dev->Base + DWC3_DCFG, Config);
    if ((MmioRead32 (Dev->Base + DWC3_DCFG) & DWC3_DCFG_SPEED_MASK) !=
        (FullSpeed ? DWC3_DCFG_FULLSPEED : DWC3_DCFG_HIGHSPEED)) {
      gBS->RestoreTPL (OldTpl);
      return EFI_DEVICE_ERROR;
    }
  }
  Dev->Platform.MaximumSpeed = FullSpeed ? UsbBusSpeedFull : UsbBusSpeedHigh;
  RefSay ("[USBTEST b213] speed=%a DCFG=%08x; no PHY tuning changed\n",
          FullSpeed ? "full" : "high", Config);
  RefSay ("[USBTEST b200] after vote: GCTL=%08x DCTL=%08x DSTS=%08x\n",
          MmioRead32 (Dev->Base + DWC3_GCTL), MmioRead32 (Dev->Base + DWC3_DCTL),
          MmioRead32 (Dev->Base + DWC3_DSTS));
  if ((MmioRead32 (Dev->Base + DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT) != 0) {
    RefSay ("[USBTEST b200] restart halted controller with device events masked\n");
    Status = Dwc3RunStop (Dev, FALSE);
    if (!EFI_ERROR (Status)) Status = Dwc3RunStop (Dev, TRUE);
    if (EFI_ERROR (Status)) {
      gBS->RestoreTPL (OldTpl);
      return Status;
    }
  }
  Dev->DmaTestAllowed = TRUE;
  Dev->EnumerationPending = TRUE;
  RefSay ("[USBTEST b200] GUI requested one DMA test: DSTS=%08x DEVTEN=%08x\n",
          MmioRead32 (Dev->Base + DWC3_DSTS), MmioRead32 (Dev->Base + DWC3_DEVTEN));
  // Fetching a TRB tests the DMA path independently of host traffic. A manual
  // test may therefore run while DSTS still says Disconnected; no auto retry.
  Status = Dwc3ActivateEnumeration (Dev);
  UsbfnPlatformDmaSnapshot ("after-manual", FALSE);
  if (!EFI_ERROR (Status)) UsbfnPlatformDmaReport ();
  if (EFI_ERROR (Status)) {
    Dev->DmaTestAllowed = FALSE;
    Dev->EnumerationPending = FALSE;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC EFI_STATUS EFIAPI
Dwc3ManualTest (IN USB_BRINGUP_PROTOCOL *This)
{
  return Dwc3ManualTestMode (This, FALSE);
}

STATIC EFI_STATUS EFIAPI
Dwc3ManualTestFullSpeed (IN USB_BRINGUP_PROTOCOL *This)
{
  return Dwc3ManualTestMode (This, TRUE);
}

STATIC
EFI_STATUS
EFIAPI
Dwc3GetEndpointMaxPacketSize (
  IN  EFI_USBFN_IO_PROTOCOL *This,
  IN  EFI_USB_ENDPOINT_TYPE  EndpointType,
  IN  EFI_USB_BUS_SPEED      BusSpeed,
  OUT UINT16                *MaxPacketSize)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3GetEndpointMaxPacketSize\n");
    }
    Seen++;
  }
  DWC3_DEV *Dev;

  if (This == NULL || MaxPacketSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (EndpointType != UsbEndpointControl && EndpointType != UsbEndpointBulk) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  if (BusSpeed == UsbBusSpeedUnknown) {
    BusSpeed = Dev->Platform.MaximumSpeed;
  } else if (BusSpeed > UsbBusSpeedMaximum) {
    return EFI_INVALID_PARAMETER;
  }

  *MaxPacketSize = Dwc3MaxPacketSizeForSpeed (EndpointType, BusSpeed);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3GetDeviceInfo (
  IN     EFI_USBFN_IO_PROTOCOL    *This,
  IN     EFI_USBFN_DEVICE_INFO_ID  Id,
  IN OUT UINTN                    *BufferSize,
  OUT    VOID                     *Buffer OPTIONAL)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3GetDeviceInfo\n");
    }
    Seen++;
  }
  CHAR16 *String;
  UINTN   Size;

  if (This == NULL || BufferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (*BufferSize != 0 && Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  switch (Id) {
    case EfiUsbDeviceInfoSerialNumber:
      String = mSerialNumber;
      break;
    case EfiUsbDeviceInfoManufacturerName:
      String = mManufacturerName;
      break;
    case EfiUsbDeviceInfoProductName:
      String = mProductName;
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  Size = StrSize (String);

  if (*BufferSize < Size) {
    *BufferSize = Size;
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (Buffer, String, Size);
  *BufferSize = Size;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3GetVendorIdProductId (
  IN  EFI_USBFN_IO_PROTOCOL *This,
  OUT UINT16                *Vid,
  OUT UINT16                *Pid)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3GetVendorIdProductId\n");
    }
    Seen++;
  }
  DWC3_DEV *Dev;

  if (This == NULL || Vid == NULL || Pid == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  *Vid = Dev->Platform.VendorId;
  *Pid = Dev->Platform.ProductId;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3AbortTransfer (
  IN EFI_USBFN_IO_PROTOCOL        *This,
  IN UINT8                         EndpointIndex,
  IN EFI_USBFN_ENDPOINT_DIRECTION  Direction)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3AbortTransfer\n");
    }
    Seen++;
  }
  DWC3_DEV  *Dev;
  EFI_STATUS Status;
  UINT8      PhysEpNum;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  Status = Dwc3ValidateEndpoint (Dev, EndpointIndex, Direction, &PhysEpNum);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Dev->Endpoints[PhysEpNum].Enabled) {
    return EFI_INVALID_PARAMETER;
  }

  if (PhysEpNum <= DWC3_EP0_IN) {
    // A bus-reset callback may cancel the old request after SETUP is rearmed.
    if (Dev->Ep0State == Dwc3Ep0StateSetup) {
      return Dev->Ep0DmaUnsafe ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    }

    return Dwc3Ep0OutStart (Dev);
  }

  Dwc3EndTransfer (Dev, PhysEpNum);
  if (Dev->Endpoints[PhysEpNum].TransferStarted) {
    return EFI_DEVICE_ERROR;
  }

  Dev->Endpoints[PhysEpNum].TransferBuffer = NULL;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3GetEndpointStallState (
  IN     EFI_USBFN_IO_PROTOCOL        *This,
  IN     UINT8                         EndpointIndex,
  IN     EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  IN OUT BOOLEAN                      *State)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3GetEndpointStallState\n");
    }
    Seen++;
  }
  DWC3_DEV  *Dev;
  EFI_STATUS Status;
  UINT8      PhysEpNum;

  if (This == NULL || State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  Status = Dwc3ValidateEndpoint (Dev, EndpointIndex, Direction, &PhysEpNum);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *State = Dev->Endpoints[PhysEpNum].Stalled;

  return EFI_SUCCESS;
}

// Qualcomm UsbDeviceDxe uses scalar BOOLEAN at slot +0x40 (reverse
// UsbDeviceDxe.c:2426/2469, BLR at 0x2ccc/0x2ddc), not MU's BOOLEAN *.
typedef EFI_STATUS (EFIAPI *DWC3_QCOM_SET_ENDPOINT_STALL_STATE)(
  EFI_USBFN_IO_PROTOCOL *, UINT8, EFI_USBFN_ENDPOINT_DIRECTION, BOOLEAN);

STATIC
EFI_STATUS
EFIAPI
Dwc3SetEndpointStallState (
  IN EFI_USBFN_IO_PROTOCOL        *This,
  IN UINT8                        EndpointIndex,
  IN EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  IN BOOLEAN                      State)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3SetEndpointStallState\n");
    }
    Seen++;
  }
  DWC3_DEV  *Dev;
  EFI_STATUS Status;
  UINT8      PhysEpNum;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  Status = Dwc3ValidateEndpoint (Dev, EndpointIndex, Direction, &PhysEpNum);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!Dev->Endpoints[PhysEpNum].Enabled) {
    return EFI_INVALID_PARAMETER;
  }

  if (PhysEpNum <= DWC3_EP0_IN) {
    if (State) {
      return Dwc3Ep0StallAndRestart (Dev);
    }

    return EFI_SUCCESS;
  }

  return Dwc3SetHalt (Dev, PhysEpNum, State);
}

STATIC
VOID
Dwc3InstallQualcommStallSlot (IN EFI_USBFN_IO_PROTOCOL *Protocol)
{
  DWC3_QCOM_SET_ENDPOINT_STALL_STATE Entry = Dwc3SetEndpointStallState;

  // Local ABI boundary; leave the standard UsbFunctionIo.h definition intact.
  Protocol->SetEndpointStallState = (EFI_USBFN_IO_SET_ENDPOINT_STALL_STATE)Entry;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3EventHandler (
  IN     EFI_USBFN_IO_PROTOCOL     *This,
  OUT    EFI_USBFN_MESSAGE         *Message,
  IN OUT UINTN                     *PayloadSize,
  OUT    EFI_USBFN_MESSAGE_PAYLOAD *Payload)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3EventHandler\n");
    }
    Seen++;
  }
  DWC3_DEV     *Dev;
  DWC3_MESSAGE  Pending;
  EFI_STATUS    Status;

  if (This == NULL || Message == NULL || PayloadSize == NULL || Payload == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (*PayloadSize < sizeof (EFI_USBFN_MESSAGE_PAYLOAD)) {
    *PayloadSize = sizeof (EFI_USBFN_MESSAGE_PAYLOAD);
    return EFI_BUFFER_TOO_SMALL;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  if (Dev->EnumerationPending) {
    Status = Dwc3ActivateEnumeration (Dev);
    if (EFI_ERROR (Status) && Status != EFI_NOT_READY) {
      return Status;
    }
  }


  if (!Dev->Started) {
    return EFI_NOT_READY;
  }

  if (Dev->MessageHead == Dev->MessageTail) {
    Status = Dwc3ProcessEvents (Dev);
    if (EFI_ERROR (Status) && Status != EFI_NOT_READY) {
      return Status;
    }
  }

  if (!Dwc3PopMessage (Dev, &Pending)) {
    *Message     = EfiUsbMsgNone;
    *PayloadSize = 0;
    return EFI_SUCCESS;
  }

  *Message     = Pending.Message;
  *PayloadSize = sizeof (EFI_USBFN_MESSAGE_PAYLOAD);

  CopyMem (Payload, &Pending.Payload, sizeof (EFI_USBFN_MESSAGE_PAYLOAD));

  return EFI_SUCCESS;
}

STATIC
UINT64
Dwc3TransferBufferPhys (
  IN DWC3_DEV *Dev,
  IN VOID     *Buffer
  )
{
  UINT8 Index;

  if (Buffer == Dev->Ep0Buffer) {
    return (UINT64)Dev->Ep0BufferPhys;
  }

  for (Index = 0; Index < DWC3_MAX_TRANSFER_BUFFERS; Index++) {
    if (Dev->TransferBuffers[Index].Buffer == Buffer &&
        Dev->TransferBuffers[Index].Phys != 0) {
      return (UINT64)Dev->TransferBuffers[Index].Phys;
    }
  }

  // Buffers allocated by this driver's own DMA allocator are identity mapped.
  return (UINT64)(UINTN)Buffer;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3Transfer (
  IN     EFI_USBFN_IO_PROTOCOL        *This,
  IN     UINT8                         EndpointIndex,
  IN     EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  IN OUT UINTN                        *BufferSize,
  IN OUT VOID                         *Buffer)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3Transfer\n");
    }
    Seen++;
  }
  DWC3_DEV      *Dev;
  DWC3_ENDPOINT *Endpoint;
  EFI_STATUS     Status;
  UINT8          PhysEpNum;
  UINT32         TrbLength;
  UINT32         TrbType;
  VOID          *DmaBuffer;

  if (This == NULL || BufferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (*BufferSize != 0 && Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  Status = Dwc3ValidateEndpoint (Dev, EndpointIndex, Direction, &PhysEpNum);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Endpoint = &Dev->Endpoints[PhysEpNum];

  if (!Endpoint->Enabled) {
    DEBUG ((EFI_D_ERROR, "DWC3: Physical EP %u is not configured\n", PhysEpNum));
    return EFI_NOT_READY;
  }

  if (*BufferSize > DWC3_MAX_TRANSFER_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  if (Endpoint->TransferStarted && PhysEpNum > DWC3_EP0_IN) {
    DEBUG ((EFI_D_ERROR, "DWC3: Physical EP %u already has a transfer in progress\n", PhysEpNum));
    return EFI_NOT_READY;
  }

  TrbLength = (UINT32)*BufferSize;
  DmaBuffer = Buffer;

  if (PhysEpNum <= DWC3_EP0_IN) {
    if (Dev->Ep0DmaUnsafe) {
      return EFI_DEVICE_ERROR;
    }

    if (Dev->Ep0State == Dwc3Ep0StateStatus) {
      if (TrbLength != 0) {
        return EFI_INVALID_PARAMETER;
      }

      return Dwc3Ep0RequestStatus (Dev, PhysEpNum, Buffer);
    }

    if (Dev->Ep0State != Dwc3Ep0StateData) {
      return EFI_NOT_READY;
    }

    if ((PhysEpNum == DWC3_EP0_IN) != Dev->Ep0ExpectIn) {
      return EFI_INVALID_PARAMETER;
    }

    // Both directions share one TRB. Never cancel/overwrite a live data stage.
    if (Dev->Endpoints[DWC3_EP0_OUT].TransferStarted ||
        Dev->Endpoints[DWC3_EP0_IN].TransferStarted) {
      return EFI_NOT_READY;
    }

    Dev->Ep0Bounced = FALSE;

    if (TrbLength > Dev->SetupPacket.Length) {
      TrbLength   = Dev->SetupPacket.Length;
      *BufferSize = TrbLength;
    }

    TrbType = DWC3_TRBCTL_CONTROL_DATA;

    if (Direction == EfiUsbEndpointDirectionDeviceRx) {
      if (TrbLength > DWC3_EP0_BOUNCE_SIZE) {
        return EFI_INVALID_PARAMETER;
      }

      Dev->Ep0Bounced      = TRUE;
      Dev->Ep0BounceTarget = Buffer;
      Dev->Ep0BounceLength = TrbLength;

      DmaBuffer = Dev->Ep0Buffer;
      TrbLength = (UINT32)ALIGN_VALUE (TrbLength, Endpoint->MaxPacketSize);

      if (TrbLength > DWC3_EP0_BOUNCE_SIZE) {
        TrbLength = DWC3_EP0_BOUNCE_SIZE;
      }
    }
  } else {
    TrbType = DWC3_TRBCTL_NORMAL;

    if (Direction == EfiUsbEndpointDirectionDeviceRx && Endpoint->MaxPacketSize != 0) {
      TrbLength = (UINT32)ALIGN_VALUE (TrbLength, Endpoint->MaxPacketSize);
    }
  }

  if (TrbLength != 0 && DmaBuffer == Buffer) {
    if (Direction == EfiUsbEndpointDirectionDeviceTx) {
      WriteBackDataCacheRange (Buffer, TrbLength);
    } else {
      WriteBackInvalidateDataCacheRange (Buffer, TrbLength);
    }
  }

  Endpoint->TransferBuffer    = Buffer;
  Endpoint->TransferTrbLength = TrbLength;

  Status = Dwc3StartTransfer (Dev, PhysEpNum,
                              Dwc3TransferBufferPhys (Dev, DmaBuffer),
                              TrbLength, TrbType);
  if (EFI_ERROR (Status)) {
    Endpoint->TransferBuffer = NULL;
    if (PhysEpNum <= DWC3_EP0_IN) {
      Dwc3Ep0StallAndRestart (Dev);
    }
    return Status;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3GetMaxTransferSize (
  IN  EFI_USBFN_IO_PROTOCOL *This,
  OUT UINTN                 *MaxTransferSize)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3GetMaxTransferSize\n");
    }
    Seen++;
  }
  if (This == NULL || MaxTransferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *MaxTransferSize = DWC3_MAX_TRANSFER_SIZE;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3AllocateTransferBuffer (
  IN  EFI_USBFN_IO_PROTOCOL *This,
  IN  UINTN                  Size,
  OUT VOID                 **Buffer)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3AllocateTransferBuffer\n");
    }
    Seen++;
  }
  DWC3_DEV             *Dev;
  EFI_PHYSICAL_ADDRESS  Phys;
  UINTN                 Pages;
  UINT8                 Index;

  if (This == NULL || Buffer == NULL || Size == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  for (Index = 0; Index < DWC3_MAX_TRANSFER_BUFFERS; Index++) {
    if (Dev->TransferBuffers[Index].Buffer == NULL) {
      break;
    }
  }

  if (Index == DWC3_MAX_TRANSFER_BUFFERS) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Control data lives here for as long as the host is enumerating, and the controller
  // reads it over AXI. It has to obey the same rules as the TRB rings: inside DDR and
  // uncached. AllocateAlignedPages gave neither, which put a GET_DESCRIPTOR payload
  // above the controller's reach while every other buffer had already been fixed.
  //
  Pages = EFI_SIZE_TO_PAGES (Size);
  Phys  = 0;
  *Buffer = Dwc3AllocateDmaBuffer (EFI_PAGES_TO_SIZE (Pages), &Phys);
  if (*Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Dev->TransferBuffers[Index].Buffer = *Buffer;
  Dev->TransferBuffers[Index].Phys   = Phys;
  Dev->TransferBuffers[Index].Pages  = Pages;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3FreeTransferBuffer (
  IN EFI_USBFN_IO_PROTOCOL *This,
  IN VOID                  *Buffer)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3FreeTransferBuffer\n");
    }
    Seen++;
  }
  DWC3_DEV *Dev;
  UINT8     Index;

  if (This == NULL || Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  for (Index = 0; Index < DWC3_MAX_TRANSFER_BUFFERS; Index++) {
    if (Dev->TransferBuffers[Index].Buffer == Buffer) {
      Dwc3FreeDmaBuffer (Buffer, EFI_PAGES_TO_SIZE (Dev->TransferBuffers[Index].Pages));

      Dev->TransferBuffers[Index].Buffer = NULL;
      Dev->TransferBuffers[Index].Phys   = 0;
      Dev->TransferBuffers[Index].Pages  = 0;

      return EFI_SUCCESS;
    }
  }

  return EFI_INVALID_PARAMETER;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3StartController (IN EFI_USBFN_IO_PROTOCOL *This)
{
  DWC3_DEV  *Dev;
  EFI_STATUS Status;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  if (Dev->Started) {
    return EFI_SUCCESS;
  }

  RefSay ("[REF] StartController: PHY init begin\n");
  Status = UsbfnPlatformPhyInit ();
  RefSay ("[REF] StartController: PHY init -> %r\n", Status);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "DWC3: Failed to initialise the USB PHY! Status = %r\n", Status));
    return Status;
  }

  RefSay ("[REF] StartController: CoreInit begin\n");
  Status = Dwc3CoreInit (Dev);
  if (EFI_ERROR (Status)) {
    UsbfnPlatformPhyExit ();
    return Status;
  }

  RefSay ("[REF] StartController: CoreInit done, returning\n");

  // CoreInit completed a controller reset, so old DMA ownership is gone.
  Dev->Ep0DmaUnsafe     = FALSE;
  Dev->Attached         = FALSE;
  Dev->Started          = TRUE;
  Dev->MessageHead      = 0;
  Dev->MessageTail      = 0;
  Dev->EventTraceBudget = DWC3_EVENT_TRACE_BUDGET;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3StopController (IN EFI_USBFN_IO_PROTOCOL *This)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3StopController\n");
    }
    Seen++;
  }
  DWC3_DEV *Dev;
  UINT8     Index;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  if (!Dev->Started) {
    return EFI_SUCCESS;
  }

  Dwc3RunStop (Dev, FALSE);

  for (Index = 0; Index < Dev->NumPhysicalEps; Index++) {
    Dwc3DisableEndpoint (Dev, Index);
  }

  Dwc3CoreExit (Dev);
  UsbfnPlatformPhyExit ();

  Dev->Started     = FALSE;
  Dev->Attached    = FALSE;
  Dev->Speed       = UsbBusSpeedUnknown;
  Dev->MessageHead = 0;
  Dev->MessageTail = 0;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3SetEndpointPolicy (
  IN EFI_USBFN_IO_PROTOCOL        *This,
  IN UINT8                         EndpointIndex,
  IN EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  IN EFI_USBFN_POLICY_TYPE         PolicyType,
  IN UINTN                         BufferSize,
  IN VOID                         *Buffer)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3SetEndpointPolicy\n");
    }
    Seen++;
  }
  DWC3_DEV  *Dev;
  EFI_STATUS Status;
  UINT8      PhysEpNum;

  if (This == NULL || Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  Status = Dwc3ValidateEndpoint (Dev, EndpointIndex, Direction, &PhysEpNum);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  switch (PolicyType) {
    case EfiUsbPolicyZeroLengthTermination:
      if (BufferSize < sizeof (BOOLEAN)) {
        return EFI_INVALID_PARAMETER;
      }

      Dev->Endpoints[PhysEpNum].ZeroLengthTermination = *(BOOLEAN *)Buffer;
      return EFI_SUCCESS;

    case EfiUsbPolicyMaxTransactionSize:
    case EfiUsbPolicyZeroLengthTerminationSupport:
      return EFI_UNSUPPORTED;

    default:
      return EFI_INVALID_PARAMETER;
  }
}

/**
  Write a formatted line to the debug serial port.


  @param[in] Format  Printf style format string.
  @param[in] ...     Arguments.
**/
VOID
EFIAPI
RefSay (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Buf[192];
  VA_LIST  Marker;
  UINTN    Len;

  VA_START (Marker, Format);
  Len = AsciiVSPrint (Buf, sizeof (Buf) - 1, Format, Marker);
  VA_END (Marker);

  //
  // No SerialPortInitialize() here. The touch driver's log helper never called it and
  // has printed thousands of lines reliably, while this one called it before every
  // single line - and every USB hang so far happened immediately after a printed line,
  // which is what a hang inside the next line's SerialPortInitialize() looks like.
  // SerialTextConsoleDxe's own comment says it plainly: on FrameBufferSerialPortLib,
  // SerialPortInitialize() resets the console. The port is initialised once at driver
  // entry; doing it per line is not needed and is the first suspect for these hangs.
  //
  SerialPortWrite ((UINT8 *)Buf, Len);

}

/**
  Qualcomm extension slot at +0x90. Configures endpoints and activates the port, the same
  work ConfigureEnableEndpoints does; the SuperSpeed argument is ignored.
**/
STATIC
EFI_STATUS
EFIAPI
Dwc3ExtConfigureEnableEndpointsEx (
  IN EFI_USBFN_IO_PROTOCOL  *This,
  IN VOID                   *DeviceInfo,
  IN VOID                   *SsDeviceInfo
  )
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3ExtConfigureEnableEndpointsEx\n");
    }
    Seen++;
  }
//[LOGOUT]   RefSay ("[REF] +0x90 ConfigureEnableEndpointsEx ENTER\n");
  return Dwc3ConfigureEnableEndpoints ((VOID *)This, DeviceInfo);
}

/**
  Qualcomm extension slot at +0x98, called at the end of UsbDeviceStart with the device
  descriptor. Its purpose is refreshing SuperSpeed descriptors, which do not exist here.
  Reporting success is the correct behaviour for a USB2-only device, and returning an
  error would fail the whole Start.
**/
STATIC
EFI_STATUS
EFIAPI
Dwc3ExtUpdateSsDescriptor (
  IN EFI_USBFN_IO_PROTOCOL  *This,
  IN VOID                   *DeviceDescriptor
  )
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3ExtUpdateSsDescriptor\n");
    }
    Seen++;
  }
  (VOID)This;
  (VOID)DeviceDescriptor;
//[LOGOUT]   RefSay ("[REF] +0x98 UsbUpdateSSDescriptor ENTER -> success\n");
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Dwc3GetEndpointPolicy (
  IN     EFI_USBFN_IO_PROTOCOL        *This,
  IN     UINT8                         EndpointIndex,
  IN     EFI_USBFN_ENDPOINT_DIRECTION  Direction,
  IN     EFI_USBFN_POLICY_TYPE         PolicyType,
  IN OUT UINTN                        *BufferSize,
  IN OUT VOID                         *Buffer)
{
  {
    STATIC UINT32 Seen;
    if (Seen < 3) {
      RefSay ("[REF] usbfn: Dwc3GetEndpointPolicy\n");
    }
    Seen++;
  }
  DWC3_DEV  *Dev;
  EFI_STATUS Status;
  UINT8      PhysEpNum;

  if (This == NULL || BufferSize == NULL || Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Dev = DWC3_DEV_FROM_PROTOCOL (This);

  Status = Dwc3ValidateEndpoint (Dev, EndpointIndex, Direction, &PhysEpNum);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  switch (PolicyType) {
    case EfiUsbPolicyMaxTransactionSize:
      if (*BufferSize < sizeof (UINTN)) {
        *BufferSize = sizeof (UINTN);
        return EFI_BUFFER_TOO_SMALL;
      }

      *(UINTN *)Buffer = DWC3_MAX_TRANSFER_SIZE;
      *BufferSize      = sizeof (UINTN);
      return EFI_SUCCESS;

    case EfiUsbPolicyZeroLengthTerminationSupport:
      if (*BufferSize < sizeof (BOOLEAN)) {
        *BufferSize = sizeof (BOOLEAN);
        return EFI_BUFFER_TOO_SMALL;
      }

      *(BOOLEAN *)Buffer = TRUE;
      *BufferSize        = sizeof (BOOLEAN);
      return EFI_SUCCESS;

    case EfiUsbPolicyZeroLengthTermination:
      if (*BufferSize < sizeof (BOOLEAN)) {
        *BufferSize = sizeof (BOOLEAN);
        return EFI_BUFFER_TOO_SMALL;
      }

      *(BOOLEAN *)Buffer = Dev->Endpoints[PhysEpNum].ZeroLengthTermination;
      *BufferSize        = sizeof (BOOLEAN);
      return EFI_SUCCESS;

    default:
      return EFI_INVALID_PARAMETER;
  }
}

STATIC
VOID
Dwc3FreeResources (IN DWC3_DEV *Dev)
{
  UINT8 Index;

  if (Dev == NULL) {
    return;
  }

  for (Index = 0; Index < DWC3_MAX_PHYSICAL_EP; Index++) {
    if (Index == DWC3_EP0_IN) {
      continue;
    }

    if (Dev->Endpoints[Index].TrbPool != NULL) {
      Dwc3FreeDmaBuffer (Dev->Endpoints[Index].TrbPool, sizeof (DWC3_TRB) * DWC3_TRB_PER_EP);
    }
  }

  if (Dev->Ep0Buffer != NULL) {
    Dwc3FreeDmaBuffer (Dev->Ep0Buffer, DWC3_EP0_BOUNCE_SIZE);
  }

  if (Dev->EventBuffer != NULL) {
    Dwc3FreeDmaBuffer (Dev->EventBuffer, DWC3_EVENT_BUFFER_SIZE);
  }

  FreePool (Dev);
}

EFI_STATUS
EFIAPI
InitUsbfnDwc3Driver (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  DWC3_DEV  *Dev;
  UINT8      Index;

  Dev = AllocateZeroPool (sizeof (DWC3_DEV));
  if (Dev == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = UsbfnPlatformGetData (&Dev->Platform);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "DWC3: Unsupported platform! Status = %r\n", Status));
    FreePool (Dev);
    return Status;
  }

  //
  // DxeCore needs this to change the translation tables; SetMemorySpaceAttributes alone
  // only edits the GCD. It may not exist this early, which is why the buffers are forced
  // uncached again in Dwc3StartDevice.
  //
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mDwc3Cpu);
  RefSay ("[REF] CPU arch protocol -> %r\n", Status);
  if (EFI_ERROR (Status)) {
    mDwc3Cpu = NULL;
  }

  Dev->Signature      = DWC3_DEV_SIGNATURE;
  Dev->Base           = Dev->Platform.ControllerBase;
  Dev->NumPhysicalEps = DWC3_MAX_PHYSICAL_EP;
  Dev->Speed          = UsbBusSpeedUnknown;

  Dev->EventBuffer = Dwc3AllocateDmaBuffer (DWC3_EVENT_BUFFER_SIZE, &Dev->EventBufferPhys);
  if (Dev->EventBuffer == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Failure;
  }

  Dev->Ep0Buffer = Dwc3AllocateDmaBuffer (DWC3_EP0_BOUNCE_SIZE, &Dev->Ep0BufferPhys);
  if (Dev->Ep0Buffer == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Failure;
  }

  for (Index = 0; Index < DWC3_MAX_PHYSICAL_EP; Index++) {
    Dev->Endpoints[Index].PhysEpNum = Index;

    if (Index == DWC3_EP0_IN) {
      Dev->Endpoints[Index].TrbPool     = Dev->Endpoints[DWC3_EP0_OUT].TrbPool;
      Dev->Endpoints[Index].TrbPoolPhys = Dev->Endpoints[DWC3_EP0_OUT].TrbPoolPhys;
      continue;
    }

    Dev->Endpoints[Index].TrbPool = Dwc3AllocateDmaBuffer (sizeof (DWC3_TRB) * DWC3_TRB_PER_EP,
                                                           &Dev->Endpoints[Index].TrbPoolPhys);
    if (Dev->Endpoints[Index].TrbPool == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Failure;
    }
  }

  Dev->UsbfnIo.Revision                 = EFI_USBFN_IO_PROTOCOL_REVISION;
  Dev->UsbfnIo.DetectPort               = Dwc3DetectPort;
  Dev->UsbfnIo.ConfigureEnableEndpoints = Dwc3ConfigureEnableEndpoints;
  Dev->UsbfnIo.GetEndpointMaxPacketSize = Dwc3GetEndpointMaxPacketSize;
  Dev->UsbfnIo.GetDeviceInfo            = Dwc3GetDeviceInfo;
  Dev->UsbfnIo.GetVendorIdProductId     = Dwc3GetVendorIdProductId;
  Dev->UsbfnIo.AbortTransfer            = Dwc3AbortTransfer;
  Dev->UsbfnIo.GetEndpointStallState    = Dwc3GetEndpointStallState;
  Dwc3InstallQualcommStallSlot (&Dev->UsbfnIo);
  Dev->UsbfnIo.EventHandler             = Dwc3EventHandler;
  Dev->UsbfnIo.Transfer                 = Dwc3Transfer;
  Dev->UsbfnIo.GetMaxTransferSize       = Dwc3GetMaxTransferSize;
  Dev->UsbfnIo.AllocateTransferBuffer   = Dwc3AllocateTransferBuffer;
  Dev->UsbfnIo.FreeTransferBuffer       = Dwc3FreeTransferBuffer;
  Dev->UsbfnIo.StartController          = Dwc3StartController;
  Dev->UsbfnIo.StopController           = Dwc3StopController;
  Dev->UsbfnIo.SetEndpointPolicy        = Dwc3SetEndpointPolicy;
  Dev->UsbfnIo.GetEndpointPolicy        = Dwc3GetEndpointPolicy;

  //
  // Fill the two Qualcomm extension slots. Both report success: this port is USB2 only, so
  // there are no SuperSpeed descriptors to update and the Ex variant would configure the
  // same endpoints the non-Ex call already configured.
  //
  Dev->ExtConfigureEnableEndpointsEx = (VOID *)Dwc3ExtConfigureEnableEndpointsEx;
  Dev->ExtUsbUpdateSSDescriptor      = (VOID *)Dwc3ExtUpdateSsDescriptor;

  Dev->Bringup.Revision = 2;
  Dev->Bringup.TestOnce = Dwc3ManualTest;
  Dev->Bringup.Context = Dev;
  Dev->Bringup.TestFullSpeedOnce = Dwc3ManualTestFullSpeed;

  {
    EFI_GUID BringupGuid = USB_BRINGUP_PROTOCOL_GUID;
    Status = gBS->InstallMultipleProtocolInterfaces (&Dev->Handle,
                                                     &gEfiUsbFunctionIoProtocolGuid, &Dev->UsbfnIo,
                                                     &BringupGuid, &Dev->Bringup, NULL);
  }
  Dwc3BuildSerialNumber();

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to install USB Function IO Protocol! Status = %r\n", Status));
    goto Failure;
  }

  return EFI_SUCCESS;

Failure:
  Dwc3FreeResources (Dev);

  return Status;
}
