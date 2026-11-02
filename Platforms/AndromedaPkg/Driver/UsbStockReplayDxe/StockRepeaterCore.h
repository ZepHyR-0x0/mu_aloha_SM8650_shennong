// SPDX-License-Identifier: BSD-2-Clause-Patent
// PM8550B device-mode sequence from shennong's qti-pmic-eusb2 driver.
// Only the named repeater registers are writable; preserve unrelated bits.
typedef EFI_STATUS (EFIAPI *STOCK_RPTR_READ)(VOID *, UINT16, UINT8 *);
typedef EFI_STATUS (EFIAPI *STOCK_RPTR_WRITE)(VOID *, UINT16, UINT8);
typedef VOID (EFIAPI *STOCK_RPTR_WAIT)(UINTN);
typedef struct {
  VOID *Context;
  STOCK_RPTR_READ Read;
  STOCK_RPTR_WRITE Write;
  STOCK_RPTR_WAIT Wait;
} STOCK_RPTR_IO;

STATIC EFI_STATUS
StockRepeaterUpdate (STOCK_RPTR_IO *Io, UINT16 Register, UINT8 Mask, UINT8 Value)
{
  UINT8 Before, After, Wanted;
  EFI_STATUS Status = Io->Read (Io->Context, Register, &Before);
  if (EFI_ERROR (Status)) return Status;
  Wanted = (UINT8)((Before & ~Mask) | (Value & Mask));
  if (Wanted != Before) {
    Status = Io->Write (Io->Context, Register, Wanted);
    if (EFI_ERROR (Status)) return Status;
  }
  Status = Io->Read (Io->Context, Register, &After);
  if (EFI_ERROR (Status)) return Status;
  return After == Wanted ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
StockRepeaterReady (STOCK_RPTR_IO *Io, UINT8 *StatusByte)
{
  EFI_STATUS Status;
  UINTN Index;
  if (Io == NULL || Io->Read == NULL || Io->Write == NULL || Io->Wait == NULL || StatusByte == NULL)
    return EFI_INVALID_PARAMETER;
  for (Index = 0; Index < 5; ++Index) {
    Status = Io->Read (Io->Context, 0xFD08, StatusByte);
    if (EFI_ERROR (Status)) return Status;
    if ((*StatusByte & BIT7) != 0) return EFI_SUCCESS;
    if (Index != 4) Io->Wait (20);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
StockRepeaterPrepare (STOCK_RPTR_IO *Io, UINT8 *StatusByte)
{
  STATIC CONST UINT8 Values[12] = { 0, 15, 4, 7, 3, 1, 3, 5, 2, 0, 3, 2 };
  STATIC CONST UINT8 Masks[12]  = { 7, 15, 7, 7, 7, 3, 3, 7, 3, 3, 3, 3 };
  EFI_STATUS Status;
  UINTN Index;
  if (Io == NULL || Io->Read == NULL || Io->Write == NULL || Io->Wait == NULL || StatusByte == NULL)
    return EFI_INVALID_PARAMETER;
  *StatusByte = 0;
  Status = StockRepeaterUpdate (Io, 0xFD40, 3, BIT0);
  if (!EFI_ERROR (Status)) Status = StockRepeaterUpdate (Io, 0xFD41, BIT7, 0);
  // Kernel reset(true): release repeater reset before eUSB2 PHY POR/init.
  if (!EFI_ERROR (Status)) Status = StockRepeaterUpdate (Io, 0xFD46, BIT7, BIT7);
  for (Index = 0; Index < ARRAY_SIZE (Values) && !EFI_ERROR (Status); ++Index)
    Status = StockRepeaterUpdate (Io, (UINT16)(0xFD50 + Index), Masks[Index], Values[Index]);
  // Host-mode workaround survives regulator/enable cycling. Clear in device mode.
  if (!EFI_ERROR (Status)) Status = StockRepeaterUpdate (Io, 0xFDE8, BIT6, 0);
  if (!EFI_ERROR (Status)) Status = StockRepeaterUpdate (Io, 0xFDED, BIT6, 0);
  return EFI_ERROR (Status) ? Status : StockRepeaterReady (Io, StatusByte);
}
