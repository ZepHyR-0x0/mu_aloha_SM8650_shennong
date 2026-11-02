/** @file
  A minimal PCD protocol, backed by a small in-memory table.

  Why this exists:

  The platform has no PCD database. PcdLib was mapped to BasePcdLibNull to keep 55
  modules from waiting on gEfiPcdProtocolGuid, but BasePcdLibNull does not store
  anything - every LibPcdGet* asserts and returns 0, and every LibPcdSet* is a no-op.
  That is fine for a module that only reads a FixedAtBuild PCD, because the build
  resolves those to literals and never calls the library at all. It is not fine for
  a DynamicEx one.

  MsGraphicsPkg has exactly one, and the whole GUI depends on it:

    FrontPage.c:1930   PcdSet64S (PcdCurrentPointerState, (UINT64)(UINTN)&mDisplayEngineState);
    FormDisplay.c:2068 (DISPLAY_ENGINE_SHARED_STATE *)(UINTN)PcdGet64 (PcdCurrentPointerState);
    FormDisplay.c:2087 ASSERT (NULL != FormData && NULL != MasterFrameSharedState);

  FrontPage writes a pointer into the PCD and DisplayEngine reads it back - that is
  the shared buffer between the two. With BasePcdLibNull the write vanished, the read
  returned 0, and the assert fired. The interface rendered and then exited.

  So this driver publishes the real protocol and PcdLib goes back to DxePcdLib. The
  table is keyed by (token space GUID, token number), defaults to zero, and is large
  enough for far more entries than this platform actually uses. A PCD that is never
  written reads as zero, which is what a DynamicEx PCD initialised to 0 should do.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/Pcd.h>

#define PCD_TABLE_ENTRIES  64

typedef struct {
  EFI_GUID  Guid;
  UINTN     Token;
  UINT64    Value;
  BOOLEAN   Used;
} PCD_SLOT;

STATIC PCD_SLOT  mSlots[PCD_TABLE_ENTRIES];

STATIC
PCD_SLOT *
FindSlot (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           Token,
  IN BOOLEAN         Create
  )
{
  UINTN  Index;
  UINTN  Free = PCD_TABLE_ENTRIES;

  for (Index = 0; Index < PCD_TABLE_ENTRIES; Index++) {
    if (!mSlots[Index].Used) {
      if (Free == PCD_TABLE_ENTRIES) {
        Free = Index;
      }
      continue;
    }
    if ((mSlots[Index].Token == Token) && CompareGuid (&mSlots[Index].Guid, Guid)) {
      return &mSlots[Index];
    }
  }

  if (!Create || (Free == PCD_TABLE_ENTRIES)) {
    return NULL;
  }

  CopyGuid (&mSlots[Free].Guid, Guid);
  mSlots[Free].Token = Token;
  mSlots[Free].Value = 0;
  mSlots[Free].Used  = TRUE;
  return &mSlots[Free];
}

STATIC
UINT64
GetValue (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           Token
  )
{
  PCD_SLOT  *Slot = FindSlot (Guid, Token, FALSE);

  return (Slot == NULL) ? 0 : Slot->Value;
}

STATIC
VOID
SetValue (
  IN CONST EFI_GUID  *Guid,
  IN UINTN           Token,
  IN UINT64          Value
  )
{
  PCD_SLOT  *Slot = FindSlot (Guid, Token, TRUE);

  if (Slot != NULL) {
    Slot->Value = Value;
  }
}

//
// Token-space forms take the token space GUID from a global variable named after the
// token space, which the caller cannot pass in. Nothing in this platform uses them,
// so they read and write through the null GUID rather than pretending otherwise.
//
STATIC EFI_GUID  mNullGuid = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

STATIC VOID EFIAPI SetSku (IN UINTN Sku) { (VOID)Sku; }

STATIC UINT8  EFIAPI Get8 (IN UINTN T)       { return (UINT8)GetValue (&mNullGuid, T); }
STATIC UINT16 EFIAPI Get16 (IN UINTN T)      { return (UINT16)GetValue (&mNullGuid, T); }
STATIC UINT32 EFIAPI Get32 (IN UINTN T)      { return (UINT32)GetValue (&mNullGuid, T); }
STATIC UINT64 EFIAPI Get64 (IN UINTN T)      { return GetValue (&mNullGuid, T); }
STATIC VOID * EFIAPI GetPtr (IN UINTN T)     { return (VOID *)(UINTN)GetValue (&mNullGuid, T); }
STATIC BOOLEAN EFIAPI GetBool (IN UINTN T)   { return (BOOLEAN)(GetValue (&mNullGuid, T) != 0); }
STATIC UINTN EFIAPI GetSize (IN UINTN T)     { return sizeof (UINT64); }

STATIC UINT8  EFIAPI Get8Ex (IN CONST EFI_GUID *G, IN UINTN T)  { return (UINT8)GetValue (G, T); }
STATIC UINT16 EFIAPI Get16Ex (IN CONST EFI_GUID *G, IN UINTN T) { return (UINT16)GetValue (G, T); }
STATIC UINT32 EFIAPI Get32Ex (IN CONST EFI_GUID *G, IN UINTN T) { return (UINT32)GetValue (G, T); }
STATIC UINT64 EFIAPI Get64Ex (IN CONST EFI_GUID *G, IN UINTN T) { return GetValue (G, T); }
STATIC VOID * EFIAPI GetPtrEx (IN CONST EFI_GUID *G, IN UINTN T) { return (VOID *)(UINTN)GetValue (G, T); }
STATIC BOOLEAN EFIAPI GetBoolEx (IN CONST EFI_GUID *G, IN UINTN T) { return (BOOLEAN)(GetValue (G, T) != 0); }
STATIC UINTN EFIAPI GetSizeEx (IN CONST EFI_GUID *G, IN UINTN T) { (VOID)G; (VOID)T; return sizeof (UINT64); }

STATIC EFI_STATUS EFIAPI Set8 (IN UINTN T, IN UINT8 V)   { SetValue (&mNullGuid, T, V); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Set16 (IN UINTN T, IN UINT16 V) { SetValue (&mNullGuid, T, V); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Set32 (IN UINTN T, IN UINT32 V) { SetValue (&mNullGuid, T, V); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Set64 (IN UINTN T, IN UINT64 V) { SetValue (&mNullGuid, T, V); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI SetPtr (IN UINTN T, IN OUT UINTN *SizeOfBuffer, IN VOID *Buffer)
{
  (VOID)SizeOfBuffer;
  SetValue (&mNullGuid, T, (UINT64)(UINTN)Buffer);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI SetBool (IN UINTN T, IN BOOLEAN V) { SetValue (&mNullGuid, T, V ? 1 : 0); return EFI_SUCCESS; }

STATIC EFI_STATUS EFIAPI Set8Ex (IN CONST EFI_GUID *G, IN UINTN T, IN UINT8 V)   { SetValue (G, T, V); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Set16Ex (IN CONST EFI_GUID *G, IN UINTN T, IN UINT16 V) { SetValue (G, T, V); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Set32Ex (IN CONST EFI_GUID *G, IN UINTN T, IN UINT32 V) { SetValue (G, T, V); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Set64Ex (IN CONST EFI_GUID *G, IN UINTN T, IN UINT64 V) { SetValue (G, T, V); return EFI_SUCCESS; }

STATIC EFI_STATUS EFIAPI SetPtrEx (IN CONST EFI_GUID *G, IN UINTN T, IN OUT UINTN *SizeOfBuffer, IN VOID *Buffer)
{
  (VOID)SizeOfBuffer;
  SetValue (G, T, (UINT64)(UINTN)Buffer);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI SetBoolEx (IN CONST EFI_GUID *G, IN UINTN T, IN BOOLEAN V) { SetValue (G, T, V ? 1 : 0); return EFI_SUCCESS; }

STATIC EFI_STATUS EFIAPI CallbackOnSet (
  IN CONST EFI_GUID        *Guid,
  IN UINTN                 TokenNumber,
  IN PCD_PROTOCOL_CALLBACK CallBackFunction
  )
{
  (VOID)Guid; (VOID)TokenNumber; (VOID)CallBackFunction;
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI CancelCallback (
  IN CONST EFI_GUID        *Guid,
  IN UINTN                 TokenNumber,
  IN PCD_PROTOCOL_CALLBACK CallBackFunction
  )
{
  (VOID)Guid; (VOID)TokenNumber; (VOID)CallBackFunction;
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI GetNextToken (IN CONST EFI_GUID *Guid, IN OUT UINTN *TokenNumber)
{
  (VOID)Guid; (VOID)TokenNumber;
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS EFIAPI GetNextTokenSpace (IN OUT CONST EFI_GUID **Guid)
{
  (VOID)Guid;
  return EFI_NOT_FOUND;
}

STATIC PCD_PROTOCOL  mPcdProtocol = {
  SetSku,
  Get8,  Get16, Get32, Get64, GetPtr, GetBool, GetSize,
  Get8Ex, Get16Ex, Get32Ex, Get64Ex, GetPtrEx, GetBoolEx, GetSizeEx,
  Set8,  Set16, Set32, Set64, SetPtr, SetBool,
  Set8Ex, Set16Ex, Set32Ex, Set64Ex, SetPtrEx, SetBoolEx,
  CallbackOnSet,
  CancelCallback,
  GetNextToken,
  GetNextTokenSpace
};

EFI_STATUS
EFIAPI
PcdShimDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  ZeroMem (mSlots, sizeof (mSlots));

  return gBS->InstallMultipleProtocolInterfaces (
                &ImageHandle,
                &gEfiPcdProtocolGuid,
                &mPcdProtocol,
                NULL
                );
}
