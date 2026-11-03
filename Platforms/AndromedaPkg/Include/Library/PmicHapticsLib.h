/** Stock Shennong PMIC haptics operations. SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PMIC_HAPTICS_LIB_H_
#define PMIC_HAPTICS_LIB_H_
#include <Uefi.h>
typedef enum {
  PmicHapticsNativeDuty,
  PmicHapticsDirect,
  PmicHapticsAutoRes,
  PmicHapticsModeCount
} PMIC_HAPTICS_MODE;
EFI_STATUS PmicHapticsSetEnabled (IN BOOLEAN Enabled);
EFI_STATUS PmicHapticsPulse (IN UINTN Microseconds);
EFI_STATUS PmicHapticsConfigure (IN UINT32 VmaxMv, IN UINT8 Amplitude);
EFI_STATUS PmicHapticsConfigureIntensity (IN UINT32 Percent);
EFI_STATUS PmicHapticsConfigureMode (IN PMIC_HAPTICS_MODE Mode);
EFI_STATUS PmicHapticsDiagnostics (VOID);
BOOLEAN PmicHapticsIsEnabled (VOID);
#endif
