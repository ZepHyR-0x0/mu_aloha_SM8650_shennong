/** @file Read-only UFS/FAT browser. Handles never use CREATE/WRITE/SetInfo/Flush.
  Volumes are admitted only after verifying the lower storage guard's callbacks.
*/
#include <Uefi.h>
#include <Protocol/SimpleWindowManager.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/UsbBringup.h>
#include <Protocol/SimpleTextInEx.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/MsUiThemeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <UIToolKit/TouchGesture.h>
#include "../Driver/ReadOnlyStorageDxe/ReadOnlyStorage.h"
#include "FrontPageEventService.h"

#define FB_MAX_ITEMS 256
#define FB_PATH_CHARS 512
#define FB_NAME_CHARS 256
#define FB_PREVIEW_BYTES 512
#define FB_BACK 1
#define FB_PREV 2
#define FB_NEXT 3
#define FB_CLOSE 4
#define FB_ROW_BASE 16

typedef struct {
  CHAR16 Name[FB_NAME_CHARS];
  EFI_HANDLE Handle;
  UINT64 Size;
  BOOLEAN Directory;
  BOOLEAN Mounted;
} FB_ITEM;
typedef struct {
  MS_SIMPLE_WINDOW_MANAGER_PROTOCOL *Swm;
  EFI_HANDLE Window;
  EFI_ABSOLUTE_POINTER_PROTOCOL *Pointer;
  EFI_IMAGE_OUTPUT Output;
  READ_ONLY_STORAGE_POLICY *Policy;
  FB_ITEM Items[FB_MAX_ITEMS];
  UINTN Count, Selected, Top, Rows, Width, Height, RowHeight, RowTop;
  UINTN FontHeight, ToolbarTop, ToolbarHeight, FooterTop;
  BOOLEAN TouchActive, SwipeAllowed, Dragging;
  UINT64 SwipeY;
  UINTN SwipeTop;
  EFI_HANDLE Volume;
  EFI_FILE_PROTOCOL *Directory;
  CHAR16 Path[FB_PATH_CHARS];
  CHAR16 Status[192];
  CHAR16 PreviewName[FB_NAME_CHARS];
  UINT64 PreviewSize;
  UINT8 Preview[FB_PREVIEW_BYTES];
  UINTN PreviewLength;
  UINTN PreviewTop;
  BOOLEAN Previewing;
  UIT_TOUCH_GESTURE Tap;
} FB;
STATIC EFI_GUID mGuardGuid = READ_ONLY_STORAGE_GUID;
STATIC EFI_GUID mRamDeviceGuid=USB_RAM_DISK_DEVICE_GUID;
STATIC EFI_GUID mWindowGuid = {0x49e87ab3,0x9031,0x4d81,{0x98,0x19,0x33,0x17,0,0,0,1}};
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mBg = {20,20,20,0}, mFg = {240,240,240,0}, mSel = {120,70,20,0}, mButtonBg = {45,45,45,0};

