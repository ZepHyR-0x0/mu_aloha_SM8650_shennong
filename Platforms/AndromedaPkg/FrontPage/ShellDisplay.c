// SPDX-License-Identifier: BSD-2-Clause-Patent
// A session-local text console. Use the system's fixed 8x19 glyphs, enlarged
// by an integer scale, and keep all text and scrolling above the docked OSK.
#include <Uefi.h>
#include <Uefi/UefiInternalFormRepresentation.h>
#include <Guid/MdeModuleHii.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiFont.h>
#include <Protocol/OnScreenKeyboard.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "ShellDisplay.h"

#define SD_WIDTH 8U
#define SD_HEIGHT 19U
typedef struct { CHAR16 Char;UINT8 Attribute; } SD_CELL;
STATIC struct {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Out;
  EFI_SIMPLE_TEXT_OUTPUT_MODE Mode;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_HII_FONT_PROTOCOL *Font;
  MS_ONSCREEN_KEYBOARD_PROTOCOL *Osk;
  UINT32 OskMode;
  SD_CELL *Cells;
  BOOLEAN *Dirty;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Line;
  UINT8 Glyphs[95][SD_WIDTH*SD_HEIGHT];
  BOOLEAN Cached[95];
  UINTN Scale,Columns,Rows,X;
  EFI_HANDLE Handle,OldOutHandle,OldErrHandle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *OldOut,*OldErr;
} mSd;
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL mSdColors[16]={
  {0,0,0,0},{0x98,0,0,0},{0,0x98,0,0},{0x98,0x98,0,0},
  {0,0,0x98,0},{0x98,0,0x98,0},{0,0x98,0x98,0},{0xc0,0xc0,0xc0,0},
  {0x60,0x60,0x60,0},{0xff,0x60,0x60,0},{0x60,0xff,0x60,0},{0xff,0xff,0x60,0},
  {0x60,0x60,0xff,0},{0xff,0x60,0xff,0},{0x60,0xff,0xff,0},{0xff,0xff,0xff,0}
};
STATIC VOID SdCrc (VOID)
{
  gST->Hdr.CRC32=0;
  (VOID)gBS->CalculateCrc32(&gST->Hdr,gST->Hdr.HeaderSize,&gST->Hdr.CRC32);
}
STATIC UINT8 *SdGlyph (CHAR16 Char)
{
  EFI_IMAGE_OUTPUT *Image=NULL;
  BOOLEAN Loaded=FALSE;
  UINTN X,Y,At;
  if (Char<32 || Char>126) Char=L'?';
  At=Char-32;
  if (!mSd.Cached[At]) {
    // NULL explicitly selects the fixed system font. An all-zero font info
    // requests an empty font name at size zero and fails on the real HII DB.
    if (!EFI_ERROR(mSd.Font->GetGlyph(mSd.Font,Char,NULL,&Image,NULL)) && Image!=NULL && Image->Image.Bitmap!=NULL) {
      Loaded=TRUE;
      for (Y=0;Y<MIN(Image->Height,SD_HEIGHT);Y++)
        for (X=0;X<MIN(Image->Width,SD_WIDTH);X++) {
          EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P=&Image->Image.Bitmap[Y*Image->Width+X];
          mSd.Glyphs[At][Y*SD_WIDTH+X]=(UINT8)(P->Red!=0 || P->Green!=0 || P->Blue!=0);
        }
    }
    if (Image!=NULL) { if (Image->Image.Bitmap!=NULL) FreePool(Image->Image.Bitmap);FreePool(Image); }
    // Do not make a transient glyph lookup failure permanent for the session.
    mSd.Cached[At]=Loaded;
  }
  return mSd.Glyphs[At];
}
STATIC VOID SdPaint (VOID)
{
  UINTN Row,Col,X,Y,W=mSd.Columns*SD_WIDTH*mSd.Scale,H=SD_HEIGHT*mSd.Scale;
  for (Row=0;Row<mSd.Rows;Row++) {
    if (!mSd.Dirty[Row]) continue;
    for (Col=0;Col<mSd.Columns;Col++) {
      SD_CELL *Cell=&mSd.Cells[Row*mSd.Columns+Col];
      UINT8 *Glyph=SdGlyph(Cell->Char);
      for (Y=0;Y<H;Y++) for (X=0;X<SD_WIDTH*mSd.Scale;X++) {
        BOOLEAN On=Glyph[(Y/mSd.Scale)*SD_WIDTH+X/mSd.Scale]!=0;
        if (mSd.Mode.CursorVisible && Row==(UINTN)mSd.Mode.CursorRow && Col==(UINTN)mSd.Mode.CursorColumn && Y>=H-2*mSd.Scale) On=!On;
        mSd.Line[Y*W+Col*SD_WIDTH*mSd.Scale+X]=mSdColors[On?(Cell->Attribute&15):(Cell->Attribute>>4)&7];
      }
    }
    (VOID)mSd.Gop->Blt(mSd.Gop,mSd.Line,EfiBltBufferToVideo,0,0,mSd.X,Row*H,W,H,W*sizeof(*mSd.Line));
    mSd.Dirty[Row]=FALSE;
  }
}
STATIC VOID SdBlank (UINTN Row)
{
  UINTN I;
  for (I=0;I<mSd.Columns;I++) {
    mSd.Cells[Row*mSd.Columns+I].Char=L' ';
    mSd.Cells[Row*mSd.Columns+I].Attribute=(UINT8)mSd.Mode.Attribute;
  }
  mSd.Dirty[Row]=TRUE;
}
STATIC VOID SdNewLine (VOID)
{
  UINTN I;
  if (++mSd.Mode.CursorRow<(INT32)mSd.Rows) return;
  mSd.Mode.CursorRow=(INT32)mSd.Rows-1;
  CopyMem(mSd.Cells,mSd.Cells+mSd.Columns,(mSd.Rows-1)*mSd.Columns*sizeof(*mSd.Cells));
  SdBlank(mSd.Rows-1);
  for (I=0;I<mSd.Rows;I++) mSd.Dirty[I]=TRUE;
}
STATIC EFI_STATUS EFIAPI SdClear (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
  UINTN I;(VOID)This;
  mSd.Mode.CursorColumn=mSd.Mode.CursorRow=0;
  for (I=0;I<mSd.Rows;I++) SdBlank(I);
  SdPaint();return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SdReset (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Extended)
{ (VOID)Extended;mSd.Mode.Attribute=EFI_LIGHTGRAY;return SdClear(This); }
STATIC EFI_STATUS EFIAPI SdOutput (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{
  SD_CELL *Cell;(VOID)This;
  if (String==NULL) return EFI_INVALID_PARAMETER;
  mSd.Dirty[mSd.Mode.CursorRow]=TRUE;
  for (;*String!=0;String++) {
    if (*String==L'\r') mSd.Mode.CursorColumn=0;
    else if (*String==L'\n') SdNewLine();
    else if (*String==L'\b') {
      if (mSd.Mode.CursorColumn>0) mSd.Mode.CursorColumn--;
      else if (mSd.Mode.CursorRow>0) { mSd.Mode.CursorRow--;mSd.Mode.CursorColumn=(INT32)mSd.Columns-1; }
    } else if (*String==WIDE_CHAR || *String==NARROW_CHAR) continue;
    else {
      Cell=&mSd.Cells[mSd.Mode.CursorRow*mSd.Columns+mSd.Mode.CursorColumn];
      Cell->Char=*String;Cell->Attribute=(UINT8)mSd.Mode.Attribute;
      mSd.Dirty[mSd.Mode.CursorRow]=TRUE;
      if (++mSd.Mode.CursorColumn==(INT32)mSd.Columns) { mSd.Mode.CursorColumn=0;SdNewLine(); }
    }
  }
  mSd.Dirty[mSd.Mode.CursorRow]=TRUE;SdPaint();return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SdTest (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String)
{ (VOID)This;return String!=NULL?EFI_SUCCESS:EFI_INVALID_PARAMETER; }
STATIC EFI_STATUS EFIAPI SdQuery (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode, UINTN *Columns, UINTN *Rows)
{
  (VOID)This;if (Mode!=0 || Columns==NULL || Rows==NULL) return EFI_UNSUPPORTED;
  *Columns=mSd.Columns;*Rows=mSd.Rows;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SdSetMode (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode)
{ return Mode==0?SdClear(This):EFI_UNSUPPORTED; }
STATIC EFI_STATUS EFIAPI SdAttribute (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute)
{ (VOID)This;if (Attribute>0x7f) return EFI_UNSUPPORTED;mSd.Mode.Attribute=(INT32)Attribute;return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI SdCursor (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row)
{
  (VOID)This;if (Column>=mSd.Columns || Row>=mSd.Rows) return EFI_UNSUPPORTED;
  mSd.Dirty[mSd.Mode.CursorRow]=TRUE;mSd.Mode.CursorColumn=(INT32)Column;mSd.Mode.CursorRow=(INT32)Row;
  mSd.Dirty[Row]=TRUE;SdPaint();return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SdEnableCursor (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible)
{ (VOID)This;mSd.Mode.CursorVisible=Visible;mSd.Dirty[mSd.Mode.CursorRow]=TRUE;SdPaint();return EFI_SUCCESS; }
VOID FpShellDisplayEnd (VOID)
{
  if (mSd.OldOut!=NULL) {
    gST->ConOut=mSd.OldOut;gST->ConsoleOutHandle=mSd.OldOutHandle;
    gST->StdErr=mSd.OldErr;gST->StandardErrorHandle=mSd.OldErrHandle;SdCrc();
  }
  if (mSd.Handle!=NULL) (VOID)gBS->UninstallProtocolInterface(mSd.Handle,&gEfiSimpleTextOutProtocolGuid,&mSd.Out);
  if (mSd.Osk!=NULL) {
    (VOID)mSd.Osk->ShowKeyboard(mSd.Osk,FALSE);
    (VOID)mSd.Osk->ShowKeyboardIcon(mSd.Osk,FALSE);
    (VOID)mSd.Osk->SetKeyboardMode(mSd.Osk,mSd.OskMode);
  }
  if (mSd.Cells!=NULL) FreePool(mSd.Cells);
  if (mSd.Dirty!=NULL) FreePool(mSd.Dirty);
  if (mSd.Line!=NULL) FreePool(mSd.Line);
  ZeroMem(&mSd,sizeof(mSd));
}
EFI_STATUS FpShellDisplayBegin (EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop)
{
  EFI_STATUS Status;UINTN Height,Width;SWM_RECT Bounds;
  if (mSd.Cells!=NULL || Gop==NULL || Gop->Mode==NULL || Gop->Mode->Info==NULL) return EFI_NOT_READY;
  mSd.Gop=Gop;Width=Gop->Mode->Info->HorizontalResolution;Height=Gop->Mode->Info->VerticalResolution;
  Status=gBS->LocateProtocol(&gEfiHiiFontProtocolGuid,NULL,(VOID **)&mSd.Font);
  if (EFI_ERROR(Status)) goto Fail;
  if (!EFI_ERROR(gBS->LocateProtocol(&gMsOSKProtocolGuid,NULL,(VOID **)&mSd.Osk)) && mSd.Osk!=NULL) {
    (VOID)mSd.Osk->GetKeyboardMode(mSd.Osk,&mSd.OskMode);
    (VOID)mSd.Osk->SetKeyboardSize(mSd.Osk,100);
    (VOID)mSd.Osk->SetKeyboardPosition(mSd.Osk,BottomCenter,Docked);
    (VOID)mSd.Osk->SetKeyboardMode(mSd.Osk,OSK_MODE_SELF_REFRESH);
    (VOID)mSd.Osk->ShowKeyboardIcon(mSd.Osk,FALSE);
    Status=mSd.Osk->ShowKeyboard(mSd.Osk,TRUE);
    if (EFI_ERROR(Status)) goto Fail;
    if (!EFI_ERROR(mSd.Osk->GetKeyboardBounds(mSd.Osk,&Bounds)) && Bounds.Top>SD_HEIGHT && Bounds.Top<Height)
      Height=Bounds.Top-16;
    else Height=Height*2/3;
  }
  mSd.Scale=MAX((UINTN)1,MIN((UINTN)4,Width/(SD_WIDTH*60)));
  mSd.Columns=Width/(SD_WIDTH*mSd.Scale);mSd.Rows=Height/(SD_HEIGHT*mSd.Scale);
  if (mSd.Columns<20 || mSd.Rows<4) { Status=EFI_UNSUPPORTED;goto Fail; }
  mSd.X=(Width-mSd.Columns*SD_WIDTH*mSd.Scale)/2;
  mSd.Cells=AllocateZeroPool(mSd.Columns*mSd.Rows*sizeof(*mSd.Cells));
  mSd.Dirty=AllocateZeroPool(mSd.Rows*sizeof(*mSd.Dirty));
  mSd.Line=AllocatePool(mSd.Columns*SD_WIDTH*SD_HEIGHT*mSd.Scale*mSd.Scale*sizeof(*mSd.Line));
  if (mSd.Cells==NULL || mSd.Dirty==NULL || mSd.Line==NULL) { Status=EFI_OUT_OF_RESOURCES;goto Fail; }
  mSd.Mode.MaxMode=1;mSd.Mode.Attribute=EFI_LIGHTGRAY;mSd.Mode.CursorVisible=TRUE;
  mSd.Out=(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){SdReset,SdOutput,SdTest,SdQuery,SdSetMode,SdAttribute,SdClear,SdCursor,SdEnableCursor,&mSd.Mode};
  Status=gBS->InstallProtocolInterface(&mSd.Handle,&gEfiSimpleTextOutProtocolGuid,EFI_NATIVE_INTERFACE,&mSd.Out);
  if (EFI_ERROR(Status)) goto Fail;
  mSd.OldOut=gST->ConOut;mSd.OldOutHandle=gST->ConsoleOutHandle;
  mSd.OldErr=gST->StdErr;mSd.OldErrHandle=gST->StandardErrorHandle;
  gST->ConOut=gST->StdErr=&mSd.Out;gST->ConsoleOutHandle=gST->StandardErrorHandle=mSd.Handle;SdCrc();
  return SdClear(&mSd.Out);
Fail:
  FpShellDisplayEnd();return Status;
}
