/** Shell entry for the shared platform tests. SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/PlatformTestLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/ShellParameters.h>
STATIC VOID EFIAPI Output (CONST CHAR8 *Line, VOID *Context)
{
  (VOID)Context;
  Print (L"%a", Line);
}
STATIC EFI_STATUS Number(CONST CHAR16 *Text, UINT32 *Value)
{
  UINTN N;CHAR16 *End=NULL;
  if(Text==NULL || *Text==0 || RETURN_ERROR(StrDecimalToUintnS(Text,&End,&N)) ||
     End==NULL || *End!=0 || N>MAX_UINT32) return EFI_INVALID_PARAMETER;
  *Value=(UINT32)N;return EFI_SUCCESS;
}
STATIC VOID Help(VOID)
{
  Print(L"PlatformTest.efi platform | devices | wireless-scan | bluetooth | sensors | wlan-link\n"
        L"PlatformTest.efi transports | glink | mhi | gyro | gps (read-only prerequisites)\n"
        L"PlatformTest.efi bootcheck (ARM64 ACPI/GOP/RNG/memory attributes)\n"
        L"PlatformTest.efi rpmb-info | rpmb-read <address> <frames 1..4>\n"
        L"PlatformTest.efi acpi-read <table-index 0..63> <offset> <bytes 1..4096>\n"
        L"PlatformTest.efi brightness-status | flash-status | flash <channel 0..1> <ms 50..300> <mA 25/50/75/100>\n"
        L"RPMB commands only read. Raw frames have no verified MAC authentication.\n");
}
EFI_STATUS EFIAPI UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  PLATFORM_TEST_RESULT Result;
  USB_HOST_TEST_RESULT Usb;
  WLAN_TEST_RESULT Wlan;
  EFI_STATUS Status;
  EFI_SHELL_PARAMETERS_PROTOCOL *Arguments = NULL;
  (VOID)SystemTable;
  gBS->HandleProtocol (ImageHandle,&gEfiShellParametersProtocolGuid,(VOID **)&Arguments);
  if(Arguments==NULL) return EFI_NOT_FOUND;
  if(Arguments->Argc>1 && StrCmp(Arguments->Argv[1],L"help")==0) {Help();return Arguments->Argc==2?EFI_SUCCESS:EFI_INVALID_PARAMETER;}
  if(Arguments->Argc>1 && StrCmp(Arguments->Argv[1],L"rpmb-info")==0) {
    PLATFORM_RPMB_RESULT Rpmb;
    if(Arguments->Argc!=2) return EFI_INVALID_PARAMETER;
    return PlatformRpmbRead(Output,NULL,0,0,&Rpmb);
  }
  if(Arguments->Argc>1 && StrCmp(Arguments->Argv[1],L"rpmb-read")==0) {
    PLATFORM_RPMB_RESULT Rpmb;UINT32 Address,Frames;
    if(Arguments->Argc!=4 || EFI_ERROR(Number(Arguments->Argv[2],&Address)) ||
       EFI_ERROR(Number(Arguments->Argv[3],&Frames)) || Frames==0) return EFI_INVALID_PARAMETER;
    return PlatformRpmbRead(Output,NULL,Address,Frames,&Rpmb);
  }
  if(Arguments->Argc>1 && StrCmp(Arguments->Argv[1],L"acpi-read")==0) {
    UINT32 Index,Offset,Bytes;
    if(Arguments->Argc!=5 || EFI_ERROR(Number(Arguments->Argv[2],&Index)) ||
       EFI_ERROR(Number(Arguments->Argv[3],&Offset)) || EFI_ERROR(Number(Arguments->Argv[4],&Bytes))) return EFI_INVALID_PARAMETER;
    return PlatformAcpiRead(Output,NULL,Index,Offset,Bytes);
  }
  if(Arguments->Argc>1 && StrCmp(Arguments->Argv[1],L"brightness-status")==0) {
    if(Arguments->Argc!=2) return EFI_INVALID_PARAMETER;
    return PlatformBrightnessStatus(Output,NULL);
  }
  if(Arguments->Argc>1 && StrCmp(Arguments->Argv[1],L"flash-status")==0) {
    PLATFORM_FLASH_RESULT Flash;
    if(Arguments->Argc!=2) return EFI_INVALID_PARAMETER;
    return PlatformFlashTest(Output,NULL,0,0,0,&Flash);
  }
  if(Arguments->Argc>1 && StrCmp(Arguments->Argv[1],L"flash")==0) {
    PLATFORM_FLASH_RESULT Flash;UINT32 Channel,Ms,Ma;
    if(Arguments->Argc!=5 || EFI_ERROR(Number(Arguments->Argv[2],&Channel)) ||
       EFI_ERROR(Number(Arguments->Argv[3],&Ms)) || EFI_ERROR(Number(Arguments->Argv[4],&Ma)) || Ms==0) return EFI_INVALID_PARAMETER;
    return PlatformFlashTest(Output,NULL,Channel,Ms,Ma,&Flash);
  }
  if (Arguments != NULL && Arguments->Argc == 2) {
    CONST CHAR16 *Option = Arguments->Argv[1];
    if(StrCmp(Option,L"bootcheck")==0)return PlatformWinBootCheck(Output,NULL);
    if(StrCmp(Option,L"transports")==0 || StrCmp(Option,L"glink")==0 || StrCmp(Option,L"mhi")==0){
      PLATFORM_TRANSPORT_RESULT Transport;
      return PlatformTransportTestSelected(Output,NULL,&Transport,StrCmp(Option,L"glink")==0?1:StrCmp(Option,L"mhi")==0?2:3);
    }
    if (StrCmp(Option,L"sensors")==0 || StrCmp(Option,L"gyro")==0 || StrCmp(Option,L"gps")==0) {
      PLATFORM_SENSOR_RESULT Sensors;
      PLATFORM_TRANSPORT_RESULT Transport;
      Status=PlatformSensorTest(Output,NULL,&Sensors);
      PlatformTransportTestSelected(Output,NULL,&Transport,1);
      return Status;
    }
    if (StrCmp(Option,L"devices")==0 || StrCmp(Option,L"wireless-scan")==0 || StrCmp(Option,L"bluetooth")==0) {
      WIRELESS_SURVEY_RESULT *Radio = AllocateZeroPool(sizeof(*Radio));
      UINTN Attempt;
      if(Radio==NULL) return EFI_OUT_OF_RESOURCES;
      Status = EFI_SUCCESS;
      if(StrCmp(Option,L"bluetooth")==0) Status=PlatformBluetoothRefresh();
      else if(StrCmp(Option,L"wireless-scan")==0) {
        Status=PlatformWirelessScan();
        if(!EFI_ERROR(Status) || Status==EFI_ALREADY_STARTED) {
          for(Attempt=0;Attempt<500;Attempt++) {
            Status=PlatformWirelessGetResult(Radio);
            if(EFI_ERROR(Status) || !Radio->WifiPending) break;
            gBS->Stall(20000);
          }
          if(!EFI_ERROR(Status)) Status=Radio->WifiPending?EFI_TIMEOUT:Radio->WifiStatus;
        }
      }
      PlatformWirelessTest(Output,NULL,Radio);
      FreePool(Radio);
      return Status;
    }
    if(StrCmp(Option,L"wlan-link")==0) {
      WLAN_PCIE_TEST_RESULT Pcie;
      return PlatformWlanPcieTest(Output,NULL,&Pcie);
    }
    if(StrCmp(Option,L"platform")!=0) {Help();return EFI_INVALID_PARAMETER;}
  }
  else if(Arguments->Argc!=1) {Help();return EFI_INVALID_PARAMETER;}
  Status = PlatformHardwareTest (Output, NULL, &Result);
  Print (L"USB host check: %r\n", PlatformUsbHostTest (Output, NULL, &Usb));
  Print (L"WLAN readiness: %r\n", PlatformWlanTest (Output, NULL, &Wlan));
  return Status;
}