STATIC VOID FbTrace (CONST CHAR8 *Format, EFI_STATUS Status, UINTN Count)
{
  CHAR8 Text[160];
  UINTN N = AsciiSPrint (Text, sizeof (Text), Format, Status, Count);
  SerialPortWrite ((UINT8 *)Text, N);
}
STATIC VOID FbText (FB *B, CHAR16 *Text, UINTN X, UINTN Y, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Bg)
{
  EFI_FONT_DISPLAY_INFO Font;
  EFI_IMAGE_OUTPUT *Output = &B->Output;
  ZeroMem (&Font, sizeof (Font));
  Font.FontInfoMask = EFI_FONT_INFO_ANY_FONT;
  // Use an installed theme font. An arbitrary 32px size fell back to 19px.
  Font.FontInfo.FontSize = (UINT16)B->FontHeight;
  Font.ForegroundColor = mFg;
  Font.BackgroundColor = *Bg;
  B->Swm->StringToWindow (B->Swm, B->Window,
    EFI_HII_OUT_FLAG_CLIP | EFI_HII_OUT_FLAG_CLIP_CLEAN_X | EFI_HII_OUT_FLAG_CLIP_CLEAN_Y |
    EFI_HII_IGNORE_LINE_BREAK | EFI_HII_DIRECT_TO_SCREEN,
    Text, &Font, &Output, X, Y, NULL, NULL, NULL);
}
STATIC VOID FbFill (FB *B, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color, UINTN X, UINTN Y, UINTN W, UINTN H)
{
  B->Swm->BltWindow (B->Swm, B->Window, Color, EfiBltVideoFill, 0, 0, X, Y, W, H, 0);
}
STATIC VOID FbPaint (FB *B)
{
  UINTN I, Row, N;
  CHAR16 Line[FB_NAME_CHARS + 64];
  CHAR16 *Buttons[] = {L"Back", L"Prev", L"Next", L"Exit"};
  FbFill (B, &mBg, 0, 0, B->Width, B->Height);
  FbText (B, L"Files (read only)", 20, 20, &mBg);
  FbText (B, B->Volume == NULL ? L"UFS / RAM / FAT (read only)" : B->Path, 20, B->FontHeight + 40, &mBg);
  for (I = 0; I < 4; I++) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Bg = B->Tap.Armed && B->Tap.LastTarget == I + 1 ? &mSel : &mButtonBg;
    FbFill (B, Bg, I * B->Width / 4 + 4, B->ToolbarTop, B->Width / 4 - 8, B->ToolbarHeight);
    FbText (B, Buttons[I], I * B->Width / 4 + 12, B->ToolbarTop + 16, Bg);
  }
  if (B->Previewing) {
    UnicodeSPrint (Line, sizeof (Line), L"%s  (%Lu bytes, first %u shown)", B->PreviewName, B->PreviewSize, B->PreviewLength);
    FbText (B, Line, 20, B->RowTop, &mBg);
    // Bounded hex + ASCII preview; binary content is never executed or parsed.
    for (I = 0; I < B->Rows - 1 && (B->PreviewTop + I) * 8 < B->PreviewLength; I++) {
      UINTN Start = (B->PreviewTop + I) * 8;
      N = UnicodeSPrint (Line, sizeof (Line), L"%04x  ", (UINT32)Start);
      for (Row = Start; Row < MIN (B->PreviewLength, Start + 8); Row++)
        N += UnicodeSPrint (Line + N, sizeof (Line) - N * sizeof (CHAR16), L"%02x ", B->Preview[Row]);
      N += UnicodeSPrint (Line + N, sizeof (Line) - N * sizeof (CHAR16), L"  ");
      for (Row = Start; Row < MIN (B->PreviewLength, Start + 8); Row++) {
        UINT8 C = B->Preview[Row];
        Line[N++] = C >= 32 && C < 127 ? C : L'.';
      }
      Line[N] = 0;
      FbText (B, Line, 20, B->RowTop + (I + 1) * B->RowHeight, &mBg);
    }
  } else {
    for (I = 0; I < B->Rows && B->Top + I < B->Count; I++) {
      FB_ITEM *Item = &B->Items[B->Top + I];
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Bg = B->Selected == B->Top + I ? &mSel : &mBg;
      FbFill (B, Bg, 10, B->RowTop + I * B->RowHeight, B->Width - 20, B->RowHeight - 2);
      UnicodeSPrint (Line, sizeof (Line), L"%s %s  %Lu bytes", Item->Directory ? L"[DIR]" : L"[FILE]", Item->Name, Item->Size);
      if (B->Volume == NULL) UnicodeSPrint (Line, sizeof (Line), L"%s %s  %Lu MiB", Item->Mounted ? L"[FAT]" : L"[RAW]", Item->Name, Item->Size / 1024 / 1024);
      FbText (B, Line, 20, B->RowTop + I * B->RowHeight + 10, Bg);
    }
  }
  FbText (B, B->Status, 20, B->FooterTop - B->FontHeight - 20, &mBg);
  FbFill (B, &mButtonBg, 10, B->FooterTop, B->Width - 20, B->ToolbarHeight);
  FbText (B, B->Volume == NULL ? L"Exit to menu" : L"Back / return", 25, B->FooterTop + 16, &mButtonBg);
}
STATIC VOID FbResetList (FB *B)
{
  B->Count = B->Selected = B->Top = 0;
  B->Previewing = FALSE;
  ZeroMem (&B->Tap, sizeof (B->Tap));
  B->TouchActive = B->Dragging = FALSE;
}
STATIC EFI_STATUS FbVolumes (FB *B)
{
  EFI_HANDLE *Handles;
  UINTN Count, I;
  EFI_STATUS Status;
  FbResetList (B);
  B->Volume = NULL;
  StrCpyS (B->Path, FB_PATH_CHARS, L"\\");
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) return Status;
  for (I = 0; I < Count && B->Count < FB_MAX_ITEMS; I++) {
    EFI_PARTITION_INFO_PROTOCOL *Part;
    EFI_BLOCK_IO_PROTOCOL *Block;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
    EFI_DEVICE_PATH_PROTOCOL *Path;
    BOOLEAN Ram=FALSE,Gpt;
    FB_ITEM *Item;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiBlockIoProtocolGuid, (VOID **)&Block)) ||
        Block->Media == NULL || !Block->Media->MediaPresent || Block->Media->BlockSize == 0) continue;
    Gpt=!EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiPartitionInfoProtocolGuid,(VOID **)&Part)) && Part->Type==PARTITION_TYPE_GPT;
    if (!Gpt && !EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiDevicePathProtocolGuid,(VOID **)&Path)) && Path!=NULL &&
        Path->Type==HARDWARE_DEVICE_PATH && Path->SubType==HW_VENDOR_DP &&
        (Path->Length[0]|((UINT16)Path->Length[1]<<8))==sizeof(VENDOR_DEVICE_PATH))
      Ram=CompareGuid(&((VENDOR_DEVICE_PATH *)Path)->Guid,&mRamDeviceGuid);
    if (!Gpt && !Ram) continue;
    Item = &B->Items[B->Count++];
    ZeroMem (Item, sizeof (*Item));
    if (Ram) StrCpyS(Item->Name,FB_NAME_CHARS,L"RAMDISK (volatile)");
    else StrnCpyS (Item->Name, FB_NAME_CHARS, Part->Info.Gpt.PartitionName, 36);
    Item->Handle = Handles[I];
    Item->Size = Block->Media->LastBlock < MAX_UINT64 / Block->Media->BlockSize ?
                 (Block->Media->LastBlock + 1) * Block->Media->BlockSize : 0;
    Item->Mounted = (BOOLEAN)(B->Policy->IsProtected (Handles[I]) &&
      !EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)));
  }
  gBS->FreePool (Handles);
  UnicodeSPrint (B->Status, sizeof (B->Status), L"%u partitions. RAW includes ext4/F2FS/encrypted volumes.", B->Count);
  FbTrace ("[FILES b319] volumes -> %r count=%u\r\n", EFI_SUCCESS, B->Count);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FbReadDirectory (FB *B)
{
  EFI_STATUS Status;
  UINTN Size, Guard;
  // A corrupt filesystem must not supply an unbounded filename or record.
  union { UINT64 Align; UINT8 Bytes[sizeof (EFI_FILE_INFO) + FB_NAME_CHARS * sizeof (CHAR16)]; } Record;
  EFI_FILE_INFO *Info = (EFI_FILE_INFO *)Record.Bytes;
  FbResetList (B);
  if (!B->Policy->IsProtected (B->Volume)) return EFI_WRITE_PROTECTED;
  Status = B->Directory->SetPosition (B->Directory, 0);
  if (EFI_ERROR (Status)) return Status;
  for (Guard = 0; Guard < 4096 && B->Count < FB_MAX_ITEMS; Guard++) {
    Size = sizeof (Record);
    ZeroMem (&Record, sizeof (Record));
    Status = B->Directory->Read (B->Directory, &Size, &Record);
    if (EFI_ERROR (Status) || Size == 0) break;
    if (Size < SIZE_OF_EFI_FILE_INFO + sizeof (CHAR16) || Size > sizeof (Record) ||
        Info->Size > Size || Info->Size < SIZE_OF_EFI_FILE_INFO + sizeof (CHAR16)) return EFI_VOLUME_CORRUPTED;
    // Limit the terminator search to bytes actually returned by the driver.
    {
      UINTN Length, Limit = (Info->Size - SIZE_OF_EFI_FILE_INFO) / sizeof (CHAR16);
      for (Length = 0; Length < Limit && Info->FileName[Length] != 0; Length++);
      if (Length == Limit || Length == 0 || Length >= FB_NAME_CHARS) return EFI_VOLUME_CORRUPTED;
      if (StrCmp (Info->FileName, L".") == 0 || StrCmp (Info->FileName, L"..") == 0) continue;
      if (StrStr (Info->FileName, L"\\") != NULL || StrStr (Info->FileName, L"/") != NULL) return EFI_VOLUME_CORRUPTED;
    }
    {
      FB_ITEM *Item = &B->Items[B->Count++];
      ZeroMem (Item, sizeof (*Item));
      StrCpyS (Item->Name, FB_NAME_CHARS, Info->FileName);
      Item->Size = Info->FileSize;
      Item->Directory = (BOOLEAN)((Info->Attribute & EFI_FILE_DIRECTORY) != 0);
    }
  }
  if (Guard == 4096) Status = EFI_ABORTED;
  UnicodeSPrint (B->Status, sizeof (B->Status), L"%u entries%s. Writes blocked: %u", B->Count,
                 B->Count == FB_MAX_ITEMS ? L" (display limit reached)" : L"", B->Policy->Denied);
  FbTrace ("[FILES b319] directory -> %r count=%u\r\n", Status, B->Count);
  return Status;
}
STATIC EFI_STATUS FbEnter (FB *B)
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File = NULL;
  FB_ITEM Item;
  if (B->Count == 0 || B->Selected >= B->Count || B->Previewing) return EFI_NOT_FOUND;
  Item = B->Items[B->Selected];
  if (B->Volume == NULL) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
    if (!Item.Mounted || !B->Policy->IsProtected (Item.Handle)) {
      StrCpyS (B->Status, 192, L"No supported filesystem. Raw partitions are never modified.");
      return EFI_UNSUPPORTED;
    }
    Status = gBS->HandleProtocol (Item.Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) return Status;
    Status = Fs->OpenVolume (Fs, &File);
    if (EFI_ERROR (Status)) return Status;
    B->Directory = File;
    B->Volume = Item.Handle;
    StrCpyS (B->Path, FB_PATH_CHARS, L"\\");
    return FbReadDirectory (B);
  }
  if (!B->Policy->IsProtected (B->Volume)) return EFI_WRITE_PROTECTED;
  if (Item.Directory && StrLen (B->Path) + StrLen (Item.Name) + 2 >= FB_PATH_CHARS) return EFI_BAD_BUFFER_SIZE;
  Status = B->Directory->Open (B->Directory, &File, Item.Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) return Status;
  if (Item.Directory) {
    if (StrLen (B->Path) > 1) StrCatS (B->Path, FB_PATH_CHARS, L"\\");
    StrCatS (B->Path, FB_PATH_CHARS, Item.Name);
    B->Directory->Close (B->Directory);
    B->Directory = File;
    return FbReadDirectory (B);
  }
  B->PreviewLength = sizeof (B->Preview);
  Status = File->Read (File, &B->PreviewLength, B->Preview);
  File->Close (File);
  if (EFI_ERROR (Status) || B->PreviewLength > sizeof (B->Preview)) return EFI_VOLUME_CORRUPTED;
  StrCpyS (B->PreviewName, FB_NAME_CHARS, Item.Name);
  B->PreviewSize = Item.Size;
  B->Previewing = TRUE;
  B->PreviewTop = 0;
  StrCpyS (B->Status, 192, L"Hex / ASCII preview. Up returns to the directory.");
  FbTrace ("[FILES b319] preview -> %r bytes=%u\r\n", Status, B->PreviewLength);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FbBack (FB *B)
{
  UINTN N;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root = NULL, *Parent = NULL;
  EFI_STATUS Status;
  CHAR16 Path[FB_PATH_CHARS];
  if (B->Previewing) { B->Previewing = FALSE; return EFI_SUCCESS; }
  if (B->Volume == NULL) return EFI_SUCCESS;
  if (!B->Policy->IsProtected (B->Volume)) return EFI_WRITE_PROTECTED;
  if (StrCmp (B->Path, L"\\") == 0) {
    B->Directory->Close (B->Directory); B->Directory = NULL;
    return FbVolumes (B);
  }
  StrCpyS (Path, FB_PATH_CHARS, B->Path);
  N = StrLen (Path);
  while (N > 1 && Path[N - 1] != L'\\') N--;
  Path[N > 1 ? N - 1 : 1] = 0;
  Status = gBS->HandleProtocol (B->Volume, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) return Status;
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) return Status;
  Status = Root->Open (Root, &Parent, Path, EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (EFI_ERROR (Status)) return Status;
  StrCpyS (B->Path, FB_PATH_CHARS, Path);
  B->Directory->Close (B->Directory); B->Directory = Parent;
  return FbReadDirectory (B);
}
STATIC BOOLEAN EFIAPI FbNotify (VOID *Context) { return TRUE; }

