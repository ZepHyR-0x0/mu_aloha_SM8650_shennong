// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef FIRMWARE_INPUT_H
#define FIRMWARE_INPUT_H
#include <Protocol/SimpleTextInEx.h>
#define FIRMWARE_INPUT_PROTOCOL_GUID \
  {0x292e4d32,0x7bda,0x4c5c,{0xa9,0x37,0x8c,0x56,0x16,0x74,0x72,0x08}}
typedef struct _FIRMWARE_INPUT_PROTOCOL FIRMWARE_INPUT_PROTOCOL;
struct _FIRMWARE_INPUT_PROTOCOL {
  EFI_STATUS (EFIAPI *Inject)(FIRMWARE_INPUT_PROTOCOL *, CONST EFI_KEY_DATA *);
  VOID (EFIAPI *SuppressPower)(FIRMWARE_INPUT_PROTOCOL *, UINTN Milliseconds);
};
STATIC inline VOID FirmwareSuppressPower (UINTN Milliseconds)
{
  EFI_GUID Guid=FIRMWARE_INPUT_PROTOCOL_GUID;
  FIRMWARE_INPUT_PROTOCOL *Input=NULL;
  if (!EFI_ERROR(gBS->LocateProtocol(&Guid,NULL,(VOID **)&Input)) && Input!=NULL)
    Input->SuppressPower(Input,Milliseconds);
}
#endif
