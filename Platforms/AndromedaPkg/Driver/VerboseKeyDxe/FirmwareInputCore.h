// SPDX-License-Identifier: BSD-2-Clause-Patent
// The keypad has no release event. Require a quiet gap after every raw Power
// repeat, including discarded repeats, before allowing the next activation.
#define FIRMWARE_POWER_GAP_MS 500U
typedef struct {
  UINT64 PowerUntil;
  EFI_KEY_DATA Keys[32];
  UINTN Read, Write, Count;
} FIRMWARE_INPUT_CORE;
STATIC BOOLEAN FiAccept (FIRMWARE_INPUT_CORE *Input, CONST EFI_INPUT_KEY *Key, UINT64 Now)
{
  BOOLEAN Accept;
  if (Key->UnicodeChar!=CHAR_CARRIAGE_RETURN) return TRUE;
  Accept=Now>=Input->PowerUntil;
  Input->PowerUntil=MAX(Input->PowerUntil,Now+FIRMWARE_POWER_GAP_MS);
  return Accept;
}
STATIC EFI_STATUS FiInject (FIRMWARE_INPUT_CORE *Input, CONST EFI_KEY_DATA *Key)
{
  if (Key==NULL || (Key->Key.ScanCode==0 && Key->Key.UnicodeChar==0)) return EFI_INVALID_PARAMETER;
  if (Input->Count==ARRAY_SIZE(Input->Keys)) return EFI_NOT_READY;
  Input->Keys[Input->Write]=*Key;
  Input->Write=(Input->Write+1)%ARRAY_SIZE(Input->Keys);Input->Count++;
  return EFI_SUCCESS;
}
STATIC BOOLEAN FiRead (FIRMWARE_INPUT_CORE *Input, EFI_KEY_DATA *Key)
{
  if (Input->Count==0) return FALSE;
  *Key=Input->Keys[Input->Read];
  Input->Read=(Input->Read+1)%ARRAY_SIZE(Input->Keys);Input->Count--;
  return TRUE;
}