// One input path for rows, persistent navigation buttons and vertical scrolling.
STATIC UINTN FbHandleTouch (FB *B, EFI_ABSOLUTE_POINTER_STATE *Touch, UINT64 Ms, BOOLEAN *Dirty)
{
  BOOLEAN Down = SWM_IS_FINGER_DOWN (*Touch);
  UINTN Target = 0;
  UIT_TAP_RESULT Tap;
  if (Touch->CurrentX < B->Width) {
    if (Touch->CurrentY >= B->ToolbarTop && Touch->CurrentY < B->ToolbarTop + B->ToolbarHeight)
      Target = FB_BACK + (UINTN)(Touch->CurrentX * 4 / B->Width);
    else if (Touch->CurrentY >= B->FooterTop && Touch->CurrentY < B->FooterTop + B->ToolbarHeight)
      Target = B->Volume == NULL ? FB_CLOSE : FB_BACK;
    else if (!B->Previewing && Touch->CurrentY >= B->RowTop && Touch->CurrentY < B->RowTop + B->Rows * B->RowHeight) {
      UINTN I = B->Top + ((UINTN)Touch->CurrentY - B->RowTop) / B->RowHeight;
      if (I < B->Count) Target = FB_ROW_BASE + I;
    }
  }
  if (Down && !B->TouchActive) {
    B->TouchActive = TRUE;
    B->SwipeAllowed = (BOOLEAN)(Touch->CurrentX < B->Width && Touch->CurrentY >= B->RowTop && Touch->CurrentY < B->FooterTop - B->FontHeight - 20);
    B->Dragging = FALSE;
    B->SwipeY = Touch->CurrentY;
    B->SwipeTop = B->Previewing ? B->PreviewTop : B->Top;
  }
  if (B->TouchActive && B->SwipeAllowed &&
      UitTouchDistance (Touch->CurrentY, B->SwipeY) > MAX ((UINTN)20, B->FontHeight / 2)) {
    B->Dragging = TRUE;
    B->Tap.Cancelled = TRUE;
    B->Tap.Armed = FALSE;
  }
  if (B->TouchActive && B->Dragging) {
    UINTN Total = B->Previewing ? (B->PreviewLength + 7) / 8 : B->Count;
    UINTN Visible = B->Previewing ? B->Rows - 1 : B->Rows;
    UINTN MaxTop = Total > Visible ? Total - Visible : 0;
    INT64 Delta = (INT64)B->SwipeY - (INT64)MIN (Touch->CurrentY, B->Height);
    INT64 Top = (INT64)B->SwipeTop + Delta / (INT64)B->RowHeight;
    UINTN NewTop = Top < 0 ? 0 : MIN ((UINTN)Top, MaxTop);
    if (B->Previewing) {
      if (B->PreviewTop != NewTop) *Dirty = TRUE;
      B->PreviewTop = NewTop;
    } else {
      if (B->Top != NewTop) *Dirty = TRUE;
      B->Top = NewTop;
      if (B->Selected < B->Top) B->Selected = B->Top;
      if (B->Count != 0 && B->Selected >= B->Top + B->Rows) B->Selected = B->Top + B->Rows - 1;
    }
    if (!Down) {
      B->TouchActive = B->Dragging = FALSE;
      ZeroMem (&B->Tap, sizeof (B->Tap));
    }
    return 0; // A swipe must never open the item underneath its release point.
  }
  Tap = UitTouchTap (&B->Tap, Down, Target, Touch->CurrentX, Touch->CurrentY, Ms, MAX ((UINTN)24, B->Width / 50));
  if (!Down) B->TouchActive = FALSE;
  if (Tap == UitTapFocus) {
    if (Target >= FB_ROW_BASE) B->Selected = Target - FB_ROW_BASE;
    *Dirty = TRUE;
  }
  // Tool buttons are direct actions. Only files and directories need two taps.
  if (Target > 0 && Target < FB_ROW_BASE && (Tap == UitTapFocus || Tap == UitTapConfirm)) {
    ZeroMem (&B->Tap, sizeof (B->Tap));
    return Target;
  }
  return Tap == UitTapConfirm ? Target : 0;
}

