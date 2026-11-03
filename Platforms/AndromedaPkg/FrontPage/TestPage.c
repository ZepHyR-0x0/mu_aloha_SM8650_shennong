/** @file
  FrontPage hardware test page.

  Copyright (c) 2026, ProjectMu shennong port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Protocol/SimpleWindowManager.h>
#include <Protocol/SimpleTextInEx.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MsUiThemeLib.h>
#include <UIToolKit/TouchGesture.h>
#include "FrontPage.h"
#include "HapticControl.h"
#include "HapticSlider.h"
#include "FrontPageEventService.h"
#include <Library/PlatformTestLib.h>

#define TS_TOGGLE  1
#define TS_BACK    2
#define TS_PLATFORM 3
#define TS_POWER 4
#define TS_FILES 5
#define TS_DISPLAY 6
#define TS_USB_HOST 7
#define TS_WLAN 8
#define TS_FLASH 9
#define TS_TEST_COUNT 8
STATIC UINT32 mHapticDurationMs = 500, mHapticPercent = 100;
STATIC PMIC_HAPTICS_MODE mHapticPageMode = PmicHapticsDirect;
STATIC CONST UINTN mTestActions[TS_TEST_COUNT+1] = {
  TS_TOGGLE, TS_PLATFORM, TS_POWER, TS_FILES, TS_DISPLAY, TS_USB_HOST, TS_WLAN, TS_FLASH, TS_BACK
};
STATIC CONST UINTN mNavigation[] = {
  TS_TOGGLE,TS_PLATFORM,TS_POWER,TS_FILES,TS_DISPLAY,TS_USB_HOST,TS_WLAN,TS_FLASH,TS_BACK
};

typedef struct {
  MS_SIMPLE_WINDOW_MANAGER_PROTOCOL  *Swm;
  EFI_HANDLE                         Window;
  EFI_ABSOLUTE_POINTER_PROTOCOL      *Pointer;
  EFI_IMAGE_OUTPUT                   Output;
  UIT_TOUCH_GESTURE                  Tap;
  UINTN                              Width;
  UINTN                              Height;
  UINTN                              Font;
  UINTN                              Left;
  UINTN                              Right;
  UINTN                              Top;
  UINTN                              Bottom;
  UINTN                              ButtonHeight;
  UINTN                              LineHeight;
  UINTN                              Focus;
  UINT64                             TouchX, TouchY;
  UINTN                              TouchPresses;
  BOOLEAN                            FingerDown;
  BOOLEAN                            PlatformSampled;
  PLATFORM_TEST_RESULT               Platform;
  PLATFORM_RPMB_RESULT               Rpmb;
  PLATFORM_FLASH_RESULT              Flash;
  USB_HOST_TEST_RESULT               UsbHost;
  WLAN_TEST_RESULT                   Wlan;
  WLAN_PCIE_TEST_RESULT              WlanPcie;
  BOOLEAN                           WlanPcieSampled;
  UINTN                              ResultPage;
  BOOLEAN                            HapticEnabled;
  EFI_STATUS                         LastStatus;
  CHAR16                             Message[128];
} TEST_PAGE;

STATIC EFI_GUID  mTestWindowGuid = {
  0x7C0B17D4, 0x6E67, 0x4F18, { 0x9A, 0x3E, 0xA2, 0x95, 0x8E, 0x67, 0x42, 0x11 }
};

STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mTestBg       = { 20, 20, 20, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mTestFg       = { 240, 240, 240, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mTestButton   = { 45, 45, 45, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mTestSelected = { 120, 70, 20, 0 };

STATIC
VOID
TsFill (
  TEST_PAGE                       *Page,
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Color,
  UINTN                           X,
  UINTN                           Y,
  UINTN                           Width,
  UINTN                           Height
  )
{
  Page->Swm->BltWindow (
                Page->Swm,
                Page->Window,
                Color,
                EfiBltVideoFill,
                0,
                0,
                X,
                Y,
                Width,
                Height,
                0
                );
}

STATIC
VOID
TsText (
  TEST_PAGE                       *Page,
  CHAR16                          *Text,
  UINTN                           X,
  UINTN                           Y,
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Background
  )
{
  EFI_FONT_DISPLAY_INFO  Font;
  EFI_IMAGE_OUTPUT       *Output;

  ZeroMem (&Font, sizeof (Font));
  Output = &Page->Output;
  Font.FontInfoMask = EFI_FONT_INFO_ANY_FONT;
  Font.FontInfo.FontSize = (UINT16)Page->Font;
  Font.ForegroundColor = mTestFg;
  Font.BackgroundColor = *Background;
  Page->Swm->StringToWindow (
               Page->Swm,
               Page->Window,
               EFI_HII_OUT_FLAG_CLIP | EFI_HII_OUT_FLAG_CLIP_CLEAN_X |
               EFI_HII_OUT_FLAG_CLIP_CLEAN_Y | EFI_HII_IGNORE_LINE_BREAK |
               EFI_HII_DIRECT_TO_SCREEN,
               Text,
               &Font,
               &Output,
               X,
               Y,
               NULL,
               NULL,
               NULL
               );
}

STATIC UINTN ButtonTop (TEST_PAGE *Page, UINTN Index)
{
  return Page->Top + Page->LineHeight * 3 + Index * (Page->ButtonHeight + 16);
}
STATIC VOID TsPaint (TEST_PAGE *Page)
{
  CHAR16 Line[160];
  CONST CHAR16 *Names[TS_TEST_COUNT] = {L"Vibration: duration / intensity / mode",
    L"Platform: ACPI / SMC / TZ / RPMB read", L"Power / battery readings", L"Storage: UFS / USB read-only files",
    L"Display colors / touch / brightness status", L"USB host / U-disk read check", L"Devices: WiFi / Bluetooth / Gyro / GPS",
    L"Flash: short 50 mA pulse (100 ms)"};
  UINTN Index;
  UINTN Width = Page->Right - Page->Left;
  UINTN StatusY = ButtonTop (Page, TS_TEST_COUNT) + 16;
  TsFill (Page, &mTestBg, 0, 0, Page->Width, Page->Height);
  TsText (Page, L"TEST", Page->Left, Page->Top, &mTestBg);
  TsText (Page, L"Select a test. Results reflect this boot.", Page->Left, Page->Top + Page->LineHeight, &mTestBg);
  for (Index = 0; Index < TS_TEST_COUNT; Index++) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color = Page->Focus == mTestActions[Index] ? &mTestSelected : &mTestButton;
    UINTN Y = ButtonTop (Page, Index);
    TsFill (Page, Color, Page->Left, Y, Width, Page->ButtonHeight);
    TsText (Page, (CHAR16 *)Names[Index], Page->Left+24, Y+16, Color);
  }
  UnicodeSPrint (Line, sizeof (Line), L"Last action: %r", Page->LastStatus);
  TsText (Page, Line, Page->Left, StatusY, &mTestBg);
  TsText (Page, Page->Message, Page->Left, StatusY+Page->LineHeight, &mTestBg);
  UnicodeSPrint (Line, sizeof (Line), L"Touch: X=%lu Y=%lu presses=%u", Page->TouchX, Page->TouchY, Page->TouchPresses);
  TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*2, &mTestBg);
  if (Page->ResultPage == TS_USB_HOST) {
    UnicodeSPrint (Line, sizeof (Line), L"Host: %r    Disk read: %r", Page->UsbHost.HostStatus, Page->UsbHost.StorageStatus);
    TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*3, &mTestBg);
    UnicodeSPrint (Line, sizeof (Line), L"Host=%u USB interfaces=%u Block=%u", Page->UsbHost.HostHandles,
      Page->UsbHost.DeviceHandles, Page->UsbHost.BlockHandles);
    TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*4, &mTestBg);
    UnicodeSPrint (Line, sizeof (Line), L"Mounted=%u Root reads passed=%u", Page->UsbHost.FileSystemHandles, Page->UsbHost.ReadableVolumes);
    TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*5, &mTestBg);
    TsText (Page, Page->UsbHost.HostHandles == 0 ? L"Host initialization is not available yet." :
      L"Open Storage to browse readable volumes.", Page->Left, StatusY+Page->LineHeight*6, &mTestBg);
  } else if (Page->ResultPage == TS_FLASH) {
    UnicodeSPrint(Line,sizeof(Line),L"Flash: %r  started=%u restored=%u",Page->Flash.Status,Page->Flash.Started,Page->Flash.Restored);
    TsText(Page,Line,Page->Left,StatusY+Page->LineHeight*3,&mTestBg);
  } else if (Page->ResultPage == TS_WLAN) {
    UnicodeSPrint (Line, sizeof (Line), L"WiFi provider: %r    Root bridges=%u", Page->Wlan.WifiStatus, Page->Wlan.RootBridges);
    TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*3, &mTestBg);
    UnicodeSPrint (Line, sizeof (Line), L"PCIe stage=%u error=%u detail=%x device=%08x", Page->WlanPcie.Stage,
      Page->WlanPcie.Error, Page->WlanPcie.Detail, Page->WlanPcie.Device);
    TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*4, &mTestBg);
    UnicodeSPrint (Line, sizeof (Line), L"Cleanup=%r retained rails=%x PIPE=%uHz", Page->WlanPcie.CleanupStatus,
      Page->WlanPcie.RetainedRails, Page->WlanPcie.PipeHz);
    TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*5, &mTestBg);
    TsText (Page, L"Scan / connection / traffic are not tested.", Page->Left, StatusY+Page->LineHeight*6, &mTestBg);
  } else if (Page->PlatformSampled) {
    UnicodeSPrint (Line, sizeof (Line), L"ACPI: %r    SMC transport: %r", Page->Platform.AcpiStatus, Page->Platform.ScmStatus);
    TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*3, &mTestBg);
    UnicodeSPrint (Line, sizeof (Line), L"PIL: %r    ShmBridge handles: %u", Page->Platform.PilStatus, Page->Platform.ShmHandles);
    TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*4, &mTestBg);
    UnicodeSPrint (Line, sizeof (Line), L"USB=%u  Touch=%u  GOP=%u  FS=%u  Block=%u", Page->Platform.UsbHandles,
      Page->Platform.TouchHandles, Page->Platform.DisplayHandles, Page->Platform.FileSystemHandles, Page->Platform.BlockHandles);
    TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*5, &mTestBg);
    UnicodeSPrint(Line,sizeof(Line),L"RPMB: %r frames=%u counter=%u/%u",Page->Rpmb.Status,Page->Rpmb.Frames,
      Page->Rpmb.CounterBefore,Page->Rpmb.CounterAfter);
    TsText (Page, Line, Page->Left, StatusY+Page->LineHeight*6, &mTestBg);
  } else {
    TsText (Page, L"Run Platform to read service status.", Page->Left, StatusY+Page->LineHeight*3, &mTestBg);
  }
  if (Page->PlatformSampled && Page->ResultPage == TS_PLATFORM) {
    UnicodeSPrint (Line,sizeof(Line),L"ACPI links: %r  tables=%u  enabled CPUs=%u",Page->Platform.AcpiLinksStatus,
      Page->Platform.AcpiTables,Page->Platform.AcpiEnabledCpus);
    TsText (Page,Line,Page->Left,StatusY+Page->LineHeight*7,&mTestBg);
  } else TsText (Page, L"Confirm hardware action separately.",
    Page->Left, StatusY+Page->LineHeight*7, &mTestBg);
  TsFill (Page, Page->Focus == TS_BACK ? &mTestSelected : &mTestButton,
    Page->Left, Page->Bottom, Width, Page->ButtonHeight);
  TsText (Page, L"Back", Page->Left+24, Page->Bottom+16,
    Page->Focus == TS_BACK ? &mTestSelected : &mTestButton);
}
STATIC VOID TsDisplayTest (TEST_PAGE *Page, EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Keys)
{
  STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL Colors[6] = {
    {0,0,255,0}, {0,255,0,0}, {255,0,0,0}, {255,255,255,0}, {128,128,128,0}, {0,0,0,0}
  };
  EFI_KEY_DATA Key;
  BOOLEAN Exit = FALSE;
  BOOLEAN Dirty = TRUE;
  UINTN X = Page->Width/2;
  UINTN Y = Page->Height/2;
  UINTN Presses = 0;
  BOOLEAN Down = FALSE;
  while (!Exit) {
    EFI_ABSOLUTE_POINTER_STATE Touch;
    if (Dirty) {
      UINTN Index;
      CHAR16 Line[160];
      for (Index = 0; Index < 6; Index++) {
        UINTN Left = Page->Width*Index/6;
        UINTN Right = Page->Width*(Index+1)/6;
        TsFill (Page, &Colors[Index], Left, 0, Right-Left, Page->Height);
      }
      TsFill (Page, &mTestBg, 0, 0, Page->Width, Page->Top+Page->LineHeight*2);
      TsText (Page, L"DISPLAY / TOUCH", Page->Left, Page->Top, &mTestBg);
      UnicodeSPrint (Line, sizeof (Line), L"X=%u  Y=%u  presses=%u. Esc or Back to return.", X, Y, Presses);
      TsText (Page, Line, Page->Left, Page->Top+Page->LineHeight, &mTestBg);
      if (X < Page->Width && Y < Page->Height) {
        UINTN Left = X>12 ? X-12 : 0;
        UINTN Top = Y>12 ? Y-12 : 0;
        TsFill (Page, &mTestSelected, Left, Y,
          MIN ((UINTN)25, Page->Width-Left), MIN ((UINTN)3, Page->Height-Y));
        TsFill (Page, &mTestSelected, X, Top,
          MIN ((UINTN)3, Page->Width-X), MIN ((UINTN)25, Page->Height-Top));
      }
      TsFill (Page, &mTestButton, Page->Left, Page->Bottom, Page->Right-Page->Left, Page->ButtonHeight);
      TsText (Page, L"Back", Page->Left+24, Page->Bottom+16, &mTestButton);
      Dirty = FALSE;
    }
    if (Keys != NULL && Keys->ReadKeyStrokeEx (Keys, &Key) == EFI_SUCCESS &&
        (Key.Key.ScanCode == SCAN_ESC || Key.Key.UnicodeChar == CHAR_BACKSPACE)) Exit = TRUE;
    if (Page->Pointer->GetState (Page->Pointer, &Touch) == EFI_SUCCESS) {
      BOOLEAN Finger = SWM_IS_FINGER_DOWN (Touch);
      X = (UINTN)Touch.CurrentX;
      Y = (UINTN)Touch.CurrentY;
      if (Finger && !Down) Presses++;
      if (!Finger && Down && X >= Page->Left && X < Page->Right &&
          Y >= Page->Bottom && Y < Page->Bottom+Page->ButtonHeight) Exit = TRUE;
      Down = Finger;
      Dirty = TRUE;
    }
    FpServiceEvents ();
    gBS->Stall (20000);
  }
  ZeroMem (&Page->Tap, sizeof (Page->Tap));
}

STATIC VOID EFIAPI TsPlatformOutput (CONST CHAR8 *Line, VOID *Context)
{
  (VOID)Line;
  (VOID)Context;
  FpServiceEvents ();
}

STATIC VOID TsHapticSlider (TEST_PAGE *Page, UINTN Y, UINT32 Value, UINT32 Maximum, BOOLEAN Focused)
{
  UINTN Left=Page->Left+24, Right=Page->Right-24;
  UINTN Knob=Left+(Right-Left)*Value/Maximum, Radius=MAX((UINTN)12,Page->Font/3);
  UINTN Center=Y+Page->ButtonHeight/2;
  TsFill(Page,&mTestButton,Left,Center-6,Right-Left,12);
  if(Knob>Left) TsFill(Page,&mTestSelected,Left,Center-6,Knob-Left,12);
  TsFill(Page,Focused?&mTestFg:&mTestSelected,Knob-Radius,Center-Radius,Radius*2,Radius*2);
}

STATIC VOID TsHapticPage (TEST_PAGE *Page, EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Keys, EFI_EVENT PaintEvent)
{
  UINT32 Duration=mHapticDurationMs, Percent=mHapticPercent;
  PMIC_HAPTICS_MODE Mode=mHapticPageMode;
  UINTN Focus=1, Drag=0, Y[6], I;
  BOOLEAN Exit=FALSE, Dirty=TRUE, Finger=FALSE;
  UIT_TOUCH_GESTURE Tap;
  CONST CHAR16 *Modes[]={L"Native duty",L"Direct",L"Auto resonance"};
  CHAR16 Line[160];
  Y[0]=Page->Top+Page->LineHeight*3;
  Y[1]=Y[0]+Page->ButtonHeight+Page->LineHeight*2;
  Y[2]=Y[1]+Page->ButtonHeight+Page->LineHeight;
  Y[3]=Y[2]+Page->ButtonHeight+16;
  Y[4]=Y[3]+Page->ButtonHeight+16;
  Y[5]=Page->Bottom;
  if(Y[4]+Page->ButtonHeight+Page->LineHeight*3>=Page->Bottom) {
    Page->LastStatus=EFI_UNSUPPORTED;return;
  }
  ZeroMem(&Tap,sizeof(Tap));
  Page->LastStatus=FpHapticStop();
  if(EFI_ERROR(Page->LastStatus)) return;
  Page->LastStatus=FpHapticConfigureLevel(Duration*1000,Percent,Mode);
  {
    CHAR8 Layout[192];
    UINTN N=AsciiSPrint(Layout,sizeof(Layout),"[HAPTIC UI LAYOUT] page=open width=%u height=%u duration_y=%u intensity_y=%u mode_y=%u play_y=%u stop_y=%u back_y=%u\r\n",
      (UINT32)Page->Width,(UINT32)Page->Height,(UINT32)Y[0],(UINT32)Y[1],(UINT32)Y[2],(UINT32)Y[3],(UINT32)Y[4],(UINT32)Y[5]);
    SerialPortWrite((UINT8 *)Layout,N);
  }
  while(!Exit) {
    UINTN Action=0;
    BOOLEAN Commit=FALSE;
    EFI_KEY_DATA Key;
    if(FpHapticIsEnabled()) {
      EFI_STATUS Status=FpHapticPoll();
      if(EFI_ERROR(Status) || !FpHapticIsEnabled()) {Page->LastStatus=Status;Dirty=TRUE;}
    }
    if(Dirty || gBS->CheckEvent(PaintEvent)==EFI_SUCCESS) {
      TsFill(Page,&mTestBg,0,0,Page->Width,Page->Height);
      TsText(Page,L"VIBRATION",Page->Left,Page->Top,&mTestBg);
      TsText(Page,L"Drag to adjust. Stop before changing settings.",Page->Left,Page->Top+Page->LineHeight,&mTestBg);
      UnicodeSPrint(Line,sizeof(Line),L"Duration: %u ms",Duration);
      TsText(Page,Line,Page->Left,Y[0]-Page->LineHeight,&mTestBg);
      TsHapticSlider(Page,Y[0],(Duration-50)/10,95,Focus==1);
      UnicodeSPrint(Line,sizeof(Line),L"Intensity: %u%%%s",Percent,Percent==0?L" (muted)":L"");
      TsText(Page,Line,Page->Left,Y[1]-Page->LineHeight,&mTestBg);
      TsHapticSlider(Page,Y[1],Percent,100,Focus==2);
      for(I=2;I<6;I++) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color=Focus==I+1?&mTestSelected:&mTestButton;
        TsFill(Page,Color,Page->Left,Y[I],Page->Right-Page->Left,Page->ButtonHeight);
        if(I==2) UnicodeSPrint(Line,sizeof(Line),L"Mode: %s",Modes[Mode]);
        else StrCpyS(Line,ARRAY_SIZE(Line),I==3?L"Play once":I==4?L"Stop":L"Back");
        TsText(Page,Line,Page->Left+24,Y[I]+16,Color);
      }
      UnicodeSPrint(Line,sizeof(Line),L"%s  Last action: %r",FpHapticIsEnabled()?L"Playing":L"Stopped",Page->LastStatus);
      TsText(Page,Line,Page->Left,Y[4]+Page->ButtonHeight+Page->LineHeight,&mTestBg);
      TsText(Page,L"Keyboard: up/down to select, left/right to adjust.",Page->Left,Y[4]+Page->ButtonHeight+Page->LineHeight*2,&mTestBg);
      Dirty=FALSE;
    }
    if(Keys!=NULL && Keys->ReadKeyStrokeEx(Keys,&Key)==EFI_SUCCESS) {
      if(Key.Key.ScanCode==SCAN_ESC || Key.Key.UnicodeChar==CHAR_BACKSPACE) Action=6;
      else if(Key.Key.ScanCode==SCAN_UP) {Focus=Focus==1?6:Focus-1;Dirty=TRUE;}
      else if(Key.Key.ScanCode==SCAN_DOWN) {Focus=Focus==6?1:Focus+1;Dirty=TRUE;}
      else if(Key.Key.ScanCode==SCAN_LEFT || Key.Key.ScanCode==SCAN_RIGHT) {
        BOOLEAN Increase=Key.Key.ScanCode==SCAN_RIGHT;
        if(Focus==1) {Duration=Increase?MIN(Duration+10,1000):MAX(Duration,60)-10;Commit=TRUE;}
        if(Focus==2) {Percent=Increase?MIN(Percent+5,100):MAX(Percent,5)-5;Commit=TRUE;}
      } else if(Key.Key.UnicodeChar==CHAR_CARRIAGE_RETURN || Key.Key.UnicodeChar==L' ') Action=Focus;
      Drag=0;ZeroMem(&Tap,sizeof(Tap));
    }
    {
      EFI_ABSOLUTE_POINTER_STATE Touch;
      if(Page->Pointer->GetState(Page->Pointer,&Touch)==EFI_SUCCESS) {
        BOOLEAN Down=SWM_IS_FINGER_DOWN(Touch);
        UINTN Target=0;
        if(Touch.CurrentX>=Page->Left && Touch.CurrentX<Page->Right) {
          for(I=0;I<6;I++) if(Touch.CurrentY>=Y[I] && Touch.CurrentY<Y[I]+Page->ButtonHeight) Target=I+1;
        }
        if(Down && !Finger && Target>=1 && Target<=2) {
          Focus=Target;Dirty=TRUE;ZeroMem(&Tap,sizeof(Tap));
          if(FpHapticIsEnabled()) Page->LastStatus=EFI_ALREADY_STARTED;
          else Drag=Target;
        }
        if(Drag!=0) {
          if(Drag==1) Duration=50+10*HapticSliderUnits(Touch.CurrentX,Page->Left+24,Page->Right-24,95);
          else Percent=HapticSliderUnits(Touch.CurrentX,Page->Left+24,Page->Right-24,100);
          Dirty=TRUE;
          if(!Down) {Drag=0;Commit=TRUE;}
          ZeroMem(&Tap,sizeof(Tap));
        } else if(Target>=3 || !Down) {
          UIT_TAP_RESULT Result=UitTouchTap(&Tap,Down,Target,Touch.CurrentX,Touch.CurrentY,
            GetTimeInNanoSecond(GetPerformanceCounter())/1000000,MAX((UINTN)24,(Page->Right-Page->Left)/50));
          if(Target>=3 && (Result==UitTapFocus || Result==UitTapConfirm)) {Action=Focus=Target;ZeroMem(&Tap,sizeof(Tap));}
        }
        Finger=Down;
      }
    }
    if(Action==3) {Mode=(PMIC_HAPTICS_MODE)((Mode+1)%PmicHapticsModeCount);Commit=TRUE;}
    if(Commit) {
      Page->LastStatus=FpHapticConfigureLevel(Duration*1000,Percent,Mode);
      if(!EFI_ERROR(Page->LastStatus)) {mHapticDurationMs=Duration;mHapticPercent=Percent;mHapticPageMode=Mode;}
      else {Duration=mHapticDurationMs;Percent=mHapticPercent;Mode=mHapticPageMode;}
      Dirty=TRUE;
    }
    if(Action==4) {Page->LastStatus=FpHapticStart();Dirty=TRUE;}
    if(Action==5 || Action==6) {
      Page->LastStatus=FpHapticStop();Dirty=TRUE;
      if(Action==6 && !EFI_ERROR(Page->LastStatus)) Exit=TRUE;
    }
    FpServiceEvents();gBS->Stall(20000);
  }
  Page->HapticEnabled=FpHapticIsEnabled();
  ZeroMem(&Page->Tap,sizeof(Page->Tap));
}

// List paging uses copies from the resident survey. No callback points here.
STATIC VOID TsDevicePage (TEST_PAGE *Page, EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Keys, EFI_EVENT PaintEvent)
{
  WIRELESS_SURVEY_RESULT *Radio=AllocateZeroPool(sizeof(*Radio));
  PLATFORM_SENSOR_RESULT Sensors;
  EFI_STATUS Last=EFI_NOT_READY;
  EFI_KEY_DATA Key;
  UINTN Focus=1,Mode=0,First=0,Generation=MAX_UINT32;
  BOOLEAN Exit=FALSE,Dirty=TRUE;
  UIT_TOUCH_GESTURE Tap;
  CONST CHAR16 *Names[]={L"WiFi scan",L"Bluetooth list",L"Gyro / GPS",L"WLAN PCIe link"};
  if(Radio==NULL) return;
  ZeroMem(&Sensors,sizeof(Sensors));ZeroMem(&Tap,sizeof(Tap));
  PlatformWirelessGetResult(Radio);
  while(!Exit) {
    UINTN Action=0,I,Width=Page->Right-Page->Left,Half=Width/2;
    UINTN ControlsY=Page->Top+Page->LineHeight*2;
    UINTN ResultsY=ControlsY+2*(Page->ButtonHeight+16)+Page->LineHeight;
    UINTN PagingY=Page->Bottom-Page->ButtonHeight-16;
    UINTN Rows=MIN((UINTN)8,(PagingY>ResultsY+Page->LineHeight*3)?(PagingY-ResultsY)/Page->LineHeight-3:0);
    CHAR16 Line[160];
    PlatformWirelessGetResult(Radio);
    if(Generation!=Radio->Generation) {Generation=Radio->Generation;Dirty=TRUE;}
    if(Dirty || gBS->CheckEvent(PaintEvent)==EFI_SUCCESS) {
      TsFill(Page,&mTestBg,0,0,Page->Width,Page->Height);
      TsText(Page,L"DEVICES",Page->Left,Page->Top,&mTestBg);
      TsText(Page,L"Select a device test. Lists update in this boot.",Page->Left,Page->Top+Page->LineHeight,&mTestBg);
      for(I=0;I<4;I++) {
        UINTN X=Page->Left+(I%2)*(Half+8),Y=ControlsY+(I/2)*(Page->ButtonHeight+16);
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color=Focus==I+1?&mTestSelected:&mTestButton;
        TsFill(Page,Color,X,Y,Half-8,Page->ButtonHeight);
        TsText(Page,(CHAR16 *)Names[I],X+16,Y+16,Color);
      }
      UnicodeSPrint(Line,sizeof(Line),L"Last action: %r",Last);
      TsText(Page,Line,Page->Left,ResultsY,&mTestBg);
      if(Mode==1) {
        UnicodeSPrint(Line,sizeof(Line),L"WiFi: %r  scanning=%u  networks=%u",Radio->WifiStatus,Radio->WifiPending,Radio->NetworkCount);
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight,&mTestBg);
        UnicodeSPrint(Line,sizeof(Line),L"Providers: WiFi2=%u  legacy=%u%s",Radio->WifiProviders,Radio->LegacyWifiProviders,
          Radio->WifiTruncated?L"  (list capped)":L"");
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight*2,&mTestBg);
        for(I=0;I<Rows && First+I<MIN(Radio->NetworkCount,WIRELESS_SURVEY_MAX);I++) {
          WIRELESS_NETWORK *N=&Radio->Networks[First+I];
          UnicodeSPrint(Line,sizeof(Line),L"%u. %s  quality=%u/100",First+I+1,N->SsidLength?N->Name:L"<hidden>",N->Quality);
          TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight*(I+3),&mTestBg);
        }
        if(Radio->NetworkCount==0) TsText(Page,Radio->WifiPending?L"Waiting for the scan result...":
          Radio->WifiProviders==0?L"WiFi scan driver is unavailable.":L"No networks returned.",Page->Left,ResultsY+Page->LineHeight*3,&mTestBg);
      } else if(Mode==2) {
        UnicodeSPrint(Line,sizeof(Line),L"Bluetooth cache: %r  devices=%u",Radio->BluetoothStatus,Radio->DeviceCount);
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight,&mTestBg);
        UnicodeSPrint(Line,sizeof(Line),L"Providers: classic=%u BLE=%u%s",Radio->BluetoothProviders,Radio->BleProviders,
          Radio->BluetoothTruncated?L"  (list capped)":L"");
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight*2,&mTestBg);
        for(I=0;I<Rows/2 && First+I<MIN(Radio->DeviceCount,WIRELESS_SURVEY_MAX);I++) {
          WIRELESS_DEVICE *D=&Radio->Devices[First+I];
          UnicodeSPrint(Line,sizeof(Line),L"%u. %s",First+I+1,D->Name[0]?D->Name:L"<unnamed>");
          TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight*(I*2+3),&mTestBg);
          UnicodeSPrint(Line,sizeof(Line),L"%02x:%02x:%02x:%02x:%02x:%02x  %s type=%u",D->Address[5],D->Address[4],
            D->Address[3],D->Address[2],D->Address[1],D->Address[0],D->LowEnergy?L"BLE":L"Classic",D->AddressType);
          TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight*(I*2+4),&mTestBg);
        }
        if(Radio->DeviceCount==0) TsText(Page,Radio->BluetoothProviders+Radio->BleProviders==0?
          L"Bluetooth driver is unavailable.":L"The provider cache contains no devices.",Page->Left,ResultsY+Page->LineHeight*3,&mTestBg);
      } else if(Mode==3) {
        UnicodeSPrint(Line,sizeof(Line),L"Gyroscope: %r   GPS fix: %r",Sensors.GyroStatus,Sensors.GpsStatus);
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight,&mTestBg);
        UnicodeSPrint(Line,sizeof(Line),L"ADSP MDT: %r  %u bytes",Sensors.AdspStatus,Sensors.AdspBytes);
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight*2,&mTestBg);
        UnicodeSPrint(Line,sizeof(Line),L"Sensor config: %r  %u bytes",Sensors.SensorConfigStatus,Sensors.SensorConfigBytes);
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight*3,&mTestBg);
        UnicodeSPrint(Line,sizeof(Line),L"Modem MDT: %r  %u bytes",Sensors.ModemStatus,Sensors.ModemBytes);
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight*4,&mTestBg);
        TsText(Page,L"Firmware reads are available prerequisites.",Page->Left,ResultsY+Page->LineHeight*5,&mTestBg);
        TsText(Page,L"Sensor samples and GPS fixes are unavailable.",Page->Left,ResultsY+Page->LineHeight*6,&mTestBg);
      } else if(Mode==4) {
        UnicodeSPrint(Line,sizeof(Line),L"WiFi provider: %r  PCIe endpoints=%u",Page->Wlan.WifiStatus,Page->Wlan.WlanEndpoints);
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight,&mTestBg);
        UnicodeSPrint(Line,sizeof(Line),L"PCIe stage=%u error=%u device=%08x",Page->WlanPcie.Stage,Page->WlanPcie.Error,Page->WlanPcie.Device);
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight*2,&mTestBg);
        UnicodeSPrint(Line,sizeof(Line),L"Cleanup=%r PIPE=%u Hz",Page->WlanPcie.CleanupStatus,Page->WlanPcie.PipeHz);
        TsText(Page,Line,Page->Left,ResultsY+Page->LineHeight*3,&mTestBg);
      } else TsText(Page,L"WiFi scan, Bluetooth cache, or sensor dependencies.",Page->Left,ResultsY+Page->LineHeight,&mTestBg);
      for(I=0;I<2;I++) {
        UINTN X=Page->Left+I*(Half+8);
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color=Focus==I+5?&mTestSelected:&mTestButton;
        TsFill(Page,Color,X,PagingY,Half-8,Page->ButtonHeight);
        TsText(Page,I?L"Next page":L"Previous page",X+16,PagingY+16,Color);
      }
      TsFill(Page,Focus==7?&mTestSelected:&mTestButton,Page->Left,Page->Bottom,Width,Page->ButtonHeight);
      TsText(Page,L"Back",Page->Left+24,Page->Bottom+16,Focus==7?&mTestSelected:&mTestButton);
      Dirty=FALSE;
    }
    if(Keys!=NULL && Keys->ReadKeyStrokeEx(Keys,&Key)==EFI_SUCCESS) {
      if(Key.Key.ScanCode==SCAN_ESC || Key.Key.UnicodeChar==CHAR_BACKSPACE) Action=7;
      else if(Key.Key.ScanCode==SCAN_UP) {Focus=Focus==1?7:Focus-1;Dirty=TRUE;}
      else if(Key.Key.ScanCode==SCAN_DOWN) {Focus=Focus==7?1:Focus+1;Dirty=TRUE;}
      else if(Key.Key.UnicodeChar==CHAR_CARRIAGE_RETURN || Key.Key.UnicodeChar==L' ') Action=Focus;
      ZeroMem(&Tap,sizeof(Tap));
    }
    {
      EFI_ABSOLUTE_POINTER_STATE Touch;
      if(Page->Pointer->GetState(Page->Pointer,&Touch)==EFI_SUCCESS) {
        UINTN Target=0;UIT_TAP_RESULT TapResult;
        if(Touch.CurrentX>=Page->Left && Touch.CurrentX<Page->Right) {
          UINTN Column=Touch.CurrentX<Page->Left+Half?0:1;
          for(I=0;I<2;I++) {
            UINTN Y=ControlsY+I*(Page->ButtonHeight+16);
            if(Touch.CurrentY>=Y && Touch.CurrentY<Y+Page->ButtonHeight) Target=I*2+Column+1;
          }
          if(Touch.CurrentY>=PagingY && Touch.CurrentY<PagingY+Page->ButtonHeight) Target=5+Column;
          if(Touch.CurrentY>=Page->Bottom && Touch.CurrentY<Page->Bottom+Page->ButtonHeight) Target=7;
        }
        TapResult=UitTouchTap(&Tap,SWM_IS_FINGER_DOWN(Touch),Target,Touch.CurrentX,Touch.CurrentY,
          GetTimeInNanoSecond(GetPerformanceCounter())/1000000,MAX((UINTN)24,Width/50));
        if(Target!=0 && (TapResult==UitTapFocus || TapResult==UitTapConfirm)) {Action=Focus=Target;ZeroMem(&Tap,sizeof(Tap));}
      }
    }
    if(Action>=1 && Action<=4) {
      Mode=Action;First=0;
      if(Action==1) Last=PlatformWirelessScan();
      else if(Action==2) Last=PlatformBluetoothRefresh();
      else if(Action==3) Last=PlatformSensorTest(TsPlatformOutput,Page,&Sensors);
      else {
        Last=PlatformWlanTest(TsPlatformOutput,Page,&Page->Wlan);
        if(Page->Wlan.RootBridges==0 && Page->Wlan.WifiHandles==0) {
          Last=PlatformWlanPcieTest(TsPlatformOutput,Page,&Page->WlanPcie);Page->WlanPcieSampled=TRUE;
        }
      }
      PlatformWirelessTest(TsPlatformOutput,Page,Radio);Dirty=TRUE;
    } else if(Action==5 || Action==6) {
      UINTN Step=Mode==2?MAX((UINTN)1,Rows/2):MAX((UINTN)1,Rows);
      UINTN Count=Mode==2?Radio->DeviceCount:Radio->NetworkCount;
      if(Action==5) First=First>=Step?First-Step:0;
      else if(First+Step<Count) First+=Step;
      Dirty=TRUE;
    } else if(Action==7) Exit=TRUE;
    FpServiceEvents();gBS->Stall(20000);
  }
  FreePool(Radio);ZeroMem(&Page->Tap,sizeof(Page->Tap));
}

STATIC
BOOLEAN
EFIAPI
TsNotify (
  IN VOID  *Context
  )
{
  return TRUE;
}

STATIC
UINTN
TsTouch (
  TEST_PAGE                    *Page,
  EFI_ABSOLUTE_POINTER_STATE  *Touch,
  BOOLEAN                     *Dirty
  )
{
  UIT_TAP_RESULT  Result;
  UINTN           Target;
  UINTN           Width;

  Target = 0;
  Width = Page->Right - Page->Left;
  Page->TouchX = Touch->CurrentX;
  Page->TouchY = Touch->CurrentY;
  if (SWM_IS_FINGER_DOWN (*Touch) && !Page->FingerDown) Page->TouchPresses++;
  Page->FingerDown = SWM_IS_FINGER_DOWN (*Touch);
  *Dirty = TRUE;
  if (Touch->CurrentX >= Page->Left && Touch->CurrentX < Page->Right) {
    UINTN Index;
    for (Index = 0; Index < TS_TEST_COUNT; Index++) {
      UINTN Y = ButtonTop (Page, Index);
      if (Touch->CurrentY >= Y && Touch->CurrentY < Y+Page->ButtonHeight) Target = mTestActions[Index];
    }
    if (Touch->CurrentY >= Page->Bottom && Touch->CurrentY < Page->Bottom+Page->ButtonHeight) Target = TS_BACK;
  }

  Result = UitTouchTap (
                       &Page->Tap,
                       SWM_IS_FINGER_DOWN (*Touch),
                       Target,
                       Touch->CurrentX,
                       Touch->CurrentY,
                       GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000000,
                       MAX ((UINTN)24, Width / 50)
                       );
  if ((Result == UitTapFocus) || (Result == UitTapConfirm)) {
    *Dirty = TRUE;
    if (Target != 0) {
      Page->Focus = Target;
      ZeroMem (&Page->Tap, sizeof (Page->Tap));
      return Target;
    }
  }
  return 0;
}

VOID
FpTestPage (
  MS_SIMPLE_WINDOW_MANAGER_PROTOCOL  *Swm,
  EFI_GRAPHICS_OUTPUT_PROTOCOL       *Gop
  )
{
  TEST_PAGE                     *Page;
  EFI_STATUS                    Status;
  EFI_STATUS                    StopStatus;
  EFI_EVENT                     PaintEvent;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *Keys;
  EFI_KEY_DATA                  Key;
  SWM_RECT                      Frame;
  BOOLEAN                       Exit;
  BOOLEAN                       Dirty;

  if ((Swm == NULL) || (Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    return;
  }

  Page = AllocateZeroPool (sizeof (*Page));
  if (Page == NULL) {
    return;
  }

  Page->Swm = Swm;
  Page->Width = Gop->Mode->Info->HorizontalResolution;
  Page->Height = Gop->Mode->Info->VerticalResolution;
  // Keep service results readable within the portrait screen's content width.
  Page->Font = MsUiGetStandardFontHeight ();
  Page->Left = MAX ((UINTN)32, Page->Width / 10);
  Page->Right = Page->Width - Page->Left;
  Page->Top = MAX ((UINTN)80, Page->Height / 20);
  Page->ButtonHeight = Page->Font + 40;
  Page->LineHeight = Page->Font + 30;
  Page->Bottom = Page->Height > Page->Top + Page->ButtonHeight ?
                 Page->Height - Page->Top - Page->ButtonHeight : 0;
  Page->HapticEnabled = FpHapticIsEnabled ();
  Page->Focus = TS_TOGGLE;
  Page->LastStatus = EFI_SUCCESS;
  StrCpyS (Page->Message, ARRAY_SIZE (Page->Message), L"Ready.");

  if ((Page->Font == 0) || (Page->Width < 640) || (Page->Bottom < ButtonTop (Page, TS_TEST_COUNT) + 16 + Page->LineHeight * 8)) {
    FreePool (Page);
    return;
  }

  Page->Output.Width = (UINT16)Page->Width;
  Page->Output.Height = (UINT16)Page->Height;
  Page->Output.Image.Screen = Gop;
  SWM_RECT_INIT2 (Frame, 0, 0, Page->Width, Page->Height);

  Status = gBS->InstallProtocolInterface (
                  &Page->Window,
                  &mTestWindowGuid,
                  EFI_NATIVE_INTERFACE,
                  Page
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Page);
    return;
  }

  Status = Swm->RegisterClient (
                    Swm,
                    Page->Window,
                    SWM_Z_ORDER_POPUP,
                    &Frame,
                    TsNotify,
                    Page,
                    &Page->Pointer,
                    &PaintEvent
                    );
  if (EFI_ERROR (Status) || (Page->Pointer == NULL)) {
    gBS->UninstallProtocolInterface (Page->Window, &mTestWindowGuid, Page);
    FreePool (Page);
    return;
  }

  Swm->ActivateWindow (Swm, Page->Window, TRUE);
  Keys = NULL;
  if (gST->ConsoleInHandle != NULL) {
    gBS->HandleProtocol (
           gST->ConsoleInHandle,
           &gEfiSimpleTextInputExProtocolGuid,
           (VOID **)&Keys
           );
  }

  Exit = FALSE;
  Dirty = TRUE;
  while (!Exit) {
    EFI_ABSOLUTE_POINTER_STATE  Touch;
    UINTN                       Action;

    Action = 0;
    if (FpHapticIsEnabled ()) {
      Status = FpHapticPoll ();
      if (EFI_ERROR (Status) || !FpHapticIsEnabled ()) {
        Page->LastStatus = Status;
        Page->HapticEnabled = FpHapticIsEnabled ();
        StrCpyS (Page->Message, ARRAY_SIZE (Page->Message),
          EFI_ERROR (Status) ? L"Automatic stop failed; retry Stop." : L"Stopped after the selected duration.");
        Dirty = TRUE;
      }
    }
    if (Dirty || (gBS->CheckEvent (PaintEvent) == EFI_SUCCESS)) {
      TsPaint (Page);
      Dirty = FALSE;
    }

    if ((Keys != NULL) && (Keys->ReadKeyStrokeEx (Keys, &Key) == EFI_SUCCESS)) {
      if ((Key.Key.ScanCode == SCAN_ESC) || (Key.Key.UnicodeChar == CHAR_BACKSPACE)) {
        Action = TS_BACK;
      } else if ((Key.Key.UnicodeChar == CHAR_CARRIAGE_RETURN) ||
                 (Key.Key.UnicodeChar == L' '))
      {
        Action = Page->Focus;
      }
      if (Key.Key.ScanCode == SCAN_UP || Key.Key.ScanCode == SCAN_DOWN) {
        UINTN Index;
        for (Index = 0; Index < ARRAY_SIZE (mNavigation); Index++) if (mNavigation[Index] == Page->Focus) break;
        Index = Key.Key.ScanCode == SCAN_UP ? (Index+ARRAY_SIZE(mNavigation)-1)%ARRAY_SIZE(mNavigation) : (Index+1)%ARRAY_SIZE(mNavigation);
        Page->Focus = mNavigation[Index];
        Dirty = TRUE;
      }
      ZeroMem (&Page->Tap, sizeof (Page->Tap));
    }

    if (Page->Pointer->GetState (Page->Pointer, &Touch) == EFI_SUCCESS) {
      UINTN  TouchAction;

      TouchAction = TsTouch (Page, &Touch, &Dirty);
      if (TouchAction != 0) {
        Action = TouchAction;
      }
    }

    if (Action == TS_TOGGLE) {
      TsHapticPage(Page,Keys,PaintEvent);
      StrCpyS(Page->Message,ARRAY_SIZE(Page->Message),L"Vibration details: CDC logcat.");
      Dirty=TRUE;
    } else if (Action == TS_PLATFORM || Action == TS_POWER || Action == TS_FILES || Action == TS_DISPLAY ||
               Action == TS_USB_HOST || Action == TS_WLAN || Action == TS_FLASH) {
      Page->LastStatus = FpHapticStop ();
      Page->HapticEnabled = FpHapticIsEnabled ();
      if (EFI_ERROR (Page->LastStatus)) {
        StrCpyS (Page->Message, ARRAY_SIZE (Page->Message), L"Stop failed; retry before another test.");
      } else if (Action == TS_PLATFORM) {
        StrCpyS (Page->Message, ARRAY_SIZE (Page->Message), L"Reading platform services...");
        TsPaint (Page);
        Page->LastStatus = PlatformHardwareTest (TsPlatformOutput, Page, &Page->Platform);
        PlatformRpmbRead(TsPlatformOutput,Page,0,1,&Page->Rpmb);
        Page->PlatformSampled = TRUE;
        Page->ResultPage = TS_PLATFORM;
        StrCpyS (Page->Message, ARRAY_SIZE (Page->Message), L"Details: QR Log or CDC logcat.");
      } else if (Action == TS_WLAN) {
        TsDevicePage (Page, Keys, PaintEvent);
        StrCpyS (Page->Message, ARRAY_SIZE (Page->Message), L"Device details: QR Log or CDC logcat.");
      } else if (Action == TS_USB_HOST) {
        StrCpyS (Page->Message, ARRAY_SIZE (Page->Message), L"Reading existing hardware state...");
        TsPaint (Page);
        Page->LastStatus = PlatformUsbHostTest (TsPlatformOutput, Page, &Page->UsbHost);
        Page->ResultPage = Action;
        StrCpyS (Page->Message, ARRAY_SIZE (Page->Message), L"Details: QR Log or CDC logcat.");
      } else if (Action == TS_DISPLAY) {
        PlatformBrightnessStatus(TsPlatformOutput,Page);
        TsDisplayTest (Page, Keys);
      } else if (Action == TS_FLASH) {
        Page->LastStatus=PlatformFlashTest(TsPlatformOutput,Page,0,100,50,&Page->Flash);
        Page->ResultPage=TS_FLASH;
        StrCpyS(Page->Message,ARRAY_SIZE(Page->Message),L"Flash details: CDC logcat.");
      } else {
        Swm->ActivateWindow (Swm, Page->Window, FALSE);
        if (Action == TS_POWER) FpPowerStatus (Swm, Gop);
        else FpFileBrowser (Swm, Gop);
        Swm->ActivateWindow (Swm, Page->Window, TRUE);
        ZeroMem (&Page->Tap, sizeof (Page->Tap));
      }
      Dirty = TRUE;
    } else if (Action == TS_BACK) {
      Page->LastStatus = FpHapticStop ();
      Page->HapticEnabled = FpHapticIsEnabled ();
      if (EFI_ERROR (Page->LastStatus)) {
        StrCpyS (
                 Page->Message,
                 ARRAY_SIZE (Page->Message),
                 L"Stop failed. Vibration is still marked ON; retry before leaving."
                 );
        Dirty = TRUE;
      } else {
        Exit = TRUE;
      }
    }

    FpServiceEvents ();
    gBS->Stall (20000);
  }

  Swm->ActivateWindow (Swm, Page->Window, FALSE);
  StopStatus = FpHapticStop ();
  Page->HapticEnabled = FpHapticIsEnabled ();
  Swm->UnregisterClient (Swm, Page->Window);
  gBS->UninstallProtocolInterface (Page->Window, &mTestWindowGuid, Page);
  FreePool (Page);
  (VOID)StopStatus;
}
