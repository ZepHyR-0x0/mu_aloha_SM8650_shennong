/** Read-only late PMIC/secure-service observations for the bring-up GUI. */
#include <Uefi.h>
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>

// PmicDxeLa ADC interface: revision 10004, +8 GetInputProperties(Name,Len,
// Channel[2]), +10 Read(Channel[2],Result[5]). Neither method has a This arg.
// Verified against native +3BA0/+3C04 and the five output stores at +3CAC.
typedef struct { UINT32 Device, Channel; } HEALTH_ADC_CHANNEL;
typedef struct {
  UINT32 Status;
  INT32 Physical;
  UINT32 Percent, Microvolts, Code;
} HEALTH_ADC_RESULT;
typedef struct { UINT32 Stage; HEALTH_ADC_CHANNEL Channel; } HEALTH_ADC_TRACE;
typedef struct {
  UINT64 Revision;
  EFI_STATUS (EFIAPI *GetInputProperties)(CONST CHAR8 *, UINT32, HEALTH_ADC_CHANNEL *);
  EFI_STATUS (EFIAPI *Read)(HEALTH_ADC_CHANNEL *, HEALTH_ADC_RESULT *);
} HEALTH_ADC;

typedef struct _HEALTH_SCM HEALTH_SCM;
struct _HEALTH_SCM {
  UINT64 Revision;
  VOID *Methods[4];
  EFI_STATUS (EFIAPI *SendCommand)(HEALTH_SCM *, UINT32, UINT32 *, VOID *, UINTN, VOID *, UINTN);
};
STATIC_ASSERT (OFFSET_OF (HEALTH_SCM, SendCommand) == 0x28, "SCM ABI");
STATIC_ASSERT (sizeof (HEALTH_ADC_CHANNEL) == 8, "ADC channel ABI");
STATIC_ASSERT (sizeof (HEALTH_ADC_RESULT) == 20, "ADC result ABI");

STATIC EFI_GUID mHealthAdcGuid = {
  0x61740CC4, 0x916F, 0x4471, {0xB0,0xDC,0x00,0xC6,0xF4,0x3A,0x7F,0xC6}
};
STATIC EFI_GUID mHealthScmGuid = {
  0x77ED108D, 0x8524, 0x4B8B, {0x9D,0x2E,0x34,0x98,0x7A,0xEC,0xB9,0xC1}
};

EFI_STATUS
FpReadAdcDetailed (IN HEALTH_ADC *Adc, IN CONST CHAR8 *Name, OUT HEALTH_ADC_RESULT *Result,
                   OUT HEALTH_ADC_TRACE *Trace OPTIONAL)
{
  HEALTH_ADC_CHANNEL Channel;
  EFI_STATUS Status;
  UINTN Length;

  if (Name == NULL || Result == NULL) return EFI_INVALID_PARAMETER;
  if (Trace != NULL) ZeroMem (Trace, sizeof (*Trace));
  ZeroMem (Result, sizeof (*Result));
  if (Adc == NULL) return EFI_NOT_FOUND;
  if (Adc->Revision != 0x10004 || Adc->GetInputProperties == NULL || Adc->Read == NULL) return EFI_UNSUPPORTED;
  Length = AsciiStrLen (Name);
  if (Length == 0 || Length >= 32) return EFI_INVALID_PARAMETER;
  ZeroMem (&Channel, sizeof (Channel));
  if (Trace != NULL) Trace->Stage = 1;
  Status = Adc->GetInputProperties (Name, (UINT32)Length + 1, &Channel);
  if (EFI_ERROR (Status)) return Status;
  if (Trace != NULL) { Trace->Stage = 2; Trace->Channel = Channel; }
  Status = Adc->Read (&Channel, Result);
  if (EFI_ERROR (Status)) ZeroMem (Result, sizeof (*Result));
  return Status;
}

EFI_STATUS
FpReadAdc (IN HEALTH_ADC *Adc, IN CONST CHAR8 *Name, OUT HEALTH_ADC_RESULT *Result)
{
  return FpReadAdcDetailed (Adc, Name, Result, NULL);
}

EFI_STATUS
FpQueryAppId (IN HEALTH_SCM *Scm, IN CONST CHAR8 *Name, OUT UINT32 *AppId)
{
  CHAR8 Request[32];
  UINTN Length;
  EFI_STATUS Status;

  if (Name == NULL || AppId == NULL) return EFI_INVALID_PARAMETER;
  *AppId = 0;
  if (Scm == NULL || Scm->SendCommand == NULL) return EFI_NOT_FOUND;
  if (Scm->Revision != 0x50002) return EFI_UNSUPPORTED;
  Length = AsciiStrLen (Name);
  if (Length == 0 || Length >= sizeof (Request)) return EFI_INVALID_PARAMETER;
  ZeroMem (Request, sizeof (Request));
  CopyMem (Request, Name, Length);
  // EFIScm.h APP_QUERY_APPID_CMD=3. This does not load/start a TA, authenticate
  // peripheral firmware, or provision/erase RPMB. A returned handle is not a
  // functional test of that application's service.
  Status = Scm->SendCommand (Scm, 3, AppId, Request, Length + 1, NULL, 0);
  if (EFI_ERROR (Status)) *AppId = 0;
  return Status;
}

