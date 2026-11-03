/** Touch coordinates to bounded slider units; no hardware side effects.
    SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef HAPTIC_SLIDER_H_
#define HAPTIC_SLIDER_H_
#include <Uefi.h>
STATIC UINT32 HapticSliderUnits (UINT64 X, UINTN Left, UINTN Right, UINT32 Units)
{
  if (Right <= Left || Right > MAX_UINT16 || Units > 1000) return 0;
  if (X <= Left) return 0;
  if (X >= Right) return Units;
  return (UINT32)((X-Left)*Units/(Right-Left));
}
#endif
