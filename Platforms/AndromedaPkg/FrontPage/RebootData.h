// Native ResetRuntimeDxe 2930..2960 reads reason at StrSize(RESET_PARAM)=24.
// ABL RebootDevice 49090 passes 26 bytes; byte24 is the reason, byte25 padding.
#pragma pack (push, 1)
typedef struct {
  CHAR16 Text[12];
  UINT8 Reason;
  UINT8 Padding;
} FP_RESET_DATA;
#pragma pack (pop)
STATIC_ASSERT (sizeof (FP_RESET_DATA) == 26, "stock reset data size");
STATIC_ASSERT (OFFSET_OF (FP_RESET_DATA, Reason) == 24, "stock reset reason offset");
STATIC BOOLEAN
FpEncodeReset (UINT8 Reason, FP_RESET_DATA *Data)
{
  CONST CHAR16 Text[12] = L"RESET_PARAM";
  if (Data == NULL || (Reason != 1 && Reason != 2)) return FALSE;
  CopyMem (Data->Text, Text, sizeof (Text));
  Data->Reason = Reason;
  Data->Padding = 0;
  return TRUE;
}

typedef EFI_STATUS (EFIAPI *FP_PON_GET)(UINT32, UINT32, UINT8 *);
typedef EFI_STATUS (EFIAPI *FP_PON_SET)(UINT32, UINT32, UINT8, UINT8);
typedef struct {
  UINT64 Prefix[4];
  FP_PON_GET Get;
  FP_PON_SET Set;
} FP_RESET_PON;
STATIC_ASSERT (OFFSET_OF (FP_RESET_PON, Get) == 0x20, "stock PON getter ABI");
STATIC_ASSERT (OFFSET_OF (FP_RESET_PON, Set) == 0x28, "stock PON setter ABI");
STATIC UINT32 mFpResetStage;
STATIC UINT8 mFpResetBefore, mFpResetActual;
STATIC EFI_STATUS
FpStageReset (FP_RESET_PON *Pon, UINT8 Reason)
{
  EFI_STATUS Status;
  UINT8 Previous = 0, Actual = 0, Value;
  if (Pon == NULL || Pon->Get == NULL || Pon->Set == NULL || (Reason != 1 && Reason != 2)) return EFI_INVALID_PARAMETER;
  Value = Reason << 1;
  mFpResetStage=1;mFpResetBefore=mFpResetActual=0;
  Status = Pon->Get (0, 3, &Previous);
  mFpResetBefore=Previous;
  if (EFI_ERROR (Status)) return Status;
  mFpResetStage=2;
  Status = Pon->Set (0, 3, Value, 0xFE);
  if (EFI_ERROR (Status)) return Status;
  mFpResetStage=3;
  Status = Pon->Get (0, 3, &Actual);
  mFpResetActual=Actual;
  if (!EFI_ERROR (Status) && (Actual & 0xFE) == Value) return EFI_SUCCESS;
  // Verification failed: restore the prior reason, and remain in the GUI.
  mFpResetStage=4;
  if (EFI_ERROR(Pon->Set (0, 3, Previous, 0xFE))) return EFI_ABORTED;
  return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
}
