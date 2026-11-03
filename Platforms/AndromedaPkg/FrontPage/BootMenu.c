/** @file

  Pre-GUI boot menu.

  BDS uses FrontPage as its boot-manager menu, so this runs before any of the
  form/setup stack exists. Everything is drawn through the Simple Window
  Manager (BltWindow for the background, StringToWindow for text) - the same
  graphics path the rest of the UI uses - and keys come from the console input
  device the keypad driver feeds: volume up/down arrive as SCAN_UP/SCAN_DOWN,
  the power key as Enter.

  The first entry is "Enter GUI". Selection is manual, with no boot countdown.
  The rest is refreshed from the boot manager on every boot
  (EfiBootManagerRefreshAllBootOption) so the list follows BootOrder instead of
  a hard-coded set of entries.

  Copyright (C) Microsoft Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Uefi/UefiInternalFormRepresentation.h>
#include <Pi/PiFirmwareFile.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/SimpleWindowManager.h>
#include <Protocol/OnScreenKeyboard.h>
#include <Protocol/UsbBringup.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MsUiThemeLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <UIToolKit/TouchGesture.h>

#include "BootMenu.h"
#include "BootMenuIcons.h"
#include "FrontPage.h"
#include "FrontPageEventService.h"
#include "ShellDisplay.h"
#include <Protocol/FirmwareInput.h>

#define FP_BOOT_MENU_TIMEOUT_SECONDS   5
#define FP_BOOT_MENU_POLL_US           (50 * 1000)
#define FP_BOOT_MENU_MAX_VISIBLE       9
#define FP_BOOT_MENU_MAX_BOOT_OPTIONS  32
#define FP_BOOT_MENU_ICON_RASTER_SIZE  192
#define FP_BOOT_MENU_ICON_MIN_SIZE     80
#define FP_BOOT_MENU_ICON_PORTRAIT_SIZE 156
#define FP_BOOT_MENU_ICON_MIN_PAD      20
#define FP_BOOT_MENU_CARD_GAP          12
#define FP_BOOT_MENU_CARD_RADIUS       16
#define FP_BOOT_MENU_CARD_RADIUS_MAX   30
#define FP_BOOT_MENU_CARD_RADIUS_MIN   10
#define FP_BOOT_MENU_CHEVRON_MIN_SIZE  52
#define FP_BOOT_MENU_CHEVRON_MAX_SIZE  112

//
// 7C04A583-9E3E-4F1C-AD65-E05268D0B4D1, from ShellPkg.dec. Declared locally so
// this application does not need ShellPkg in its package list.
//
STATIC EFI_GUID  mShellFileGuid = {
  0x7c04a583, 0x9e3e, 0x4f1c, { 0xad, 0x65, 0xe0, 0x52, 0x68, 0xd0, 0xb4, 0xd1 }
};

STATIC EFI_GUID  mBootMenuWindowGuid = {
  0x4a763c11, 0x21ca, 0x4a45, { 0x98, 0x20, 0x7a, 0x83, 0xa8, 0x70, 0x24, 0x1f }
};

STATIC
BOOLEAN
FpMenuIsShellOption (
  IN CONST CHAR16  *Label
  )
{
  if (Label == NULL) {
    return FALSE;
  }

  //
  // The platform reports its FV shell as "Internal UEFI Shell 2.0", while
  // the fallback row installed by this menu uses "UEFI Shell". Both should be
  // recognized so the list never shows two shell rows for one shell image.
  //
  return (BOOLEAN)((StrStr (Label, L"UEFI Shell") != NULL) ||
                   (StrStr (Label, L"EFI Shell") != NULL));
}

typedef enum {
  FpMenuEntryGui,
  FpMenuEntryBootOption,
  FpMenuEntryShell,
  FpMenuEntryReboot,
  FpMenuEntryMore,
  FpMenuEntryBack,
  FpMenuEntryAndroid,
  FpMenuEntryRecovery,
  FpMenuEntryBootloader
} FP_MENU_ENTRY_TYPE;

typedef struct {
  CHAR16               *Label;
  FP_MENU_ENTRY_TYPE   Type;
  UINTN                OptionIndex;
} FP_MENU_ENTRY;

typedef struct {
  MS_SIMPLE_WINDOW_MANAGER_PROTOCOL  *Swm;
  EFI_GRAPHICS_OUTPUT_PROTOCOL       *Gop;
  EFI_HANDLE                          ImageHandle;
  EFI_HANDLE                          Window;
  EFI_IMAGE_OUTPUT                   *TextOut;
  UINTN                               Hres;
  UINTN                               Vres;
  UINTN                               MarginX;
  UINTN                               HeaderTop;
  UINTN                               TitleY;
  UINTN                               SubtitleY;
  UINTN                               ListHeadY;
  UINTN                               RowTop;
  UINTN                               RowHeight;
  UINTN                               IconSize;
  UINTN                               IconPad;
  UINTN                               ChevronSize;
  UINTN                               CardRadius;
  UINTN                               CardGap;
  UINTN                               FooterTop;
  UINTN                               FooterY;
  UINT16                              TitleFont;
  UINT16                              SubFont;
  UINT16                              RowFont;
  CHAR16                              Status[128];
  EFI_ABSOLUTE_POINTER_PROTOCOL       *Pointer;
  EFI_EVENT                           PaintEvent;
  SWM_RECT                            Frame;
  BOOLEAN                             TouchClient;
  UINTN                               MenuLevel;
  BOOLEAN                             TouchActive;
  BOOLEAN                             SwipeAllowed;
  BOOLEAN                             Dragging;
  UINT64                              SwipeY;
  UINTN                               SwipeTop;
  UIT_TOUCH_GESTURE                   Tap;
  EFI_FONT_DISPLAY_INFO              *StringInfo;
  UINT8                              *IconAlpha;
  CONST UINT8                        *IconRasterIcon;
  UINTN                               IconRasterSize;
  UINT8                              *ChevronAlpha;
  UINTN                               ChevronRasterSize;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL       *BlitBuffer;
  UINTN                               BlitBufferCount;
} FP_BOOT_MENU;

//
// EFI_GRAPHICS_OUTPUT_BLT_PIXEL is Blue/Green/Red/Reserved. These are
// deliberately warm dark neutrals; the Boot Menu is a firmware utility, not
// an OS launcher, so it should look quiet and legible on both OLED and LCD.
//
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuBg          = { 0x09, 0x0B, 0x0E, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuFg          = { 0xEE, 0xF4, 0xF7, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuMuted       = { 0xAA, 0xB5, 0xBE, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuHint        = { 0x5F, 0x68, 0x70, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuWarn        = { 0x72, 0xC9, 0x4F, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuCard        = { 0x15, 0x1A, 0x1E, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuCardSelected = { 0x1C, 0x25, 0x2B, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuLine        = { 0x24, 0x2B, 0x30, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuAccent      = { 0xD9, 0xA4, 0x4A, 0x00 };

STATIC
EFI_HANDLE
FpMenuHandle (
  IN FP_BOOT_MENU  *Menu
  )
{
  if (Menu->Window != NULL) {
    return Menu->Window;
  }

  return Menu->ImageHandle;
}

STATIC
BOOLEAN
EFIAPI
FpMenuNotify (
  IN VOID  *Context
  )
{
  (VOID)Context;
  return TRUE;
}

STATIC
VOID
FpMenuFill (
  IN FP_BOOT_MENU                    *Menu,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL   *Color,
  IN UINTN                            X,
  IN UINTN                            Y,
  IN UINTN                            Width,
  IN UINTN                            Height
  )
{
  Menu->Swm->BltWindow (
               Menu->Swm,
               FpMenuHandle (Menu),
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
EFI_STATUS
FpMenuText (
  IN FP_BOOT_MENU                   *Menu,
  IN CONST CHAR16                   *Text,
  IN UINTN                           X,
  IN UINTN                           Y,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Foreground,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Background,
  IN UINT16                          FontSize
  )
{
  EFI_STATUS             Status;
  EFI_FONT_DISPLAY_INFO  *StringInfo;

  if (Menu->StringInfo == NULL) {
    Menu->StringInfo = AllocateZeroPool (sizeof (*StringInfo));
  }

  StringInfo = Menu->StringInfo;
  if (StringInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (StringInfo, sizeof (*StringInfo));

  StringInfo->FontInfoMask           = EFI_FONT_INFO_ANY_FONT;
  StringInfo->FontInfo.FontSize      = FontSize;
  StringInfo->FontInfo.FontStyle     = EFI_HII_FONT_STYLE_NORMAL;
  CopyMem (&StringInfo->ForegroundColor, Foreground, sizeof (*Foreground));
  CopyMem (&StringInfo->BackgroundColor, Background, sizeof (*Background));

  Status = Menu->Swm->StringToWindow (
                        Menu->Swm,
                        FpMenuHandle (Menu),
                        EFI_HII_OUT_FLAG_CLIP |
                        EFI_HII_OUT_FLAG_CLIP_CLEAN_X | EFI_HII_OUT_FLAG_CLIP_CLEAN_Y |
                        EFI_HII_IGNORE_LINE_BREAK | EFI_HII_DIRECT_TO_SCREEN,
                        (EFI_STRING)Text,
                        StringInfo,
                        &Menu->TextOut,
                        X,
                        Y,
                        NULL,
                        NULL,
                        NULL
                        );

  return Status;
}

STATIC
CONST UINT8 *
FpMenuIcon (
  IN CONST FP_MENU_ENTRY  *Entry
  )
{
  CONST CHAR16  *Label;

  if (Entry == NULL) {
    return mBootMenuIconBootDevice;
  }

  if (Entry->Type == FpMenuEntryGui) {
    return mBootMenuIconFirmware;
  }

  if (Entry->Type == FpMenuEntryShell) {
    return mBootMenuIconUefiShell;
  }

  if (Entry->Type == FpMenuEntryReboot) {
    return mBootMenuIconReboot;
  }

  if (Entry->Type == FpMenuEntryBootloader) return mBootMenuIconFirmware;

  if (Entry->Type == FpMenuEntryMore) {
    return mBootMenuIconUefiConfig;
  }

  if (Entry->Type == FpMenuEntryBack) {
    return mBootMenuIconUsb;
  }

  if (Entry->Type == FpMenuEntryAndroid) {
    return mBootMenuIconAndroid;
  }

  if (Entry->Type == FpMenuEntryRecovery) {
    return mBootMenuIconRecovery;
  }

  Label = Entry->Label;
  if (Label == NULL) {
    return mBootMenuIconBootDevice;
  }

  if (FpMenuIsShellOption (Label)) {
    return mBootMenuIconUefiShell;
  }

  if (StrStr (Label, L"Linux") != NULL) {
    return mBootMenuIconLinux;
  }

  if (StrStr (Label, L"Android") != NULL) {
    return mBootMenuIconAndroid;
  }

  if ((StrStr (Label, L"Apple") != NULL) ||
      (StrStr (Label, L"iOS") != NULL) ||
      (StrStr (Label, L"iPad") != NULL) ||
      (StrStr (Label, L"macOS") != NULL) ||
      (StrStr (Label, L"Mac ") != NULL))
  {
    return mBootMenuIconApple;
  }

  if (StrStr (Label, L"Windows") != NULL) {
    return mBootMenuIconWindows;
  }

  if ((StrStr (Label, L"PXE") != NULL)) {
    return mBootMenuIconPxe;
  }

  if ((StrStr (Label, L"Network") != NULL) ||
      (StrStr (Label, L"HTTP") != NULL) ||
      (StrStr (Label, L"IPv4") != NULL) ||
      (StrStr (Label, L"IPv6") != NULL))
  {
    return mBootMenuIconNetwork;
  }

  if (StrStr (Label, L"USB") != NULL) {
    return mBootMenuIconUsb;
  }

  if ((StrStr (Label, L"UFS") != NULL) ||
      (StrStr (Label, L"eMMC") != NULL) ||
      (StrStr (Label, L"NVMe") != NULL) ||
      (StrStr (Label, L"Internal") != NULL) ||
      (StrStr (Label, L"Storage") != NULL))
  {
    return mBootMenuIconStorage;
  }

  if ((StrStr (Label, L"Bootloader") != NULL) ||
      (StrStr (Label, L"FFU") != NULL) ||
      (StrStr (Label, L"Loader") != NULL))
  {
    return mBootMenuIconFirmware;
  }

  if ((StrStr (Label, L"Setup") != NULL) ||
      (StrStr (Label, L"Config") != NULL))
  {
    return mBootMenuIconUefiConfig;
  }

  if (StrStr (Label, L"Recovery") != NULL) {
    return mBootMenuIconRecovery;
  }

  return mBootMenuIconBootDevice;
}

STATIC
CONST CHAR16 *
FpMenuSubLabel (
  IN CONST FP_MENU_ENTRY  *Entry
  )
{
  if (Entry == NULL) {
    return L"";
  }

  switch (Entry->Type) {
    case FpMenuEntryGui:
      return L"Graphical firmware interface";

    case FpMenuEntryShell:
      return L"Command-line firmware shell";

    case FpMenuEntryReboot:
      return L"Restart this device";

    case FpMenuEntryMore:
      return L"Additional boot options and actions";

    case FpMenuEntryBack:
      return L"Return to the main boot menu";

    case FpMenuEntryAndroid:
      return L"Boot Android from the selected slot";

    case FpMenuEntryRecovery:
      return L"Boot recovery from the selected slot";

    case FpMenuEntryBootloader:
      return L"Restart into Android bootloader / fastboot";

    default:
      if (FpMenuIsShellOption (Entry->Label)) {
        return L"UEFI command-line environment";
      }

      if (StrStr (Entry->Label, L"Linux") != NULL) {
        return L"Linux operating system";
      }

      if (StrStr (Entry->Label, L"Android") != NULL) {
        return L"Android operating system";
      }

      if ((StrStr (Entry->Label, L"Apple") != NULL) ||
          (StrStr (Entry->Label, L"iOS") != NULL) ||
          (StrStr (Entry->Label, L"iPad") != NULL) ||
          (StrStr (Entry->Label, L"macOS") != NULL) ||
          (StrStr (Entry->Label, L"Mac ") != NULL))
      {
        return L"Apple operating system";
      }

      if (StrStr (Entry->Label, L"Windows") != NULL) {
        return L"Microsoft Windows";
      }

      if ((StrStr (Entry->Label, L"PXE") != NULL) ||
          (StrStr (Entry->Label, L"Network") != NULL) ||
          (StrStr (Entry->Label, L"HTTP") != NULL))
      {
        return L"Network boot service";
      }

      if (StrStr (Entry->Label, L"USB") != NULL) {
        return L"Removable USB boot device";
      }

      if ((StrStr (Entry->Label, L"Internal") != NULL) ||
          (StrStr (Entry->Label, L"Storage") != NULL) ||
          (StrStr (Entry->Label, L"UFS") != NULL) ||
          (StrStr (Entry->Label, L"eMMC") != NULL) ||
          (StrStr (Entry->Label, L"NVMe") != NULL))
      {
        return L"On-device boot volume";
      }

      if ((StrStr (Entry->Label, L"Bootloader") != NULL) ||
          (StrStr (Entry->Label, L"FFU") != NULL) ||
          (StrStr (Entry->Label, L"Loader") != NULL))
      {
        return L"Firmware boot utility";
      }

      return L"Firmware boot target";
  }
}

STATIC
VOID
FpMenuBlitAlpha (
  IN FP_BOOT_MENU                         *Menu,
  IN CONST UINT8                          *Alpha,
  IN UINTN                                 Width,
  IN UINTN                                 Height,
  IN UINTN                                 X,
  IN UINTN                                 Y,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *IconColor,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Background
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixels;
  UINTN                          Index;
  UINTN                          Row;
  UINTN                          Col;
  UINT32                         Value;

  if ((Menu == NULL) || (Alpha == NULL) || (IconColor == NULL) ||
      (Background == NULL) || (Width == 0) || (Height == 0))
  {
    return;
  }

  if ((Menu->BlitBuffer == NULL) || (Menu->BlitBufferCount < (Width * Height))) {
    if (Menu->BlitBuffer != NULL) {
      FreePool (Menu->BlitBuffer);
    }

    Menu->BlitBuffer = AllocateZeroPool (Width * Height * sizeof (*Pixels));
    if (Menu->BlitBuffer == NULL) {
      Menu->BlitBufferCount = 0;
      return;
    }

    Menu->BlitBufferCount = Width * Height;
  }

  Pixels = Menu->BlitBuffer;

  for (Row = 0; Row < Height; Row++) {
    for (Col = 0; Col < Width; Col++) {
      Index = Row * Width + Col;
      Value = Alpha[Index];
      if (Value == 0) {
        Pixels[Index].Blue     = Background->Blue;
        Pixels[Index].Green    = Background->Green;
        Pixels[Index].Red      = Background->Red;
        Pixels[Index].Reserved = 0;
        continue;
      }

      Pixels[Index].Blue     = (UINT8)(((UINT32)IconColor->Blue  * Value +
                                        (UINT32)Background->Blue  * (255 - Value)) / 255);
      Pixels[Index].Green    = (UINT8)(((UINT32)IconColor->Green * Value +
                                        (UINT32)Background->Green * (255 - Value)) / 255);
      Pixels[Index].Red      = (UINT8)(((UINT32)IconColor->Red   * Value +
                                        (UINT32)Background->Red   * (255 - Value)) / 255);
      Pixels[Index].Reserved = 0;
    }
  }

  Menu->Swm->BltWindow (
               Menu->Swm,
               FpMenuHandle (Menu),
               Pixels,
               EfiBltBufferToVideo,
               0,
               0,
               X,
               Y,
               Width,
               Height,
               Width * sizeof (*Pixels)
               );

}

STATIC
VOID
FpMenuDrawIcon (
  IN FP_BOOT_MENU                     *Menu,
  IN CONST UINT8                      *Icon,
  IN UINTN                             X,
  IN UINTN                             Y,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *IconColor,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Background
  )
{
  UINTN                          Size;
  UINTN                          Index;
  UINTN                          Row;
  UINTN                          Col;
  UINTN                          SourceRow;
  UINTN                          SourceCol;

  if ((Menu == NULL) || (Icon == NULL) || (IconColor == NULL) || (Background == NULL)) {
    return;
  }

  Size = MIN (Menu->IconSize, FP_BOOT_MENU_ICON_RASTER_SIZE);
  if (Size == 0) {
    return;
  }

  if ((Menu->IconAlpha == NULL) ||
      (Menu->IconRasterIcon != Icon) ||
      (Menu->IconRasterSize != Size))
  {
    if (Menu->IconAlpha == NULL) {
      Menu->IconAlpha = AllocateZeroPool (Size * Size);
    } else if (Menu->IconRasterSize != Size) {
      FreePool (Menu->IconAlpha);
      Menu->IconAlpha = AllocateZeroPool (Size * Size);
    } else {
      ZeroMem (Menu->IconAlpha, Size * Size);
    }

    if (Menu->IconAlpha == NULL) {
      return;
    }

    for (Row = 0; Row < Size; Row++) {
      SourceRow = (Row * FP_BOOT_MENU_ICON_RASTER_SIZE) / Size;
      for (Col = 0; Col < Size; Col++) {
        SourceCol = (Col * FP_BOOT_MENU_ICON_RASTER_SIZE) / Size;
        Index     = Row * Size + Col;
        Menu->IconAlpha[Index] = Icon[SourceRow * FP_BOOT_MENU_ICON_RASTER_SIZE + SourceCol];
      }
    }

    Menu->IconRasterIcon = Icon;
    Menu->IconRasterSize = Size;
  }

  FpMenuBlitAlpha (Menu, Menu->IconAlpha, Size, Size, X, Y, IconColor, Background);
}

STATIC
UINTN
FpMenuSqrt (
  IN UINTN  Value
  )
{
  UINTN  Root;
  UINTN  Index;

  if (Value == 0) {
    return 0;
  }

  Root = Value;
  for (Index = 0; Index < 10; Index++) {
    Root = (Root + Value / Root) / 2;
  }

  while (Root > 0 && Root * Root > Value) {
    Root--;
  }

  while ((Root + 1) != 0 && (Root + 1) * (Root + 1) <= Value) {
    Root++;
  }

  return Root;
}

STATIC
UINTN
FpMenuCornerInset (
  IN UINTN  Row,
  IN UINTN  Height,
  IN UINTN  Radius
  )
{
  UINTN  FromEdge;
  UINTN  Delta;

  if ((Radius == 0) || (Height < (2 * Radius))) {
    return 0;
  }

  if (Row < Radius) {
    FromEdge = Row;
  } else if (Row >= Height - Radius) {
    FromEdge = Height - Row - 1;
  } else {
    return 0;
  }

  Delta = (FromEdge < Radius) ? (Radius - FromEdge) : 0;
  return Radius - FpMenuSqrt ((Radius * Radius) - (Delta * Delta));
}

STATIC
VOID
FpMenuDrawRoundedBox (
  IN FP_BOOT_MENU                    *Menu,
  IN UINTN                            X,
  IN UINTN                            Y,
  IN UINTN                            Width,
  IN UINTN                            Height,
  IN UINTN                            Radius,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Border,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Background
  )
{
  UINTN  Row;
  UINTN  Inset;
  UINTN  InnerRadius;
  UINTN  InnerWidth;
  UINTN  InnerHeight;
  UINTN  FillWidth;

  if ((Menu == NULL) || (Border == NULL) || (Background == NULL) ||
      (Width == 0) || (Height == 0))
  {
    return;
  }

  Radius = MIN (Radius, MIN (Width / 2, Height / 2));

  for (Row = 0; Row < Height; Row++) {
    Inset = FpMenuCornerInset (Row, Height, Radius);
    if ((2 * Inset) < Width) {
      FpMenuFill (Menu, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)Border,
                  X + Inset, Y + Row, Width - (2 * Inset), 1);
    }
  }

  if ((Width <= 2) || (Height <= 2)) {
    return;
  }

  InnerWidth  = Width - 2;
  InnerHeight = Height - 2;
  InnerRadius = (Radius > 1) ? (Radius - 1) : 0;

  for (Row = 0; Row < InnerHeight; Row++) {
    Inset = FpMenuCornerInset (Row, InnerHeight, InnerRadius);
    if ((2 * Inset) < InnerWidth) {
      FillWidth = InnerWidth - (2 * Inset);
      FpMenuFill (Menu, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)Background,
                  X + 1 + Inset, Y + 1 + Row, FillWidth, 1);
    }
  }
}

STATIC
UINTN
FpMenuPointSegmentDistanceSquared (
  IN INTN  Px,
  IN INTN  Py,
  IN INTN  Ax,
  IN INTN  Ay,
  IN INTN  Bx,
  IN INTN  By
  )
{
  INTN  Dx;
  INTN  Dy;
  INTN  LengthSquared;
  INTN  Projection;
  INTN  Cross;
  INTN  X;
  INTN  Y;

  Dx = Bx - Ax;
  Dy = By - Ay;
  LengthSquared = (Dx * Dx) + (Dy * Dy);

  if (LengthSquared == 0) {
    X = Px - Ax;
    Y = Py - Ay;
    return (UINTN)((X * X) + (Y * Y));
  }

  Projection = ((Px - Ax) * Dx) + ((Py - Ay) * Dy);
  if (Projection <= 0) {
    X = Px - Ax;
    Y = Py - Ay;
    return (UINTN)((X * X) + (Y * Y));
  }

  if (Projection >= LengthSquared) {
    X = Px - Bx;
    Y = Py - By;
    return (UINTN)((X * X) + (Y * Y));
  }

  Cross = ((Px - Ax) * Dy) - ((Py - Ay) * Dx);
  if (Cross < 0) {
    Cross = -Cross;
  }

  return (UINTN)(((UINTN)Cross * (UINTN)Cross) / (UINTN)LengthSquared);
}

STATIC
VOID
FpMenuRasterizeChevron (
  IN UINT8  *Alpha,
  IN UINTN   Size,
  IN UINTN   Thickness
  )
{
  INTN   SizeSigned;
  INTN   X1;
  INTN   Y1;
  INTN   X2;
  INTN   Y2;
  INTN   X3;
  INTN   Y3;
  INTN   Radius;
  UINTN  Row;
  UINTN  Col;
  UINTN  Index;

  if ((Alpha == NULL) || (Size == 0)) {
    return;
  }

  SizeSigned = (INTN)Size;
  Radius     = (INTN)MAX ((UINTN)2, Thickness / 2);

  X1 = (SizeSigned * 28) / 100;
  Y1 = (SizeSigned * 38) / 100;
  X2 = (SizeSigned * 55) / 100;
  Y2 = SizeSigned / 2;
  X3 = X1;
  Y3 = (SizeSigned * 62) / 100;

  for (Row = 0; Row < Size; Row++) {
    for (Col = 0; Col < Size; Col++) {
      Index = Row * Size + Col;
      if ((FpMenuPointSegmentDistanceSquared ((INTN)Col, (INTN)Row, X1, Y1, X2, Y2) <=
           (UINTN)(Radius * Radius)) ||
          (FpMenuPointSegmentDistanceSquared ((INTN)Col, (INTN)Row, X2, Y2, X3, Y3) <=
           (UINTN)(Radius * Radius)))
      {
        Alpha[Index] = 255;
      }
    }
  }
}

STATIC
VOID
FpMenuDrawChevron (
  IN FP_BOOT_MENU                         *Menu,
  IN UINTN                                 X,
  IN UINTN                                 Y,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Color,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Background
  )
{
  if ((Menu == NULL) || (Color == NULL) || (Background == NULL) ||
      (Menu->ChevronSize == 0))
  {
    return;
  }

  if ((Menu->ChevronAlpha == NULL) || (Menu->ChevronRasterSize != Menu->ChevronSize))
  {
    if (Menu->ChevronAlpha != NULL) {
      FreePool (Menu->ChevronAlpha);
    }

    Menu->ChevronAlpha = AllocateZeroPool (Menu->ChevronSize * Menu->ChevronSize);
    if (Menu->ChevronAlpha == NULL) {
      return;
    }

    FpMenuRasterizeChevron (Menu->ChevronAlpha, Menu->ChevronSize, MAX (3, Menu->ChevronSize / 5));
    Menu->ChevronRasterSize = Menu->ChevronSize;
  }

  FpMenuBlitAlpha (
    Menu,
    Menu->ChevronAlpha,
    Menu->ChevronSize,
    Menu->ChevronSize,
    X,
    Y,
    Color,
    Background
    );
}

STATIC
VOID
FpMenuDrainInput (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *SimpleTextInEx
  )
{
  EFI_KEY_DATA  KeyData;
  UINTN         Guard;

  if (SimpleTextInEx == NULL) {
    return;
  }

  //
  // Bounded: a stuck device that always reports a key must not hang the menu.
  //
  for (Guard = 0; Guard < 64; Guard++) {
    if (EFI_ERROR (SimpleTextInEx->ReadKeyStrokeEx (SimpleTextInEx, &KeyData))) {
      break;
    }
  }
}

STATIC
UINTN
FpMenuVisibleRows (
  IN FP_BOOT_MENU  *Menu
  )
{
  UINTN  Rows;

  Rows = (Menu->FooterTop > Menu->RowTop) ? ((Menu->FooterTop - Menu->RowTop) / Menu->RowHeight) : 1;
  if (Rows == 0) {
    Rows = 1;
  }

  return MIN (Rows, FP_BOOT_MENU_MAX_VISIBLE);
}

STATIC
VOID
FpMenuUpdateSelection (
  IN FP_BOOT_MENU  *Menu,
  IN UINTN          Count,
  IN OUT UINTN     *Selected,
  IN OUT UINTN     *Top
  )
{
  UINTN  Visible;

  Visible = FpMenuVisibleRows (Menu);
  if (*Selected < *Top) {
    *Top = *Selected;
  } else if (*Selected >= *Top + Visible) {
    *Top = *Selected - Visible + 1;
  }

  if (*Top + Visible > Count) {
    *Top = (Count > Visible) ? (Count - Visible) : 0;
  }
}

STATIC
UINTN
FpMenuTouchTarget (
  IN FP_BOOT_MENU                 *Menu,
  IN UINTN                         Count,
  IN UINTN                         Top,
  IN EFI_ABSOLUTE_POINTER_STATE   *Touch
  )
{
  UINTN  Row;
  UINTN  Visible;

  if ((Touch == NULL) || (Menu == NULL)) {
    return Count;
  }

  Visible = FpMenuVisibleRows (Menu);
  if ((Touch->CurrentX < Menu->MarginX) ||
      (Touch->CurrentX >= Menu->Hres - Menu->MarginX) ||
      (Touch->CurrentY < Menu->RowTop) ||
      (Touch->CurrentY >= Menu->RowTop + Visible * Menu->RowHeight))
  {
    return Count;
  }

  Row = Top + ((UINTN)Touch->CurrentY - Menu->RowTop) / Menu->RowHeight;
  if ((Row >= Count) || (Row >= Top + Visible)) {
    return Count;
  }

  return Row;
}

STATIC
UINTN
FpMenuHandleTouch (
  IN FP_BOOT_MENU                 *Menu,
  IN UINTN                         Count,
  IN OUT UINTN                    *Selected,
  IN OUT UINTN                    *Top,
  IN EFI_ABSOLUTE_POINTER_STATE   *Touch,
  IN UINT64                        Ms,
  IN OUT BOOLEAN                  *Dirty
  )
{
  BOOLEAN         Down;
  UINTN           Target;
  UINTN           TapTarget;
  UINTN           Visible;
  UINTN           MaxTop;
  INT64           Delta;
  INT64           NewTopSigned;
  UINTN           NewTop;
  UIT_TAP_RESULT  Tap;

  Down = SWM_IS_FINGER_DOWN (*Touch);
  Target = FpMenuTouchTarget (Menu, Count, *Top, Touch);
  TapTarget = (Target < Count) ? (Target + 1) : 0;

  if (Down && !Menu->TouchActive) {
    Menu->TouchActive  = TRUE;
    Menu->SwipeAllowed = (BOOLEAN)(Touch->CurrentX < Menu->Hres &&
                                   Touch->CurrentY >= Menu->RowTop &&
                                   Touch->CurrentY + Menu->RowFont + 20 < Menu->FooterTop);
    Menu->Dragging = FALSE;
    Menu->SwipeY   = Touch->CurrentY;
    Menu->SwipeTop = *Top;
  }

  if (Menu->TouchActive && Menu->SwipeAllowed &&
      UitTouchDistance (Touch->CurrentY, Menu->SwipeY) > MAX ((UINTN)20, (UINTN)Menu->RowFont / 2))
  {
    Menu->Dragging      = TRUE;
    Menu->Tap.Cancelled = TRUE;
    Menu->Tap.Armed     = FALSE;
  }

  if (Menu->TouchActive && Menu->Dragging) {
    Visible = FpMenuVisibleRows (Menu);
    if (Count > Visible) {
      MaxTop = Count - Visible;
      Delta  = (INT64)Menu->SwipeY - (INT64)MIN (Touch->CurrentY, Menu->Vres);
      NewTopSigned = (INT64)Menu->SwipeTop + Delta / (INT64)Menu->RowHeight;
      NewTop = (NewTopSigned < 0) ? 0 : MIN ((UINTN)NewTopSigned, MaxTop);
      if (*Top != NewTop) {
        *Top = NewTop;
        FpMenuUpdateSelection (Menu, Count, Selected, Top);
        Menu->Tap.Cancelled = TRUE;
        *Dirty = TRUE;
      }
    }

    if (!Down) {
      Menu->TouchActive = FALSE;
      Menu->Dragging    = FALSE;
      ZeroMem (&Menu->Tap, sizeof (Menu->Tap));
    }

    return 0;
  }

  Tap = UitTouchTap (
          &Menu->Tap,
          Down,
          TapTarget,
          Touch->CurrentX,
          Touch->CurrentY,
          Ms,
          MAX ((UINTN)24, Menu->Hres / 50)
          );

  if (!Down) {
    Menu->TouchActive = FALSE;
  }

  if (Tap == UitTapFocus && TapTarget != 0) {
    *Selected = TapTarget - 1;
    *Dirty    = TRUE;
  }

  return (Tap == UitTapConfirm && TapTarget != 0) ? TapTarget : 0;
}

STATIC
VOID
FpMenuDrawFooter (
  IN FP_BOOT_MENU  *Menu,
  IN BOOLEAN        TimeoutArmed,
  IN UINTN          SecondsLeft
  )
{
  CHAR16  Line[128];
  CHAR16  Hint[128];
  UINTN   Top;

  Top = Menu->FooterTop;
  FpMenuFill (Menu, &mMenuBg, 0, Top, Menu->Hres, Menu->Vres - Top);
  FpMenuFill (Menu, &mMenuLine, Menu->MarginX, Top, Menu->Hres - 2 * Menu->MarginX, 1);

  if (Menu->Status[0] != L'\0') {
    FpMenuText (Menu, Menu->Status, Menu->MarginX, Top, &mMenuWarn, &mMenuBg, Menu->RowFont);
  }

  UnicodeSPrint (Line, sizeof (Line), L"Power: select    Vol+/-: move");
  FpMenuText (Menu, Line, Menu->MarginX, Menu->FooterY, &mMenuHint, &mMenuBg, Menu->RowFont);

  if (TimeoutArmed) {
    UnicodeSPrint (
      Hint,
      sizeof (Hint),
      L"Double-tap: select    Auto GUI: %u s",
      SecondsLeft
      );
  } else {
    UnicodeSPrint (Hint, sizeof (Hint), L"Double-tap: select");
  }

  FpMenuText (
    Menu,
    Hint,
    Menu->MarginX,
    Menu->FooterY + Menu->RowFont + 10,
    &mMenuHint,
    &mMenuBg,
    Menu->RowFont
    );
}

STATIC
VOID
FpMenuCenterList (
  IN OUT FP_BOOT_MENU  *Menu,
  IN UINTN              Count
  )
{
  UINTN  Visible;
  UINTN  HeaderBottom;
  UINTN  Available;
  UINTN  ListBlockHeight;
  UINTN  ListTop;

  if ((Menu == NULL) || (Count == 0)) {
    return;
  }

  Visible       = MIN (FP_BOOT_MENU_MAX_VISIBLE, Count);
  HeaderBottom  = Menu->SubtitleY + Menu->SubFont + 26;
  if (Menu->FooterTop <= HeaderBottom) {
    return;
  }

  Available      = Menu->FooterTop - HeaderBottom;
  ListBlockHeight = Menu->SubFont + 16 + (Visible * Menu->RowHeight);
  if (ListBlockHeight < Available) {
    ListTop = HeaderBottom + ((Available - ListBlockHeight) / 2);
    if (ListTop > HeaderBottom + 24) {
      Menu->ListHeadY = ListTop;
      Menu->RowTop    = ListTop + Menu->SubFont + 16;
    }
  }
}

STATIC
VOID
FpMenuDrawCard (
  IN FP_BOOT_MENU  *Menu,
  IN FP_MENU_ENTRY *Entry,
  IN UINTN          Index,
  IN BOOLEAN        Selected
  )
{
  CHAR16                           Label[128];
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Background;
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Foreground;
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *IconColor;
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Border;
  UINTN                            X;
  UINTN                            Y;
  UINTN                            Width;
  UINTN                            Height;
  UINTN                            ChevronX;
  UINTN                            ChevronY;
  UINTN                            TextX;
  UINTN                            TextY;

  X      = Menu->MarginX;
  Y      = Menu->RowTop + Index * Menu->RowHeight;
  Width  = Menu->Hres - 2 * Menu->MarginX;
  Height = (Menu->RowHeight > Menu->CardGap) ? (Menu->RowHeight - Menu->CardGap) : Menu->RowHeight;

  Background = Selected ? &mMenuCardSelected : &mMenuCard;
  Foreground = Selected ? &mMenuFg : &mMenuFg;
  IconColor  = Selected ? &mMenuAccent : &mMenuMuted;
  Border     = Selected ? &mMenuAccent : &mMenuLine;

  FpMenuDrawRoundedBox (
    Menu,
    X,
    Y,
    Width,
    Height,
    Menu->CardRadius,
    Border,
    Background
    );

  if (Selected) {
    FpMenuFill (
      Menu,
      (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)&mMenuAccent,
      X + 1,
      Y + Menu->CardRadius,
      4,
      (Height > (2 * Menu->CardRadius)) ? (Height - (2 * Menu->CardRadius)) : 0
      );
  }

  FpMenuDrawIcon (
    Menu,
    FpMenuIcon (Entry),
    X + Menu->IconPad,
    Y + (Height - Menu->IconSize) / 2,
    IconColor,
    Background
    );

  ChevronX = X + Width - Menu->IconPad - Menu->ChevronSize;
  ChevronY = Y + (Height - Menu->ChevronSize) / 2;
  FpMenuDrawChevron (Menu, ChevronX, ChevronY, IconColor, Background);

  if ((Entry != NULL) && (Entry->Label != NULL)) {
    StrnCpyS (Label, sizeof (Label) / sizeof (Label[0]), Entry->Label, sizeof (Label) / sizeof (Label[0]) - 1);
  } else {
    StrnCpyS (Label, sizeof (Label) / sizeof (Label[0]), L"(unnamed)", sizeof (Label) / sizeof (Label[0]) - 1);
  }

  TextX = X + Menu->IconPad + Menu->IconSize + Menu->IconPad;
  TextY = Y + (Height - Menu->RowFont - Menu->SubFont - 3) / 2;
  FpMenuText (Menu, Label, TextX, TextY, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)Foreground, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)Background, Menu->RowFont);
  FpMenuText (
    Menu,
    FpMenuSubLabel (Entry),
    TextX,
    TextY + Menu->RowFont + 3,
    (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)&mMenuMuted,
    (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)Background,
    Menu->SubFont
    );
}

STATIC
VOID
FpMenuDrawChrome (
  IN FP_BOOT_MENU  *Menu
  )
{
  FpMenuFill (Menu, &mMenuBg, 0, 0, Menu->Hres, Menu->Vres);
  FpMenuText (Menu, L"SYSTEM STARTUP", Menu->MarginX, Menu->HeaderTop, &mMenuAccent, &mMenuBg, Menu->SubFont);
  FpMenuText (Menu, L"Boot Menu", Menu->MarginX, Menu->TitleY, &mMenuFg, &mMenuBg, Menu->TitleFont);
  FpMenuText (Menu, L"Choose a boot target.", Menu->MarginX, Menu->SubtitleY, &mMenuMuted, &mMenuBg, Menu->SubFont);
  FpMenuText (Menu, L"AVAILABLE TARGETS", Menu->MarginX, Menu->ListHeadY, &mMenuHint, &mMenuBg, Menu->SubFont);
}

STATIC
VOID
FpMenuDrawCards (
  IN FP_BOOT_MENU  *Menu,
  IN FP_MENU_ENTRY *Entries,
  IN UINTN          Count,
  IN UINTN          Selected,
  IN UINTN          Top
  )
{
  UINTN  Index;
  UINTN  Visible;

  Visible = MIN (FpMenuVisibleRows (Menu), (Count > Top) ? (Count - Top) : 0);
  for (Index = 0; Index < Visible; Index++) {
    FpMenuDrawCard (Menu, &Entries[Top + Index], Index, (Top + Index) == Selected);
  }
}

STATIC
VOID
FpMenuRedrawSelection (
  IN FP_BOOT_MENU  *Menu,
  IN FP_MENU_ENTRY *Entries,
  IN UINTN          Count,
  IN UINTN          OldTop,
  IN UINTN          OldSelected,
  IN UINTN          NewTop,
  IN UINTN          NewSelected
  )
{
  UINTN  Visible;

  if (OldTop != NewTop) {
    // The visible window moved. Redraw the whole card stack; the static chrome
    // does not need to be repainted here.
    FpMenuDrawCards (Menu, Entries, Count, NewSelected, NewTop);
    return;
  }

  if (OldSelected == NewSelected) {
    return;
  }

  Visible = FpMenuVisibleRows (Menu);
  if ((OldSelected >= OldTop) && (OldSelected < OldTop + Visible)) {
    FpMenuDrawCard (Menu, &Entries[OldSelected], OldSelected - OldTop, FALSE);
  }

  if ((NewSelected >= NewTop) && (NewSelected < NewTop + Visible)) {
    FpMenuDrawCard (Menu, &Entries[NewSelected], NewSelected - NewTop, TRUE);
  }
}

STATIC
VOID
FpMenuDraw (
  IN FP_BOOT_MENU  *Menu,
  IN FP_MENU_ENTRY *Entries,
  IN UINTN          Count,
  IN UINTN          Selected,
  IN UINTN          Top,
  IN BOOLEAN        TimeoutArmed,
  IN UINTN          SecondsLeft
  )
{
  FpMenuDrawChrome (Menu);
  FpMenuDrawCards (Menu, Entries, Count, Selected, Top);
  FpMenuDrawFooter (Menu, TimeoutArmed, SecondsLeft);
}

STATIC
EFI_STATUS
FpMenuStartShell (
  IN EFI_HANDLE        ImageHandle,
  IN CONST EFI_GUID   *ShellGuid,
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop
  )
{
  EFI_STATUS  Status;
  VOID        *Section      = NULL;
  UINTN       SectionSize   = 0;
  EFI_HANDLE  ShellHandle   = NULL;

  Status = GetSectionFromAnyFv (
             ShellGuid,
             EFI_SECTION_PE32,
             0,
             &Section,
             &SectionSize
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LoadImage (
                  TRUE,
                  ImageHandle,
                  NULL,
                  Section,
                  SectionSize,
                  &ShellHandle
                  );
  if (!EFI_ERROR (Status)) {
    EFI_GUID UsbGuid=USB_BRINGUP_PROTOCOL_GUID;
    USB_BRINGUP_PROTOCOL *Usb=NULL;BOOLEAN CdcAttached=FALSE;
    Status=FpShellDisplayBegin(Gop);
    if (EFI_ERROR(Status)) { gBS->UnloadImage(ShellHandle);FreePool(Section);return Status; }
    if (!EFI_ERROR(gBS->LocateProtocol(&UsbGuid,NULL,(VOID **)&Usb)) &&
        Usb!=NULL && Usb->Revision>=5 && Usb->SetShellConsole!=NULL)
      CdcAttached=!EFI_ERROR(Usb->SetShellConsole(Usb,TRUE));
    Status = gBS->StartImage (ShellHandle, NULL, NULL);
    if (CdcAttached) (VOID)Usb->SetShellConsole(Usb,FALSE);
    FpShellDisplayEnd();
    gBS->UnloadImage (ShellHandle);
  }

  FreePool (Section);
  return Status;
}

STATIC
UINTN
FpMenuPrepare (
  IN FP_MENU_ENTRY                     *Entries,
  IN UINTN                              MainCapacity,
  IN UINTN                              MoreCapacity,
  IN BOOLEAN                            MoreLevel,
  IN EFI_BOOT_MANAGER_LOAD_OPTION      *BootOptions,
  IN UINTN                              BootCount
  )
{
  UINTN       Count;
  UINTN       Index;
  UINTN       AndroidIndex;
  UINTN       RecoveryIndex;
  FP_MENU_ENTRY *Entry;

  Count = 0;
  AndroidIndex = BootCount;
  RecoveryIndex = BootCount;

  if (BootOptions != NULL) {
    for (Index = 0; Index < BootCount; Index++) {
      if ((AndroidIndex == BootCount) &&
          (BootOptions[Index].Description != NULL) &&
          (StrStr (BootOptions[Index].Description, L"Android") != NULL))
      {
        AndroidIndex = Index;
      }

      if ((RecoveryIndex == BootCount) &&
          (BootOptions[Index].Description != NULL) &&
          (StrStr (BootOptions[Index].Description, L"Recovery") != NULL))
      {
        RecoveryIndex = Index;
      }
    }
  }

  if (!MoreLevel) {
    Entry = Entries;
    if (MainCapacity < 6) {
      return 0;
    }

    Entry[Count].Label = L"GUI";
    Entry[Count].Type  = FpMenuEntryGui;
    Count++;

    Entry[Count].Label        = L"Android";
    Entry[Count].Type         = FpMenuEntryAndroid;
    Entry[Count].OptionIndex  = AndroidIndex;
    Count++;

    Entry[Count].Label        = L"Recovery";
    Entry[Count].Type         = FpMenuEntryRecovery;
    Entry[Count].OptionIndex  = RecoveryIndex;
    Count++;

    Entry[Count].Label = L"UEFI Shell";
    Entry[Count].Type  = FpMenuEntryShell;
    Count++;

    Entry[Count].Label = L"Reboot to bootloader";
    Entry[Count].Type  = FpMenuEntryBootloader;
    Count++;

    Entry[Count].Label = L"More boot options...";
    Entry[Count].Type  = FpMenuEntryMore;
    Count++;
    return Count;
  }

  Entry = Entries;
  for (Index = 0; Index < BootCount; Index++) {
    if (BootOptions != NULL && BootOptions[Index].Description != NULL &&
        FpMenuIsShellOption (BootOptions[Index].Description))
    {
      continue;
    }

    if (Count >= MoreCapacity) {
      break;
    }

    Entry[Count].Label = (BootOptions != NULL && BootOptions[Index].Description != NULL)
                           ? BootOptions[Index].Description
                           : L"(unnamed boot option)";
    Entry[Count].Type        = FpMenuEntryBootOption;
    Entry[Count].OptionIndex = Index;
    Count++;
  }

  if (MoreCapacity > Count) {
    Entry[Count].Label = L"Reboot";
    Entry[Count].Type  = FpMenuEntryReboot;
    Count++;
  }

  if (MoreCapacity > Count) {
    Entry[Count].Label = L"Back";
    Entry[Count].Type  = FpMenuEntryBack;
    Count++;
  }

  return Count;
}

STATIC
BOOLEAN
FpMenuBootLoadOption (
  IN EFI_BOOT_MANAGER_LOAD_OPTION       *BootOption,
  IN BOOLEAN                             Cdc
  )
{
  EFI_GUID               UsbGuid = USB_BRINGUP_PROTOCOL_GUID;
  USB_BRINGUP_PROTOCOL   *Usb;
  BOOLEAN                CdcAttached;

  Usb         = NULL;
  CdcAttached = FALSE;

  if (Cdc &&
      !EFI_ERROR (gBS->LocateProtocol (&UsbGuid, NULL, (VOID **)&Usb)) &&
      (Usb != NULL) &&
      (Usb->Revision >= 5) &&
      (Usb->SetShellConsole != NULL))
  {
    CdcAttached = !EFI_ERROR (Usb->SetShellConsole (Usb, TRUE));
  }

  EfiBootManagerBoot (BootOption);

  if (CdcAttached) {
    (VOID)Usb->SetShellConsole (Usb, FALSE);
  }

  return FALSE;
}

STATIC
BOOLEAN
FpMenuActivate (
  IN FP_BOOT_MENU                       *Menu,
  IN FP_MENU_ENTRY                      *Entries,
  IN UINTN                               Count,
  IN UINTN                               Selected,
  IN UINTN                               Top,
  IN EFI_BOOT_MANAGER_LOAD_OPTION       *BootOptions,
  IN UINTN                               BootCount,
  IN EFI_HANDLE                          ImageHandle,
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *SimpleTextInEx,
  IN OUT UINTN                          *MenuLevel
  )
{
  EFI_STATUS                    Status;
  FirmwareSuppressPower(500);

  switch (Entries[Selected].Type) {
    case FpMenuEntryGui:
      return TRUE;

    case FpMenuEntryReboot:
      gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
      return FALSE;

    case FpMenuEntryMore:
      *MenuLevel = 1;
      return FALSE;

    case FpMenuEntryBack:
      *MenuLevel = 0;
      return FALSE;

    case FpMenuEntryAndroid:
    case FpMenuEntryRecovery:
    case FpMenuEntryBootloader:
      if (Entries[Selected].Type != FpMenuEntryAndroid) {
        Status = FpPrepareReboot (Entries[Selected].Type==FpMenuEntryBootloader ?
                                 FRONT_PAGE_ACTION_REBOOT_FASTBOOT : FRONT_PAGE_ACTION_REBOOT_RECOVERY);
        if (!EFI_ERROR (Status)) {
          Status = FpRebootSelected ();
        }

        FpRebootErrorText (Menu->Status, sizeof (Menu->Status), Status);
      } else {
        // Android boots through the resident ABL on the next normal cold boot.
        // No UFS/BCB write is used; ABL selects the active slot (boot_a/boot_b).
        gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
        UnicodeSPrint (Menu->Status, sizeof (Menu->Status), L"Reset failed");
      }

      FpMenuDrainInput (SimpleTextInEx);
      FpMenuDraw (Menu, Entries, Count, Selected, Top, FALSE, 0);
      return FALSE;

    case FpMenuEntryShell:
      Menu->Status[0] = L'\0';
      Menu->Swm->ActivateWindow(Menu->Swm,FpMenuHandle(Menu),FALSE);
      Status=FpMenuStartShell(ImageHandle,&mShellFileGuid,Menu->Gop);
      Menu->Swm->ActivateWindow(Menu->Swm,FpMenuHandle(Menu),TRUE);
      Menu->Swm->EnableMousePointer(Menu->Swm,FALSE);
      FirmwareSuppressPower(500);

      if (EFI_ERROR (Status)) {
        UnicodeSPrint (Menu->Status, sizeof (Menu->Status), L"Shell failed: %r", Status);
      }

      FpMenuDrainInput (SimpleTextInEx);
      FpMenuDraw (Menu, Entries, Count, Selected, Top, FALSE, 0);
      return FALSE;

    case FpMenuEntryBootOption:
      if ((BootOptions == NULL) || (Entries[Selected].OptionIndex >= BootCount)) {
        UnicodeSPrint (Menu->Status, sizeof (Menu->Status), L"Boot target unavailable");
        FpMenuDraw (Menu, Entries, Count, Selected, Top, FALSE, 0);
        return FALSE;
      }

      Menu->Status[0] = L'\0';
      (VOID)FpMenuBootLoadOption (
              &BootOptions[Entries[Selected].OptionIndex],
              FpMenuIsShellOption (Entries[Selected].Label)
              );
      UnicodeSPrint (Menu->Status, sizeof (Menu->Status), L"Boot failed");
      FpMenuDrainInput (SimpleTextInEx);
      FpMenuDraw (Menu, Entries, Count, Selected, Top, FALSE, 0);
      return FALSE;

    default:
      return FALSE;
  }
}

EFI_STATUS
EFIAPI
FpBootMenuShow (
  IN MS_SIMPLE_WINDOW_MANAGER_PROTOCOL  *Swm,
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL       *Gop,
  IN EFI_HANDLE                          ImageHandle,
  IN BOOLEAN                             AutoGui
  )
{
  EFI_STATUS                         Status;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *SimpleTextInEx;
  EFI_BOOT_MANAGER_LOAD_OPTION       *BootOptions;
  UINTN                              BootCount;
  UINTN                              AllocatedBootCount;
  FP_MENU_ENTRY                      *Entries;
  UINTN                              Count;
  UINTN                              MaxEntries;
  UINTN                              MainCapacity;
  UINTN                              MoreCapacity;
  FP_BOOT_MENU                       Menu;
  UINTN                              Selected;
  UINTN                              Top;
  UINTN                              SecondsLeft;
  UINTN                              LastSeconds;
  UINTN                              FooterHeight;
  UINTN                              MinMargin;
  UINTN                              ContentWidth;
  UINTN                              Action;
  BOOLEAN                            TimeoutArmed;
  BOOLEAN                            Redraw;
  BOOLEAN                            FullRedraw;
  BOOLEAN                            ViewChanged;
  UINTN                              PrevSelected;
  UINTN                              PrevTop;
  UINTN                              LastLevel;
  UINT64                             StartNs;
  UINT64                             NowNs;
  EFI_KEY_DATA                       KeyData;
  EFI_ABSOLUTE_POINTER_STATE         Touch;

  if ((Swm == NULL) || (Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Swm->EnableMousePointer(Swm,FALSE);
  FirmwareSuppressPower(500);

  BootOptions        = NULL;
  BootCount          = 0;
  AllocatedBootCount = 0;
  Entries            = NULL;

  SimpleTextInEx = NULL;
  if ((gST->ConsoleInHandle != NULL) &&
      !EFI_ERROR (gBS->HandleProtocol (
                    gST->ConsoleInHandle,
                    &gEfiSimpleTextInputExProtocolGuid,
                    (VOID **)&SimpleTextInEx
                    )))
  {
  } else {
    SimpleTextInEx = NULL;
  }

  ZeroMem (&Menu, sizeof (Menu));
  Menu.Swm         = Swm;
  Menu.Gop         = Gop;
  Menu.ImageHandle = ImageHandle;
  Menu.Hres        = Gop->Mode->Info->HorizontalResolution;
  Menu.Vres        = Gop->Mode->Info->VerticalResolution;
  Menu.SubFont     = MsUiGetSmallFontHeight ();
  Menu.TitleFont   = MsUiGetLargeFontHeight ();
  Menu.RowFont     = MsUiGetMediumFontHeight ();

  MinMargin   = MAX ((UINTN)24, Menu.Hres / 36);
  ContentWidth = (Menu.Hres > (2 * MinMargin))
                  ? MIN (Menu.Hres - (2 * MinMargin), (UINTN)1080)
                  : Menu.Hres;
  Menu.MarginX = (Menu.Hres > ContentWidth) ? ((Menu.Hres - ContentWidth) / 2) : 0;

  Menu.HeaderTop = MAX ((UINTN)24, Menu.Vres / 32);
  Menu.TitleY      = Menu.HeaderTop + Menu.SubFont + 12;
  Menu.SubtitleY   = Menu.TitleY + Menu.TitleFont + 8;
  Menu.ListHeadY   = Menu.SubtitleY + Menu.SubFont + 26;
  Menu.RowTop      = Menu.ListHeadY + Menu.SubFont + 16;

  if (Menu.Hres > Menu.Vres) {
    Menu.RowHeight = MAX (Menu.RowFont + Menu.SubFont + 32,
                          MIN (Menu.Vres / 4, (UINTN)136));
    Menu.IconSize  = MIN (
                       FP_BOOT_MENU_ICON_RASTER_SIZE,
                       MAX (FP_BOOT_MENU_ICON_MIN_SIZE,
                            (Menu.RowHeight > 24) ? (Menu.RowHeight - 24) : FP_BOOT_MENU_ICON_MIN_SIZE)
                       );
  } else {
    Menu.RowHeight = MAX (Menu.RowFont + Menu.SubFont + 96,
                          Menu.Vres / 11);
    Menu.IconSize  = FP_BOOT_MENU_ICON_PORTRAIT_SIZE;
  }

  Menu.IconPad      = MAX (FP_BOOT_MENU_ICON_MIN_PAD,
                           MIN ((UINTN)30, Menu.IconSize / 3));
  Menu.ChevronSize  = MAX (FP_BOOT_MENU_CHEVRON_MIN_SIZE,
                           MIN (FP_BOOT_MENU_CHEVRON_MAX_SIZE, (Menu.IconSize * 2) / 3));
  Menu.CardRadius   = MAX (FP_BOOT_MENU_CARD_RADIUS_MIN,
                           MIN (FP_BOOT_MENU_CARD_RADIUS_MAX, Menu.IconSize / 4));
  Menu.CardGap  = MAX (FP_BOOT_MENU_CARD_GAP, Menu.Vres / 220);
  Menu.Status[0] = L'\0';

  Menu.TextOut = AllocateZeroPool (sizeof (EFI_IMAGE_OUTPUT));
  if (Menu.TextOut == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Menu.TextOut->Width        = (UINT16)Menu.Hres;
  Menu.TextOut->Height       = (UINT16)Menu.Vres;
  Menu.TextOut->Image.Screen = Gop;

  if (Menu.Hres > Menu.Vres) {
    FooterHeight = MAX (Menu.RowFont + Menu.SubFont + 64,
                        MIN (Menu.Vres / 4, (UINTN)170));
  } else {
    FooterHeight = MAX (Menu.RowFont + Menu.SubFont + 64, Menu.Vres / 14);
    FooterHeight = MIN (FooterHeight, (UINTN)260);
  }

  if (Menu.Vres > Menu.RowTop + Menu.RowHeight + FooterHeight) {
    Menu.FooterTop = Menu.Vres - FooterHeight;
  } else {
    Menu.FooterTop = Menu.RowTop + Menu.RowHeight;
  }

  Menu.FooterY = Menu.FooterTop + 12;
  if ((2 * Menu.RowFont + 10) < FooterHeight) {
    Menu.FooterY += (FooterHeight - (2 * Menu.RowFont + 10)) / 2;
  }
  if (Menu.FooterY < Menu.FooterTop + 12) {
    Menu.FooterY = Menu.FooterTop + 12;
  }

  //
  // Register a full-screen client window.  Doing this through SWM gives us
  // the same aggregated AbsolutePointer protocol used by the rest of the UI.
  // If registration fails, the menu remains usable through the keypad.
  //
  SWM_RECT_INIT2 (Menu.Frame, 0, 0, Menu.Hres, Menu.Vres);
  Status = gBS->InstallProtocolInterface (
                  &Menu.Window,
                  &mBootMenuWindowGuid,
                  EFI_NATIVE_INTERFACE,
                  &Menu
                  );
  if (!EFI_ERROR (Status)) {
    Status = Swm->RegisterClient (
                    Swm,
                    Menu.Window,
                    SWM_Z_ORDER_CLIENT,
                    &Menu.Frame,
                    FpMenuNotify,
                    &Menu,
                    &Menu.Pointer,
                    &Menu.PaintEvent
                    );
    if (!EFI_ERROR (Status)) {
      Menu.TouchClient = TRUE;
      Swm->ActivateWindow (Swm, Menu.Window, TRUE);
    } else {
      gBS->UninstallProtocolInterface (Menu.Window, &mBootMenuWindowGuid, &Menu);
      Menu.Window = NULL;
    }
  }

  //
  // Give the user something to look at while the boot manager scans devices.
  //
  FpMenuFill (&Menu, &mMenuBg, 0, 0, Menu.Hres, Menu.Vres);
  FpMenuText (&Menu, L"Boot Menu", Menu.MarginX, Menu.TitleY, &mMenuFg, &mMenuBg, Menu.TitleFont);
  FpMenuText (&Menu, L"Scanning boot options...", Menu.MarginX, Menu.RowTop, &mMenuHint, &mMenuBg, Menu.RowFont);

  //
  // Dynamic list: BootOrder as it stands now, not a build-time list.
  // Boot options are already persisted by BDS, so read them directly instead
  // of re-scanning every UFS partition on each menu open. Only rebuild when the
  // variable store has no boot entries yet.
  //
  BootOptions = EfiBootManagerGetLoadOptions (&BootCount, LoadOptionTypeBoot);
  if (BootOptions == NULL) {
    EfiBootManagerRefreshAllBootOption ();
    BootOptions = EfiBootManagerGetLoadOptions (&BootCount, LoadOptionTypeBoot);
  }

  if (BootOptions == NULL) {
    BootCount = 0;
  }

  AllocatedBootCount = BootCount;
  if (BootCount > FP_BOOT_MENU_MAX_BOOT_OPTIONS) {
    BootCount = FP_BOOT_MENU_MAX_BOOT_OPTIONS;
  }

  MainCapacity = 6;
  MoreCapacity = BootCount + 2;
  MaxEntries   = MainCapacity + MoreCapacity;
  Entries    = AllocateZeroPool (MaxEntries * sizeof (*Entries));
  if (Entries == NULL) {
    if (BootOptions != NULL) {
      EfiBootManagerFreeLoadOptions (BootOptions, AllocatedBootCount);
    }

    FreePool (Menu.TextOut);
    return EFI_OUT_OF_RESOURCES;
  }

  Menu.MenuLevel = 0;
  Count = FpMenuPrepare (
            Entries,
            MainCapacity,
            MoreCapacity,
            FALSE,
            BootOptions,
            BootCount
            );

  FpMenuCenterList (&Menu, Count);

  Selected     = 0;
  Top          = 0;
  TimeoutArmed = AutoGui;
  LastSeconds  = FP_BOOT_MENU_TIMEOUT_SECONDS + 1;
  Menu.TouchActive  = FALSE;
  Menu.SwipeAllowed = FALSE;
  Menu.Dragging     = FALSE;
  Menu.SwipeY       = 0;
  Menu.SwipeTop     = Top;
  ZeroMem (&Menu.Tap, sizeof (Menu.Tap));
  LastLevel = Menu.MenuLevel;

  //
  // The press that started the phone may still be queued; do not let it pick
  // an entry.
  //
  FpMenuDrainInput (SimpleTextInEx);

  FpMenuDraw (&Menu, Entries, Count, Selected, Top, TimeoutArmed, FP_BOOT_MENU_TIMEOUT_SECONDS);

  StartNs = GetTimeInNanoSecond (GetPerformanceCounter ());
  SecondsLeft = FP_BOOT_MENU_TIMEOUT_SECONDS;
  while (TRUE) {
    FpServiceEvents();
    NowNs = GetTimeInNanoSecond (GetPerformanceCounter ());

    if (TimeoutArmed) {
      SecondsLeft = (NowNs - StartNs >= (UINT64)FP_BOOT_MENU_TIMEOUT_SECONDS * 1000000000ULL) ? 0 :
                    FP_BOOT_MENU_TIMEOUT_SECONDS - (UINTN)((NowNs - StartNs) / 1000000000ULL);
      if (SecondsLeft != LastSeconds) {
        LastSeconds = SecondsLeft;
        FpMenuDrawFooter (&Menu, TRUE, SecondsLeft);
      }
    }

    Redraw       = FALSE;
    FullRedraw   = FALSE;
    Action       = 0;
    PrevSelected = Selected;
    PrevTop      = Top;

    if ((Menu.PaintEvent != NULL) && (gBS->CheckEvent (Menu.PaintEvent) == EFI_SUCCESS)) {
      Redraw     = TRUE;
      FullRedraw = TRUE;
    }

    if (SimpleTextInEx != NULL) {
      Status = SimpleTextInEx->ReadKeyStrokeEx (SimpleTextInEx, &KeyData);
      if (Status == EFI_SUCCESS) {
        if (TimeoutArmed) {
          TimeoutArmed = FALSE;
          FpMenuDrawFooter (&Menu, FALSE, 0);
        }

        if (KeyData.Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
          Action = Selected + 1;
        } else if (KeyData.Key.ScanCode == SCAN_UP) {
          Selected = (Selected == 0) ? (Count - 1) : (Selected - 1);
          Redraw   = TRUE;
        } else if (KeyData.Key.ScanCode == SCAN_DOWN) {
          Selected = (Selected + 1) % Count;
          Redraw   = TRUE;
        } else if (KeyData.Key.ScanCode == SCAN_PAGE_UP) {
          UINTN  VisibleRows;

          VisibleRows = FpMenuVisibleRows (&Menu);
          Selected = (Selected > VisibleRows) ? (Selected - VisibleRows) : 0;
          Redraw   = TRUE;
        } else if (KeyData.Key.ScanCode == SCAN_PAGE_DOWN) {
          UINTN  VisibleRows;
          UINTN  Step;

          VisibleRows = FpMenuVisibleRows (&Menu);
          Step        = (VisibleRows > 0) ? VisibleRows : 1;
          Selected    = MIN (Count - 1, Selected + Step);
          Redraw      = TRUE;
        } else {
          Redraw = TRUE;
        }
      }
    }

    if (Menu.Pointer != NULL) {
      Status = Menu.Pointer->GetState (Menu.Pointer, &Touch);
      if (Status == EFI_SUCCESS) {
        UINTN   TouchAction;
        BOOLEAN TouchDirty;

        TouchDirty  = FALSE;
        TouchAction = FpMenuHandleTouch (
                        &Menu,
                        Count,
                        &Selected,
                        &Top,
                        &Touch,
                        NowNs / 1000000ULL,
                        &TouchDirty
                        );

        if (TimeoutArmed && (Touch.ActiveButtons != 0 || Menu.TouchActive || TouchDirty || TouchAction != 0)) {
          TimeoutArmed = FALSE;
          FpMenuDrawFooter (&Menu, FALSE, 0);
        }

        if (TouchAction != 0) {
          Action = TouchAction;
        }

        if (TouchDirty) {
          if (TimeoutArmed) {
            TimeoutArmed = FALSE;
          }

          Redraw = TRUE;
        }
      }
    }

    if (Action != 0) {
      if (FpMenuActivate (
            &Menu,
            Entries,
            Count,
            Action - 1,
            Top,
            BootOptions,
            BootCount,
            ImageHandle,
            SimpleTextInEx,
            &Menu.MenuLevel
            ))
      {
        goto Done;
      }

      if (Menu.MenuLevel != LastLevel) {
        ViewChanged = TRUE;
        LastLevel = Menu.MenuLevel;
        Count = FpMenuPrepare (
                  Entries,
                  MainCapacity,
                  MoreCapacity,
                  (Menu.MenuLevel != 0),
                  BootOptions,
                  BootCount
                  );
        Selected = 0;
        Top      = 0;
        TimeoutArmed = FALSE;
        if (TimeoutArmed) {
          StartNs = GetTimeInNanoSecond (GetPerformanceCounter ());
          LastSeconds = FP_BOOT_MENU_TIMEOUT_SECONDS + 1;
        }
        Menu.TouchActive  = FALSE;
        Menu.SwipeAllowed = FALSE;
        Menu.Dragging     = FALSE;
        Menu.SwipeY       = 0;
        Menu.SwipeTop     = Top;
        ZeroMem (&Menu.Tap, sizeof (Menu.Tap));
        Redraw = TRUE;
      } else {
        ViewChanged = FALSE;
      }

      if (!ViewChanged) {
        Selected = Action - 1;
      }
      Redraw = TRUE;
      FpMenuUpdateSelection (&Menu, Count, &Selected, &Top);
      if (ViewChanged) {
        FpMenuDraw (&Menu, Entries, Count, Selected, Top, TimeoutArmed, 0);
      } else {
        FpMenuRedrawSelection (&Menu, Entries, Count, PrevTop, PrevSelected, Top, Selected);
        FpMenuDrawFooter (&Menu, TimeoutArmed, 0);
      }
      continue;
    }

    if (Redraw) {
      FpMenuUpdateSelection (&Menu, Count, &Selected, &Top);
      if (FullRedraw) {
        FpMenuDraw (&Menu, Entries, Count, Selected, Top, TimeoutArmed, 0);
      } else {
        FpMenuRedrawSelection (&Menu, Entries, Count, PrevTop, PrevSelected, Top, Selected);
      }
    }

    if (TimeoutArmed && SecondsLeft == 0) break;
    FpServiceDelay (FP_BOOT_MENU_POLL_US);
  }

Done:
  FreePool (Entries);
  if (BootOptions != NULL) {
    EfiBootManagerFreeLoadOptions (BootOptions, AllocatedBootCount);
  }

  if (Menu.TouchClient && (Menu.Window != NULL)) {
    Swm->ActivateWindow (Swm, Menu.Window, FALSE);
    Swm->UnregisterClient (Swm, Menu.Window);
    gBS->UninstallProtocolInterface (Menu.Window, &mBootMenuWindowGuid, &Menu);
  }

  if (Menu.StringInfo != NULL) {
    FreePool (Menu.StringInfo);
  }

  if (Menu.IconAlpha != NULL) {
    FreePool (Menu.IconAlpha);
  }

  if (Menu.ChevronAlpha != NULL) {
    FreePool (Menu.ChevronAlpha);
  }

  if (Menu.BlitBuffer != NULL) {
    FreePool (Menu.BlitBuffer);
  }

  FreePool (Menu.TextOut);
  return EFI_SUCCESS;
}
