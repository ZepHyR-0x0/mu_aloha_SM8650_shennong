// Copyright (c) 2026, ProjectMu shennong port.
// SPDX-License-Identifier: BSD-2-Clause-Patent
// ABL's USB-only client: InitUsbController event -> StartEx -> HandleEvent.
// The three unmodified stock DXE modules own all hardware and control requests.
#include <Uefi.h>
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EFIUsbDevice.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/UsbBringup.h>
#include <Protocol/UsbFunctionIo.h>
#include <Protocol/ApplicationEventService.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/FirmwareInput.h>
#include "../../FrontPage/BootLog.h"
#if defined (MDE_CPU_AARCH64)
#include <Library/ArmLib.h>
#endif

#pragma pack(1)
typedef struct {
  EFI_USB_CONFIG_DESCRIPTOR Config;
  EFI_USB_INTERFACE_DESCRIPTOR Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR Out;
  EFI_USB_ENDPOINT_DESCRIPTOR In;
} STOCK_HS_CONFIG;
typedef struct {
  EFI_USB_CONFIG_DESCRIPTOR Config;
  EFI_USB_INTERFACE_DESCRIPTOR Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR Out;
  UINT8 OutCompanion[6];
  EFI_USB_ENDPOINT_DESCRIPTOR In;
  UINT8 InCompanion[6];
} STOCK_SS_CONFIG;
#pragma pack()

STATIC EFI_USB_DEVICE_DESCRIPTOR mHsDevice = {
  18, 1, 0x0200, 0, 0, 0, 64, 0x18D1, 0xD00D, 0x0100, 0, 0, 0, 1
};
STATIC EFI_USB_DEVICE_DESCRIPTOR mSsDevice = {
  18, 1, 0x0300, 0, 0, 0, 9, 0x18D1, 0xD00D, 0x0100, 0, 0, 0, 1
};
STATIC EFI_USB_DEVICE_QUALIFIER_DESCRIPTOR mQualifier = {
  10, 6, 0x0200, 0, 0, 0, 64, 1, 0
};
STATIC STOCK_HS_CONFIG mHsConfig = {
  { 9, 2, sizeof (STOCK_HS_CONFIG), 1, 1, 0, 0x80, 50 },
  { 9, 4, 0, 0, 2, 0xFF, 0x42, 3, 0 },
  { 7, 5, 0x01, 2, 512, 0 }, { 7, 5, 0x81, 2, 512, 0 }
};
STATIC STOCK_SS_CONFIG mSsConfig = {
  { 9, 2, sizeof (STOCK_SS_CONFIG), 1, 1, 0, 0x80, 50 },
  { 9, 4, 0, 0, 2, 0xFF, 0x42, 3, 0 },
  { 7, 5, 0x01, 2, 1024, 0 }, { 6, 0x30, 0, 0, 0, 0 },
  { 7, 5, 0x81, 2, 1024, 0 }, { 6, 0x30, 0, 0, 0, 0 }
};
STATIC UINT8 mBos[] = {
  5, 15, 22, 0, 2, 7, 16, 2, 2, 0, 0, 0,
  10, 16, 3, 0, 0x0E, 0, 1, 0, 0, 0
};
STATIC UINT8 mLanguage[] = { 4, 3, 9, 4 };
STATIC VOID *mHsDescriptors[] = { &mHsConfig, NULL };
STATIC VOID *mSsDescriptors[] = { &mSsConfig, NULL };
STATIC EFI_USB_STRING_DESCRIPTOR *mStrings[] = { (VOID *)mLanguage };
STATIC USB_DEVICE_DESCRIPTOR_SET mSet = {
  &mHsDevice, mHsDescriptors, &mSsDevice, mSsDescriptors,
  &mQualifier, mBos, 1, mStrings
};
STATIC EFI_GUID mInitGuid = {
  0x1C0CFFCE, 0xFC8D, 0x4E44, {0x8C,0x78,0x9C,0x9E,0x5B,0x53,0x0D,0x36}
};
STATIC EFI_GUID mConfigGuid = {
  0xE722B03F, 0xB250, 0x42CE, {0x8E,0xBD,0x5B,0xD5,0x18,0x12,0xD0,0x37}
};
STATIC EFI_USB_DEVICE_PROTOCOL *mDevice;
STATIC EFI_EVENT mPoll;
STATIC EFI_EVENT mExit;
STATIC BOOLEAN mAttempted;
STATIC BOOLEAN mInitSignaled;
STATIC UINT32 mErrors;
STATIC UINT32 mEvents;
STATIC EFI_HANDLE mImageHandle;
STATIC BOOLEAN mPolling;
STATIC UINT32 mPollCalls;
STATIC APPLICATION_EVENT_SERVICE_PROTOCOL mApplicationService;
STATIC VOID ConsoleRestore (VOID);

//
// FILE_GUID of Binaries/QcomPkg/Drivers/UsbDeviceDxe. Its .depex is
// "SPMI AND SMEM". Loaded-image enumeration only measures current residency:
// CoreStartImage unloads a driver whose entry returns an error. A missing resident
// handle therefore does not prove that its FFS was absent or never dispatched.
//
STATIC EFI_GUID mUsbDeviceDxeFileGuid = {
  0x3299A266, 0x15F0, 0x4346, { 0x83, 0x18, 0x71, 0x63, 0x36, 0x73, 0x6D, 0x3E }
};
STATIC EFI_GUID mSpmiGuid = {
  0xFA5F306B, 0xF47D, 0x4AC4, {0xA4,0x7D,0x88,0x2F,0x82,0x04,0xEC,0x30}
};
STATIC EFI_GUID mSmemGuid = {
  0xF4E5C7D0, 0xD239, 0x47CB, {0xAA,0xCD,0x7F,0x66,0xEF,0x76,0x32,0x38}
};

STATIC VOID
StockSay (IN CONST CHAR8 *Format, ...)
{
  CHAR8 Buffer[192];
  VA_LIST Marker;
  UINTN Length;
  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  SerialPortWrite ((UINT8 *)Buffer, Length);
}

STATIC BOOLEAN StockPathHasFileGuid (EFI_DEVICE_PATH_PROTOCOL *Path, EFI_GUID *Guid);
STATIC VOID StockResetReason (VOID);
#include <Library/WcdUsbLib.h>
STATIC VOID StockCheckpoint (CONST CHAR8 *Phase)
{
  STATIC UINTN Count;
  EFI_TPL Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  EFI_STATUS Status;
  gBS->RestoreTPL (Tpl);
  if (Tpl != TPL_APPLICATION || Count >= 24) return;
  Count++;
  StockSay ("[USBLOG b319] checkpoint %a enter\r\n", Phase);
  Status = BootLogSave (Phase);
  StockSay ("[USBLOG b319] checkpoint %a -> %r\r\n", Phase, Status);
}
STATIC BOOLEAN StockFunctionMessage (EFI_USBFN_MESSAGE Message, UINTN Size, EFI_USBFN_MESSAGE_PAYLOAD *Payload);
STATIC EFI_STATUS StockConfigureSwitch (BOOLEAN Reset);
STATIC EFI_STATUS StockConfigureRepeater (VOID);
#include "StockTrace.h"

