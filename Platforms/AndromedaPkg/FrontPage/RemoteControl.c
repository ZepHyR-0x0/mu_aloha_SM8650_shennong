// SPDX-License-Identifier: BSD-2-Clause-Patent
// One-shot Shell execution captures stdout/stderr without changing GUI drawing.
#include <Uefi.h>
#include <Pi/PiFirmwareFile.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/UsbBringup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include "FrontPage.h"
#include "FrontPageEventService.h"
#include "RemoteControl.h"
#include "BootLog.h"

STATIC USB_BRINGUP_PROTOCOL *mRemoteUsb;
STATIC BOOLEAN mBootPrepared;
STATIC EFI_HANDLE mRemoteParent,mCaptureHandle;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL mCaptureOut;
STATIC EFI_SIMPLE_TEXT_OUTPUT_MODE mCaptureMode;
STATIC struct { CHAR8 *Data;UINTN Capacity,Bytes;BOOLEAN Overflow; } mCapture;

STATIC VOID CaptureCodePoint(UINT32 Code)
{
  UINT8 Encoded[4];UINTN Count;
  if (Code<0x80) { Encoded[0]=(UINT8)Code;Count=1; }
  else if (Code<0x800) { Encoded[0]=(UINT8)(0xc0|(Code>>6));Encoded[1]=(UINT8)(0x80|(Code&63));Count=2; }
  else if (Code<0x10000) {
    Encoded[0]=(UINT8)(0xe0|(Code>>12));Encoded[1]=(UINT8)(0x80|((Code>>6)&63));Encoded[2]=(UINT8)(0x80|(Code&63));Count=3;
  } else {
    Encoded[0]=(UINT8)(0xf0|(Code>>18));Encoded[1]=(UINT8)(0x80|((Code>>12)&63));
    Encoded[2]=(UINT8)(0x80|((Code>>6)&63));Encoded[3]=(UINT8)(0x80|(Code&63));Count=4;
  }
  if (Count>mCapture.Capacity-mCapture.Bytes) { mCapture.Overflow=TRUE;return; }
  if (!mCapture.Overflow) { CopyMem(mCapture.Data+mCapture.Bytes,Encoded,Count);mCapture.Bytes+=Count; }
}
STATIC EFI_STATUS EFIAPI CaptureOutput(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *Text)
{
  UINT32 Code;(VOID)This;
  if (Text==NULL) return EFI_INVALID_PARAMETER;
  for (;*Text!=0;Text++) {
    Code=*Text;
    if (Code>=0xd800 && Code<=0xdbff && Text[1]>=0xdc00 && Text[1]<=0xdfff) {
      Code=0x10000+((Code-0xd800)<<10)+(Text[1]-0xdc00);Text++;
    } else if (Code>=0xd800 && Code<=0xdfff) Code=0xfffd;
    CaptureCodePoint(Code);
  }
  FpServiceEvents();
  return mCapture.Overflow?EFI_BUFFER_TOO_SMALL:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CaptureReset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,BOOLEAN Extended)
{ (VOID)This;(VOID)Extended;return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI CaptureTest(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *Text)
{ (VOID)This;return Text==NULL?EFI_INVALID_PARAMETER:EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI CaptureQuery(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,UINTN Mode,UINTN *Columns,UINTN *Rows)
{
  (VOID)This;if (Mode!=0 || Columns==NULL || Rows==NULL) return EFI_UNSUPPORTED;
  *Columns=120;*Rows=50;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CaptureSetMode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,UINTN Mode)
{ (VOID)This;if (Mode!=0) return EFI_UNSUPPORTED;mCaptureMode.Mode=0;return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI CaptureAttribute(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,UINTN Attribute)
{ (VOID)This;mCaptureMode.Attribute=(INT32)Attribute;return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI CaptureClear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{ (VOID)This;mCaptureMode.CursorColumn=mCaptureMode.CursorRow=0;return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI CaptureCursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,UINTN Column,UINTN Row)
{
  (VOID)This;if (Column>=120 || Row>=50) return EFI_UNSUPPORTED;
  mCaptureMode.CursorColumn=(INT32)Column;mCaptureMode.CursorRow=(INT32)Row;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CaptureEnable(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,BOOLEAN Visible)
{ (VOID)This;mCaptureMode.CursorVisible=Visible;return EFI_SUCCESS; }
STATIC VOID RemoteTableCrc(VOID)
{ gST->Hdr.CRC32=0;(VOID)gBS->CalculateCrc32(gST,gST->Hdr.HeaderSize,&gST->Hdr.CRC32); }

STATIC VOID RemoteBootCheckpoint(CONST CHAR8 *Command,CONST CHAR8 *Phase,EFI_STATUS Status)
{
  EFI_STATUS Saved;
  DEBUG((DEBUG_ERROR,"[EFIBOOT] phase=%a command=%a status=%r\r\n",Phase,Command,Status));
  // Preserve the boot boundary in the existing private logfs snapshot. The
  // RAM ring is reset on the next firmware boot; an early USB snapshot alone
  // cannot explain an EFI application that subsequently resets the phone.
  Saved=BootLogSave(Phase);
  DEBUG((DEBUG_ERROR,"[EFIBOOT] phase=%a snapshot=%r\r\n",Phase,Saved));
}

STATIC EFI_STATUS RemoteShell(CONST CHAR8 *Command,CHAR8 *Output,UINTN Capacity,UINTN *Bytes)
{
  STATIC EFI_GUID ShellFile={0x7c04a583,0x9e3e,0x4f1c,{0xad,0x65,0xe0,0x52,0x68,0xd0,0xb4,0xd1}};
  VOID *Section=NULL;UINTN SectionBytes=0;EFI_HANDLE Shell=NULL;
  EFI_LOADED_IMAGE_PROTOCOL *Image;EFI_STATUS Status;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *SavedOut,*SavedErr;EFI_HANDLE SavedOutHandle,SavedErrHandle;
  CHAR16 Options[256];
  BOOLEAN Launch=Output==NULL && Capacity==0;
  if (Command==NULL || *Command==0 || Bytes==NULL || (!Launch && (Output==NULL || Capacity==0))) return EFI_INVALID_PARAMETER;
  *Bytes=0;
  Status=GetSectionFromAnyFv(&ShellFile,EFI_SECTION_PE32,0,&Section,&SectionBytes);
  if (EFI_ERROR(Status)) {
    if(Launch)RemoteBootCheckpoint(Command,"efi-shell-section-failed",Status);
    return Status;
  }
  Status=gBS->LoadImage(FALSE,mRemoteParent,NULL,Section,SectionBytes,&Shell);
  FreePool(Section);if (EFI_ERROR(Status)) {
    if(Launch)RemoteBootCheckpoint(Command,"efi-shell-load-failed",Status);
    return Status;
  }
  Status=gBS->HandleProtocol(Shell,&gEfiLoadedImageProtocolGuid,(VOID **)&Image);
  if (EFI_ERROR(Status)) {
    if(Launch)RemoteBootCheckpoint(Command,"efi-shell-protocol-failed",Status);
    gBS->UnloadImage(Shell);return Status;
  }
  UnicodeSPrint(Options,sizeof(Options),L"Shell.efi -nostartup -nointerrupt -nomap -noversion -exit %a",Command);
  Image->LoadOptions=Options;Image->LoadOptionsSize=(UINT32)StrSize(Options);
  if(Launch){
    // A boot application can remain running or call ExitBootServices. Keep
    // the real system console; no capture protocol/stack context is installed.
    // Manual CDC launches bypass EfiBootManagerBoot. Its ReadyToBoot event
    // installs the resident kernel errata hook and finalizes ACPI/BGRT.
    // Signal once per firmware session, before the application's memory map.
    if(!mBootPrepared){
      RemoteBootCheckpoint(Command,"efi-ready-to-boot-before",EFI_SUCCESS);
      EfiSignalEventReadyToBoot();mBootPrepared=TRUE;
      RemoteBootCheckpoint(Command,"efi-ready-to-boot-after",EFI_SUCCESS);
    }
    Status=gBS->SetWatchdogTimer(0,0,0,NULL);
    RemoteBootCheckpoint(Command,"efi-watchdog-disabled",Status);
    RemoteBootCheckpoint(Command,"efi-shell-start",EFI_SUCCESS);
    Status=gBS->StartImage(Shell,NULL,NULL);
    RemoteBootCheckpoint(Command,"efi-shell-return",Status);
    (VOID)gBS->UnloadImage(Shell);return Status;
  }
  ZeroMem(&mCapture,sizeof(mCapture));mCapture.Data=Output;mCapture.Capacity=Capacity;
  ZeroMem(&mCaptureMode,sizeof(mCaptureMode));mCaptureMode.MaxMode=1;mCaptureMode.Attribute=7;
  mCaptureOut=(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){CaptureReset,CaptureOutput,CaptureTest,CaptureQuery,CaptureSetMode,
    CaptureAttribute,CaptureClear,CaptureCursor,CaptureEnable,&mCaptureMode};
  Status=gBS->InstallMultipleProtocolInterfaces(&mCaptureHandle,&gEfiSimpleTextOutProtocolGuid,&mCaptureOut,NULL);
  if (EFI_ERROR(Status)) { gBS->UnloadImage(Shell);return Status; }
  SavedOut=gST->ConOut;SavedErr=gST->StdErr;SavedOutHandle=gST->ConsoleOutHandle;SavedErrHandle=gST->StandardErrorHandle;
  gST->ConOut=gST->StdErr=&mCaptureOut;gST->ConsoleOutHandle=gST->StandardErrorHandle=mCaptureHandle;RemoteTableCrc();
  Status=gBS->StartImage(Shell,NULL,NULL);
  gST->ConOut=SavedOut;gST->StdErr=SavedErr;gST->ConsoleOutHandle=SavedOutHandle;gST->StandardErrorHandle=SavedErrHandle;RemoteTableCrc();
  // DXE unloads an application when StartImage returns; Image is no longer
  // owned here. The stack LoadOptions remain valid throughout its execution.
  (VOID)gBS->UninstallMultipleProtocolInterfaces(mCaptureHandle,&gEfiSimpleTextOutProtocolGuid,&mCaptureOut,NULL);
  mCaptureHandle=NULL;(VOID)gBS->UnloadImage(Shell);
  *Bytes=mCapture.Bytes;if (mCapture.Overflow) Status=EFI_BAD_BUFFER_SIZE;
  ZeroMem(&mCapture,sizeof(mCapture));return Status;
}
STATIC EFI_STATUS EFIAPI RemoteExecute(VOID *Context,CONST CHAR8 *Command,CHAR8 *Output,UINTN Capacity,UINTN *Bytes)
{
  EFI_STATUS Status;(VOID)Context;
  if (Bytes==NULL || Command==NULL) return EFI_INVALID_PARAMETER;
  *Bytes=0;
  if (AsciiStrnCmp(Command,"shell ",6)==0) return RemoteShell(Command+6,Output,Capacity,Bytes);
  if (AsciiStrnCmp(Command,"boot ",5)==0) return RemoteShell(Command+5,NULL,0,Bytes);
  if (AsciiStrCmp(Command,"reboot system")==0) {
    gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);return EFI_DEVICE_ERROR;
  }
  if (AsciiStrCmp(Command,"reboot bootloader")==0 || AsciiStrCmp(Command,"reboot recovery")==0) {
    Status=FpPrepareReboot(AsciiStrCmp(Command,"reboot bootloader")==0?
      FRONT_PAGE_ACTION_REBOOT_FASTBOOT:FRONT_PAGE_ACTION_REBOOT_RECOVERY);
    return EFI_ERROR(Status)?Status:FpRebootSelected();
  }
  return EFI_UNSUPPORTED;
}
VOID FpRemoteRegister(EFI_HANDLE ImageHandle)
{
  EFI_GUID Guid=USB_BRINGUP_PROTOCOL_GUID;USB_BRINGUP_PROTOCOL *Usb=NULL;
  mRemoteParent=ImageHandle;
  if (!EFI_ERROR(gBS->LocateProtocol(&Guid,NULL,(VOID **)&Usb)) && Usb!=NULL && Usb->Revision>=6 && Usb->SetRemoteExecutor!=NULL &&
      !EFI_ERROR(Usb->SetRemoteExecutor(Usb,RemoteExecute,NULL))) mRemoteUsb=Usb;
}
VOID FpRemoteUnregister(VOID)
{
  if (mRemoteUsb!=NULL) (VOID)mRemoteUsb->SetRemoteExecutor(mRemoteUsb,NULL,NULL);
  mRemoteUsb=NULL;mRemoteParent=NULL;
}