STATIC VOID
HealthSay (IN CONST CHAR8 *Format, ...)
{
  CHAR8 Buffer[256];
  VA_LIST Marker;
  UINTN Length;
  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);
  SerialPortWrite ((UINT8 *)Buffer, Length);
}

STATIC BOOLEAN
HealthRamRange (IN UINTN Address, IN UINTN Size)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
  EFI_STATUS Status;
  if (Address == 0 || Size == 0) return FALSE;
  Status = gDS->GetMemorySpaceDescriptor (Address, &Desc);
  return (BOOLEAN)(!EFI_ERROR (Status) && Desc.GcdMemoryType == EfiGcdMemoryTypeSystemMemory &&
          (Desc.Attributes & EFI_MEMORY_RP) == 0 && Address >= Desc.BaseAddress &&
          Address - Desc.BaseAddress <= Desc.Length && Size <= Desc.Length - (Address - Desc.BaseAddress));
}

STATIC VOID
HealthAdcBackend (IN HEALTH_ADC *Adc)
{
  UINTN Base;
  UINTN Config;
  UINT32 Count;
  // The current PMIC build keeps API RVAs 32748/3BA0/3C04 but shifts late
  // data by 16 bytes relative to the orig dump. Verify the actual load opcodes
  // before inspecting its private state. Never patch or call private functions.
  if ((UINTN)Adc < 0x32748) return;
  Base = (UINTN)Adc - 0x32748;
  if ((UINTN)Adc->GetInputProperties != Base + 0x3BA0 || (UINTN)Adc->Read != Base + 0x3C04 ||
      !HealthRamRange (Base + 0x3BD4, 4) || !HealthRamRange (Base + 0x3C60, 4) ||
      *(UINT32 *)(Base + 0x3BD4) != 0xF9439D00 || *(UINT32 *)(Base + 0x3C60) != 0xF9439D08 ||
      !HealthRamRange (Base + 0x39738, 8) || !HealthRamRange (Base + 0x3AB00, 16)) {
    HealthSay ("[HEALTH b200] ADC private layout not verified; skipped\n");
    return;
  }
  Config = *(UINTN *)(Base + 0x3AB08);
  HealthSay ("[HEALTH b200] ADC backend init=%u token=%lx config=%lx\n",
             *(UINT8 *)(Base + 0x3AB00), *(UINT64 *)(Base + 0x39738), (UINT64)Config);
  if (!HealthRamRange (Config, 12)) return;
  Count = *(UINT32 *)(Config + 8);
  HealthSay ("[HEALTH b200] ADC backend device count=%u\n", Count);
  if (Count > 3 || !HealthRamRange (Base + 0x3AB18, Count * 16)) return;
  {
    UINT32 Index;
    for (Index = 0; Index < Count; Index++) {
      UINTN Device = *(UINTN *)(Base + 0x3AB18 + Index * 16);
      UINTN Table = 0;
      UINTN Method = 0;
      if (HealthRamRange (Device, sizeof (UINTN))) Table = *(UINTN *)Device;
      if (HealthRamRange (Table, sizeof (UINTN))) Method = *(UINTN *)Table;
      HealthSay ("[HEALTH b200] ADC converter%u device=%lx table=%lx lookup=%lx\n",
                 Index, (UINT64)Device, (UINT64)Table, (UINT64)Method);
    }
  }
}