STATIC VOID
StockReleasePump (VOID)
{
  StockTraceEnd ();
  mApplicationService.WakeEvent = NULL;
  if (mPoll != NULL) {
    gBS->SetTimer (mPoll, TimerCancel, 0);
    gBS->CloseEvent (mPoll);
    mPoll = NULL;
  }
  if (mExit != NULL) {
    gBS->CloseEvent (mExit);
    mExit = NULL;
  }
}

STATIC VOID EFIAPI
StockExit (IN EFI_EVENT Event, IN VOID *Context)
{
  (VOID)Event;
  (VOID)Context;
  ConsoleRestore ();
  StockReleasePump ();
}

STATIC EFI_TPL
StockCurrentTpl (VOID)
{
  EFI_TPL Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Tpl);
  return Tpl;
}

STATIC EFI_STATUS EFIAPI StockPoll (IN APPLICATION_EVENT_SERVICE_PROTOCOL *This);
STATIC VOID StockConsoleInput (CONST UINT8 *Data, UINTN Bytes);
STATIC EFI_STATUS StockConsoleRead (CHAR8 *Data, UINTN Capacity, UINTN *Bytes);
STATIC BOOLEAN mConsoleAttached;
#include "UsbConsoleCore.h"
#include "UsbFunctions.h"
#include "UsbRamDisk.h"
#include "UsbRamTransfer.h"
#include "UsbRemoteCommands.h"
STATIC EFI_STATUS StockModeReset (VOID);
STATIC VOID StockModeDrain (VOID);
#include "CdcRecoveryCore.h"
STATIC CDC_RECOVERY mCdcRecovery;
#include "StockFunctionClient.h"
#include "UsbShellConsole.h"

STATIC EFI_STATUS StockModeReset (VOID)
{
  EFI_STATUS Status;
  UINT32 Dctl,Dsts;
  if(StockCurrentTpl()!=TPL_APPLICATION || mPolling)return EFI_NOT_READY;
  Dctl=MmioRead32(0xA60C704);Dsts=MmioRead32(0xA60C70C);
  // DWC3 DCTL.RunStop=31 and DSTS.DevCtrlHlt=22. Native Stop can return
  // success after its halt wait timed out; never release DMA on that alone.
  if((Dctl&BIT31)!=0)return EFI_DEVICE_ERROR;
  // A normally halted controller is ready for the stock Stop/Start flow.
  // Replaying common-init here invalidates native state even when it succeeds.
  if((Dsts&BIT22)!=0)return EFI_SUCCESS;
  if(mBoardCore==NULL || mOriginalCoreInit==NULL || !StockTraceReadable(mBoardCore,0xC8))return EFI_NOT_READY;
  StockSay("[USBRESET] native common init enter dctl=%08x dsts=%08x halted=%u\r\n",Dctl,Dsts,(Dsts&BIT22)!=0);
  (VOID)BootLogSave("usb-native-reset-enter");
  // Use the verified stock common-init entry with its full clock/PHY path.
  // No init-event redispatch, native image patch or raw controller write.
  Status=StockBoardCoreInit(mBoardCore,FALSE);
  Dctl=MmioRead32(0xA60C704);Dsts=MmioRead32(0xA60C70C);
  StockSay("[USBRESET] native common init -> %r dctl=%08x dsts=%08x halted=%u\r\n",
    Status,Dctl,Dsts,(Dsts&BIT22)!=0);
  (VOID)BootLogSave("usb-native-reset-result");
  if(EFI_ERROR(Status))return Status;
  return (Dctl&BIT31)==0 && (Dsts&BIT22)!=0?EFI_SUCCESS:EFI_DEVICE_ERROR;
}

STATIC VOID StockModeDrain (VOID)
{
  UINT64 Deadline;
  UINT32 Polls=0;
  if(mActiveMode!=USB_MODE_RAMDISK && mActiveMode!=USB_MODE_STORAGE)return;
  if(mPoll==NULL || mPolling || StockCurrentTpl()!=TPL_APPLICATION)return;
  mFunctions.Quiescing=TRUE;
  Deadline=StockTraceNowMs()+5000;
  StockSay("[USBMODE] drain current MSC transaction state=%u ctrl=%u\r\n",mFunctions.State,mFunctions.ControlState);
  // Complete the current control/BOT response and stop queueing new CBWs.
  // The waiting CBW receive can be cancelled by the stock lifecycle.
  while(!UfMscQuiescent(&mFunctions) && !mFunctions.Failed &&
        StockTraceNowMs()<Deadline && Polls++<512){
    EFI_STATUS S=StockPoll(NULL);
    if(S==EFI_SECURITY_VIOLATION)break;
    gBS->Stall(1000);
  }
  StockSay("[USBMODE] drain complete=%u polls=%u state=%u ctrl=%u\r\n",
    UfMscQuiescent(&mFunctions),Polls,mFunctions.State,mFunctions.ControlState);
}

