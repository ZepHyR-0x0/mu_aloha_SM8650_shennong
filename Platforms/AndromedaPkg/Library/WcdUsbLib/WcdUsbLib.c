/** WCD939x paged I2C access and USB DP/DM routing, from the SM8650 kernel. */
#include <Library/WcdUsbLib.h>
#include <Library/TimerLib.h>

STATIC EFI_STATUS
WcdByte (IN WCD_USB_IO *Io, IN BOOLEAN Read, IN UINT16 Register, IN OUT UINT8 *Value)
{
  UINT8 Page;
  UINT8 Zero = 0;
  UINT32 Count = 0;
  UINT32 Rc;
  UINT32 Restore;
  BOOLEAN Valid;
  if (Io == NULL || Value == NULL || Io->Handle == 0 || Io->Read == NULL || Io->Write == NULL) return EFI_INVALID_PARAMETER;
  Page = (UINT8)(Register >> 8);
  Rc = Io->Write (Io->Handle, Io->Config, 0, 1, &Page, 1, &Count, 2500);
  if (Rc != 0 || Count != 1) {
    Count = 0;
    Io->Write (Io->Handle, Io->Config, 0, 1, &Zero, 1, &Count, 2500);
    return EFI_DEVICE_ERROR;
  }
  Count = 0;
  Rc = (Read ? Io->Read : Io->Write)(Io->Handle, Io->Config, Register & 0xFF, 1, Value, 1, &Count, 2500);
  Valid = (BOOLEAN)(Rc == 0 && Count == 1);
  Count = 0;
  // Leave the selector at page0 for other firmware clients. Page changes are
  // transient addressing operations, not writes to a device partition.
  Restore = Io->Write (Io->Handle, Io->Config, 0, 1, &Zero, 1, &Count, 2500);
  return Valid && Restore == 0 && Count == 1 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

EFI_STATUS
WcdReadRegister (IN WCD_USB_IO *Io, IN UINT16 Register, OUT UINT8 *Value)
{
  EFI_STATUS Status;
  if (Value == NULL) return EFI_INVALID_PARAMETER;
  *Value = 0;
  Status = WcdByte (Io, TRUE, Register, Value);
  if (EFI_ERROR (Status)) *Value = 0;
  return Status;
}

STATIC EFI_STATUS
WcdUpdate (IN WCD_USB_IO *Io, IN UINT16 Register, IN UINT8 Mask, IN UINT8 Value)
{
  UINT8 Before;
  UINT8 After;
  UINT8 Expected;
  EFI_STATUS Status = WcdReadRegister (Io, Register, &Before);
  if (EFI_ERROR (Status)) return Status;
  Expected = (UINT8)((Before & ~Mask) | (Value & Mask));
  Status = WcdByte (Io, FALSE, Register, &Expected);
  if (EFI_ERROR (Status)) return Status;
  Status = WcdReadRegister (Io, Register, &After);
  if (EFI_ERROR (Status)) return Status;
  return (After & Mask) == (Expected & Mask) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

EFI_STATUS
WcdResetUsb (IN WCD_USB_IO *Io)
{
  UINT8 Value = 0;
  EFI_STATUS Status = WcdByte (Io, FALSE, 0x414, &Value);
  if (EFI_ERROR (Status)) return Status;
  MicroSecondDelay (2000);
  Value = 3;
  Status = WcdByte (Io, FALSE, 0x414, &Value);
  MicroSecondDelay (2000);
  return Status;
}

EFI_STATUS
WcdConfigureUsb (IN WCD_USB_IO *Io)
{
  EFI_STATUS Status;
  // wcd939x-i2c.c defaults: DPR->DP, DNL->DN; USB_SS mode5.
  // Standby/device enable bit7 plus DP/DM switches bits3/4. Preserve AUX bits;
  // disable MIC/SENSE/AGND audio switches in this USB-only path.
  Status = WcdUpdate (Io, 0x404, 0x3C, 0x14);
  if (!EFI_ERROR (Status)) Status = WcdUpdate (Io, 0x419, 0x07, 0x05);
  if (!EFI_ERROR (Status)) Status = WcdUpdate (Io, 0x403, 0x9F, 0x98);
  // wcd_usbss_dpdm_switch_update(true,true) enables the USB equalizer.
  // Keep the existing bandwidth/tuning bits rather than inventing a setting.
  if (!EFI_ERROR (Status)) Status = WcdUpdate (Io, 0x415, 0x80, 0x80);
  return Status;
}
