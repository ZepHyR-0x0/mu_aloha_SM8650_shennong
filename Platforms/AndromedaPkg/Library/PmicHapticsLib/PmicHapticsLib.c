/**
  Bounded Shennong haptics playback shared by GUI and Shell.
  GUID 0133062C resolves to PmicDxeLa +32448, Play +1EF4 -> +128C8,
  GetDeviceIndex +2038 -> +12D10. The reverse dump's library names for
  these functions are misleading; their instructions and Mi Haptic strings
  identify the implementation. The linked native writer at +1DAB4 is a no-op.
  Use public SPMI for the verified native board configuration, restoring every
  changed configuration byte only after playback has stopped.
  Android's lra-f0-default is in Hz and must not be passed to SetLraResonance.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/PmicHapticsLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>

typedef struct {
  UINT64 Revision;
  EFI_STATUS (EFIAPI *Play)(UINT32 DeviceIndex, UINT32 Enable);
  VOID *Reserved[9];
  EFI_STATUS (EFIAPI *GetDeviceIndex)(UINT32 *DeviceIndex);
  VOID *SetLraResonance;
} PMIC_HAPTICS_PROTOCOL;
STATIC_ASSERT (OFFSET_OF (PMIC_HAPTICS_PROTOCOL, Play) == 8, "Haptics Play ABI");
STATIC_ASSERT (OFFSET_OF (PMIC_HAPTICS_PROTOCOL, GetDeviceIndex) == 0x58, "Haptics index ABI");
STATIC EFI_GUID mGuid = {0x0133062C,0xC2A3,0x4C1A,{0x85,0x52,0xE7,0xCB,0x0E,0x5F,0xDE,0xA9}};
STATIC PMIC_HAPTICS_PROTOCOL *mProtocol;
STATIC UINT32 mIndex = MAX_UINT32;
// TRUE also means an unsuccessful Play may have partially enabled hardware.
STATIC BOOLEAN mStopRequired;
typedef struct {
  UINT64 Revision;
  UINT32 (EFIAPI *Read)(VOID *, UINT32, UINT32, UINT32, UINT32, UINT8 *, UINT32, UINT32 *);
  UINT32 (EFIAPI *Write)(VOID *, UINT32, UINT32, UINT32, UINT32, UINT8 *, UINT32);
} HAPTICS_SPMI;
STATIC EFI_GUID mSpmiGuid = {0xFA5F306B,0xF47D,0x4AC4,{0xA4,0x7D,0x88,0x2F,0x82,0x04,0xEC,0x30}};
STATIC HAPTICS_SPMI *mSpmi;
STATIC UINTN mPmicBase;
// Persistent configuration only. F066 is write-one-to-clear, never saved/restored.
// F063 is the auto-resonance register in the verified native register map.
STATIC CONST UINT16 mRegisters[] = {0xF060,0xF048,0xF067,0xF126,0xF05C,0xF05D,0xF063,0xF046};
// Captured Shennong DT: 130 Hz, 7692 us, normal VMAX 1600 mV,
// calibration VMAX 1750 mV. TLRA_STEP_US is 5: 7692/5 = 0x602.
// Native 0x477 remains an identity check, not the selected motor period.
STATIC CONST UINT8 mValues[] = {0xB1,0x20,0x0A,0xFF,0x06,0x02,0x00,0x80};
STATIC UINT32 mVmaxMv = 1600;
STATIC UINT8 mAmplitude = 0xFF;
STATIC PMIC_HAPTICS_MODE mMode = PmicHapticsNativeDuty;
STATIC UINT8 mSaved[ARRAY_SIZE (mRegisters)], mSavedPlay;
STATIC BOOLEAN mRestore[ARRAY_SIZE (mRegisters)];
STATIC VOID Trace (CONST CHAR8 *Stage, EFI_STATUS Status);

// Exact prefixes of the linked Shennong providers. No internal function calls.
STATIC EFI_STATUS ValidateOwner (VOID *Interface, UINT32 TableRva, CONST UINT32 *Slots, UINTN Count)
{
  EFI_HANDLE *Handles = NULL;
  EFI_STATUS Status;
  UINTN Images = 0, I, J, Base;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Images, &Handles);
  if (EFI_ERROR (Status)) return Status;
  Status = EFI_UNSUPPORTED;
  for (I = 0; I < Images; I++) {
    EFI_LOADED_IMAGE_PROTOCOL *Image = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiLoadedImageProtocolGuid, (VOID **)&Image)) || Image == NULL) continue;
    Base = (UINTN)Image->ImageBase;
    if ((UINTN)Interface < Base || (UINTN)Interface - Base != TableRva ||
        TableRva > Image->ImageSize || (Count+1)*sizeof(UINT64) > Image->ImageSize-TableRva) continue;
    for (J = 0; J < Count; J++) {
      if (Slots[J] >= Image->ImageSize ||
          ReadUnaligned64 ((UINT64 *)((UINT8 *)Interface + (J+1)*8)) != Base + Slots[J]) break;
    }
    if (J == Count) {
      if (TableRva == 0x32448) {
        if (Image->ImageSize < 0x3A9C8) continue;
        mPmicBase = Base;
      }
      Status = EFI_SUCCESS; break;
    }
  }
  gBS->FreePool (Handles);
  return Status;
}

STATIC EFI_STATUS ReadByte (UINT16 Register, UINT8 *Value)
{
  UINT32 Done = 0;
  UINT32 Rc = mSpmi->Read (mSpmi, 0, 7, 0, Register, Value, 1, &Done);
  if (Rc != 0 || Done != 1) {
    CHAR8 Text[128];
    UINTN N = AsciiSPrint (Text,sizeof(Text),"[HAPTIC IO] read sid=7 reg=%04x rc=%u done=%u\r\n",Register,Rc,Done);
    SerialPortWrite ((UINT8 *)Text,N);
  }
  return Rc == 0 && Done == 1 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS WriteByte (UINT16 Register, UINT8 Value)
{
  UINTN I;
  // Enforce the allowlist at the physical write boundary as well as callers.
  if (Register != 0xF04C && Register != 0xF066) {
    for (I = 0; I < ARRAY_SIZE (mRegisters); I++) if (Register == mRegisters[I]) break;
    if (I == ARRAY_SIZE (mRegisters)) return EFI_ACCESS_DENIED;
  }
  UINT32 Rc = mSpmi->Write (mSpmi, 0, 7, 0, Register, &Value, 1);
  CHAR8 Text[128];
  UINTN N = AsciiSPrint (Text,sizeof(Text),"[HAPTIC IO] write sid=7 reg=%04x target=%02x rc=%u\r\n",Register,Value,Rc);
  SerialPortWrite ((UINT8 *)Text,N);
  return Rc == 0 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS WriteChecked (UINT16 Register, UINT8 Value)
{
  UINT8 Actual = 0;
  EFI_STATUS Status = WriteByte (Register, Value);
  if (!EFI_ERROR (Status)) {
    Status = ReadByte (Register, &Actual);
    if (!EFI_ERROR (Status) && Actual != Value) Status = EFI_DEVICE_ERROR;
  }
  CHAR8 Text[128];
  UINTN N = AsciiSPrint (Text,sizeof(Text),"[HAPTIC CHECK] reg=%04x target=%02x actual=%02x -> %r\r\n",Register,Value,Actual,Status);
  SerialPortWrite ((UINT8 *)Text,N);
  return Status;
}

STATIC EFI_STATUS ClearFault (VOID)
{
  UINT8 Fault = 0;
  EFI_STATUS Status = WriteByte (0xF066, 0x07);
  if (!EFI_ERROR (Status)) {
    Status = ReadByte (0xF00C, &Fault);
    if (!EFI_ERROR (Status) && (Fault & 0x07) != 0) Status = EFI_DEVICE_ERROR;
  }
  CHAR8 Text[128];
  UINTN N = AsciiSPrint (Text,sizeof(Text),"[HAPTIC W1C] clear=07 status_reg=F00C fault=%02x -> %r\r\n",Fault,Status);
  SerialPortWrite ((UINT8 *)Text,N);
  return Status;
}

// Validate heap-backed native objects before reading them. Never treat an
// arbitrary native pointer as MMIO or call private native entry points.
STATIC BOOLEAN BootData (UINTN Address, UINTN Length)
{
  EFI_MEMORY_DESCRIPTOR Map[512], Descriptor;
  UINTN Bytes = sizeof (Map), Key, Stride = 0, Offset;
  UINT32 Version;
  if (Address == 0 || Length == 0 || Address > MAX_UINTN - Length ||
      EFI_ERROR (gBS->GetMemoryMap (&Bytes, Map, &Key, &Stride, &Version)) ||
      Bytes > sizeof (Map) || Stride < sizeof (Descriptor) || Stride > Bytes) return FALSE;
  for (Offset = 0; Offset <= Bytes - Stride; Offset += Stride) {
    UINT64 Span;
    CopyMem (&Descriptor, (UINT8 *)Map + Offset, sizeof (Descriptor));
    if (Descriptor.Type != EfiBootServicesData ||
        (Descriptor.Attribute & (EFI_MEMORY_RP | EFI_MEMORY_RO)) != 0 ||
        Descriptor.NumberOfPages > MAX_UINT64 / EFI_PAGE_SIZE) continue;
    Span = Descriptor.NumberOfPages * EFI_PAGE_SIZE;
    if (Address >= Descriptor.PhysicalStart && Address - Descriptor.PhysicalStart <= Span &&
        Length <= Span - (Address - Descriptor.PhysicalStart)) return TRUE;
  }
  return FALSE;
}

STATIC EFI_STATUS ValidateBoard (VOID)
{
  UINTN Object, Comm, Config;
  if (mPmicBase == 0 || ReadUnaligned32 ((UINT32 *)(mPmicBase + 0x1DAB4)) != 0x2A1F03E0 ||
      ReadUnaligned32 ((UINT32 *)(mPmicBase + 0x1DAB8)) != 0xD65F03C0 ||
      *(UINT8 *)(mPmicBase + 0x39AF4) != 0 || *(UINT8 *)(mPmicBase + 0x39AF5) != 1) return EFI_UNSUPPORTED;
  Object = (UINTN)ReadUnaligned64 ((UINT64 *)(mPmicBase + 0x3A3D0 + 7*8));
  Config = (UINTN)ReadUnaligned64 ((UINT64 *)(mPmicBase + 0x3A9C0));
  if (Config == 0) {
    // cfg is lazy. The verified no-op Stop path initializes the native DT
    // cache without starting the motor; retain and verify its original values.
    EFI_STATUS Status = mProtocol->Play (mIndex, 0);
    Trace ("Initialize native configuration through disabled Stop", Status);
    if (EFI_ERROR (Status)) return Status;
    Config = (UINTN)ReadUnaligned64 ((UINT64 *)(mPmicBase + 0x3A9C0));
  }
  if (!BootData (Object, 0x20) || !BootData (Config, 0x24)) return EFI_UNSUPPORTED;
  Comm = (UINTN)ReadUnaligned64 ((UINT64 *)Object);
  if (!BootData (Comm, 8) || ReadUnaligned32 ((UINT32 *)Comm) != 7 ||
      ReadUnaligned64 ((UINT64 *)(Object + 0x10)) != mPmicBase + 0x32C6E ||
      ReadUnaligned16 ((UINT16 *)(Object + 0x18)) != 0xF000 ||
      *(UINT8 *)(Object + 0x1A) != 0x40 ||
      ReadUnaligned32 ((UINT32 *)Config) != 1 ||
      ReadUnaligned32 ((UINT32 *)(Config + 4)) != 10 ||
      ReadUnaligned32 ((UINT32 *)(Config + 8)) != 1400 ||
      ReadUnaligned32 ((UINT32 *)(Config + 0xC)) != 0x477 ||
      ReadUnaligned32 ((UINT32 *)(Config + 0x10)) != 1 ||
      ReadUnaligned32 ((UINT32 *)(Config + 0x14)) != 0) return EFI_UNSUPPORTED;
  Trace ("Board SID7:F000 VMAX=1400 raw-period=0477; native writer no-op", EFI_SUCCESS);
  return EFI_SUCCESS;
}

STATIC VOID Snapshot (CONST CHAR8 *Stage)
{
  CONST UINT16 Registers[] = {0xF001,0xF005,0xF046,0xF04C,0xF126,0xF00B,0xF00C,0xF048,0xF049,0xF060,0xF063,0xF067,0xF05C,0xF05D,0xF066,0xF068};
  UINTN I, N;
  UINT8 Value;
  EFI_STATUS Status;
  CHAR8 Text[128];
  for (I = 0; I < ARRAY_SIZE (Registers); I++) {
    Value = 0;
    Status = ReadByte (Registers[I], &Value);
    N = AsciiSPrint (Text, sizeof(Text), "[HAPTIC REG] %a sid=7 reg=%04x -> %r value=%02x\r\n",
                    Stage, Registers[I], Status, Value);
    SerialPortWrite ((UINT8 *)Text, N);
  }
}

STATIC EFI_STATUS RestoreConfiguration (VOID)
{
  EFI_STATUS Status = EFI_SUCCESS, Current;
  UINTN I;
  for (I = 0; I < ARRAY_SIZE (mRegisters); I++) {
    if (!mRestore[I]) continue;
    Current = WriteChecked (mRegisters[I], mSaved[I]);
    if (!EFI_ERROR (Current)) mRestore[I] = FALSE;
    else Status = Current;
  }
  Trace ("Restore configuration", Status);
  return Status;
}

STATIC EFI_STATUS PrepareModule (VOID)
{
  EFI_STATUS Status;
  UINT8 Type, Play;
  UINTN I;
  Status = ValidateBoard ();
  if (EFI_ERROR (Status)) return Status;
  // PmicDxe's register map names F005 as peripheral type 0x0A. The captured
  // Shennong DT places HV-haptics on SID 7 at F000/F100/F200.
  Status = ReadByte (0xF005, &Type);
  if (EFI_ERROR (Status)) return Status;
  if (Type != 0x0A) return EFI_UNSUPPORTED;
  Status = ReadByte (0xF04C, &Play);
  if (EFI_ERROR (Status)) return Status;
  if ((Play & BIT7) != 0) return EFI_ALREADY_STARTED;
  if (Play != 1) return EFI_UNSUPPORTED;
  mSavedPlay = Play;
  // Finish all reads before making any change. Fault status must be clear.
  for (I = 0; I < ARRAY_SIZE (mRegisters); I++) {
    Status = ReadByte (mRegisters[I], &mSaved[I]);
    if (EFI_ERROR (Status)) return Status;
  }
  Status = ReadByte (0xF00B, &Type);
  // HPWR_DISABLED=0 and HPWR_READY=3 are both valid in the vendor kernel.
  if (EFI_ERROR (Status) || ((Type & 3) != 0 && (Type & 3) != 3)) return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  Status = ReadByte (0xF00C, &Type);
  if (EFI_ERROR (Status)) return Status;
  {
    CHAR8 Text[128];
    UINTN N=AsciiSPrint(Text,sizeof(Text),"[HAPTIC FAULT] stopped preflight F00C=%02x mode=%u\r\n",Type,(UINT32)mMode);
    SerialPortWrite((UINT8 *)Text,N);
  }
  // Kernel AUTO_RES_ERROR is a sticky bit (BIT1), cleared before play.
  // An earlier auto-resonance trial must not permanently block direct play.
  // Recover that isolated latch only while stopped; retain HPWR/SC rejection.
  if (Type == BIT1) {
    Status=WriteByte(0xF066,BIT1);
    if (!EFI_ERROR(Status)) Status=ReadByte(0xF00C,&Type);
    if (EFI_ERROR(Status) || Type!=0) return EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
    Trace("Cleared stopped auto-resonance error",EFI_SUCCESS);
  }
  if (Type != 0) return EFI_DEVICE_ERROR;
  Snapshot ("before");
  mStopRequired = TRUE;
  for (I = 0; I < ARRAY_SIZE (mRegisters); I++) {
    UINT8 Value = I == ARRAY_SIZE (mRegisters)-1 ? (mSaved[I] | BIT7) : mValues[I];
    if (mRegisters[I] == 0xF048) Value = (UINT8)(mVmaxMv / 50);
    if (mRegisters[I] == 0xF126) Value = mAmplitude;
    if (mRegisters[I] == 0xF060 && mMode != PmicHapticsNativeDuty) Value = mSaved[I];
    if (mRegisters[I] == 0xF063) {
      Value = mSaved[I];
      if (mMode == PmicHapticsDirect) Value &= (UINT8)~BIT7;
      if (mMode == PmicHapticsAutoRes) Value |= BIT7;
    }
    if (Value == mSaved[I]) continue;
    // A failed write may already have taken effect; retain cleanup ownership.
    mRestore[I] = TRUE;
    Status = WriteChecked (mRegisters[I], Value);
    if (EFI_ERROR (Status)) return Status;
  }
  return ClearFault ();
}

EFI_STATUS PmicHapticsConfigureMode (IN PMIC_HAPTICS_MODE Mode)
{
  if (mStopRequired) return EFI_ALREADY_STARTED;
  if ((UINTN)Mode >= PmicHapticsModeCount) return EFI_INVALID_PARAMETER;
  mMode = Mode;
  CHAR8 Text[128];
  UINTN N = AsciiSPrint (Text,sizeof(Text),"[HAPTIC MODE] mode=%u (0=native-duty 1=direct 2=autores)\r\n",(UINT32)Mode);
  SerialPortWrite ((UINT8 *)Text,N);
  return EFI_SUCCESS;
}

EFI_STATUS PmicHapticsConfigure (IN UINT32 VmaxMv, IN UINT8 Amplitude)
{
  if (mStopRequired) return EFI_ALREADY_STARTED;
  if (VmaxMv < 1400 || VmaxMv > 1750 || VmaxMv % 50 != 0) return EFI_INVALID_PARAMETER;
  mVmaxMv = VmaxMv;
  mAmplitude = Amplitude;
  {
    CHAR8 Text[128];
    UINTN N = AsciiSPrint (Text,sizeof(Text),"[HAPTIC SET] vmax_mv=%u amplitude=%u TLRA=0602 target_hz=130\r\n",VmaxMv,Amplitude);
    SerialPortWrite ((UINT8 *)Text,N);
  }
  return EFI_SUCCESS;
}

EFI_STATUS PmicHapticsConfigureIntensity (IN UINT32 Percent)
{
  if (Percent > 100) return EFI_INVALID_PARAMETER;
  // Match qcom-hv-haptics DIRECT_PLAY gain: amplitude=255*gain/0x7fff.
  // Shennong's ordinary qcom,vmax-mv is 1600; f0-vmax is calibration only.
  UINT32 Gain = 0x7fff * Percent / 100;
  UINT8 Amplitude = (UINT8)(255 * Gain / 0x7fff);
  EFI_STATUS Status = PmicHapticsConfigure (1600, Amplitude);
  CHAR8 Text[160];
  UINTN N = AsciiSPrint (Text,sizeof(Text),"[HAPTIC GAIN] percent=%u ff_gain=%u amplitude=%u vmax_mv=1600 -> %r\r\n",Percent,Gain,Amplitude,Status);
  SerialPortWrite ((UINT8 *)Text,N);
  return Status;
}

STATIC VOID Trace (CONST CHAR8 *Stage, EFI_STATUS Status)
{
  CHAR8 Text[160];
  UINTN Bytes = AsciiSPrint (Text, sizeof (Text), "[HAPTIC] %a index=%u -> %r\r\n", Stage, mIndex, Status);
  SerialPortWrite ((UINT8 *)Text, Bytes);
}

STATIC EFI_STATUS Locate (VOID)
{
  EFI_STATUS Status;
  CONST UINT32 HapticsSlots[] = {0x1EF4,0x1F18,0x1F38,0x1F58,0x1F78,0x1F98,
                                0x1FB8,0x1FD8,0x1FF8,0x2018,0x2038,0x2068};
  CONST UINT32 SpmiSlots[] = {0x13EC,0x14AC};
  if (AsciiStrCmp ((CONST CHAR8 *)PcdGetPtr (PcdSmbiosSystemRetailModel), "shennong") != 0) return EFI_UNSUPPORTED;
  if (mProtocol == NULL) {
    Status = gBS->LocateProtocol (&mGuid, NULL, (VOID **)&mProtocol);
    if (EFI_ERROR (Status)) { mProtocol = NULL; return Status; }
    if (mProtocol == NULL || mProtocol->Revision != 0x10003 ||
        mProtocol->Play == NULL || mProtocol->GetDeviceIndex == NULL) {
      mProtocol = NULL;
      return EFI_UNSUPPORTED;
    }
    Status = ValidateOwner (mProtocol, 0x32448, HapticsSlots, ARRAY_SIZE (HapticsSlots));
    if (EFI_ERROR (Status)) { mProtocol = NULL; return Status; }
  }
  if (mIndex == MAX_UINT32) {
    Status = mProtocol->GetDeviceIndex (&mIndex);
    if (EFI_ERROR (Status) || mIndex != 7) {
      mIndex = MAX_UINT32;
      return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
    }
  }
  if (mSpmi == NULL) {
    Status = gBS->LocateProtocol (&mSpmiGuid, NULL, (VOID **)&mSpmi);
    if (EFI_ERROR (Status)) { mSpmi = NULL; return Status; }
    if (mSpmi == NULL || mSpmi->Revision != 0x10003 || mSpmi->Read == NULL || mSpmi->Write == NULL) {
      mSpmi = NULL;
      return EFI_UNSUPPORTED;
    }
    Status = ValidateOwner (mSpmi, 0x7098, SpmiSlots, ARRAY_SIZE (SpmiSlots));
    if (EFI_ERROR (Status)) { mSpmi = NULL; return Status; }
  }
  return EFI_SUCCESS;
}

EFI_STATUS PmicHapticsDiagnostics (VOID)
{
  EFI_STATUS Status = Locate ();
  if (EFI_ERROR (Status)) { Trace ("Diagnostic locate", Status); return Status; }
  Status = ValidateBoard ();
  Trace ("Diagnostic board", Status);
  if (!EFI_ERROR (Status)) Snapshot ("status");
  return Status;
}

EFI_STATUS PmicHapticsSetEnabled (IN BOOLEAN Enabled)
{
  EFI_STATUS Status;
  EFI_STATUS Cleanup;
  UINTN Attempt;
  if (!Enabled && !mStopRequired) return EFI_SUCCESS;
  if (Enabled && mStopRequired) return EFI_ALREADY_STARTED;
  if (Enabled && mAmplitude == 0) { Trace ("Muted; playback skipped", EFI_SUCCESS); return EFI_SUCCESS; }
  Status = Locate ();
  if (EFI_ERROR (Status)) { Trace ("Locate/index", Status); return Status; }
  if (!Enabled) {
    for (Attempt = 0; Attempt < 3; Attempt++) {
      Status = WriteChecked (0xF04C, mSavedPlay);
      Trace ("Stop", Status);
      if (!EFI_ERROR (Status)) {
        // Allow brake to settle before changing VMAX or module configuration.
        gBS->Stall (20000);
        Status = RestoreConfiguration ();
        if (!EFI_ERROR (Status)) { Snapshot ("after"); mStopRequired = FALSE; return EFI_SUCCESS; }
      }
      if (Attempt < 2) gBS->Stall (10000);
    }
    return Status;
  }
  Status = PrepareModule ();
  if (EFI_ERROR (Status)) {
    Trace ("Prepare configuration", Status);
    if (mStopRequired) {
      Cleanup = PmicHapticsSetEnabled (FALSE);
      if (EFI_ERROR (Cleanup)) return Cleanup;
    }
    return Status;
  }
  Status = WriteChecked (0xF04C, 0x81);
  Trace ("Play (verified native configuration via SPMI)", Status);
  Snapshot ("during");
  if (EFI_ERROR (Status)) {
    Cleanup = PmicHapticsSetEnabled (FALSE);
    if (EFI_ERROR (Cleanup)) return Cleanup;
  }
  return Status;
}

EFI_STATUS PmicHapticsPulse (IN UINTN Microseconds)
{
  EFI_STATUS Status;
  EFI_STATUS StopStatus;
  if (Microseconds == 0 || Microseconds > 1000000) return EFI_INVALID_PARAMETER;
  if (mStopRequired) return EFI_ALREADY_STARTED;
  if (mAmplitude == 0) { Trace ("Muted; pulse skipped", EFI_SUCCESS); return EFI_SUCCESS; }
  Status = PmicHapticsSetEnabled (TRUE);
  if (!EFI_ERROR (Status)) gBS->Stall (Microseconds);
  StopStatus = PmicHapticsSetEnabled (FALSE);
  return EFI_ERROR (StopStatus) ? StopStatus : Status;
}

BOOLEAN PmicHapticsIsEnabled (VOID) { return mStopRequired; }