STATIC EFI_STATUS EFIAPI
StockPoll (IN APPLICATION_EVENT_SERVICE_PROTOCOL *This)
{
  USB_DEVICE_EVENT Message;
  USB_DEVICE_EVENT_DATA Data;
  UINTN Size = sizeof (Data);
  EFI_STATUS Status;
  EFI_TPL Tpl;
  UINT32 Call;
  BOOLEAN NativeSucceeded;
  BOOLEAN CableFell=FALSE;
  (VOID)This;
  if (mDevice == NULL || mPoll == NULL) return EFI_NOT_READY;
  if (mPolling) return EFI_ALREADY_STARTED;
  Tpl = StockCurrentTpl ();
  if (Tpl != TPL_APPLICATION) return EFI_NOT_READY;
  mPolling = TRUE;
  Call = ++mPollCalls;
  // Four paired markers cover early lazy initialization without scrolling.
  // Print before MMIO/HandleEvent so a non-returning native call is visible.
  if (Call <= 12) {
    StockSay ("[USBSTOCK b319] t=%lu poll %u begin TPL=%u HandleEvent=%lx\r\n",
              StockTraceNowMs (), Call, (UINT32)Tpl, (UINT64)(UINTN)mDevice->HandleEvent);
    if (Call == 1) {
#if defined (MDE_CPU_AARCH64)
      StockSay ("[USBSTOCK b319] EL=%u TCR=%lx TTBR0=%lx\r\n",
                (UINT32)(ArmReadCurrentEL () >> 2), ArmGetTCR (),
                (UINT64)ArmGetTTBR0BaseAddress ());
#endif
    }
    StockSay ("[USBSTOCK b319] poll %u native call DCTL=%08x DSTS=%08x\r\n",
              Call, MmioRead32 (0xA60C704), MmioRead32 (0xA60C70C));
  }
  ZeroMem (&Data, sizeof (Data));
  Message = UsbDeviceEventNoEvent;
  StockTraceBegin ();
  StockSmmuReport ();
  // The incorrect raw STARTTRANSFER probes were removed in b319.
  StockLinkProbe ();
  StockCableProbe (Call, TRUE);
  mRawCompletionValid = FALSE;
  Status = mDevice->HandleEvent (&Message, &Size, &Data);
  NativeSucceeded = !EFI_ERROR (Status);
  StockCableProbe (Call, FALSE);
  StockEventProbe ();
  // This marker must precede cleanup, trace restoration, or Raise/RestoreTPL.
  if (Call <= 12) StockSay ("[USBSTOCK b319] t=%lu poll %u native-return %r event=%u size=%u\r\n",
                           StockTraceNowMs (), Call, Status, (UINT32)Message, (UINT32)Size);
  StockTraceEnd ();
  if(!mUsbModeSwitchBusy && mActiveMode==USB_MODE_CDC && mTraceContext!=NULL &&
     StockTraceReadable(mTraceContext+0x181,1))
    CableFell=CrCableObserve(&mCdcRecovery,StockTraceNowMs(),mTraceContext[0x181]==1);
  if (!EFI_ERROR(Status)) Status=StockFunctionEvent(Message,Size,&Data);
  if (!EFI_ERROR(Status) && mFunctions.Mode==USB_MODE_CDC) Status=UfCdcNext(&mFunctions);
  // EFIUsbDevice.h transfers ownership of OEM event Context to its caller.
  if (NativeSucceeded && Message == UsbDeviceEventOemEvent && Size >= sizeof (Data.OemData) && Data.OemData.Context != NULL) {
    if (Call <= 12) StockSay ("[USBSTOCK b319] oem-free enter context=%lx\r\n", (UINT64)(UINTN)Data.OemData.Context);
    FreePool (Data.OemData.Context);
    if (Call <= 12) StockSay ("[USBSTOCK b319] oem-free leave\r\n");
  }
  Tpl = StockCurrentTpl ();
  mPolling = FALSE;
  if(CableFell){
    StockSay("[CDC] physical detach; cancel session, defer stock rearm outside pump\r\n");
    mUsbConfigured=FALSE;
    (VOID)UfConnected(&mFunctions,FALSE);
    RamUploadService(FALSE,FALSE);
    (VOID)BootLogSave("cdc-physical-detach");
  }
  if (mUsbSavePending) { mUsbSavePending=FALSE;(VOID)BootLogSave("usb-configured-state"); }
  StockMscSaveService(Status);
  if (Call == 13)
    // The stock attach path power-cycles the USB GDSC and re-inits the core
    // after StartEx already configured it, so the endpoint enable bitmap may be
    // lost. Carry DCTL/DALEPENA/DSTS on the heartbeat: if dale drops to 0 the
    // staged Submit is aimed at a disabled endpoint, which is exactly the shape
    // of the hang.
    StockSay ("[USBSTOCK b319] poll %u alive %r event=%u dctl=%08x dale=%08x dsts=%08x\r\n",
      Call, Status, (UINT32)Message, MmioRead32 (0xA60C704), MmioRead32 (0xA60C720),
      MmioRead32 (0xA60C70C));
  if (Call <= 12) StockSay ("[USBSTOCK b319] t=%lu poll %u returned %r TPL=%u event=%u\r\n",
                           StockTraceNowMs (), Call, Status, (UINT32)Tpl, (UINT32)Message);
  if (Tpl != TPL_APPLICATION) {
    StockSay ("[USBSTOCK b319] native TPL changed to %u; service disabled\r\n", (UINT32)Tpl);
    StockReleasePump ();
    return EFI_ABORTED;
  }
  if(Status==EFI_SECURITY_VIOLATION)(VOID)CrObserve(&mCdcRecovery,StockTraceNowMs(),FALSE,FALSE,FALSE,TRUE);
  if(!mUsbModeSwitchBusy && mActiveMode==USB_MODE_CDC && !mRemote.Running && !mRemote.Reboot && !mRemote.ModeSwitch && !mConsoleAttached &&
     CrObserve(&mCdcRecovery,StockTraceNowMs(),mTraceContext!=NULL && StockTraceReadable(mTraceContext+0x181,1) && mTraceContext[0x181]==1,
               mUsbConfigured,mFunctions.Failed,FALSE)){
    StockSay("[CDC] recovery attempt=%u configured=%u class_failed=%u\r\n",mCdcRecovery.Attempts,mUsbConfigured,mFunctions.Failed);
    StockSessionReset();RamUploadService(FALSE,FALSE);
    EFI_STATUS Recovery=StockSelectMode(NULL,USB_MODE_CDC);
    StockSay("[CDC] recovery Stop/StartEx -> %r\r\n",Recovery);return Recovery;
  }
  // Shell execution can itself service USB. Leave mPolling before calling the
  // application; its Running flag prevents recursive command dispatch.
  if (!mUsbModeSwitchBusy && Status!=EFI_SECURITY_VIOLATION && mActiveMode==USB_MODE_CDC) {
    StockCommandService(mFunctions.Connected,mFunctions.TxBusy);
    if (mRemote.HeaderBytes!=0) mFunctions.ReplyActive=TRUE;
    if(mRemote.ModeSwitch && mRemote.AckSubmitted && !mFunctions.TxBusy && !mFunctions.CdcZlpPending){
      UINT32 Mode=mRemote.SwitchMode,Seconds=mRemote.ReturnSeconds,Speed=mRemote.SwitchSpeed;
      BOOLEAN SpeedSwitch=mRemote.SpeedSwitch;StockCommandReset();
      EFI_STATUS Switch=SpeedSwitch?StockSelectSpeed(NULL,Speed):StockSelectMode(NULL,Mode);
      if(!EFI_ERROR(Switch) && Mode==USB_MODE_RAMDISK && Seconds!=0)mCdcReturnAt=StockTraceNowMs()+(UINT64)Seconds*1000;
      return Switch;
    }
  }
  if(mSpeedFallbackAt!=0 && !mUsbModeSwitchBusy){
    if(mUsbConfigured)mSpeedFallbackAt=0;
    else if(StockTraceNowMs()>=mSpeedFallbackAt){
      mSpeedFallbackAt=0;StockSay("[USBSPEED] SS configuration timeout; returning to HS CDC\r\n");
      mRequestedSpeed=USB_SPEED_HIGH;mNegotiatedSpeed=0;
      return StockSelectMode(NULL,USB_MODE_CDC);
    }
  }
  if(mCdcReturnAt!=0 && StockTraceNowMs()>=mCdcReturnAt){
    StockSay("[USBBOT] timed return cbw=%u op=%02x state=%u rx=%u tx=%u failed=%u\r\n",
      (UINT32)mBotCommands,mMscLastOp,mFunctions.State,mFunctions.RxBusy,mFunctions.TxBusy,mFunctions.Failed);
    (VOID)BootLogSave("usb-msc-timed-return");
    mCdcReturnAt=0;return StockSelectMode(NULL,USB_MODE_CDC);
  }
  if (EFI_ERROR (Status) && Status != EFI_NOT_READY) {
    if (mErrors<4) mErrors++;
    if (mErrors == 1) StockSay ("[USBSTOCK b319] HandleEvent -> %r\r\n", Status);
    if (Status==EFI_SECURITY_VIOLATION) {
      StockSay ("[USBMODE b339] DMA/security failure; service stopped\r\n");
      StockReleasePump ();
    } else if (mErrors==3) {
      // A class transfer failure must not stop native bus-event handling.
      // The host's RESET/BOT reset is what clears the failed class state.
      // Closing the pump here prevented recovery and left an unknown device
      // until reboot. Continue at 10 ms while waiting for the recovery event.
      StockSay("[USBMODE b339] waiting for host reset; native=%u class-failed=%u\r\n",NativeSucceeded,mFunctions.Failed);
      (VOID)gBS->SetTimer(mPoll,TimerPeriodic,100000);
    }
    return Status;
  }
  if (mErrors!=0 && mPoll!=NULL) {
    (VOID)gBS->SetTimer(mPoll,TimerPeriodic,10000);
    StockSay ("[USBMODE b339] bus/class event service recovered\r\n");
  }
  mErrors = 0;
  if (!EFI_ERROR (Status) && Message != UsbDeviceEventNoEvent && mEvents++ < 32) {
    StockSay ("[USBSTOCK b319] event=%u size=%u DCTL=%08x DSTS=%08x\r\n",
              (UINT32)Message, (UINT32)Size,
              MmioRead32 (0xA60C704), MmioRead32 (0xA60C70C));
  }
  return Status;
}

