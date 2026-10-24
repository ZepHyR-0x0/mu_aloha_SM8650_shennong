/** @file
  TouchProbeDxe - can we reach the touch hardware at all?

  shennong uses a Synaptics TCM panel on the GENI SPI controller at 0x0A90000:

    spi@a90000 { compatible = "qcom,spi-geni"; reg = <0xa90000 0x4000>; }
      synaptics_tcm@0 {
        synaptics,irq-gpio   = <&tlmm 0xa2 0x2008>;   pin 162
        synaptics,reset-gpio = <&tlmm 0xa1 0x00>;     pin 161
        max-x = 0x5a0 (1440), max-y = 0xc80 (3200)
      }

  Before writing a driver on top of that, three things have to be true, and none
  of them is known yet:

    1. TLMM at 0x0F000000 is reachable and the per-pin registers read sanely
    2. the GENI SE at 0x0A90000 is clocked and out of reset
    3. the panel's avdd/iovdd rails are on

  This driver only reads. It writes nothing, arms no interrupts and touches no
  pin configuration, so it cannot disturb a boot that currently reaches the GUI.

  Copyright (c) 2026, ProjectMu shennong port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>

#define TLMM_BASE        0x0F000000ULL
#define TLMM_PIN_STRIDE  0x1000ULL
#define TLMM_RESET_PIN   161
#define TLMM_IRQ_PIN     162

#define GENI_SPI_BASE    0x0A90000ULL

STATIC
VOID
Say (
  IN CONST CHAR8  *Text
  )
{
  SerialPortInitialize ();
  SerialPortWrite ((UINT8 *)Text, AsciiStrLen (Text));
}

STATIC
VOID
SayPin (
  IN CONST CHAR8  *What,
  IN UINTN        Pin
  )
{
  UINT32  Cfg;
  UINT32  Io;
  CHAR8   Buf[128];
  UINTN   Len;

  Cfg = MmioRead32 (TLMM_BASE + TLMM_PIN_STRIDE * Pin);
  Io  = MmioRead32 (TLMM_BASE + TLMM_PIN_STRIDE * Pin + 0x4);

  Len = AsciiSPrint (
          Buf, sizeof (Buf),
          "[TS] %a pin %d cfg=%08x io=%08x\n",
          What, Pin, Cfg, Io
          );
  SerialPortWrite ((UINT8 *)Buf, Len);
}

EFI_STATUS
EFIAPI
TouchProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CHAR8  Buf[160];
  UINTN  Len;
  UINT32 V;

  Say ("[TS] probe start\n");

  //
  // TLMM: a per-pin register block every 0x1000. If the block is not clocked or
  // not mapped, the read faults instead of returning nonsense, which is itself
  // the answer we are after.
  //
  SayPin ("reset", TLMM_RESET_PIN);
  SayPin ("irq  ", TLMM_IRQ_PIN);

  //
  // GENI SE: dump the first four words. On a healthy controller CFG and STATUS
  // are non-zero and STATUS does not sit in the reset value.
  //
  V   = MmioRead32 (GENI_SPI_BASE + 0x00);
  Len = AsciiSPrint (Buf, sizeof (Buf), "[TS] geni 0x0A90000 +00 = %08x\n", V);
  SerialPortWrite ((UINT8 *)Buf, Len);

  V   = MmioRead32 (GENI_SPI_BASE + 0x04);
  Len = AsciiSPrint (Buf, sizeof (Buf), "[TS] geni 0x0A90000 +04 = %08x\n", V);
  SerialPortWrite ((UINT8 *)Buf, Len);

  V   = MmioRead32 (GENI_SPI_BASE + 0x08);
  Len = AsciiSPrint (Buf, sizeof (Buf), "[TS] geni 0x0A90000 +08 = %08x\n", V);
  SerialPortWrite ((UINT8 *)Buf, Len);

  V   = MmioRead32 (GENI_SPI_BASE + 0x0C);
  Len = AsciiSPrint (Buf, sizeof (Buf), "[TS] geni 0x0A90000 +0C = %08x\n", V);
  SerialPortWrite ((UINT8 *)Buf, Len);

  Say ("[TS] probe done\n");
  return EFI_SUCCESS;
}
