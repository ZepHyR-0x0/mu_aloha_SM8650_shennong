/**
  Read-only checks of multiple platform dependencies in one Shell invocation.
  Protocol presence, valid tables, successful SMC transport and usable secure
  applications are reported separately. No PIL load, TA load or RPMB writes.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/UsbFunctionIo.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/UsbIo.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/WiFi.h>
#include <Protocol/WiFi2.h>
#include <Protocol/SimpleNetwork.h>
#include <Protocol/DiskIo.h>
#include <Guid/FileInfo.h>
#include <Library/DevicePathLib.h>
#include "../../Driver/ReadOnlyStorageDxe/ReadOnlyStorage.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PlatformTestLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

STATIC PLATFORM_TEST_OUTPUT mOutput;
STATIC VOID *mOutputContext;
STATIC PLATFORM_TEST_RESULT *mResult;
STATIC BOOLEAN mBusy;
STATIC VOID Report (CONST CHAR16 *Format, ...)
{
  CHAR16 Wide[384];
  CHAR8 Line[384];
  VA_LIST Args;
  VA_START (Args, Format);
  UnicodeVSPrint (Wide, sizeof (Wide), Format, Args);
  VA_END (Args);
  if (EFI_ERROR (UnicodeStrToAsciiStrS (Wide, Line, sizeof (Line)))) return;
  SerialPortWrite ((UINT8 *)Line, AsciiStrLen (Line));
  if (mOutput != NULL) mOutput (Line, mOutputContext);
}
STATIC EFI_GUID mUsbFnGuid = EFI_USBFN_IO_PROTOCOL_GUID;

typedef struct _PLATFORM_SCM PLATFORM_SCM;
struct _PLATFORM_SCM {
  UINT64 Revision;
  VOID *Unused[4];
  EFI_STATUS (EFIAPI *SendCommand)(PLATFORM_SCM *, UINT32, UINT32 *, VOID *, UINTN, VOID *, UINTN);
  VOID *ExitBootServices;
  EFI_STATUS (EFIAPI *SipSysCall)(PLATFORM_SCM *, UINT32, UINT32, UINT64 *, UINT64 *);
};
STATIC_ASSERT (OFFSET_OF (PLATFORM_SCM, SendCommand) == 0x28, "SCM query ABI");
STATIC_ASSERT (OFFSET_OF (PLATFORM_SCM, SipSysCall) == 0x38, "SCM SIP ABI");
typedef struct { UINT64 Revision; VOID *Allocate; VOID *Free; } PLATFORM_SHM;
typedef struct { UINT32 Device; UINT32 Channel; } ADC_CHANNEL;
typedef struct { UINT32 Status; INT32 Physical; UINT32 Percent; UINT32 Microvolts; UINT32 Code; } ADC_RESULT;
typedef struct {
  UINT64 Revision;
  EFI_STATUS (EFIAPI *GetInputProperties)(CONST CHAR8 *, UINT32, ADC_CHANNEL *);
  EFI_STATUS (EFIAPI *Read)(ADC_CHANNEL *, ADC_RESULT *);
} PLATFORM_ADC;
STATIC EFI_GUID mScmGuid = {0x77ED108D,0x8524,0x4B8B,{0x9D,0x2E,0x34,0x98,0x7A,0xEC,0xB9,0xC1}};
STATIC EFI_GUID mShmGuid = {0x9C1EB71F,0xDD6C,0x4ED5,{0x9F,0x6A,0x5C,0xC0,0xCA,0x78,0x9F,0x16}};
STATIC EFI_GUID mPilGuid = {0x1F4F1DA3,0x4D8D,0x4894,{0xBA,0x2A,0x13,0xD3,0x20,0x7F,0x97,0x39}};
STATIC EFI_GUID mAdcGuid = {0x61740CC4,0x916F,0x4471,{0xB0,0xDC,0x00,0xC6,0xF4,0x3A,0x7F,0xC6}};

STATIC UINTN CountProtocol (CONST CHAR16 *Name, EFI_GUID *Guid)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, Guid, NULL, &Count, &Handles);
  Report (L"[PLATFORM] %s handles=%u -> %r (presence only)\n", Name, Count, Status);
  if (Handles != NULL) gBS->FreePool (Handles);
  return Count;
}

STATIC BOOLEAN ImageOwns (VOID *Interface, VOID *Method, EFI_HANDLE *Images, UINTN Count, CONST CHAR16 *Name)
{
  UINTN Index;
  for (Index = 0; Index < Count; Index++) {
    EFI_LOADED_IMAGE_PROTOCOL *Image;
    UINTN Base;
    UINTN Address = (UINTN)Interface;
    UINTN Code = (UINTN)Method;
    if (EFI_ERROR (gBS->HandleProtocol (Images[Index], &gEfiLoadedImageProtocolGuid, (VOID **)&Image))) continue;
    Base = (UINTN)Image->ImageBase;
    if (Address >= Base && Address - Base < Image->ImageSize && Code >= Base && Code - Base < Image->ImageSize) {
      Report (L"[PLATFORM] %s owner handle=%p base=%p interface RVA=0x%lx method RVA=0x%lx\n",
        Name, Images[Index], Image->ImageBase, (UINT64)(Address-Base), (UINT64)(Code-Base));
      return TRUE;
    }
  }
  Report (L"[PLATFORM] %s provider not verified against a loaded image\n", Name);
  return FALSE;
}

STATIC EFI_STATUS CheckAcpi (VOID)
{
  EFI_ACPI_SDT_PROTOCOL *Sdt = NULL;
  EFI_STATUS Status;
  UINTN Index;
  BOOLEAN Bad = FALSE;
  BOOLEAN Dsdt = FALSE;
  BOOLEAN Fadt = FALSE;
  EFI_ACPI_SDT_HEADER *DsdtTable = NULL, *FacsTable = NULL, *FadtTable = NULL;
  UINTN EnabledCpus = 0;
  Status = gBS->LocateProtocol (&gEfiAcpiSdtProtocolGuid, NULL, (VOID **)&Sdt);
  Report (L"[PLATFORM] ACPI SDT -> %r\n", Status);
  if (EFI_ERROR (Status)) return Status;
  if (Sdt == NULL || Sdt->GetAcpiTable == NULL) return EFI_UNSUPPORTED;
  for (Index = 0; Index < 256; Index++) {
    EFI_ACPI_SDT_HEADER *Table = NULL;
    EFI_ACPI_TABLE_VERSION Version;
    UINTN Key;
    CHAR8 Signature[5];
    UINT8 Checksum;
    Status = Sdt->GetAcpiTable (Index, &Table, &Version, &Key);
    if (Status == EFI_NOT_FOUND) break;
    if (EFI_ERROR (Status)) return Status;
    if (Table == NULL || Table->Length < sizeof (*Table) || Table->Length > 2*1024*1024) return EFI_COMPROMISED_DATA;
    CopyMem (Signature, &Table->Signature, 4);
    Signature[4] = 0;
    Checksum = AsciiStrCmp (Signature, "FACS") == 0 ? 0 : CalculateSum8 ((UINT8 *)Table, Table->Length);
    Report (L"[PLATFORM] ACPI %a length=%u checksum=%02x key=%u\n", Signature, Table->Length, Checksum, Key);
    if (Checksum != 0) Bad = TRUE;
    if (AsciiStrCmp (Signature, "DSDT") == 0) {
      if (DsdtTable != NULL) Bad = TRUE;
      Dsdt = TRUE; DsdtTable = Table;
    }
    if (AsciiStrCmp (Signature, "FACS") == 0) {
      if (FacsTable != NULL || Table->Length < 64) Bad = TRUE;
      FacsTable = Table;
    }
    if (AsciiStrCmp (Signature, "FACP") == 0) {
      if (FadtTable != NULL) Bad = TRUE;
      Fadt = TRUE; FadtTable = Table;
    }
    if (AsciiStrCmp (Signature, "APIC") == 0) {
      UINTN Offset = 44;
      if (Table->Length < Offset) Bad = TRUE;
      while (Offset < Table->Length) {
        UINT8 *Entry = (UINT8 *)Table + Offset;
        if (Table->Length-Offset < 2 || Entry[1] < 2 || Entry[1] > Table->Length-Offset) {Bad=TRUE;break;}
        // ACPI GICC: Flags at +12; enabled bit 0. MPIDR resides at +68.
        if (Entry[0] == 0x0B) {
          if (Entry[1] < 80) {Bad=TRUE;break;}
          if ((ReadUnaligned32 ((UINT32 *)(Entry+12)) & BIT0) != 0) EnabledCpus++;
          Report (L"[PLATFORM] ACPI GICC uid=%u flags=%08x MPIDR=%lx\n",
            ReadUnaligned32 ((UINT32 *)(Entry+8)),ReadUnaligned32 ((UINT32 *)(Entry+12)),
            ReadUnaligned64 ((UINT64 *)(Entry+68)));
        }
        Offset += Entry[1];
      }
    }
  }
  Report (L"[PLATFORM] ACPI tables=%u DSDT=%u FADT=%u\n", Index, Dsdt, Fadt);
  mResult->AcpiTables = Index;
  mResult->AcpiEnabledCpus = EnabledCpus;
  mResult->AcpiLinksStatus = EFI_NOT_FOUND;
  if (FadtTable != NULL && FadtTable->Length >= 44 && DsdtTable != NULL) {
    UINT8 *Bytes = (UINT8 *)FadtTable;
    UINT64 DsdtAddress = ReadUnaligned32 ((UINT32 *)(Bytes+40));
    UINT64 FacsAddress = ReadUnaligned32 ((UINT32 *)(Bytes+36));
    if (FadtTable->Length >= 148 && ReadUnaligned64 ((UINT64 *)(Bytes+140)) != 0)
      DsdtAddress = ReadUnaligned64 ((UINT64 *)(Bytes+140));
    if (FadtTable->Length >= 140 && ReadUnaligned64 ((UINT64 *)(Bytes+132)) != 0)
      FacsAddress = ReadUnaligned64 ((UINT64 *)(Bytes+132));
    mResult->AcpiLinksStatus = DsdtAddress == (UINTN)DsdtTable &&
      (FacsAddress == 0 || (FacsTable != NULL && FacsAddress == (UINTN)FacsTable)) ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
    Report (L"[PLATFORM] ACPI FADT links=%r DSDT=%lx enumerated=%p FACS=%lx enumerated=%p enabled-cpus=%u\n",
      mResult->AcpiLinksStatus,DsdtAddress,DsdtTable,FacsAddress,FacsTable,EnabledCpus);
    if (EFI_ERROR (mResult->AcpiLinksStatus)) Bad = TRUE;
  }
  if (Index == 256 || Bad) return EFI_COMPROMISED_DATA;
  if (Index == 0 || !Dsdt || !Fadt) return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}

// Export installed bytes to the caller/log; never install, patch or write a table.
EFI_STATUS PlatformAcpiRead(PLATFORM_TEST_OUTPUT Output,VOID *Context,UINT32 Index,UINT32 Offset,UINT32 Bytes)
{
  EFI_ACPI_SDT_PROTOCOL *Sdt=NULL;EFI_ACPI_SDT_HEADER *Table=NULL;
  EFI_ACPI_TABLE_VERSION Version;UINTN Key,I,J,Length;EFI_STATUS Status;
  CHAR8 Hex[49];BOOLEAN Facs;
  if(Index>=64 || Bytes==0 || Bytes>4096) return EFI_INVALID_PARAMETER;
  if(mBusy) return EFI_ALREADY_STARTED;
  mBusy=TRUE;mOutput=Output;mOutputContext=Context;
  Status=gBS->LocateProtocol(&gEfiAcpiSdtProtocolGuid,NULL,(VOID **)&Sdt);
  if(EFI_ERROR(Status)) goto Done;
  if(Sdt==NULL || Sdt->GetAcpiTable==NULL) {Status=EFI_UNSUPPORTED;goto Done;}
  Status=Sdt->GetAcpiTable(Index,&Table,&Version,&Key);
  if(EFI_ERROR(Status)) goto Done;
  if(Table==NULL) {Status=EFI_COMPROMISED_DATA;goto Done;}
  Facs=Table->Signature==SIGNATURE_32('F','A','C','S');Length=Table->Length;
  if(Length<(Facs?64:sizeof(*Table)) || Length>256*1024 || Offset>=Length ||
     (!Facs && CalculateSum8((UINT8 *)Table,Length)!=0)) {Status=EFI_COMPROMISED_DATA;goto Done;}
  Bytes=(UINT32)MIN((UINTN)Bytes,Length-Offset);
  Report(L"[ACPI READ] index=%u signature=%08x length=%u offset=%u bytes=%u checksum=%s\n",
    Index,Table->Signature,(UINT32)Length,Offset,Bytes,Facs?L"not-applicable":L"valid");
  for(I=0;I<Bytes;I+=16) {
    ZeroMem(Hex,sizeof(Hex));
    for(J=0;J<MIN((UINTN)16,Bytes-I);J++) AsciiSPrint(Hex+J*3,sizeof(Hex)-J*3,"%02x ",((UINT8 *)Table)[Offset+I+J]);
    Report(L"[ACPI RAW] index=%u off=%06x %a\n",Index,Offset+(UINT32)I,Hex);
  }
Done:
  mOutput=NULL;mOutputContext=NULL;mBusy=FALSE;return Status;
}

STATIC VOID CheckSecure (EFI_HANDLE *Images, UINTN ImageCount)
{
  PLATFORM_SCM *Scm = NULL;
  PLATFORM_SHM *Shm = NULL;
  UINT64 *Pil = NULL;
  EFI_STATUS Status;
  UINTN Index;
  UINTN Count;
  CONST CHAR8 *Apps[] = {"keymaster", "keymaster64", "featenabler_a", "featenabler_a64"};
  Count = CountProtocol (L"ShmBridge", &mShmGuid);
  mResult->ShmHandles = Count;
  Status = gBS->LocateProtocol (&mShmGuid, NULL, (VOID **)&Shm);
  if (!EFI_ERROR (Status) && Shm != NULL) {
    Report (L"[PLATFORM] ShmBridge revision=0x%lx duplicate=%u\n", Shm->Revision, Count != 1);
    if (Shm->Revision == 0x10002) ImageOwns (Shm, Shm->Allocate, Images, ImageCount, L"ShmBridge");
  }
  Status = gBS->LocateProtocol (&mScmGuid, NULL, (VOID **)&Scm);
  Report (L"[PLATFORM] SCM -> %r\n", Status);
  mResult->ScmStatus = EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
  if (!EFI_ERROR (Status) && Scm != NULL) {
    Report (L"[PLATFORM] SCM revision=0x%lx\n", Scm->Revision);
    if (Scm->Revision != 0x50002 || Scm->SipSysCall == NULL || Scm->SendCommand == NULL ||
        !ImageOwns (Scm, (VOID *)Scm->SipSysCall, Images, ImageCount, L"SCM")) {
      Report (L"[PLATFORM] SCM ABI/provider unsupported; calls skipped\n");
    } else {
      UINT64 Parameters[10];
      UINT64 Results[6];
      ZeroMem (Parameters, sizeof (Parameters));
      ZeroMem (Results, sizeof (Results));
      Parameters[0] = 6; // Native ScmDxeCompat feature6 ABI; feature0 is not QSEE.
      Status = Scm->SipSysCall (Scm, 0x02000603, 1, Parameters, Results);
      mResult->ScmStatus = Status;
      Report (L"[PLATFORM] SMC transport query -> %r results=%lx/%lx/%lx feature=6 (QSEE version, service untested)\n", Status, Results[0], Results[1], Results[2]);
      {
        // SCM INFO/IS_CALL_AVAILABLE only. Never invoke the queried operations.
        CONST UINT32 Query[]={0x02000201,0x02000205,0x02000207,0x02000202,0x02000206};
        for(Index=0;Index<ARRAY_SIZE(Query);Index++) {
          ZeroMem(Parameters,sizeof(Parameters));ZeroMem(Results,sizeof(Results));Parameters[0]=Query[Index];
          Status=Scm->SipSysCall(Scm,0x02000601,1,Parameters,Results);
          Report(L"[SMC CAPS] query=%08x transport=%r raw=%lx/%lx/%lx\n",Query[Index],Status,Results[0],Results[1],Results[2]);
        }
      }
      for (Index = 0; Index < ARRAY_SIZE (Apps); Index++) {
        CHAR8 Request[32];
        UINT32 AppId = 0;
        UINTN Bytes = AsciiStrLen (Apps[Index]) + 1;
        ZeroMem (Request, sizeof (Request));
        CopyMem (Request, Apps[Index], Bytes);
        Status = Scm->SendCommand (Scm, 3, &AppId, Request, Bytes, NULL, 0);
        Report (L"[PLATFORM] TA query %a -> %r handle=%08x (query only)\n", Apps[Index], Status, AppId);
      }
    }
  }
  Status = gBS->LocateProtocol (&mPilGuid, NULL, (VOID **)&Pil);
  Report (L"[PLATFORM] PIL -> %r\n", Status);
  mResult->PilStatus = Status;
  if (!EFI_ERROR (Status) && (Pil == NULL || Pil[0] != 0x10005)) mResult->PilStatus = EFI_UNSUPPORTED;
  if (!EFI_ERROR (Status) && Pil != NULL && Pil[0] == 0x10005) {
    if (!ImageOwns (Pil, (VOID *)(UINTN)Pil[1], Images, ImageCount, L"PIL"))
      mResult->PilStatus = EFI_UNSUPPORTED;
    Report (L"[PLATFORM] PIL revision=0x%lx; firmware load not tested\n", Pil[0]);
  }
}

STATIC VOID CheckAdc (VOID)
{
  PLATFORM_ADC *Adc = NULL;
  EFI_STATUS Status;
  CONST CHAR8 *Names[] = {"VBATT", "VBATT_2S", "BATT_THERM_OHMS"};
  UINTN Index;
  Status = gBS->LocateProtocol (&mAdcGuid, NULL, (VOID **)&Adc);
  Report (L"[PLATFORM] PMIC ADC -> %r\n", Status);
  mResult->AdcStatus = Status;
  if (EFI_ERROR (Status)) return;
  if (Adc == NULL || Adc->Revision != 0x10004 ||
      Adc->GetInputProperties == NULL || Adc->Read == NULL) {
    mResult->AdcStatus = EFI_UNSUPPORTED;
    Report (L"[PLATFORM] ADC ABI unsupported; reads skipped\n");
    return;
  }
  for (Index = 0; Index < ARRAY_SIZE (Names); Index++) {
    ADC_CHANNEL Channel;
    ADC_RESULT Result;
    ZeroMem (&Channel, sizeof (Channel));
    ZeroMem (&Result, sizeof (Result));
    Status = Adc->GetInputProperties (Names[Index], (UINT32)AsciiStrLen (Names[Index]) + 1, &Channel);
    if (!EFI_ERROR (Status)) Status = Adc->Read (&Channel, &Result);
    Report (L"[PLATFORM] ADC %a -> %r result-status=%u physical=%d uV=%u\n",
      Names[Index], Status, Result.Status, Result.Physical, Result.Microvolts);
  }
  Report (L"[PLATFORM] ADC readings are not state-of-charge percentages\n");
}

EFI_STATUS PlatformHardwareTest (IN PLATFORM_TEST_OUTPUT Output OPTIONAL, IN VOID *Context OPTIONAL,
                                 OUT PLATFORM_TEST_RESULT *Result)
{
  EFI_HANDLE *Images = NULL;
  UINTN ImageCount = 0;
  EFI_STATUS Status;
  EFI_TIME Time;
  if (Result == NULL) return EFI_INVALID_PARAMETER;
  if (mBusy) return EFI_ALREADY_STARTED;
  mBusy = TRUE;
  mOutput = Output;
  mOutputContext = Context;
  mResult = Result;
  ZeroMem (Result, sizeof (*Result));
  Result->ScmStatus = Result->PilStatus = Result->AdcStatus = EFI_NOT_READY;
  Result->AcpiStatus = CheckAcpi ();
  Report (L"[PLATFORM] ACPI validation -> %r\n", Result->AcpiStatus);
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &ImageCount, &Images);
  if (!EFI_ERROR (Status)) CheckSecure (Images, ImageCount);
  if (Images != NULL) gBS->FreePool (Images);
  CheckAdc ();
  Result->BlockHandles = CountProtocol (L"UFS/block IO", &gEfiBlockIoProtocolGuid);
  Result->FileSystemHandles = CountProtocol (L"File systems", &gEfiSimpleFileSystemProtocolGuid);
  Result->DisplayHandles = CountProtocol (L"Display GOP", &gEfiGraphicsOutputProtocolGuid);
  Result->TouchHandles = CountProtocol (L"Touch", &gEfiAbsolutePointerProtocolGuid);
  Result->UsbHandles = CountProtocol (L"Stock USB function IO", &mUsbFnGuid);
  ZeroMem (&Time, sizeof (Time));
  Result->RtcStatus = gRT->GetTime (&Time, NULL);
  Report (L"[PLATFORM] RTC -> %r %04u-%02u-%02u %02u:%02u:%02u (firmware clock)\n",
    Result->RtcStatus, Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second);
  Report (L"[PLATFORM] Haptic action: run HapticTest.efi separately\n");
  mOutput = NULL;
  mOutputContext = NULL;
  mResult = NULL;
  mBusy = FALSE;
  return Result->AcpiStatus;
}

STATIC BOOLEAN UsbDevicePath (EFI_HANDLE Handle)
{
  EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
  if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)&Path)) ||
      Path == NULL || !IsDevicePathValid (Path, 1024)) return FALSE;
  while (!IsDevicePathEnd (Path)) {
    if (DevicePathType (Path) == MESSAGING_DEVICE_PATH &&
        (DevicePathSubType (Path) == MSG_USB_DP || DevicePathSubType (Path) == MSG_USB_CLASS_DP ||
         DevicePathSubType (Path) == MSG_USB_WWID_DP)) return TRUE;
    Path = NextDevicePathNode (Path);
  }
  return FALSE;
}

EFI_STATUS PlatformUsbHostTest (IN PLATFORM_TEST_OUTPUT Output OPTIONAL, IN VOID *Context OPTIONAL,
                               OUT USB_HOST_TEST_RESULT *Result)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, Index;
  EFI_STATUS Status;
  READ_ONLY_STORAGE_POLICY *Guard = NULL;
  EFI_GUID GuardGuid = READ_ONLY_STORAGE_GUID;
  if (Result == NULL) return EFI_INVALID_PARAMETER;
  if (mBusy) return EFI_ALREADY_STARTED;
  mBusy = TRUE; mOutput = Output; mOutputContext = Context;
  ZeroMem (Result, sizeof (*Result));
  Result->HostStatus = Result->StorageStatus = EFI_NOT_FOUND;
  Report (L"[USB HOST] Inspect existing host only; no role switch or VBUS command.\n");
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsb2HcProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    Result->HostHandles = Count;
    Result->HostStatus = EFI_NOT_READY;
    for (Index = 0; Index < Count; Index++) {
      EFI_USB2_HC_PROTOCOL *Host = NULL;
      EFI_USB_HC_STATE State = EfiUsbHcStateHalt;
      UINT8 Speed = 0, Ports = 0, Dma64 = 0;
      UINTN Port;
      Status = gBS->HandleProtocol (Handles[Index], &gEfiUsb2HcProtocolGuid, (VOID **)&Host);
      if (EFI_ERROR (Status) || Host == NULL || Host->GetCapability == NULL || Host->GetState == NULL) continue;
      Status = Host->GetCapability (Host, &Speed, &Ports, &Dma64);
      Report (L"[USB HOST] HC %u capability=%r speed=%u ports=%u dma64=%u\n", Index, Status, Speed, Ports, Dma64);
      if (EFI_ERROR (Status)) { Result->HostStatus = Status; continue; }
      Status = Host->GetState (Host, &State);
      Report (L"[USB HOST] HC %u state=%u -> %r\n", Index, State, Status);
      if (!EFI_ERROR (Status) && State == EfiUsbHcStateOperational) Result->HostStatus = EFI_SUCCESS;
      if (Host->GetRootHubPortStatus != NULL) for (Port = 0; Port < MIN (Ports, 16); Port++) {
        EFI_USB_PORT_STATUS PortStatus;
        ZeroMem (&PortStatus, sizeof (PortStatus));
        Status = Host->GetRootHubPortStatus (Host, (UINT8)Port, &PortStatus);
        Report (L"[USB HOST] port %u -> %r status=%04x change=%04x\n", Port, Status,
          PortStatus.PortStatus, PortStatus.PortChangeStatus);
      }
    }
  }
  if (Handles != NULL) gBS->FreePool (Handles);
  Handles = NULL; Count = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbIoProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) for (Index = 0; Index < Count; Index++) {
    EFI_USB_IO_PROTOCOL *Usb = NULL;
    EFI_USB_DEVICE_DESCRIPTOR Desc;
    EFI_USB_INTERFACE_DESCRIPTOR Interface;
    Result->DeviceHandles++;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiUsbIoProtocolGuid, (VOID **)&Usb);
    if (EFI_ERROR (Status) || Usb == NULL) continue;
    ZeroMem (&Desc, sizeof (Desc)); ZeroMem (&Interface, sizeof (Interface));
    if (Usb->UsbGetDeviceDescriptor != NULL) {
      Status = Usb->UsbGetDeviceDescriptor (Usb, &Desc);
      Report (L"[USB HOST] USB %u descriptor=%r VID=%04x PID=%04x\n", Index, Status, Desc.IdVendor, Desc.IdProduct);
    }
    if (Usb->UsbGetInterfaceDescriptor != NULL) {
      Status = Usb->UsbGetInterfaceDescriptor (Usb, &Interface);
      Report (L"[USB HOST] interface=%r class=%02x subclass=%02x protocol=%02x\n", Status,
        Interface.InterfaceClass, Interface.InterfaceSubClass, Interface.InterfaceProtocol);
    }
  }
  if (Handles != NULL) gBS->FreePool (Handles);
  Status = gBS->LocateProtocol (&GuardGuid, NULL, (VOID **)&Guard);
  if (EFI_ERROR (Status) || Guard == NULL || Guard->Revision < 1 || Guard->Revision > 3 || Guard->IsProtected == NULL) {
    Guard = NULL; Result->StorageStatus = EFI_WRITE_PROTECTED;
    Report (L"[USB HOST] Read-only storage guard unavailable; disk reads skipped.\n");
  }
  Handles = NULL; Count = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) for (Index = 0; Index < Count; Index++) {
    EFI_BLOCK_IO_PROTOCOL *Block = NULL;
    EFI_DISK_IO_PROTOCOL *Disk = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
    EFI_FILE_PROTOCOL *Root = NULL;
    UINT8 Bytes[512];
    if (!UsbDevicePath (Handles[Index])) continue;
    Result->BlockHandles++;
    if (Guard == NULL) continue;
    if (!Guard->IsProtected (Handles[Index])) { Result->StorageStatus = EFI_WRITE_PROTECTED; continue; }
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Block);
    if (EFI_ERROR (Status) || Block == NULL || Block->Media == NULL || !Block->Media->ReadOnly) {
      Result->StorageStatus = EFI_WRITE_PROTECTED; continue;
    }
    if (!Block->Media->MediaPresent || Block->Media->BlockSize == 0) { Result->StorageStatus = EFI_NO_MEDIA; continue; }
    Status = gBS->HandleProtocol (Handles[Index], &gEfiDiskIoProtocolGuid, (VOID **)&Disk);
    if (!EFI_ERROR (Status) && Disk != NULL && Disk->ReadDisk != NULL) {
      Status = Disk->ReadDisk (Disk, Block->Media->MediaId, 0, sizeof (Bytes), Bytes);
      Report (L"[USB HOST] block %u first 512 bytes=%r media=%u\n", Index, Status, Block->Media->MediaId);
      Result->StorageStatus = Status;
    }
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status) || Fs == NULL || Fs->OpenVolume == NULL) continue;
    Result->FileSystemHandles++;
    Status = Fs->OpenVolume (Fs, &Root);
    if (!EFI_ERROR (Status) && Root != NULL) {
      union { UINT64 Align; UINT8 Bytes[1024]; } Record;
      UINTN Size = sizeof (Record);
      Status = Root->Read (Root, &Size, &Record);
      if (!EFI_ERROR (Status) && Size > sizeof (Record)) Status = EFI_VOLUME_CORRUPTED;
      if (!EFI_ERROR (Status)) Result->ReadableVolumes++;
      Root->Close (Root);
    } else if (!EFI_ERROR (Status)) Status = EFI_COMPROMISED_DATA;
    Report (L"[USB HOST] filesystem %u root-directory read=%r\n", Index, Status);
    Result->StorageStatus = Status;
  }
  if (Handles != NULL) gBS->FreePool (Handles);
  Report (L"[USB HOST] HC=%u interfaces=%u block=%u FS=%u readable=%u\n", Result->HostHandles,
    Result->DeviceHandles, Result->BlockHandles, Result->FileSystemHandles, Result->ReadableVolumes);
  if (Result->HostHandles == 0) Report (L"[USB HOST] Missing role/VBUS/PHY/xHCI initialization. Stock CDC remains active.\n");
  mOutput = NULL; mOutputContext = NULL; mBusy = FALSE;
  return EFI_ERROR (Result->HostStatus) ? Result->HostStatus : Result->StorageStatus;
}

EFI_STATUS PlatformWlanTest (IN PLATFORM_TEST_OUTPUT Output OPTIONAL, IN VOID *Context OPTIONAL,
                            OUT WLAN_TEST_RESULT *Result)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, Index;
  EFI_STATUS Status;
  if (Result == NULL) return EFI_INVALID_PARAMETER;
  if (mBusy) return EFI_ALREADY_STARTED;
  mBusy = TRUE; mOutput = Output; mOutputContext = Context;
  ZeroMem (Result, sizeof (*Result));
  Result->RootBridges = CountProtocol (L"PCI root bridge", &gEfiPciRootBridgeIoProtocolGuid);
  Result->PciStatus = Result->RootBridges == 0 ? EFI_NOT_FOUND : EFI_NOT_READY;
  Result->WifiStatus = EFI_NOT_FOUND;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    Result->PciHandles = Count;
    for (Index = 0; Index < Count; Index++) {
      EFI_PCI_IO_PROTOCOL *Pci = NULL;
      UINT8 Config[16];
      UINT16 Vendor, Device;
      if (Result->RootBridges == 0) break;
      Status = gBS->HandleProtocol (Handles[Index], &gEfiPciIoProtocolGuid, (VOID **)&Pci);
      if (EFI_ERROR (Status) || Pci == NULL || Pci->Pci.Read == NULL) continue;
      ZeroMem (Config, sizeof (Config));
      Status = Pci->Pci.Read (Pci, EfiPciIoWidthUint8, 0, sizeof (Config), Config);
      if (EFI_ERROR (Status)) { Report (L"[WLAN] PCI %u config read=%r\n", Index, Status); continue; }
      Vendor = ReadUnaligned16 ((UINT16 *)Config); Device = ReadUnaligned16 ((UINT16 *)(Config+2));
      Report (L"[WLAN] PCI %u VID=%04x DID=%04x class=%02x/%02x/%02x\n", Index,
        Vendor, Device, Config[11], Config[10], Config[9]);
      if (Vendor == 0x17cb && Device == 0x1107 && Config[11] == 2) Result->WlanEndpoints++;
    }
  }
  if (Handles != NULL) gBS->FreePool (Handles);
  if (Result->WlanEndpoints != 0) Result->PciStatus = EFI_SUCCESS;
  Result->WifiHandles = CountProtocol (L"WiFi", &gEfiWiFiProtocolGuid);
  Result->WifiHandles += CountProtocol (L"WiFi2", &gEfiWiFi2ProtocolGuid);
  if (Result->WifiHandles != 0) Result->WifiStatus = EFI_NOT_READY;
  Handles = NULL; Count = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleNetworkProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    Result->NetworkHandles = Count;
    for (Index = 0; Index < Count; Index++) {
      EFI_SIMPLE_NETWORK_PROTOCOL *Net = NULL;
      VOID *Wifi = NULL;
      BOOLEAN Wireless;
      Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleNetworkProtocolGuid, (VOID **)&Net);
      if (EFI_ERROR (Status) || Net == NULL || Net->Mode == NULL) continue;
      Wireless = !EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiWiFi2ProtocolGuid, &Wifi)) ||
                 !EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiWiFiProtocolGuid, &Wifi));
      Report (L"[WLAN] SNP %u wireless=%u state=%u media-known=%u media=%u\n", Index, Wireless,
        Net->Mode->State, Net->Mode->MediaPresentSupported, Net->Mode->MediaPresent);
      // Link state is not evidence of scan, association or packet transfer.
    }
  }
  if (Handles != NULL) gBS->FreePool (Handles);
  Report (L"[WLAN] root=%u PCI=%u target=%u WiFi=%u SNP=%u\n", Result->RootBridges,
    Result->PciHandles, Result->WlanEndpoints, Result->WifiHandles, Result->NetworkHandles);
  if (Result->RootBridges == 0) Report (L"[WLAN] PCIe root bridge missing; MMIO probing skipped.\n");
  if (Result->WifiHandles == 0) Report (L"[WLAN] CNSS/MHI/QMI firmware and UEFI WiFi provider required. Scan/association untested.\n");
  mOutput = NULL; mOutputContext = NULL; mBusy = FALSE;
  return EFI_ERROR (Result->PciStatus) ? Result->PciStatus : Result->WifiStatus;
}