STATIC VOID EFIAPI
StockNoop (IN EFI_EVENT Event, IN VOID *Context)
{
  (VOID)Event;
  (VOID)Context;
}

//
// Does this device path name the given firmware volume file?
//
STATIC
BOOLEAN
StockPathHasFileGuid (
  IN EFI_DEVICE_PATH_PROTOCOL  *Path,
  IN EFI_GUID                  *Guid
  )
{
  while (!IsDevicePathEnd (Path)) {
    if ((DevicePathType (Path) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Path) == MEDIA_PIWG_FW_FILE_DP)) {
      if (CompareGuid (&((MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)Path)->FvFileName, Guid)) {
        return TRUE;
      }
    }
    Path = NextDevicePathNode (Path);
  }
  return FALSE;
}

STATIC
UINTN
StockCountProtocol (
  IN EFI_GUID  *Guid
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  EFI_STATUS  Status;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, Guid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    return 0;
  }
  FreePool (Handles);
  return Count;
}

//
// Report residency separately from protocol publication. The native entry at
// 0x1144 -> 0x1504 -> 0x2004 installs UsbDevice directly, not DriverBinding.
// Connecting a resident image is therefore not a repair for this native entry.
//
STATIC
VOID
StockProbeDeviceDxe (
  VOID
  )
{
  EFI_HANDLE                 *Handles;
  UINTN                      Count;
  UINTN                      Index;
  UINTN                      Loaded;
  EFI_LOADED_IMAGE_PROTOCOL  *Image;
  EFI_STATUS                 Status;
  BOOLEAN                    Found;

  Handles = NULL;
  Count   = 0;
  Loaded  = 0;
  Found   = FALSE;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid,
                                    NULL, &Count, &Handles);
  if (!EFI_ERROR (Status) && (Handles != NULL)) {
    for (Index = 0; Index < Count; Index++) {
      Image = NULL;
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiLoadedImageProtocolGuid,
                                          (VOID **)&Image)) || (Image == NULL)) {
        continue;
      }
      Loaded++;
      if ((Image->FilePath != NULL) &&
          StockPathHasFileGuid (Image->FilePath, &mUsbDeviceDxeFileGuid)) {
        Found = TRUE;
        StockSay ("[USBSTOCK b319] UsbDeviceDxe image base=%lx size=%lx\r\n",
                  (UINT64)(UINTN)Image->ImageBase,
                  (UINT64)Image->ImageSize);
      }
    }
    FreePool (Handles);
  }

  StockSay ("[USBSTOCK b319] images=%u UsbDeviceDxe=%a (residency only)\r\n",
            (UINT32)Loaded, Found ? "RESIDENT" : "NOT-RESIDENT");
  StockSay ("[USBSTOCK b319] UsbDevice handles=%u UsbfnIo handles=%u\r\n",
            (UINT32)StockCountProtocol (&gEfiUsbDeviceProtocolGuid),
            (UINT32)StockCountProtocol (&gEfiUsbFunctionIoProtocolGuid));

}

