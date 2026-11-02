/** @file
  Detects a held volume-up key early in DXE and turns the framebuffer serial
  console on so UEFI debug output is visible during bring-up.

  The prebuilt ButtonsDxe installs SimpleTextInputEx on the keypad device path
  early in DXE. This driver runs immediately after it (FV order) and reads the
  buffered key state before key-press events are consumed by later consumers.

  Copyright (c) 2026, Project Mu port. SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Configuration/BootDevices.h>
#include <Protocol/SimpleTextInEx.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FrameBufferSerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/FirmwareInput.h>
#include "FirmwareInputCore.h"

STATIC FIRMWARE_INPUT_CORE mInput;
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *mKeypadEx;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL *mKeypad;
STATIC EFI_INPUT_READ_KEY_EX mReadEx;
STATIC EFI_INPUT_READ_KEY mRead;
STATIC EFI_EVENT mNativeWaitEx,mNativeWait,mFilteredWaitEx,mFilteredWait;
STATIC EFI_EVENT mVerboseSampleEvent;
STATIC UINT64 mVerboseSampleUntil;
STATIC UINT64 FiNow (VOID) { return GetTimeInNanoSecond(GetPerformanceCounter())/1000000; }
STATIC VOID EFIAPI SampleVerbose (EFI_EVENT Event, VOID *Context)
{
  EFI_KEY_DATA Key;
  UINTN Index;
  (VOID)Context;
  if (FiNow() < mVerboseSampleUntil && !FbConGetVerbose()) {
    // Preserve unrelated input in the injection queue. Sampling must never
    // stall DXE or consume the key which the boot menu will need.
    for (Index=0;Index<32 && mInput.Count<ARRAY_SIZE(mInput.Keys);Index++) {
      if (EFI_ERROR(mReadEx(mKeypadEx,&Key))) break;
      if (Key.Key.ScanCode==SCAN_UP) { FbConSetVerbose(TRUE);break; }
      (VOID)FiInject(&mInput,&Key);
    }
  }
  if (Event!=NULL && (FiNow()>=mVerboseSampleUntil || FbConGetVerbose())) {
    (VOID)gBS->CloseEvent(Event);
    mVerboseSampleEvent=NULL;
  }
}
STATIC VOID EFIAPI FilterWait (EFI_EVENT Event, VOID *Context)
{
  EFI_EVENT Native=(EFI_EVENT)Context;
  if (mInput.Count!=0 || (Native!=NULL && !EFI_ERROR(gBS->CheckEvent(Native))))
    (VOID)gBS->SignalEvent(Event);
}
STATIC EFI_STATUS EFIAPI FilterReadEx (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *Key)
{
  EFI_STATUS Status;UINTN I;
  if (Key==NULL) return EFI_INVALID_PARAMETER;
  for (I=0;I<64;I++) {
    Status=FiRead(&mInput,Key)?EFI_SUCCESS:mReadEx(This,Key);
    if (EFI_ERROR(Status)) return Status;
    if (FiAccept(&mInput,&Key->Key,FiNow())) return EFI_SUCCESS;
  }
  return EFI_NOT_READY;
}
STATIC EFI_STATUS EFIAPI FilterRead (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
  EFI_STATUS Status;EFI_KEY_DATA Data;UINTN I;
  if (Key==NULL) return EFI_INVALID_PARAMETER;
  for (I=0;I<64;I++) {
    if (FiRead(&mInput,&Data)) { *Key=Data.Key;Status=EFI_SUCCESS; }
    else Status=mRead(This,Key);
    if (EFI_ERROR(Status)) return Status;
    if (FiAccept(&mInput,Key,FiNow())) return EFI_SUCCESS;
  }
  return EFI_NOT_READY;
}
STATIC EFI_STATUS EFIAPI InjectKey (FIRMWARE_INPUT_PROTOCOL *This, CONST EFI_KEY_DATA *Key)
{
  EFI_STATUS Status;(VOID)This;
  Status=FiInject(&mInput,Key);
  if (!EFI_ERROR(Status)) {
    (VOID)gBS->SignalEvent(mKeypadEx->WaitForKeyEx);
    if (mKeypad!=NULL) (VOID)gBS->SignalEvent(mKeypad->WaitForKey);
  }
  return Status;
}
STATIC VOID EFIAPI SuppressPower (FIRMWARE_INPUT_PROTOCOL *This, UINTN Milliseconds)
{
  (VOID)This;mInput.PowerUntil=MAX(mInput.PowerUntil,FiNow()+Milliseconds);
}
STATIC FIRMWARE_INPUT_PROTOCOL mFirmwareInput={InjectKey,SuppressPower};

EFI_STATUS
EFIAPI
VerboseKeyInit (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                        Status;
  EFI_DEVICE_PATH_PROTOCOL         *Path;
  EFI_HANDLE                        Handle = NULL;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *In     = NULL;

  Path = (EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath;
  Status = gBS->LocateDevicePath (
                  &gEfiSimpleTextInputExProtocolGuid,
                  &Path,
                  &Handle
                  );
  if (EFI_ERROR (Status) || (Handle == NULL)) {
    return EFI_SUCCESS;
  }

  Status = gBS->HandleProtocol (
                  Handle,
                  &gEfiSimpleTextInputExProtocolGuid,
                  (VOID **)&In
                  );
  if (EFI_ERROR (Status) || (In == NULL)) {
    return EFI_SUCCESS;
  }

  mNativeWaitEx=In->WaitForKeyEx;
  Status=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_NOTIFY,FilterWait,mNativeWaitEx,&mFilteredWaitEx);
  if (EFI_ERROR(Status)) return Status;
  mKeypadEx=In;mReadEx=In->ReadKeyStrokeEx;In->WaitForKeyEx=mFilteredWaitEx;
  In->ReadKeyStrokeEx=FilterReadEx;
  if (!EFI_ERROR(gBS->HandleProtocol(Handle,&gEfiSimpleTextInProtocolGuid,(VOID **)&mKeypad))) {
    mRead=mKeypad->ReadKeyStroke;mKeypad->ReadKeyStroke=FilterRead;
    mNativeWait=mKeypad->WaitForKey;
    Status=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_NOTIFY,FilterWait,mNativeWait,&mFilteredWait);
    if (!EFI_ERROR(Status)) mKeypad->WaitForKey=mFilteredWait;
  }
  EFI_GUID Guid=FIRMWARE_INPUT_PROTOCOL_GUID;
  Status=gBS->InstallProtocolInterface(&Handle,&Guid,EFI_NATIVE_INTERFACE,&mFirmwareInput);
  if (EFI_ERROR(Status)) {
    In->ReadKeyStrokeEx=mReadEx;
    In->WaitForKeyEx=mNativeWaitEx;gBS->CloseEvent(mFilteredWaitEx);
    if (mKeypad!=NULL) {
      mKeypad->ReadKeyStroke=mRead;mKeypad->WaitForKey=mNativeWait;
      if (mFilteredWait!=NULL) gBS->CloseEvent(mFilteredWait);
    }
  } else {
    mVerboseSampleUntil=FiNow()+600;
    SampleVerbose(NULL,NULL);
    if (!FbConGetVerbose()) {
      EFI_STATUS TimerStatus=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,
        SampleVerbose,NULL,&mVerboseSampleEvent);
      if (!EFI_ERROR(TimerStatus)) {
        TimerStatus=gBS->SetTimer(mVerboseSampleEvent,TimerPeriodic,50000);
        if (EFI_ERROR(TimerStatus)) {
          (VOID)gBS->CloseEvent(mVerboseSampleEvent);mVerboseSampleEvent=NULL;
        }
      }
    }
  }

  return Status;
}

