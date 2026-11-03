/** @file
  PM8550B HV-haptics control used by the GUI TEST page.

  Copyright (c) 2026, ProjectMu shennong port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef HAPTIC_CONTROL_H_
#define HAPTIC_CONTROL_H_

#include <Uefi.h>
#include <Library/PmicHapticsLib.h>

EFI_STATUS
FpHapticStart (
  VOID
  );

EFI_STATUS
FpHapticStop (
  VOID
  );

EFI_STATUS
FpHapticToggle (
  OUT BOOLEAN  *Enabled
  );

BOOLEAN
FpHapticIsEnabled (
  VOID
  );

EFI_STATUS FpHapticPoll (VOID);
EFI_STATUS FpHapticConfigure (UINTN Microseconds, UINT32 VmaxMv);
EFI_STATUS FpHapticConfigureLevel (UINTN Microseconds, UINT32 Percent, PMIC_HAPTICS_MODE Mode);

#endif