// Retry a waiting DXE after the native function controller has been connected.
// If no resident copy remains, load the exact memory-FV PE once to capture the
// actual LoadImage/StartImage status. This does not read or write phone storage.
STATIC EFI_STATUS
StockRecoverDeviceDxe (VOID)
{
  EFI_STATUS Status;
  EFI_TPL Tpl;
  VOID *Dependency = NULL;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  UINTN Index;
  EFI_HANDLE Native = NULL;

  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Tpl);
  StockSay ("[USBSTOCK b319] native recovery TPL=%u\r\n", (UINT32)Tpl);
  if (Tpl != TPL_APPLICATION) return EFI_NOT_READY;
  if (gDS != NULL && gDS->Dispatch != NULL) {
    Status = gDS->Dispatch ();
    StockSay ("[USBSTOCK b319] DXE Dispatch -> %r\r\n", Status);
    if (!EFI_ERROR (gBS->LocateProtocol (&gEfiUsbDeviceProtocolGuid, NULL, (VOID **)&mDevice)) && mDevice != NULL) {
      return EFI_SUCCESS;
    }
    if (Status == EFI_ALREADY_STARTED) return Status;
  }
  // Do not create a duplicate when an entry succeeded but published no interface.
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || Handles == NULL) {
    if (Handles != NULL) FreePool (Handles);
    // EFI_NOT_FOUND means no resident images. Other errors cannot establish
    // that loading another copy is safe.
    if (Status != EFI_NOT_FOUND) return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }
  if (!EFI_ERROR (Status) && Handles != NULL) {
    for (Index = 0; Index < Count; Index++) {
      EFI_LOADED_IMAGE_PROTOCOL *Image = NULL;
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiLoadedImageProtocolGuid, (VOID **)&Image)) &&
          Image != NULL && Image->FilePath != NULL && StockPathHasFileGuid (Image->FilePath, &mUsbDeviceDxeFileGuid)) {
        FreePool (Handles);
        return EFI_ALREADY_STARTED;
      }
    }
    FreePool (Handles);
  }
  Status = gBS->LocateProtocol (&mSpmiGuid, NULL, &Dependency);
  StockSay ("[USBSTOCK b319] native dependency SPMI -> %r\r\n", Status);
  if (EFI_ERROR (Status) || Dependency == NULL) return EFI_NOT_FOUND;
  Dependency = NULL;
  Status = gBS->LocateProtocol (&mSmemGuid, NULL, &Dependency);
  StockSay ("[USBSTOCK b319] native dependency SMEM -> %r\r\n", Status);
  if (EFI_ERROR (Status) || Dependency == NULL) return EFI_NOT_FOUND;

  Handles = NULL;
  Count = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiFirmwareVolume2ProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || Handles == NULL) {
    if (Handles != NULL) FreePool (Handles);
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }
  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < Count; Index++) {
    EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv = NULL;
    VOID *Pe = NULL;
    UINTN Bytes = 0;
    UINT32 Authentication = 0;
    EFI_DEVICE_PATH_PROTOCOL *Path;
    MEDIA_FW_VOL_FILEPATH_DEVICE_PATH File;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv);
    if (EFI_ERROR (Status) || Fv == NULL) {
      if (!EFI_ERROR (Status)) Status = EFI_NOT_FOUND;
      continue;
    }
    Status = Fv->ReadSection (Fv, &mUsbDeviceDxeFileGuid, EFI_SECTION_PE32, 0, &Pe, &Bytes, &Authentication);
    if (EFI_ERROR (Status) || Pe == NULL || Bytes == 0) {
      if (Pe != NULL) FreePool (Pe);
      if (!EFI_ERROR (Status)) Status = EFI_LOAD_ERROR;
      continue;
    }
    StockSay ("[USBSTOCK b319] UsbDeviceDxe PE present in FV bytes=%u auth=%x\r\n", (UINT32)Bytes, Authentication);
    if ((Authentication & EFI_AUTH_STATUS_TEST_FAILED) != 0) {
      FreePool (Pe);
      Status = EFI_SECURITY_VIOLATION;
      break;
    }
    ZeroMem (&File, sizeof (File));
    File.Header.Type = MEDIA_DEVICE_PATH;
    File.Header.SubType = MEDIA_PIWG_FW_FILE_DP;
    SetDevicePathNodeLength (&File.Header, sizeof (File));
    CopyGuid (&File.FvFileName, &mUsbDeviceDxeFileGuid);
    Path = AppendDevicePathNode (DevicePathFromHandle (Handles[Index]), &File.Header);
    if (Path == NULL) {
      FreePool (Pe);
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }
    Status = gBS->LoadImage (FALSE, mImageHandle, Path, Pe, Bytes, &Native);
    FreePool (Path);
    FreePool (Pe);
    StockSay ("[USBSTOCK b319] native LoadImage -> %r handle=%lx\r\n", Status, (UINT64)(UINTN)Native);
    if (!EFI_ERROR (Status) && Native != NULL) {
      Status = gBS->StartImage (Native, NULL, NULL);
      // CoreStartImage itself closes/unloads on failure; Native may now be stale.
      StockSay ("[USBSTOCK b319] native StartImage -> %r\r\n", Status);
    } else if (Native != NULL) {
      gBS->UnloadImage (Native);
    } else if (!EFI_ERROR (Status)) {
      Status = EFI_LOAD_ERROR;
    }
    break; // exactly one PE load/start attempt per ENABLE USB, never one per FV
  }
  FreePool (Handles);
  return Status;
}

// Restore WCD USB routing after native common-init/reset. Native GPIO(2,6)
// and the separate TLMM152 WCD path must not be confused with repeater FD46.
// New before/restored readbacks establish the real board state.
STATIC EFI_STATUS
StockConfigureSwitch (BOOLEAN Reset)
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
  STATIC UINTN Reports;
  STATIC CONST UINT16 Registers[] = { 0x403, 0x404, 0x419, 0x415 };
  UINT8 Before[4] = {0}, After[4] = {0};
  UINTN Index;
  BOOLEAN Log = Reports++ < 6;

  Status = gBS->LocateProtocol (&I2cGuid, NULL, (VOID **)&Protocol);
  if (EFI_ERROR (Status) || Protocol == NULL) {
    StockSay ("[USB] wcd i2c protocol -> %r\r\n", Status);
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }
  Open = (I2C_OPEN)*(UINT64 *)(Protocol + 8);
  Close = (I2C_CLOSE)*(UINT64 *)(Protocol + 0x28);
  ZeroMem (&Io, sizeof (Io));
  Io.Config = Config;
  Io.Read = (WCD_I2C_TRANSFER)*(UINT64 *)(Protocol + 0x10);
  Io.Write = (WCD_I2C_TRANSFER)*(UINT64 *)(Protocol + 0x18);
  if (Open == NULL || Io.Read == NULL || Io.Write == NULL || Close == NULL) {
    StockSay ("[USB] wcd i2c methods missing\r\n");
    return EFI_UNSUPPORTED;
  }
  Rc = Open (0x0E, &Io.Handle);
  if (Log) StockSay ("[USB] wcd open rc=%u handle=%lx\r\n", Rc, Io.Handle);
  if (Rc != 0 || Io.Handle == 0) return EFI_DEVICE_ERROR;
  for (Index = 0; Index < ARRAY_SIZE (Registers); ++Index) {
    Status = WcdReadRegister (&Io, Registers[Index], &Before[Index]);
    if (EFI_ERROR (Status)) break;
  }
  if (Log) StockSay ("[USBWCD b327] after-native before cfg=%02x sel=%02x mode=%02x eq=%02x -> %r\r\n",
    Before[0], Before[1], Before[2], Before[3], Status);
  if (!EFI_ERROR (Status) && Reset) Status = WcdResetUsb (&Io);
  if (!EFI_ERROR (Status)) Status = WcdConfigureUsb (&Io);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < ARRAY_SIZE (Registers); ++Index) {
      Status = WcdReadRegister (&Io, Registers[Index], &After[Index]);
      if (EFI_ERROR (Status)) break;
    }
  }
  if (Log) StockSay ("[USBWCD b327] restored cfg=%02x sel=%02x mode=%02x eq=%02x -> %r\r\n",
    After[0], After[1], After[2], After[3], Status);
  Rc = Close (Io.Handle);
  if (Log) StockSay ("[USB] wcd close rc=%u\r\n", Rc);
  if (Rc != 0 && !EFI_ERROR (Status)) Status = EFI_DEVICE_ERROR;
  return Status;
}

