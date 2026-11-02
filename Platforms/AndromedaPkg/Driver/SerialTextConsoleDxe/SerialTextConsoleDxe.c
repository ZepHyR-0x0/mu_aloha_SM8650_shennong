/** @file
  SerialTextConsoleDxe - a SimpleTextOut/SimpleTextIn console backed by
  SerialPortLib, i.e. by FrameBufferSerialPortLib on this port.

  Why this driver exists
  ----------------------
  BDS and the UEFI shell print through gST->ConOut. On this port the only
  console device was GraphicsConsoleDxe, which needs a GOP plus HII fonts and
  silently renders nothing, so BDS ran but every line it printed was lost.

  The Qualcomm prebuilt SimpleTextInOutSerial does exactly what we want (it
  loads "SerialPort Lib" through the ShLib HOB and writes text straight to
  SerialPortWrite) but its entry point returns an error on this platform and
  trips ASSERT_EFI_ERROR in its own AutoGen.c, so it is unusable here.

  This driver is the same idea, written from scratch and fully under our control:
  OutputString converts CHAR16 to CHAR8 and calls SerialPortWrite, which reaches
  the framebuffer console directly. No GOP, no HII font, no Qualcomm config
  database.

  Copyright (c) 2026, Project Mu port. SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/SerialPortLib.h>
#include <Library/FrameBufferSerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>

//
// One vendor device path node plus an end node. ConPlatformDxe only treats a
// handle as a console candidate when it carries a device path.
//
#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH         Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} SERIAL_TEXT_DEVICE_PATH;
#pragma pack()

STATIC SERIAL_TEXT_DEVICE_PATH  mDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 }
    },
    { 0x5E1C3A10, 0x6D2B, 0x4C8F, { 0x9A, 0x47, 0x3B, 0x1E, 0x8F, 0x2D, 0x6C, 0x05 } }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC EFI_SIMPLE_TEXT_OUTPUT_MODE  mTextOutMode = {
  -1,                                        // MaxMode (set in entry point)
  -1,                                        // Mode
  EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK),  // Attribute
  0,                                         // CursorColumn
  0,                                         // CursorRow
  FALSE                                      // CursorVisible
};

//
// --- SimpleTextOut ---------------------------------------------------------
//

STATIC
EFI_STATUS
EFIAPI
SerialTextReset (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN BOOLEAN                          ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialTextOutputString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  CHAR8  Buffer[128];
  UINTN  Index, Columns, Rows;
  EFI_STATUS Status;

  if (String == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = FbConTextGeometry (&Columns, &Rows);
  if (EFI_ERROR (Status)) return Status;

  Index = 0;
  while (*String != L'\0') {
    if (*String == L'\r') This->Mode->CursorColumn = 0;
    else if (*String == L'\n') {
      This->Mode->CursorColumn = 0;
      if (++This->Mode->CursorRow >= (INT32)Rows) This->Mode->CursorRow = 0;
    } else if (*String == L'\b') {
      if (This->Mode->CursorColumn > 0) This->Mode->CursorColumn--;
    } else if (*String >= 0x20) {
      if (++This->Mode->CursorColumn >= (INT32)Columns) {
        This->Mode->CursorColumn = 0;
        if (++This->Mode->CursorRow >= (INT32)Rows) This->Mode->CursorRow = 0;
      }
    }
    if (*String == L'\n') {
      Buffer[Index++] = '\r';
      Buffer[Index++] = '\n';
    } else if (*String == L'\r') {
      Buffer[Index++] = '\r';
    } else if (*String == L'\b') {
      Buffer[Index++] = '\b';
    } else if (*String < 0x80) {
      Buffer[Index++] = (CHAR8)*String;
    } else {
      // No Unicode font on this console yet.
      Buffer[Index++] = '?';
    }

    if (Index >= sizeof (Buffer) - 4) {
      SerialPortWrite ((UINT8 *)Buffer, Index);
      Index = 0;
    }

    String++;
  }

  if (Index != 0) {
    SerialPortWrite ((UINT8 *)Buffer, Index);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialTextTestString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialTextQueryMode (
  IN  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN  UINTN                            ModeNumber,
  OUT UINTN                            *Columns,
  OUT UINTN                            *Rows
  )
{
  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  if ((Columns == NULL) || (Rows == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  return FbConTextGeometry (Columns, Rows);
}

STATIC
EFI_STATUS
EFIAPI
SerialTextSetMode (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            ModeNumber
  )
{
  if (ModeNumber != 0) return EFI_UNSUPPORTED;
  This->Mode->Mode = 0;
  This->Mode->CursorColumn = This->Mode->CursorRow = 0;
  return FbConTextClear ();
}

STATIC
EFI_STATUS
EFIAPI
SerialTextSetAttribute (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            Attribute
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialTextClearScreen (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This
  )
{
  //
  // Explicit clear: SerialPortInitialize is idempotent after the first call.
  //
  This->Mode->CursorColumn = This->Mode->CursorRow = 0;
  return FbConTextClear ();
}

STATIC
EFI_STATUS
EFIAPI
SerialTextSetCursorPosition (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            Column,
  IN UINTN                            Row
  )
{
  EFI_STATUS Status = FbConTextCursor (Column, Row);
  if (!EFI_ERROR (Status)) {
    This->Mode->CursorColumn = (INT32)Column;
    This->Mode->CursorRow = (INT32)Row;
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
SerialTextEnableCursor (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN BOOLEAN                          Visible
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  mSerialTextOut = {
  SerialTextReset,
  SerialTextOutputString,
  SerialTextTestString,
  SerialTextQueryMode,
  SerialTextSetMode,
  SerialTextSetAttribute,
  SerialTextClearScreen,
  SerialTextSetCursorPosition,
  SerialTextEnableCursor,
  &mTextOutMode
};

//
// --- SimpleTextIn ----------------------------------------------------------
//
// There is no input device on this port yet. The protocol still has to exist:
// BDS and the shell dereference gST->ConIn unconditionally.
//

STATIC
EFI_STATUS
EFIAPI
SerialTextInReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialTextInReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  return EFI_NOT_READY;
}

STATIC
VOID
EFIAPI
SerialTextInWaitForKey (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  // Never signals: there is nothing to type on.
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mSerialTextIn = {
  SerialTextInReset,
  SerialTextInReadKeyStroke,
  NULL // WaitForKey, created in the entry point
};

/**
  Entry point.

  @param[in] ImageHandle  Image handle of this driver.
  @param[in] SystemTable  Pointer to the EFI System Table.

  @retval EFI_SUCCESS  Console installed.
  @retval Others       Protocol installation failed.
**/
EFI_STATUS
EFIAPI
EfiEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  //
  // FrameBufferSerialPortLib has no constructor; initialise the console before
  // anything writes to it.
  //
  SerialPortInitialize ();

  {
    STATIC CHAR8 MsgEntry[] = "[CON] entry\n";
    SerialPortWrite ((UINT8 *)MsgEntry, sizeof (MsgEntry) - 1);
  }

  mTextOutMode.MaxMode = 1;
  mTextOutMode.Mode    = 0;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  SerialTextInWaitForKey,
                  NULL,
                  &mSerialTextIn.WaitForKey
                  );
  if (EFI_ERROR (Status)) {
    STATIC CHAR8 MsgFail[] = "[CON] CreateEvent FAILED\n";
    SerialPortWrite ((UINT8 *)MsgFail, sizeof (MsgFail) - 1);
    return Status;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiSimpleTextOutProtocolGuid,
                  &mSerialTextOut,
                  &gEfiSimpleTextInProtocolGuid,
                  &mSerialTextIn,
                  NULL
                  );

  if (EFI_ERROR (Status)) {
    STATIC CHAR8 MsgFail2[] = "[CON] InstallMultipleProtocolInterfaces FAILED\n";
    SerialPortWrite ((UINT8 *)MsgFail2, sizeof (MsgFail2) - 1);
  } else {
    STATIC CHAR8 MsgOk[] = "[CON] ConOut/ConIn installed\n";
    SerialPortWrite ((UINT8 *)MsgOk, sizeof (MsgOk) - 1);
  }

  return Status;
}
