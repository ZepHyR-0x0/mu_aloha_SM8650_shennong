// SPDX-License-Identifier: BSD-2-Clause-Patent
// A valid synthetic calendar for the read-only uptime clock, not wall time.
#pragma once

STATIC BOOLEAN
VirtualRtcFromSeconds (UINT64 Seconds, EFI_TIME *Time)
{
  STATIC CONST UINT8 MonthDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  UINT64 Days = Seconds / 86400;
  UINT64 Year = 2019 + (Days / 146097) * 400;
  UINTN Month = 0, N;
  BOOLEAN Leap;
  Days %= 146097;
  if (Year > 9999) return FALSE;
  for (;;) {
    Leap = Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
    N = Leap ? 366 : 365;
    if (Days < N) break;
    Days -= N;
    if (++Year > 9999) return FALSE;
  }
  for (;;) {
    N = MonthDays[Month] + (Month == 1 && Leap ? 1 : 0);
    if (Days < N) break;
    Days -= N;
    Month++;
  }
  *Time = (EFI_TIME){0};
  Time->Year = (UINT16)Year;
  Time->Month = (UINT8)(Month + 1);
  Time->Day = (UINT8)(Days + 1);
  Seconds %= 86400;
  Time->Hour = (UINT8)(Seconds / 3600);
  Time->Minute = (UINT8)((Seconds % 3600) / 60);
  Time->Second = (UINT8)(Seconds % 60);
  Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
  return TRUE;
}