#include "StockRepeaterCore.h"
typedef UINT32 (EFIAPI *STOCK_SPMI_READ)(VOID *, UINT32, UINT32, UINT32,
  UINT32, UINT8 *, UINT32, UINT32 *);
typedef UINT32 (EFIAPI *STOCK_SPMI_WRITE)(VOID *, UINT32, UINT32, UINT32,
  UINT32, UINT8 *, UINT32);
typedef struct { VOID *Protocol; STOCK_SPMI_READ Read; STOCK_SPMI_WRITE Write; } STOCK_SPMI_IO;

STATIC EFI_STATUS EFIAPI
StockRepeaterReadByte (VOID *Context, UINT16 Register, UINT8 *Value)
{
  STOCK_SPMI_IO *Io = Context;
  UINT32 Done = 0, Rc;
  *Value = 0;
  Rc = Io->Read (Io->Protocol, 0, 7, 0, Register, Value, 1, &Done);
  return Rc == 0 && Done == 1 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS EFIAPI
StockRepeaterWriteByte (VOID *Context, UINT16 Register, UINT8 Value)
{
  STOCK_SPMI_IO *Io = Context;
  return Io->Write (Io->Protocol, 0, 7, 0, Register, &Value, 1) == 0 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC VOID EFIAPI
StockRepeaterWait (UINTN Us) { gBS->Stall (Us); }

STATIC EFI_STATUS
StockConfigureRepeater (VOID)
{
  EFI_GUID Guid = { 0xFA5F306B, 0xF47D, 0x4AC4, {0xA4,0x7D,0x88,0x2F,0x82,0x04,0xEC,0x30} };
  STATIC UINTN Reports;
  STATIC CONST UINT16 Registers[] = { 0xFD46, 0xFD08, 0xFD40, 0xFD41, 0xFDE8, 0xFDED };
  STOCK_SPMI_IO Bus;
  STOCK_RPTR_IO Io;
  EFI_STATUS Status;
  UINT8 Before[6] = {0}, After[6] = {0}, Ready = 0;
  UINTN Index;
  BOOLEAN Log = Reports++ < 6;
  ZeroMem (&Bus, sizeof (Bus));
  Status = gBS->LocateProtocol (&Guid, NULL, &Bus.Protocol);
  if (EFI_ERROR (Status) || Bus.Protocol == NULL) return EFI_NOT_FOUND;
  Bus.Read = (STOCK_SPMI_READ)*(UINT64 *)((UINT8 *)Bus.Protocol + 8);
  Bus.Write = (STOCK_SPMI_WRITE)*(UINT64 *)((UINT8 *)Bus.Protocol + 0x10);
  if (Bus.Read == NULL || Bus.Write == NULL || gBS->Stall == NULL) return EFI_UNSUPPORTED;
  Io.Context = &Bus; Io.Read = StockRepeaterReadByte; Io.Write = StockRepeaterWriteByte; Io.Wait = StockRepeaterWait;
  for (Index = 0; Index < ARRAY_SIZE (Registers); ++Index) {
    Status = Io.Read (Io.Context, Registers[Index], &Before[Index]);
    if (EFI_ERROR (Status)) return Status;
  }
  if (Log) StockSay ("[USBRPTR b327] before en=%02x ok=%02x mode=%02x hiz=%02x force=%02x/%02x\r\n",
    Before[0], Before[1], Before[2], Before[3], Before[4], Before[5]);
  Status = StockRepeaterPrepare (&Io, &Ready);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < ARRAY_SIZE (Registers); ++Index) {
      Status = Io.Read (Io.Context, Registers[Index], &After[Index]);
      if (EFI_ERROR (Status)) break;
    }
  }
  if (Log) StockSay ("[USBRPTR b327] prepared -> %r en=%02x ok=%02x mode=%02x hiz=%02x force=%02x/%02x\r\n",
    Status, After[0], Ready, After[2], After[3], After[4], After[5]);
  return Status;
}

// DetectPort fails with EFI_DEVICE_ERROR and Dev_Attached_Entry then reports
// "Failed to detect port type", so the device stack never learns which port it
// is on and never enumerates - regardless of how well the link behaves.
//
// The cause is in UsbPwrCtrlDxe, not here: its 11 methods are all gated by one
// byte at RVA 0x8378, and that byte is set to 1 only when UsbPwrCtrlLib_Init
// (RVA 0x4038) returns exactly EFI_SUCCESS from the single call in EfiEntry.
// That module's DEPEX is TRUE, so DXE may dispatch it before PmicDxe publishes
// PmicSchg/PmicVersion/ADC; the locate then fails, Init returns UNSUPPORTED, the
// gate stays 0 forever and every later call reports UNSUPPORTED.
//
// Read the gate, and if it is closed, run Init again now that the PMIC drivers
// are up, then publish the gate. Diagnosis and fix in one place, and every step
// is printed so a wrong assumption is visible instead of silent.
STATIC VOID
StockPwrCtrlFix (VOID)
{
  EFI_GUID PwrGuid = { 0xE07DF17E, 0xE79E, 0x4150,
    { 0x93, 0x78, 0x50, 0x62, 0x3A, 0x14, 0x99, 0x4A } };
  EFI_LOADED_IMAGE_PROTOCOL *Image;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  UINTN Index;
  VOID *Protocol = NULL;
  EFI_STATUS Status;
  UINT8 *Base = NULL;
  UINT8 *Gate;

  Status = gBS->LocateProtocol (&PwrGuid, NULL, &Protocol);
  StockSay ("[USB] pwrctrl protocol -> %r iface=%lx\r\n", Status, (UINT64)(UINTN)Protocol);
  if (EFI_ERROR (Status) || Protocol == NULL) return;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) return;
  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiLoadedImageProtocolGuid, (VOID **)&Image))) continue;
    if (Image->ImageBase == NULL) continue;
    if ((UINTN)Protocol >= (UINTN)Image->ImageBase &&
        (UINTN)Protocol < (UINTN)Image->ImageBase + (UINTN)Image->ImageSize) {
      Base = Image->ImageBase;
      break;
    }
  }
  FreePool (Handles);
  if (Base == NULL) {
    StockSay ("[USB] pwrctrl image not found\r\n");
    return;
  }
  Gate = Base + 0x8378;
  StockSay ("[USB] pwrctrl base=%lx gate=%u\r\n", (UINT64)(UINTN)Base, (UINT32)*Gate);
  if (*Gate == 1) return;
  Status = ((EFI_STATUS (EFIAPI *)(VOID))(Base + 0x4038))();
  StockSay ("[USB] pwrctrl reinit -> %r gate=%u\r\n", Status, (UINT32)*Gate);
  if (!EFI_ERROR (Status) && *Gate == 0) {
    *Gate = 1;
    StockSay ("[USB] pwrctrl gate forced to 1\r\n");
  }
}

