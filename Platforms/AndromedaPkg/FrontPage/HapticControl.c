/** GUI adapter for the shared PMIC haptics library. SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/PmicHapticsLib.h>
#include <Library/TimerLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include "HapticControl.h"

STATIC UINT64 mStarted;
STATIC UINTN mDuration = 500000;
EFI_STATUS FpHapticConfigureLevel (UINTN Microseconds, UINT32 Percent, PMIC_HAPTICS_MODE Mode)
{
  if (Microseconds < 50000 || Microseconds > 1000000 || Percent > 100 || (UINTN)Mode >= PmicHapticsModeCount) return EFI_INVALID_PARAMETER;
  EFI_STATUS Status = PmicHapticsConfigureMode (Mode);
  if (!EFI_ERROR (Status)) Status = PmicHapticsConfigureIntensity (Percent);
  if (!EFI_ERROR (Status)) mDuration = Microseconds;
  CHAR8 Text[160];
  UINTN N = AsciiSPrint (Text,sizeof(Text),"[HAPTIC UI] duration_ms=%u intensity=%u mode=%u -> %r\r\n",(UINT32)(Microseconds/1000),Percent,(UINT32)Mode,Status);
  SerialPortWrite ((UINT8 *)Text,N);
  return Status;
}
EFI_STATUS FpHapticConfigure (UINTN Microseconds, UINT32 VmaxMv)
{
  EFI_STATUS Status;
  if (Microseconds < 50000 || Microseconds > 1000000) return EFI_INVALID_PARAMETER;
  Status = PmicHapticsConfigure (VmaxMv, 0xFF);
  if (!EFI_ERROR (Status)) {
    CHAR8 Text[96];
    UINTN N;
    mDuration = Microseconds;
    N = AsciiSPrint (Text,sizeof(Text),"[HAPTIC SET] duration_us=%u vmax_mv=%u\r\n",(UINT32)Microseconds,VmaxMv);
    SerialPortWrite ((UINT8 *)Text,N);
  }
  return Status;
}
EFI_STATUS FpHapticStart (VOID)
{
  EFI_STATUS Status = PmicHapticsSetEnabled (TRUE);
  if (!EFI_ERROR (Status)) mStarted = GetTimeInNanoSecond (GetPerformanceCounter ());
  return Status;
}
EFI_STATUS FpHapticStop (VOID) { return PmicHapticsSetEnabled (FALSE); }
EFI_STATUS FpHapticToggle (OUT BOOLEAN *Enabled)
{
  EFI_STATUS Status;
  if (Enabled == NULL) return EFI_INVALID_PARAMETER;
  Status = PmicHapticsIsEnabled () ? FpHapticStop () : FpHapticStart ();
  *Enabled = PmicHapticsIsEnabled ();
  return Status;
}
BOOLEAN FpHapticIsEnabled (VOID) { return PmicHapticsIsEnabled (); }
EFI_STATUS FpHapticPoll (VOID)
{
  if (PmicHapticsIsEnabled () &&
      GetTimeInNanoSecond (GetPerformanceCounter ()) - mStarted >= (UINT64)mDuration*1000)
    return FpHapticStop ();
  return EFI_SUCCESS;
}
