/** Read-only power observations. A protocol handle is not a remote liveness test. */
#ifndef FP_POWER_STATUS_H_
#define FP_POWER_STATUS_H_
#include <Uefi.h>

typedef struct {
  UINT64 Revision;
  EFI_STATUS (EFIAPI *GetChargerPresence)(BOOLEAN *);
  EFI_STATUS (EFIAPI *GetBatteryPresence)(BOOLEAN *);
  EFI_STATUS (EFIAPI *GetBatteryVoltage)(UINT32 *);
} FP_CHARGER_GETTERS;
STATIC_ASSERT (OFFSET_OF (FP_CHARGER_GETTERS, GetBatteryVoltage) == 0x18, "ChargerEx ABI");

typedef enum {
  FpProtocolCharger, FpProtocolAdc, FpProtocolPmicGlink, FpProtocolGlink,
  FpProtocolScm, FpProtocolPil, FpProtocolUsbDevice, FpProtocolUsbFn,
  FpProtocolStorage, FpProtocolBatteryBackend, FpProtocolCount
} FP_STATUS_PROTOCOL;

typedef struct {
  EFI_STATUS Protocol[FpProtocolCount];
  UINT64 ChargerRevision;
  BOOLEAN Sampled;
  EFI_STATUS ChargerStatus, BatteryStatus, VoltageStatus;
  BOOLEAN ChargerPresent, BatteryPresent;
  UINT32 Millivolts;
} FP_POWER_SNAPSHOT;

VOID FpInspectPower (FP_POWER_SNAPSHOT *Snapshot);
VOID FpReadPowerGetters (FP_CHARGER_GETTERS *Charger, FP_POWER_SNAPSHOT *Snapshot);
VOID FpSamplePower (FP_POWER_SNAPSHOT *Snapshot);
#endif