STATIC EFI_STATUS EFIAPI
StockTest (IN USB_BRINGUP_PROTOCOL *This)
{
  EFI_STATUS Status;
  EFI_TPL Tpl;
  EFI_EVENT Init = NULL;
  VOID *Config = NULL;
  VOID *FnIo = NULL;
  (VOID)This;
  if (mAttempted) return EFI_ALREADY_STARTED;
  if (StockCurrentTpl () != TPL_APPLICATION) return EFI_NOT_READY;
  Status = gBS->LocateProtocol (&mConfigGuid, NULL, &Config);
  StockSay ("[USBSTOCK b319] UsbConfig -> %r interface=%lx\r\n", Status, (UINT64)(UINTN)Config);
  StockResetReason ();
  if (EFI_ERROR (Status) || Config == NULL) return EFI_NOT_FOUND;
  mAttempted = TRUE;
  // ABL A2C80/A2C94/A2CA4: create, signal, close the native init event first.
  StockSay ("[USBSTOCK b319] InitUsbController signal begin\r\n");
  if (!mInitSignaled) {
    Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, StockNoop, NULL, &mInitGuid, &Init);
    if (EFI_ERROR (Status)) return Status;
    Status = gBS->SignalEvent (Init);
    gBS->CloseEvent (Init);
  } else Status=EFI_SUCCESS;
  StockSay ("[USBSTOCK b319] InitUsbController signal -> %r\r\n", Status);
  if (EFI_ERROR (Status)) return Status;
  // Locate the function and device interfaces AFTER configuration, as ABL does.
  Status = gBS->LocateProtocol (&gEfiUsbFunctionIoProtocolGuid, NULL, &FnIo);
  StockSay ("[USBSTOCK b319] UsbfnIo -> %r interface=%lx\r\n", Status, (UINT64)(UINTN)FnIo);
  if (EFI_ERROR (Status) || FnIo == NULL) return EFI_NOT_FOUND;
  StockProbeDeviceDxe ();
  Status = gBS->LocateProtocol (&gEfiUsbDeviceProtocolGuid, NULL, (VOID **)&mDevice);
  StockSay ("[USBSTOCK b319] UsbDevice -> %r interface=%lx\r\n", Status, (UINT64)(UINTN)mDevice);
  if (EFI_ERROR (Status) || mDevice == NULL) {
    Status = StockRecoverDeviceDxe ();
    StockSay ("[USBSTOCK b319] native recovery -> %r\r\n", Status);
    if (EFI_ERROR (Status)) return Status;
    Status = gBS->LocateProtocol (&gEfiUsbDeviceProtocolGuid, NULL, (VOID **)&mDevice);
    StockSay ("[USBSTOCK b319] UsbDevice after recovery -> %r interface=%lx\r\n", Status, (UINT64)(UINTN)mDevice);
  }
  if (EFI_ERROR (Status) || mDevice == NULL) return EFI_NOT_FOUND;
  if (mDevice->StartEx == NULL || mDevice->HandleEvent == NULL || mDevice->Stop == NULL) return EFI_UNSUPPORTED;
  // InitUsbController publishes the resident protocol chain once. Function
  // changes use its Stop/StartEx lifecycle instead of dispatching init again
  // against live controller objects and transfer pools.
  mInitSignaled=TRUE;
  Tpl = StockCurrentTpl ();
  StockSay ("[USBSTOCK b319] before StartEx TPL=%u\r\n", (UINT32)Tpl);
  if (Tpl != TPL_APPLICATION) return EFI_ABORTED;
  // Stock StartEx requires BOTH HS and SS descriptor sets (native 3C8C..3CAC).
  StockPwrCtrlFix ();
  // Board PHY setup runs at the verified native core-init boundary below.
  StockSay ("[USBSTOCK b319] StartEx begin (native descriptor and EP0 state machines)\r\n");
  StockCheckpoint ("usb-start-ex-enter");
  Status = mDevice->StartEx (&mSet);
  Tpl = StockCurrentTpl ();
  StockSay ("[USBSTOCK b319] StartEx -> %r TPL=%u\r\n", Status, (UINT32)Tpl);
  if (Tpl != TPL_APPLICATION) return EFI_ABORTED;
  if (EFI_ERROR (Status)) return Status;
  Status=StockFunctionCreate ();
  if (EFI_ERROR(Status)) return Status;
  StockTracePrepare (Config, FnIo);
  // Keep native cable state. CorePoll handles attach/init and detach/stop
  // once per real edge; forcing +181 suppresses attach and repeats cleanup.
  // Native CorePoll owns the complete PHY/clock/core initialization sequence.
  // ABL polls at application TPL. A timer callback would instead run native
  // lazy PHY/core/DMA initialization at TPL_CALLBACK and suppress lower events.
  Status = gBS->CreateEvent (EVT_TIMER, 0, NULL, NULL, &mPoll);
  if (!EFI_ERROR (Status)) {
    Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, StockExit, NULL, &gEfiEventExitBootServicesGuid, &mExit);
  }
  if (!EFI_ERROR (Status)) Status = gBS->SetTimer (mPoll, TimerPeriodic, 10000);
  if (!EFI_ERROR (Status)) {
    mActiveMode=mDesiredMode;
    mApplicationService.WakeEvent = mPoll;
    // First native call is synchronous in ENABLE USB. Later calls are serviced by
    // SWM's WaitForEvent loop at the same TPL, including while viewing RAM LOG.
    StockCheckpoint ("usb-first-poll-enter");
    Status = StockPoll (&mApplicationService);
    if (Status == EFI_NOT_READY) Status = EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    StockReleasePump ();
    // StockSelectMode owns stop/free, including failure and leaked TPL paths.
  }
  StockSay ("[USBSTOCK b319] application event pump -> %r; no periodic alive logs\r\n", Status);
  return Status;
}

