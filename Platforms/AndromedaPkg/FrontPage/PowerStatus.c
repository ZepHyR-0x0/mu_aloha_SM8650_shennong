/** Large read-only status popup; all buttons act on a single released tap. */
#include <Uefi.h>
#include <Protocol/SimpleWindowManager.h>
#include <Protocol/SimpleTextInEx.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/MsUiThemeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <UIToolKit/TouchGesture.h>
#include "PowerStatus.h"
#include "BootLog.h"
#include "FrontPage.h"
#include "FrontPageEventService.h"

#define PS_POWER 1
#define PS_PLATFORM 2
#define PS_BACK 3
#define PS_READ 4
#define PS_SAVE 5
typedef struct {
  MS_SIMPLE_WINDOW_MANAGER_PROTOCOL *Swm;
  EFI_HANDLE Window;
  EFI_ABSOLUTE_POINTER_PROTOCOL *Pointer;
  EFI_IMAGE_OUTPUT Output;
  FP_POWER_SNAPSHOT Sample;
  UIT_TOUCH_GESTURE Tap;
  UINTN Width, Height, Font, Left, Right, Top, Bottom, ButtonHeight, RowTop, LineHeight;
  UINTN Page;
  CHAR16 Message[128];
} PS;
STATIC EFI_GUID mPowerWindowGuid = {0x49e87ab3,0x9031,0x4d81,{0x98,0x19,0x33,0x17,0,0,0,2}};
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mPowerBg = {20,20,20,0}, mPowerFg = {240,240,240,0},
  mPowerButton = {45,45,45,0}, mPowerSelected = {120,70,20,0};

