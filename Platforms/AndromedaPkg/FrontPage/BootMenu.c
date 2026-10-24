/** @file

  Pre-GUI boot menu.

  BDS uses FrontPage as its boot-manager menu, so this runs before any of the
  form/setup stack exists. Everything is drawn through the Simple Window
  Manager (BltWindow for the background, StringToWindow for text) - the same
  graphics path the rest of the UI uses - and keys come from the console input
  device the keypad driver feeds: volume up/down arrive as SCAN_UP/SCAN_DOWN,
  the power key as Enter.

  The first entry is "Enter GUI", which is also what the 10 second timeout
  selects. The rest is refreshed from the boot manager on every boot
  (EfiBootManagerRefreshAllBootOption) so the list follows BootOrder instead of
  a hard-coded set of entries.

  Copyright (C) Microsoft Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Uefi/UefiInternalFormRepresentation.h>
#include <Pi/PiFirmwareFile.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/SimpleWindowManager.h>

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

#include "BootMenu.h"

#define FP_BOOT_MENU_TIMEOUT_SECONDS   10
#define FP_BOOT_MENU_POLL_US           (50 * 1000)
#define FP_BOOT_MENU_MAX_VISIBLE       9
#define FP_BOOT_MENU_MAX_BOOT_OPTIONS  32

//
// 7C04A583-9E3E-4F1C-AD65-E05268D0B4D1, from ShellPkg.dec. Declared locally so
// this application does not need ShellPkg in its package list.
//
STATIC CONST EFI_GUID  mShellFileGuid = {
  0x7c04a583, 0x9e3e, 0x4f1c, { 0xad, 0x65, 0xe0, 0x52, 0x68, 0xd0, 0xb4, 0xd1 }
};

//
// The stock debug shell from this phone's firmware (the 319 KB module). Declared here
// for the same reason as the one above - no ShellPkg dependency needed.
//
STATIC CONST EFI_GUID  mStockShellFileGuid = {
  0x8af09f13, 0x44c5, 0x96ec, { 0x14, 0x37, 0xdd, 0x89, 0x9c, 0xb5, 0xee, 0x5d }
};

typedef enum {
  FpMenuEntryGui,
  FpMenuEntryBootOption,
  FpMenuEntryShell,
  FpMenuEntryShellStock,
  FpMenuEntryReboot
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
  EFI_IMAGE_OUTPUT                   *TextOut;
  UINTN                               Hres;
  UINTN                               Vres;
  UINTN                               MarginX;
  UINTN                               TitleY;
  UINTN                               RowTop;
  UINTN                               RowHeight;
  UINTN                               FooterY;
  UINT16                              TitleFont;
  UINT16                              RowFont;
  CHAR16                              Status[128];
} FP_BOOT_MENU;

//
// EFI_GRAPHICS_OUTPUT_BLT_PIXEL is Blue/Green/Red/Reserved.
//
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuBg   = { 0x00, 0x00, 0x00, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuFg   = { 0xFF, 0xFF, 0xFF, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuSel  = { 0xA0, 0x40, 0x00, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuHint = { 0x80, 0x80, 0x80, 0x00 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mMenuWarn = { 0x00, 0x00, 0xFF, 0x00 };

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
               Menu->ImageHandle,
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

  StringInfo = AllocateZeroPool (sizeof (*StringInfo));
  if (StringInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  StringInfo->FontInfoMask           = EFI_FONT_INFO_ANY_FONT;
  StringInfo->FontInfo.FontSize      = FontSize;
  StringInfo->FontInfo.FontStyle     = EFI_HII_FONT_STYLE_NORMAL;
  CopyMem (&StringInfo->ForegroundColor, Foreground, sizeof (*Foreground));
  CopyMem (&StringInfo->BackgroundColor, Background, sizeof (*Background));

  Status = Menu->Swm->StringToWindow (
                        Menu->Swm,
                        Menu->ImageHandle,
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

  FreePool (StringInfo);
  return Status;
}

STATIC
VOID
FpMenuDrainInput (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *SimpleTextInEx
  )
{
  EFI_KEY_DATA  KeyData;
  UINTN         Guard;

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

  Rows = (Menu->FooterY > Menu->RowTop) ? ((Menu->FooterY - Menu->RowTop) / Menu->RowHeight) : 1;
  if (Rows == 0) {
    Rows = 1;
  }

  return MIN (Rows, FP_BOOT_MENU_MAX_VISIBLE);
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
  UINTN   Top;

  Top = (Menu->FooterY > Menu->RowHeight) ? (Menu->FooterY - Menu->RowHeight) : 0;
  FpMenuFill (Menu, &mMenuBg, 0, Top, Menu->Hres, Menu->Vres - Top);

  if (Menu->Status[0] != L'\0') {
    FpMenuText (Menu, Menu->Status, Menu->MarginX, Top, &mMenuWarn, &mMenuBg, Menu->RowFont);
  }

  if (TimeoutArmed) {
    UnicodeSPrint (
      Line,
      sizeof (Line),
      L"Power: select    Vol+/-: move    Auto GUI: %u s",
      SecondsLeft
      );
  } else {
    UnicodeSPrint (Line, sizeof (Line), L"Power: select    Vol+/-: move");
  }

  FpMenuText (Menu, Line, Menu->MarginX, Menu->FooterY, &mMenuHint, &mMenuBg, Menu->RowFont);
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
  UINTN                             Index;
  UINTN                             Row;
  UINTN                             Visible;
  CONST CHAR16                      *Label;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *Background;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *Foreground;

  FpMenuFill (Menu, &mMenuBg, 0, 0, Menu->Hres, Menu->Vres);
  FpMenuText (Menu, L"Boot Menu", Menu->MarginX, Menu->TitleY, &mMenuFg, &mMenuBg, Menu->TitleFont);

  Visible = MIN (FpMenuVisibleRows (Menu), Count - Top);
  for (Index = 0; Index < Visible; Index++) {
    Row  = Top + Index;
    Label = (Entries[Row].Label != NULL) ? Entries[Row].Label : L"(unnamed)";

    if (Row == Selected) {
      Background = &mMenuSel;
      FpMenuFill (
        Menu,
        &mMenuSel,
        (Menu->MarginX > 20) ? (Menu->MarginX - 20) : 0,
        Menu->RowTop + Index * Menu->RowHeight,
        Menu->Hres - 2 * ((Menu->MarginX > 20) ? (Menu->MarginX - 20) : 0),
        Menu->RowHeight
        );
    } else {
      Background = &mMenuBg;
    }

    Foreground = &mMenuFg;
    FpMenuText (
      Menu,
      Label,
      Menu->MarginX + 20,
      Menu->RowTop + Index * Menu->RowHeight + (Menu->RowHeight - Menu->RowFont) / 2,
      Foreground,
      Background,
      Menu->RowFont
      );
  }

  FpMenuDrawFooter (Menu, TimeoutArmed, SecondsLeft);
}

STATIC
EFI_STATUS
FpMenuStartShell (
  IN EFI_HANDLE        ImageHandle,
  IN CONST EFI_GUID   *ShellGuid
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
    Status = gBS->StartImage (ShellHandle, NULL, NULL);
    gBS->UnloadImage (ShellHandle);
  }

  FreePool (Section);
  return Status;
}

EFI_STATUS
EFIAPI
FpBootMenuShow (
  IN MS_SIMPLE_WINDOW_MANAGER_PROTOCOL  *Swm,
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL       *Gop,
  IN EFI_HANDLE                          ImageHandle
  )
{
  EFI_STATUS                         Status;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *SimpleTextInEx;
  EFI_BOOT_MANAGER_LOAD_OPTION       *BootOptions;
  UINTN                              BootCount;
  UINTN                              AllocatedBootCount;
  FP_MENU_ENTRY                      *Entries;
  UINTN                              Count;
  UINTN                              Index;
  FP_BOOT_MENU                       Menu;
  UINTN                              Selected;
  UINTN                              Top;
  UINTN                              Visible;
  UINTN                              SecondsLeft;
  UINTN                              LastSeconds;
  BOOLEAN                            TimeoutArmed;
  UINT64                             StartNs;
  UINT64                             NowNs;
  EFI_KEY_DATA                       KeyData;

  if ((Swm == NULL) || (Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  SimpleTextInEx = NULL;
  if ((gST->ConsoleInHandle == NULL) ||
      EFI_ERROR (gBS->HandleProtocol (
                       gST->ConsoleInHandle,
                       &gEfiSimpleTextInputExProtocolGuid,
                       (VOID **)&SimpleTextInEx
                       )))
  {
    //
    // Without the keypad the menu could only sit there until the timeout; go
    // straight to the GUI instead.
    //
    return EFI_NOT_FOUND;
  }

  ZeroMem (&Menu, sizeof (Menu));
  Menu.Swm         = Swm;
  Menu.Gop         = Gop;
  Menu.ImageHandle = ImageHandle;
  Menu.Hres        = Gop->Mode->Info->HorizontalResolution;
  Menu.Vres        = Gop->Mode->Info->VerticalResolution;
  Menu.MarginX     = Menu.Hres / 10;
  Menu.TitleY      = Menu.Vres / 16;
  Menu.TitleFont   = MsUiGetLargeFontHeight ();
  Menu.RowFont     = MsUiGetMediumFontHeight ();
  Menu.RowHeight   = Menu.RowFont + Menu.RowFont / 2 + 16;
  Menu.RowTop      = Menu.TitleY + Menu.TitleFont * 2;
  Menu.FooterY     = (Menu.Vres > Menu.RowHeight * 3) ? (Menu.Vres - Menu.RowHeight * 2) : (Menu.Vres / 2);
  Menu.Status[0]   = L'\0';

  Menu.TextOut = AllocateZeroPool (sizeof (EFI_IMAGE_OUTPUT));
  if (Menu.TextOut == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Menu.TextOut->Width        = (UINT16)Menu.Hres;
  Menu.TextOut->Height       = (UINT16)Menu.Vres;
  Menu.TextOut->Image.Screen = Gop;

  //
  // Give the user something to look at while the boot manager scans devices.
  //
  FpMenuFill (&Menu, &mMenuBg, 0, 0, Menu.Hres, Menu.Vres);
  FpMenuText (&Menu, L"Boot Menu", Menu.MarginX, Menu.TitleY, &mMenuFg, &mMenuBg, Menu.TitleFont);
  FpMenuText (&Menu, L"Scanning boot options...", Menu.MarginX, Menu.RowTop, &mMenuHint, &mMenuBg, Menu.RowFont);

  //
  // Dynamic list: BootOrder as it stands now, not a build-time list.
  //
  EfiBootManagerRefreshAllBootOption ();
  BootOptions = EfiBootManagerGetLoadOptions (&BootCount, LoadOptionTypeBoot);
  if (BootOptions == NULL) {
    BootCount = 0;
  }

  AllocatedBootCount = BootCount;
  if (BootCount > FP_BOOT_MENU_MAX_BOOT_OPTIONS) {
    BootCount = FP_BOOT_MENU_MAX_BOOT_OPTIONS;
  }

  Count   = 1 + BootCount + 2;
  Entries = AllocateZeroPool (Count * sizeof (*Entries));
  if (Entries == NULL) {
    if (BootOptions != NULL) {
      EfiBootManagerFreeLoadOptions (BootOptions, AllocatedBootCount);
    }

    FreePool (Menu.TextOut);
    return EFI_OUT_OF_RESOURCES;
  }

  Entries[0].Label = L"Enter GUI (FrontPage)";
  Entries[0].Type  = FpMenuEntryGui;

  for (Index = 0; Index < BootCount; Index++) {
    Entries[1 + Index].Label = (BootOptions[Index].Description != NULL)
                                 ? BootOptions[Index].Description
                                 : L"(unnamed boot option)";
    Entries[1 + Index].Type        = FpMenuEntryBootOption;
    Entries[1 + Index].OptionIndex = Index;
  }

  //
  // Both shells are listed explicitly rather than relying on the boot manager to
  // enumerate FV applications: these two rows are always present regardless of what
  // BootOrder contains.
  //
  Entries[1 + BootCount].Label = L"UEFI Shell";
  Entries[1 + BootCount].Type  = FpMenuEntryShell;

  Entries[2 + BootCount].Label = L"Reboot";
  Entries[2 + BootCount].Type  = FpMenuEntryReboot;

  Selected     = 0;
  Top          = 0;
  TimeoutArmed = TRUE;
  LastSeconds  = FP_BOOT_MENU_TIMEOUT_SECONDS + 1;
  Visible      = FpMenuVisibleRows (&Menu);

  //
  // The press that started the phone may still be queued; do not let it pick
  // an entry.
  //
  FpMenuDrainInput (SimpleTextInEx);

  FpMenuDraw (&Menu, Entries, Count, Selected, Top, TimeoutArmed, FP_BOOT_MENU_TIMEOUT_SECONDS);

  StartNs = GetTimeInNanoSecond (GetPerformanceCounter ());
  while (TRUE) {
    NowNs = GetTimeInNanoSecond (GetPerformanceCounter ());

    if (TimeoutArmed) {
      if (NowNs - StartNs >= (UINT64)FP_BOOT_MENU_TIMEOUT_SECONDS * 1000ULL * 1000ULL * 1000ULL) {
        break;
      }

      SecondsLeft = FP_BOOT_MENU_TIMEOUT_SECONDS -
                    (UINTN)((NowNs - StartNs) / (1000ULL * 1000ULL * 1000ULL));
      if (SecondsLeft != LastSeconds) {
        LastSeconds = SecondsLeft;
        FpMenuDrawFooter (&Menu, TRUE, SecondsLeft);
      }
    }

    Status = SimpleTextInEx->ReadKeyStrokeEx (SimpleTextInEx, &KeyData);
    if (Status == EFI_NOT_READY) {
      gBS->Stall (FP_BOOT_MENU_POLL_US);
      continue;
    }

    if (EFI_ERROR (Status)) {
      gBS->Stall (FP_BOOT_MENU_POLL_US);
      continue;
    }

    if (TimeoutArmed) {
      TimeoutArmed = FALSE;
      FpMenuDrawFooter (&Menu, FALSE, 0);
    }

    if (KeyData.Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      switch (Entries[Selected].Type) {
        case FpMenuEntryGui:
          goto Done;

        case FpMenuEntryReboot:
          gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
          break;

        case FpMenuEntryShell:
        case FpMenuEntryShellStock:
          Menu.Status[0] = L'\0';
          Status         = FpMenuStartShell (
                             ImageHandle,
                             (Entries[Selected].Type == FpMenuEntryShell)
                               ? &mShellFileGuid
                               : &mStockShellFileGuid
                             );
          if (EFI_ERROR (Status)) {
            UnicodeSPrint (Menu.Status, sizeof (Menu.Status), L"Shell failed: %r", Status);
          }

          FpMenuDrainInput (SimpleTextInEx);
          FpMenuDraw (&Menu, Entries, Count, Selected, Top, TimeoutArmed, 0);
          break;

        case FpMenuEntryBootOption:
          Menu.Status[0] = L'\0';
          EfiBootManagerBoot (&BootOptions[Entries[Selected].OptionIndex]);
          UnicodeSPrint (Menu.Status, sizeof (Menu.Status), L"Boot failed");
          FpMenuDrainInput (SimpleTextInEx);
          FpMenuDraw (&Menu, Entries, Count, Selected, Top, TimeoutArmed, 0);
          break;

        default:
          break;
      }

      continue;
    }

    if (KeyData.Key.ScanCode == SCAN_UP) {
      Selected = (Selected == 0) ? (Count - 1) : (Selected - 1);
    } else if (KeyData.Key.ScanCode == SCAN_DOWN) {
      Selected = (Selected + 1) % Count;
    } else {
      //
      // Any other key only disarms the timeout; the footer was already redrawn.
      //
      continue;
    }

    if (Selected < Top) {
      Top = Selected;
    } else if (Selected >= Top + Visible) {
      Top = Selected - Visible + 1;
    }

    FpMenuDraw (&Menu, Entries, Count, Selected, Top, TimeoutArmed, 0);
  }

Done:
  FreePool (Entries);
  if (BootOptions != NULL) {
    EfiBootManagerFreeLoadOptions (BootOptions, AllocatedBootCount);
  }

  FreePool (Menu.TextOut);
  return EFI_SUCCESS;
}
