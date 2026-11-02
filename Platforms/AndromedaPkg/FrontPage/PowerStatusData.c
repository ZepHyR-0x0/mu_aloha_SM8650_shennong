/** Stock ChargerEx +159C/+16E4/+17BC: x0 is the output, no This argument.
  Only getters with the verified native revision are called. No charging policy,
  current/voltage setting, off-mode transition, PAS or peripheral reset call.
*/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/UsbFunctionIo.h>
#include "../Driver/ReadOnlyStorageDxe/ReadOnlyStorage.h"
#include "BootLog.h"
#include "PowerStatus.h"

STATIC EFI_GUID mPowerGuid[FpProtocolCount] = {
  {0x6EDC8A6D,0x2663,0x43CD,{0x90,0xFF,0x46,0x21,0xFF,0xD1,0x0D,0xF5}},
  {0x61740CC4,0x916F,0x4471,{0xB0,0xDC,0x00,0xC6,0xF4,0x3A,0x7F,0xC6}},
  {0x7429EB07,0xF2E3,0x4A6B,{0xAA,0x96,0xC9,0x09,0xCD,0x95,0x6E,0xF7}},
  {0x3BFCF64D,0x00CD,0x41CB,{0xBE,0xA5,0xA7,0x6C,0x3A,0x24,0x3D,0xED}},
  {0x77ED108D,0x8524,0x4B8B,{0x9D,0x2E,0x34,0x98,0x7A,0xEC,0xB9,0xC1}},
  {0x1F4F1DA3,0x4D8D,0x4894,{0xBA,0x2A,0x13,0xD3,0x20,0x7F,0x97,0x39}},
  {0xD9D9CE48,0x44B8,0x4F49,{0x8E,0x3E,0x2A,0x3B,0x92,0x7D,0xC6,0xC1}},
  EFI_USBFN_IO_PROTOCOL_GUID,
  READ_ONLY_STORAGE_GUID,
  // ChargerEx +7088: actual battery/charger backend, separate from wrapper.
  {0x21BFB78B,0x02EA,0x4AB7,{0x99,0x40,0xB5,0x8A,0x2A,0xD6,0x58,0x29}}
};

STATIC VOID PowerTrace (CONST CHAR8 *Name, EFI_STATUS Status, UINT32 Value)
{
  CHAR8 Text[192];
  UINTN N = AsciiSPrint (Text, sizeof (Text), "[POWER b327] %a -> %r value=%u\r\n", Name, Status, Value);
  SerialPortWrite ((UINT8 *)Text, N);
}
STATIC VOID PowerCheckpoint (CONST CHAR8 *Phase)
{
  EFI_STATUS Status;
  PowerTrace (Phase, EFI_SUCCESS, 0);
  Status = BootLogSave (Phase);
  PowerTrace ("logfs checkpoint", Status, 0);
}
STATIC EFI_STATUS PowerLocate (UINTN Index, VOID **Interface)
{
  EFI_STATUS Status;
  *Interface = NULL;
  Status = gBS->LocateProtocol (&mPowerGuid[Index], NULL, Interface);
  return !EFI_ERROR (Status) && *Interface == NULL ? EFI_DEVICE_ERROR : Status;
}
VOID FpInspectPower (FP_POWER_SNAPSHOT *S)
{
  UINTN I;
  VOID *Interface;
  if (S == NULL) return;
  ZeroMem (S, sizeof (*S));
  S->ChargerStatus = S->BatteryStatus = S->VoltageStatus = EFI_NOT_READY;
  for (I = 0; I < FpProtocolCount; I++) {
    S->Protocol[I] = PowerLocate (I, &Interface);
    if (I == FpProtocolCharger && !EFI_ERROR (S->Protocol[I]))
      S->ChargerRevision = ((FP_CHARGER_GETTERS *)Interface)->Revision;
  }
}
VOID FpReadPowerGetters (FP_CHARGER_GETTERS *Charger, FP_POWER_SNAPSHOT *S)
{
  BOOLEAN Present;
  UINT32 Voltage;
  if (S == NULL) return;
  S->Sampled = TRUE;
  S->ChargerPresent = S->BatteryPresent = FALSE;
  S->Millivolts = 0;
  S->ChargerStatus = S->BatteryStatus = S->VoltageStatus = EFI_NOT_FOUND;
  if (Charger == NULL) return;
  S->ChargerRevision = Charger->Revision;
  // Header's older 10003 is not sufficient evidence for this binary's ABI.
  if (Charger->Revision != 0x10004 || Charger->GetChargerPresence == NULL ||
      Charger->GetBatteryPresence == NULL || Charger->GetBatteryVoltage == NULL) {
    S->ChargerStatus = S->BatteryStatus = S->VoltageStatus = EFI_UNSUPPORTED;
    return;
  }
  PowerCheckpoint ("power-charger-enter");
  Present = 0xAA; // Detect a getter that returns success without writing output.
  S->ChargerStatus = Charger->GetChargerPresence (&Present);
  if (!EFI_ERROR (S->ChargerStatus) && Present > 1) S->ChargerStatus = EFI_COMPROMISED_DATA;
  if (!EFI_ERROR (S->ChargerStatus)) S->ChargerPresent = Present;
  PowerTrace ("charger-source-connected", S->ChargerStatus, Present);
  PowerCheckpoint ("power-battery-enter");
  Present = 0xAA;
  S->BatteryStatus = Charger->GetBatteryPresence (&Present);
  if (!EFI_ERROR (S->BatteryStatus) && Present > 1) S->BatteryStatus = EFI_COMPROMISED_DATA;
  if (!EFI_ERROR (S->BatteryStatus)) S->BatteryPresent = Present;
  PowerTrace ("battery-present", S->BatteryStatus, Present);
  PowerCheckpoint ("power-voltage-enter");
  Voltage = MAX_UINT32;
  S->VoltageStatus = Charger->GetBatteryVoltage (&Voltage);
  // Covers one/two-cell packs, but neither proves SOC nor the source's freshness.
  if (!EFI_ERROR (S->VoltageStatus) && (Voltage < 2000 || Voltage > 10000))
    S->VoltageStatus = EFI_COMPROMISED_DATA;
  if (!EFI_ERROR (S->VoltageStatus)) S->Millivolts = Voltage;
  PowerTrace ("reported-millivolts-source-unverified", S->VoltageStatus, Voltage);
  PowerCheckpoint ("power-sample-complete");
}
VOID FpSamplePower (FP_POWER_SNAPSHOT *S)
{
  STATIC CONST CHAR8 *Names[] = { "protocol-ChargerEx", "protocol-PMIC-ADC", "protocol-PMIC-GLINK-7429eb07",
    "protocol-local-GLINK", "protocol-SCM", "protocol-PIL", "protocol-USB-Device", "protocol-USB-Function", "protocol-storage-guard",
    "protocol-battery-backend-21bfb78b" };
  STATIC_ASSERT (ARRAY_SIZE (Names) == FpProtocolCount, "power trace labels match protocol observations");
  UINTN I;
  VOID *Interface;
  EFI_STATUS Status;
  if (S == NULL) return;
  FpInspectPower (S);
  for (I = 0; I < FpProtocolCount; I++) PowerTrace (Names[I], S->Protocol[I], 0);
  Status = PowerLocate (FpProtocolCharger, &Interface);
  S->Protocol[FpProtocolCharger] = Status;
  FpReadPowerGetters (EFI_ERROR (Status) ? NULL : Interface, S);
  if (EFI_ERROR (Status)) S->ChargerStatus = S->BatteryStatus = S->VoltageStatus = Status;
}