STATIC VOID PsFill (PS *P, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Bg, UINTN X, UINTN Y, UINTN W, UINTN H)
{
  P->Swm->BltWindow (P->Swm, P->Window, Bg, EfiBltVideoFill, 0, 0, X, Y, W, H, 0);
}
STATIC VOID PsText (PS *P, CHAR16 *Text, UINTN X, UINTN Y, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Bg)
{
  EFI_FONT_DISPLAY_INFO Font;
  EFI_IMAGE_OUTPUT *Output = &P->Output;
  ZeroMem (&Font, sizeof (Font));
  Font.FontInfoMask = EFI_FONT_INFO_ANY_FONT;
  Font.FontInfo.FontSize = (UINT16)P->Font;
  Font.ForegroundColor = mPowerFg;
  Font.BackgroundColor = *Bg;
  P->Swm->StringToWindow (P->Swm, P->Window,
    EFI_HII_OUT_FLAG_CLIP | EFI_HII_OUT_FLAG_CLIP_CLEAN_X | EFI_HII_OUT_FLAG_CLIP_CLEAN_Y |
    EFI_HII_IGNORE_LINE_BREAK | EFI_HII_DIRECT_TO_SCREEN,
    Text, &Font, &Output, X, Y, NULL, NULL, NULL);
}
STATIC VOID PsLine (PS *P, UINTN Row, CHAR16 *Text)
{
  UINTN Y = P->RowTop + Row * P->LineHeight;
  if (Y + P->Font < P->Bottom - P->LineHeight) PsText (P, Text, P->Left, Y, &mPowerBg);
}
STATIC VOID PsPaint (PS *P)
{
  CHAR16 Line[160];
  CHAR16 *Tabs[] = {L"Power", L"Platform", L"Back"};
  CHAR16 *Names[] = {L"ChargerEx", L"PMIC ADC", L"PMIC GLINK", L"GLINK local",
    L"SCM", L"PIL", L"USB Device", L"USB Function I/O", L"Storage guard", L"Battery backend"};
  STATIC_ASSERT (ARRAY_SIZE (Names) == FpProtocolCount, "power labels match protocol observations");
  UINTN I, W = P->Right - P->Left;
  FP_POWER_SNAPSHOT *S = &P->Sample;
  PsFill (P, &mPowerBg, 0, 0, P->Width, P->Height);
  PsText (P, L"Power / platform status  b327", P->Left, P->Top, &mPowerBg);
  for (I = 0; I < 3; I++) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Bg = (P->Page == I && I < 2) ||
      (P->Tap.Armed && P->Tap.LastTarget == I + 1) ? &mPowerSelected : &mPowerButton;
    PsFill (P, Bg, P->Left + I * W / 3 + 4, P->Top + P->LineHeight, W / 3 - 8, P->ButtonHeight);
    PsText (P, Tabs[I], P->Left + I * W / 3 + 20, P->Top + P->LineHeight + 16, Bg);
  }
  if (P->Page == 0) {
    UnicodeSPrint (Line, sizeof (Line), L"ChargerEx: %r  rev=%lx", S->Protocol[FpProtocolCharger], S->ChargerRevision);
    PsLine (P, 0, Line);
    UnicodeSPrint (Line, sizeof (Line), L"Battery backend: %r", S->Protocol[FpProtocolBatteryBackend]);
    PsLine (P, 1, Line);
    PsLine (P, 2, L"Power source connected:");
    UnicodeSPrint (Line, sizeof (Line), L"  %s (%r)", EFI_ERROR (S->ChargerStatus) ? L"Unavailable" : S->ChargerPresent ? L"Yes" : L"No", S->ChargerStatus);
    PsLine (P, 3, Line);
    PsLine (P, 5, L"Battery present:");
    UnicodeSPrint (Line, sizeof (Line), L"  %s (%r)", EFI_ERROR (S->BatteryStatus) ? L"Unavailable" : S->BatteryPresent ? L"Yes" : L"No", S->BatteryStatus);
    PsLine (P, 6, Line);
    UnicodeSPrint (Line, sizeof (Line), L"Reported voltage: %u mV (%r)", S->Millivolts, S->VoltageStatus);
    if (EFI_ERROR (S->VoltageStatus)) UnicodeSPrint (Line, sizeof (Line), L"Reported voltage: unavailable (%r)", S->VoltageStatus);
    PsLine (P, 8, Line);
    PsLine (P, 9, L"Voltage source / freshness unverified.");
    PsLine (P, 11, L"Battery % / temperature: unavailable.");
    PsLine (P, 12, L"Source connected does not prove charging.");
    PsLine (P, 13, L"Read power takes one snapshot; no polling.");
    PsLine (P, 14, S->Sampled ? L"Snapshot complete. Read again to refresh." : L"Press Read power to call stock getters.");
  } else {
    for (I = 0; I < FpProtocolCount; I++) {
      UnicodeSPrint (Line, sizeof (Line), L"%s: %s (%r)", Names[I], EFI_ERROR (S->Protocol[I]) ? L"unavailable" : L"published", S->Protocol[I]);
      PsLine (P, I, Line);
    }
    PsLine (P, 10, L"Published protocols only; no remote test.");
    PsLine (P, 11, L"USB enumeration: use ENABLE USB + LOGREAD.");
    PsLine (P, 12, L"ADSP running / audio output: unverified.");
    PsLine (P, 13, L"Audio playback backend is not implemented.");
    PsLine (P, 14, L"Power tab rechecks protocols on selection.");
  }
  PsLine (P, 16, P->Message);
  PsFill (P, &mPowerButton, P->Left + 4, P->Bottom, W / 2 - 8, P->ButtonHeight);
  PsFill (P, &mPowerButton, P->Left + W / 2 + 4, P->Bottom, W / 2 - 8, P->ButtonHeight);
  PsText (P, L"Read power", P->Left + 20, P->Bottom + 16, &mPowerButton);
  PsText (P, L"Save log", P->Left + W / 2 + 20, P->Bottom + 16, &mPowerButton);
}
STATIC BOOLEAN EFIAPI PsNotify (VOID *Context) { return TRUE; }
STATIC UINTN PsTouch (PS *P, EFI_ABSOLUTE_POINTER_STATE *Touch, BOOLEAN *Dirty)
{
  UINTN Target = 0, W = P->Right - P->Left;
  UIT_TAP_RESULT Result;
  if (Touch->CurrentX >= P->Left && Touch->CurrentX < P->Right) {
    if (Touch->CurrentY >= P->Top + P->LineHeight && Touch->CurrentY < P->Top + P->LineHeight + P->ButtonHeight)
      Target = PS_POWER + (UINTN)(Touch->CurrentX - P->Left) * 3 / W;
    else if (Touch->CurrentY >= P->Bottom && Touch->CurrentY < P->Bottom + P->ButtonHeight)
      Target = PS_READ + (UINTN)(Touch->CurrentX - P->Left) * 2 / W;
  }
  Result = UitTouchTap (&P->Tap, SWM_IS_FINGER_DOWN (*Touch), Target, Touch->CurrentX, Touch->CurrentY,
    GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000000, MAX ((UINTN)24, P->Width / 50));
  if (Result == UitTapFocus || Result == UitTapConfirm) {
    *Dirty = TRUE;
    if (Target != 0) { ZeroMem (&P->Tap, sizeof (P->Tap)); return Target; }
  }
  return 0;
}
VOID FpPowerStatus (MS_SIMPLE_WINDOW_MANAGER_PROTOCOL *Swm, EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop)
{
  PS *P;
  EFI_STATUS Status;
  EFI_EVENT PaintEvent;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Keys = NULL;
  EFI_KEY_DATA Key;
  SWM_RECT Frame;
  BOOLEAN Exit = FALSE, Dirty = TRUE;
  if (Swm == NULL || Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL) return;
  P = AllocateZeroPool (sizeof (*P));
  if (P == NULL) return;
  P->Swm = Swm;
  P->Width = Gop->Mode->Info->HorizontalResolution;
  P->Height = Gop->Mode->Info->VerticalResolution;
  P->Font = MsUiGetLargeFontHeight ();
  P->Left = MAX ((UINTN)32, P->Width / 15);
  P->Right = P->Width - P->Left;
  P->Top = MAX ((UINTN)80, P->Height / 20);
  P->ButtonHeight = P->Font + 40;
  P->LineHeight = P->Font + 30;
  P->Bottom = P->Height > P->Top + P->ButtonHeight ? P->Height - P->Top - P->ButtonHeight : 0;
  P->RowTop = P->Top + P->LineHeight + P->ButtonHeight + 40;
  if (P->Font == 0 || P->Width < 640 || P->Bottom < P->RowTop + 18 * P->LineHeight) goto Done;
  P->Output.Width = (UINT16)P->Width;
  P->Output.Height = (UINT16)P->Height;
  P->Output.Image.Screen = Gop;
  SWM_RECT_INIT2 (Frame, 0, 0, P->Width, P->Height);
  Status = gBS->InstallProtocolInterface (&P->Window, &mPowerWindowGuid, EFI_NATIVE_INTERFACE, P);
  if (EFI_ERROR (Status)) goto Done;
  Status = Swm->RegisterClient (Swm, P->Window, SWM_Z_ORDER_POPUP, &Frame, PsNotify, P, &P->Pointer, &PaintEvent);
  if (EFI_ERROR (Status)) goto Remove;
  if (P->Pointer == NULL) goto Unregister;
  Swm->ActivateWindow (Swm, P->Window, TRUE);
  if (gST->ConsoleInHandle != NULL) gBS->HandleProtocol (gST->ConsoleInHandle, &gEfiSimpleTextInputExProtocolGuid, (VOID **)&Keys);
  FpInspectPower (&P->Sample); // No peripheral calls on entry.
  while (!Exit) {
    EFI_ABSOLUTE_POINTER_STATE Touch;
    UINTN Action = 0;
    if (Dirty || gBS->CheckEvent (PaintEvent) == EFI_SUCCESS) { PsPaint (P); Dirty = FALSE; }
    if (Keys != NULL && Keys->ReadKeyStrokeEx (Keys, &Key) == EFI_SUCCESS) {
      if (Key.Key.ScanCode == SCAN_ESC || Key.Key.UnicodeChar == CHAR_BACKSPACE) Action = PS_BACK;
      else if (Key.Key.UnicodeChar == CHAR_CARRIAGE_RETURN || Key.Key.UnicodeChar == L'r' || Key.Key.UnicodeChar == L'R') Action = PS_READ;
      else if (Key.Key.ScanCode == SCAN_LEFT || Key.Key.ScanCode == SCAN_UP) Action = PS_POWER;
      else if (Key.Key.ScanCode == SCAN_RIGHT || Key.Key.ScanCode == SCAN_DOWN) Action = PS_PLATFORM;
      else if (Key.Key.UnicodeChar == L's' || Key.Key.UnicodeChar == L'S') Action = PS_SAVE;
      ZeroMem (&P->Tap, sizeof (P->Tap));
    }
    if (P->Pointer->GetState (P->Pointer, &Touch) == EFI_SUCCESS) {
      UINTN TouchAction = PsTouch (P, &Touch, &Dirty);
      if (TouchAction != 0) Action = TouchAction;
    }
    if (Action == PS_BACK) Exit = TRUE;
    else if (Action == PS_POWER || Action == PS_PLATFORM) {
      P->Page = Action == PS_POWER ? 0 : 1;
      // Keep the last power reading while refreshing protocol availability.
      { FP_POWER_SNAPSHOT Fresh; FpInspectPower (&Fresh);
        CopyMem (P->Sample.Protocol, Fresh.Protocol, sizeof (Fresh.Protocol));
        P->Sample.ChargerRevision = Fresh.ChargerRevision; }
      Dirty = TRUE;
    } else if (Action == PS_READ) {
      StrCpyS (P->Message, ARRAY_SIZE (P->Message), L"Reading stock power getters...");
      PsPaint (P);
      FpHealthReport ();
      FpSamplePower (&P->Sample);
      P->Page = 0;
      StrCpyS (P->Message, ARRAY_SIZE (P->Message), L"Snapshot finished. Save log for analysis.");
      Dirty = TRUE;
    } else if (Action == PS_SAVE) {
      Status = BootLogSave ("power-page-manual-save");
      UnicodeSPrint (P->Message, sizeof (P->Message), L"Save to logfs: %r", Status);
      Dirty = TRUE;
    }
    FpServiceEvents ();
    FpServiceDelay (20000);
  }
  Swm->ActivateWindow (Swm, P->Window, FALSE);
Unregister:
  Swm->UnregisterClient (Swm, P->Window);
Remove:
  gBS->UninstallProtocolInterface (P->Window, &mPowerWindowGuid, P);
Done:
  FreePool (P);
}