// The three native ChargerEx getters return Unsupported before any battery
// backend call when +71E8 is not one. Observe this verified gate, never set it.
STATIC VOID
HealthChargerGate (VOID)
{
  EFI_GUID Guid = {0x6EDC8A6D,0x2663,0x43CD,{0x90,0xFF,0x46,0x21,0xFF,0xD1,0x0D,0xF5}};
  UINT64 *Table = NULL;
  UINTN Base;
  EFI_STATUS Status = gBS->LocateProtocol (&Guid, NULL, (VOID **)&Table);
  if (EFI_ERROR (Status) || Table == NULL || !HealthRamRange ((UINTN)Table, 32) ||
      (UINTN)Table < 0x70E8 || Table[0] != 0x10004) return;
  Base = (UINTN)Table - 0x70E8;
  if (Table[1] != Base + 0x159C || Table[2] != Base + 0x16E4 || Table[3] != Base + 0x17BC ||
      !HealthRamRange (Base + 0x15B4, 4) || !HealthRamRange (Base + 0x16F8, 4) ||
      !HealthRamRange (Base + 0x17D0, 4) || !HealthRamRange (Base + 0x71E8, 1) ||
      *(UINT32 *)(Base + 0x15B4) != 0x3947A108 ||
      *(UINT32 *)(Base + 0x16F8) != 0x3947A108 ||
      *(UINT32 *)(Base + 0x17D0) != 0x3947A108) {
    HealthSay ("[HEALTH b327] ChargerEx native layout not verified; gate skipped\n");
    return;
  }
  HealthSay ("[HEALTH b327] ChargerEx gate=%u (read only; gate!=1 returns Unsupported before backend)\n",
    *(UINT8 *)(Base + 0x71E8));
}

VOID
FpHealthReport (VOID)
{
  STATIC BOOLEAN Reported;
  HEALTH_ADC *Adc = NULL;
  HEALTH_SCM *Scm = NULL;
  EFI_STATUS Status;
  UINTN Index;
  STATIC CONST CHAR8 *Channels[] = { "VBATT", "VBATT_2S", "BATT_THERM_OHMS" };
  STATIC CONST CHAR8 *Applications[] = { "keymaster", "featenabler_a", "dxhdcp2" };
  EFI_GUID PmicGlink = {
    // Native PmicGlinkDxe +1728 installs GUID +A068, table +A0B8.
    // B5062BE7 is the shared kernel interface, not PMIC GLINK.
    0x7429EB07, 0xF2E3, 0x4A6B, {0xAA,0x96,0xC9,0x09,0xCD,0x95,0x6E,0xF7}
  };
  VOID *Protocol = NULL;

  if (Reported) return;
  Reported = TRUE;
  HealthChargerGate ();
  {
    EFI_GUID ProjectGuid = {0x64EBEA42,0x7F8A,0x11EA,{0xB9,0xF7,0x2B,0xBC,0x47,0x3A,0xE4,0x4B}};
    UINT64 *Project = NULL;
    UINT32 Board = 0;
    Status = gBS->LocateProtocol (&ProjectGuid, NULL, (VOID **)&Project);
    HealthSay ("[HEALTH b327] ProjectInfo -> %r\n", Status);
    if (!EFI_ERROR (Status) && HealthRamRange ((UINTN)Project, 24) && Project[0] == 0x30000 &&
        HealthRamRange ((UINTN)Project[2], 4)) {
      Status = ((EFI_STATUS (EFIAPI *)(VOID *, UINT32 *))(UINTN)Project[2])(Project, &Board);
      HealthSay ("[HEALTH b327] native/verified-cache project query -> %r value=%u\n", Status, Board);
    }
  }
  Status = gBS->LocateProtocol (&mHealthAdcGuid, NULL, (VOID **)&Adc);
  HealthSay ("[HEALTH b200] ADC -> %r interface=%lx\n", Status, (UINT64)(UINTN)Adc);
  if (!EFI_ERROR (Status) && Adc != NULL) {
    HealthAdcBackend (Adc);
    for (Index = 0; Index < ARRAY_SIZE (Channels); Index++) {
      HEALTH_ADC_RESULT Result;
      HEALTH_ADC_TRACE Trace;
      Status = FpReadAdcDetailed (Adc, Channels[Index], &Result, &Trace);
      HealthSay ("[HEALTH b200] ADC %a -> %r stage=%u dev=%u ch=%u status=%u physical=%d uV=%u percent=%u code=%u\n",
                 Channels[Index], Status, Trace.Stage, Trace.Channel.Device, Trace.Channel.Channel, Result.Status, Result.Physical,
                 Result.Microvolts, Result.Percent, Result.Code);
    }
  }
  Status = gBS->LocateProtocol (&PmicGlink, NULL, &Protocol);
  HealthSay ("[HEALTH b320] PMIC GLINK 7429eb07 -> %r interface=%lx (presence only)\n", Status, (UINT64)(UINTN)Protocol);
  Status = gBS->LocateProtocol (&mHealthScmGuid, NULL, (VOID **)&Scm);
  if (!EFI_ERROR (Status) && Scm != NULL) {
    for (Index = 0; Index < ARRAY_SIZE (Applications); Index++) {
      UINT32 AppId;
      Status = FpQueryAppId (Scm, Applications[Index], &AppId);
      HealthSay ("[HEALTH b200] QSEE query %a -> %r handle=%08x (query only)\n",
                 Applications[Index], Status, AppId);
    }
  }
}
