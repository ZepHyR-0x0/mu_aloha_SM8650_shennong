// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native Usbfn 3BAC reads one DWC DEVSPD byte, not EFI_USB_BUS_SPEED.
STATIC EFI_GUID mStockSpeedGuid = {
  0x882F8C2B, 0x9646, 0x435F, {0x8D,0xE5,0xF2,0x08,0xFF,0x80,0xC1,0xBD}
};

STATIC EFI_STATUS EFIAPI
StockUsbGetVariable (CHAR16 *Name, EFI_GUID *Guid, UINT32 *Attributes,
                     UINTN *Size, VOID *Data)
{
  if (Name == NULL || Guid == NULL ||
      CompareMem (Guid, &mStockSpeedGuid, sizeof (*Guid)) != 0 ||
      StrCmp (Name, L"UsbfnMaxSpeed") != 0)
    return gRT->GetVariable (Name, Guid, Attributes, Size, Data);
  if (Size == NULL) return EFI_INVALID_PARAMETER;
  if (Attributes != NULL) *Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS;
  if (*Size < 1) { *Size = 1; return EFI_BUFFER_TOO_SMALL; }
  if (Data == NULL) return EFI_INVALID_PARAMETER;
  *Size = 1;
  *(UINT8 *)Data = 0; // DWC HighSpeed; EFI UsbBusSpeedHigh is 3.
  return EFI_SUCCESS;
}

// Actual DwcGetNextEvent A3F8..A440: Dwc+10 -> {base,cursor,length}.
// Validate numeric geometry before the caller reads any event RAM.
STATIC BOOLEAN
StockUsbEventGeometry (UINT64 Base, UINT64 Cursor, UINT32 Length,
                       UINT64 Expected, UINT64 RegisterAddress,
                       UINT32 RegisterLength, UINT32 Count)
{
  return Base != 0 && Base == Expected && Base == RegisterAddress &&
    (Base & 3) == 0 && Length == 4096 && RegisterLength == Length &&
    Base <= MAX_UINT64 - Length && Cursor >= Base && Cursor - Base < Length &&
    (Cursor & 3) == 0 && Count != 0 && Count <= Length && (Count & 3) == 0;
}