STATIC USB_BRINGUP_PROTOCOL mBringup = { 9, StockEnableNative, NULL, NULL, StockSelectMode, StockGetMode,
  StockSetRamSize,StockGetRamSize,StockSetShellConsole,StockSetRemoteExecutor,StockCreateRamDisk,StockGetRamInfo,StockGetSwitchBusy,StockSelectSpeed,StockGetSpeed };
STATIC APPLICATION_EVENT_SERVICE_PROTOCOL mApplicationService = { 1, NULL, StockPoll };

// DT names stream 0x40. Its effective configuration and DMA behaviour are
// not yet established. Do not infer a bus reset cause from an unused slot.
// SMMUv2 candidate register window. Inspect only; a matching SMR and TYPE
// do not establish a working USB path. S2CR TYPE is [17:16], CBNDX [7:0].
#define STOCK_SMMU_BASE   0x15000000U
#define STOCK_USB_SID     0x40U

STATIC VOID
StockSmmuInspect (VOID)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Space;
  UINT32 Idr0;
  UINT32 Idr1;
  UINT32 Slots;
  UINT32 Slot;
  UINT32 Smr;
  UINT32 Mask;
  UINT32 S2cr;
  UINT32 Hits = 0;

  StockDmaCaptureHandoff ();
  if (gDS == NULL || gDS->GetMemorySpaceDescriptor == NULL) return;
  if (EFI_ERROR (gDS->GetMemorySpaceDescriptor (STOCK_SMMU_BASE, &Space))) {
    StockSay ("[USB] smmu unmapped, skip\r\n");
    return;
  }
  // ARM SMMU-500: sCR0 +0x000, IDR0 +0x020, IDR1 +0x024, GFSR +0x048,
  // SMR(n) at +0x800+n*4 and S2CR(n) at +0xC00+n*4 - four byte stride, and
  // IDR0[7:0] is the slot count. b243 used a 0x20 stride and read sCR0 as IDR0,
  // which walked off the array at 0x9C0 and took a synchronous external abort.
  Idr0 = MmioRead32 (STOCK_SMMU_BASE + 0x020);
  Idr1 = MmioRead32 (STOCK_SMMU_BASE + 0x024);
  Slots = Idr0 & 0xFFU;
  StockSay ("[USB] smmu sCR0=%08x IDR0=%08x IDR1=%08x GFSR=%08x slots=%u\r\n",
    MmioRead32 (STOCK_SMMU_BASE + 0x000), Idr0, Idr1,
    MmioRead32 (STOCK_SMMU_BASE + 0x048), Slots);
  if (Slots == 0 || Slots > 128) {
    StockSay ("[USB] smmu slot count unusable, skip\r\n");
    return;
  }
  for (Slot = 0; Slot < Slots; Slot++) {
    Smr = MmioRead32 (STOCK_SMMU_BASE + 0x800 + Slot * 4);
    if ((Smr & BIT31) == 0) continue;          // bit31 = VALID
    Mask = (Smr >> 16) & 0x7FFFU;
    if ((((Smr & 0x7FFFU) ^ STOCK_USB_SID) & ~Mask) != 0) continue;
    S2cr = MmioRead32 (STOCK_SMMU_BASE + 0xC00 + Slot * 4);
    StockSay ("[USB] smmu sid40 slot=%u SMR=%08x S2CR=%08x type=%u cb=%u\r\n",
      Slot, Smr, S2cr, (S2cr >> 16) & 3U, S2cr & 0xFFU);
    // No write: the old expression changed CBNDX instead of TYPE.
    Hits++;
  }
  if (Hits == 0) StockSay ("[USB] smmu no VALID slot matches sid40\r\n");
}

// b255 called GetResetReason from ENABLE USB and it still faulted, so the problem
// is the call itself, not how early it runs. Drop the call permanently: a probe
// that can halt the machine is not worth the information. Keep only the
// LocateProtocol result and the header words, which are plain reads.
STATIC VOID
StockResetReason (VOID)
{
  EFI_GUID Guid = { 0xA022155A, 0x4828, 0x4535,
    { 0xA4, 0x99, 0x11, 0xF1, 0x52, 0x40, 0xB9, 0x1B } };
  VOID *Protocol = NULL;
  EFI_STATUS Status;
  UINT64 *Words;

  Status = gBS->LocateProtocol (&Guid, NULL, &Protocol);
  if (EFI_ERROR (Status) || Protocol == NULL) {
    StockSay ("[USB] reset reason protocol -> %r\r\n", Status);
    return;
  }
  Words = (UINT64 *)Protocol;
  StockSay ("[USB] reset reason rev=%lx w1=%lx w2=%lx\r\n", Words[0], Words[1], Words[2]);
}

EFI_STATUS EFIAPI
UsbStockReplayEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_GUID BringupGuid = USB_BRINGUP_PROTOCOL_GUID;
  (VOID)SystemTable;
  mImageHandle = ImageHandle;
  // Before BDS/FrontPage: if the GUI still comes up after this, the QR log stays
  // usable and the screen is no longer the only channel.
  StockSmmuInspect ();
  // Read-only: are the vendor USB enable registers already programmed? On a real
  // device ABL runs fastboot (itself USB device mode) and leaves them set, which
  // is exactly why no stock driver writes them. If any reads back zero here, that
  // is the missing piece rather than anything in the stock driver.
  {
    STATIC CONST UINTN Addrs[] = { 0x0A6F8808, 0x0A6F8828, 0x01FF1118, 0x01FF1108, 0x0A6F8804 };
    STATIC CONST CHAR8 *Names[] = { "qscratch_gen", "qscratch_ref", "tcsr_clkref2", "tcsr_clkref3", "qscratch_gdsc" };
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR Space;
    UINTN Index;
    for (Index = 0; Index < 5; Index++) {
      if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor (Addrs[Index], &Space))) {
        StockSay ("[USB] %a=%08x\r\n", Names[Index], MmioRead32 (Addrs[Index]));
      } else {
        StockSay ("[USB] %a unmapped\r\n", Names[Index]);
      }
    }
  }
  StockSay ("[USB] b319 ready: plug cable, then press ENABLE USB; last line names the dying step\r\n");
  // Revision 1 intentionally supplies no forced-FS operation: stock owns speed.
  return gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &BringupGuid, &mBringup,
             &gApplicationEventServiceProtocolGuid, &mApplicationService, NULL);
}