VOID FpFileBrowser (MS_SIMPLE_WINDOW_MANAGER_PROTOCOL *Swm, EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop)
{
  FB *B;
  EFI_STATUS Status;
  EFI_EVENT PaintEvent;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Keys = NULL;
  EFI_KEY_DATA Key;
  SWM_RECT Frame;
  BOOLEAN Exit = FALSE, Dirty = TRUE;
  B = AllocateZeroPool (sizeof (*B));
  if (B == NULL) return;
  if (EFI_ERROR (gBS->LocateProtocol (&mGuardGuid, NULL, (VOID **)&B->Policy)) ||
      B->Policy->Revision < 1 || B->Policy->Revision > 3 || B->Policy->IsProtected == NULL) {
    FbTrace ("[FILES b319] refused without complete storage guard: %r %u\r\n", EFI_WRITE_PROTECTED, 0);
    goto Done;
  }
  B->Swm = Swm;
  B->Width = Gop->Mode->Info->HorizontalResolution;
  B->Height = Gop->Mode->Info->VerticalResolution;
  B->FontHeight = MsUiGetLargeFontHeight ();
  B->ToolbarTop = B->FontHeight * 2 + 70;
  B->ToolbarHeight = B->FontHeight + 40;
  B->RowTop = B->ToolbarTop + B->ToolbarHeight + 20;
  B->RowHeight = MAX (B->FontHeight + 40, B->Height / 16);
  B->FooterTop = B->Height - B->ToolbarHeight - 12;
  if (B->FontHeight == 0 || B->FooterTop <= B->RowTop + B->FontHeight + 20 + B->RowHeight * 2) goto Done;
  B->Rows = (B->FooterTop - B->FontHeight - 20 - B->RowTop) / B->RowHeight;
  B->Output.Width = (UINT16)B->Width;
  B->Output.Height = (UINT16)B->Height;
  B->Output.Image.Screen = Gop;
  SWM_RECT_INIT2 (Frame, 0, 0, B->Width, B->Height);
  Status = gBS->InstallProtocolInterface (&B->Window, &mWindowGuid, EFI_NATIVE_INTERFACE, B);
  if (EFI_ERROR (Status)) goto Done;
  Status = Swm->RegisterClient (Swm, B->Window, SWM_Z_ORDER_POPUP, &Frame, FbNotify, B, &B->Pointer, &PaintEvent);
  if (EFI_ERROR (Status)) goto RemoveWindow;
  Swm->ActivateWindow (Swm, B->Window, TRUE);
  if (gST->ConsoleInHandle != NULL) gBS->HandleProtocol (gST->ConsoleInHandle, &gEfiSimpleTextInputExProtocolGuid, (VOID **)&Keys);
  Status = FbVolumes (B);
  while (!Exit) {
    FpServiceEvents();
    EFI_ABSOLUTE_POINTER_STATE Touch;
    UINTN Action = 0;
    if (Dirty || gBS->CheckEvent (PaintEvent) == EFI_SUCCESS) { FbPaint (B); Dirty = FALSE; }
    if (Keys != NULL && Keys->ReadKeyStrokeEx (Keys, &Key) == EFI_SUCCESS) {
      ZeroMem (&B->Tap, sizeof (B->Tap));
      if (Key.Key.ScanCode == SCAN_ESC) Action = FB_CLOSE;
      else if (Key.Key.UnicodeChar == CHAR_BACKSPACE) Action = FB_BACK;
      else if (Key.Key.UnicodeChar == CHAR_CARRIAGE_RETURN) Action = B->Previewing ? FB_BACK : FB_ROW_BASE + B->Selected;
      else if (Key.Key.ScanCode == SCAN_UP) {
        if (B->Previewing && B->PreviewTop > 0) B->PreviewTop--;
        else if (!B->Previewing && B->Selected > 0) B->Selected--;
      } else if (Key.Key.ScanCode == SCAN_DOWN) {
        if (B->Previewing && (B->PreviewTop + B->Rows - 1) * 8 < B->PreviewLength) B->PreviewTop++;
        else if (!B->Previewing && B->Selected + 1 < B->Count) B->Selected++;
      }
      Dirty = TRUE;
    }
    if (B->Pointer->GetState (B->Pointer, &Touch) == EFI_SUCCESS) {
      UINTN TouchAction = FbHandleTouch (B, &Touch, GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000000, &Dirty);
      if (TouchAction != 0) Action = TouchAction;
    }
    if (Action == FB_CLOSE) Exit = TRUE;
    else if (Action == FB_BACK) { Status = FbBack (B); Dirty = TRUE; }
    else if (Action == FB_PREV) {
      if (B->Previewing) B->PreviewTop = B->PreviewTop > B->Rows - 1 ? B->PreviewTop - (B->Rows - 1) : 0;
      else { B->Top = B->Top > B->Rows ? B->Top - B->Rows : 0; B->Selected = B->Top; }
      Dirty = TRUE;
    }
    else if (Action == FB_NEXT) {
      if (B->Previewing && (B->PreviewTop + B->Rows - 1) * 8 < B->PreviewLength) B->PreviewTop += B->Rows - 1;
      else if (!B->Previewing && B->Top + B->Rows < B->Count) { B->Top += B->Rows; B->Selected = B->Top; }
      Dirty = TRUE;
    }
    else if (Action >= FB_ROW_BASE && !B->Previewing) { B->Selected = Action - FB_ROW_BASE; Status = FbEnter (B); Dirty = TRUE; }
    if (EFI_ERROR (Status) && Status != EFI_UNSUPPORTED) {
      UnicodeSPrint (B->Status, sizeof (B->Status), L"Read failed: %r", Status); Status = EFI_SUCCESS; Dirty = TRUE;
    }
    if (B->Selected < B->Top) B->Top = B->Selected;
    if (B->Selected >= B->Top + B->Rows) B->Top = B->Selected - B->Rows + 1;
    gBS->Stall (20000);
  }
  if (B->Directory != NULL) B->Directory->Close (B->Directory);
  Swm->ActivateWindow (Swm, B->Window, FALSE);
  Swm->UnregisterClient (Swm, B->Window);
RemoveWindow:
  gBS->UninstallProtocolInterface (B->Window, &mWindowGuid, B);
Done:
  FreePool (B);
}
