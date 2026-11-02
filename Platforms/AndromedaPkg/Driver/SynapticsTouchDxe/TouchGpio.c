/** @file

  Bit-banged SPI transport for the Synaptics S3910P on shennong.

  Why this exists at all: the touch controller sits behind QUPv3 serial engine 4
  (0xA90000), and on this board that engine is wired for GSI/GPI, so its FIFO and
  its traditional DMA path are both unusable - see the notes in GeniSpi.c, which
  programs the engine directly and never gets a byte out of it. Rather than
  implement a GPI DMA channel, this transport stops using the engine: the six pins
  are muxed to plain GPIO and the SPI clock is driven by software.

  The approach is not invented here. The UniXNU houji port on the same board
  (SM8650, same pins 48/49/50/51 + reset 161 + IRQ 162) reads this exact
  controller over bit-banged GPIO SPI, with the same reset timing, and reports
  the chip identified as S3910 firmware 4323384. Its transport header is
  native/touch/HoujiTouchGpio.h; the wire and report layers here stay the ones
  already written for this tree.

  One detail that decides whether any of this works, and that the first version of
  this driver got wrong: on SM8650 the GPIO value register is not bit 0. The
  pinctrl driver for this SoC declares

      .oe_bit = 9, .in_bit = 0, .out_bit = 1        (pinctrl-pineapple.c:38)

  so the output level lives in bit 1 of GPIO_IN_OUT and the input level is read
  from bit 0. Writing 1 to the reset pin therefore wrote to the input field and
  left the output bit at 0 - the controller was held in reset for the whole boot,
  which is consistent with every attempt so far failing to identify it.

**/

#include "TouchHw.h"

#include <Uefi.h>
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

extern VOID
TcmSay (
  IN CONST CHAR8  *Format,
  ...
  );

//
// Spin count per SPI half period. CpuPause is a yield hint, and each edge also
// costs two uncached MMIO accesses, so this lands somewhere around 1-2 MHz -
// slow for SPI, which is the safe direction for a bus with no timing guarantees.
//
#define TOUCH_SPI_HALF_PERIOD_SPIN  64

STATIC UINTN   mSpiClk;
STATIC UINTN   mSpiMosi;
STATIC UINTN   mSpiMiso;
STATIC UINTN   mSpiCs;
STATIC BOOLEAN mTimingLogged;

//
// Clock polarity, phase and half-period spin, so the same bus can be driven in any
// of the four SPI modes at more than one speed.
//
// These exist because sensing has run out of road. MISO carries a pull-down in the
// device tree, the interrupt line behaves as if it has one too, and the TLMM's own
// pull-up is only tens of microamps - so "reads 0 with a pull-up" cannot separate an
// external resistor from a controller actively driving the line low. Rather than
// keep interpreting that, this build varies the things a wrong guess would explain
// and looks for any answer at all.
//
STATIC UINT32  mCpol  = 0;
STATIC UINT32  mCpha  = 0;
STATIC UINT32  mSpin  = TOUCH_SPI_HALF_PERIOD_SPIN;

/**
  Set a TLMM pin's output level.

  Bit 1 of GPIO_IN_OUT on this SoC, not bit 0. See the file header.
**/
VOID
TouchPinWrite (
  IN UINT32  Pin,
  IN UINT32  Value
  )
{
  UINT32  Register;

  Register  = MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (Pin));
  Register &= ~TLMM_IO_OUT_MASK;
  if (Value != 0) {
    Register |= TLMM_IO_OUT_MASK;
  }

  MmioWrite32 (TLMM_BASE + TLMM_GPIO_IN_OUT (Pin), Register);
}

/**
  Read a TLMM pin's input level. Bit 0 of GPIO_IN_OUT on this SoC.
**/
UINT32
TouchPinRead (
  IN UINT32  Pin
  )
{
  return (MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (Pin)) & TLMM_IO_IN_MASK) != 0;
}

STATIC
VOID
TouchSpiPause (
  VOID
  )
{
  UINT32  Index;

  for (Index = 0; Index < mSpin; Index++) {
    CpuPause ();
  }
}

/**
  Set the SPI mode and clock half-period, and park the clock at its idle level.

  @param[in]  Cpol  Clock polarity: 0 idles low, 1 idles high.
  @param[in]  Cpha  Clock phase: 0 samples on the leading edge, 1 on the trailing.
  @param[in]  Spin  Busy-loop count per half period. Larger is slower.
**/
VOID
TouchSpiSetMode (
  IN UINT32  Cpol,
  IN UINT32  Cpha,
  IN UINT32  Spin
  )
{
  mCpol = (Cpol != 0) ? 1 : 0;
  mCpha = (Cpha != 0) ? 1 : 0;
  mSpin = (Spin < 8) ? 8 : Spin;

  TouchPinWrite (mSpiClk, mCpol);
}

/**
  Declare an MMIO range to the GCD and mark it uncached.

  Same shape as the GENI version: ask for the descriptor first, add the range when
  nobody has, and always end with a SetMemorySpaceAttributes(UC) whose result is
  checked - a page whose tables were never built faults on the first access, and
  on this SoC that fault resets the machine rather than printing anything.
**/
EFI_STATUS
TouchMapMmio (
  IN UINTN        Base,
  IN UINTN        Len,
  IN CONST CHAR8  *Name
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;

  ZeroMem (&Desc, sizeof (Desc));
  Status = gDS->GetMemorySpaceDescriptor (Base, &Desc);

  if (EFI_ERROR (Status) || (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, Len, EFI_MEMORY_UC);
    TcmSay ("[TP] %a @0x%lx add -> %r\r\n", Name, (UINT64)Base, Status);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = gDS->SetMemorySpaceAttributes (Base, Len, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] %a @0x%lx attr -> %r\r\n", Name, (UINT64)Base, Status);
  }

  return Status;
}

/**
  Make one TLMM pin page accessible. Every pin gets its own page: the page tables
  are per 4 KB, and asking for one big range is not the same thing.
**/
EFI_STATUS
TouchPinMap (
  IN UINT32  Pin
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  UINTN                            Page;

  Page = (TLMM_BASE + TLMM_GPIO_CFG (Pin)) & ~(UINTN)(TLMM_PAGE - 1);

  ZeroMem (&Desc, sizeof (Desc));
  Status = gDS->GetMemorySpaceDescriptor (Page, &Desc);

  if (EFI_ERROR (Status) || (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Page, TLMM_PAGE, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      TcmSay ("[TP] gpio%u page 0x%lx add -> %r\r\n", Pin, (UINT64)Page, Status);
      return Status;
    }
  } else if ((Desc.Attributes & EFI_MEMORY_UC) != 0) {
    //
    // Already uncached per the GCD; writing the same value back is a no-op, so go
    // via write-combining to force the page tables to be rebuilt.
    //
    (VOID)gDS->SetMemorySpaceAttributes (Page, TLMM_PAGE, EFI_MEMORY_WC);
  }

  Status = gDS->SetMemorySpaceAttributes (Page, TLMM_PAGE, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] gpio%u page 0x%lx attr -> %r\r\n", Pin, (UINT64)Page, Status);
  }

  return Status;
}

/**
  Mux the four SPI pins to plain GPIO and park the bus idle.

  Mode 0: clock idles low, data is sampled on the rising edge, chip select is
  active low. MISO is an input with a pull-up so it does not float when the
  controller releases the line between transfers.
**/
EFI_STATUS
TouchSpiInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Pins[4];
  UINTN       Index;

  mSpiMiso = TOUCH_SPI_PIN_0;   // 0x30
  mSpiMosi = TOUCH_SPI_PIN_1;   // 0x31
  mSpiClk  = TOUCH_SPI_PIN_2;   // 0x32
  mSpiCs   = TOUCH_SPI_PIN_3;   // 0x33

  Pins[0] = (UINT32)mSpiMiso;
  Pins[1] = (UINT32)mSpiMosi;
  Pins[2] = (UINT32)mSpiClk;
  Pins[3] = (UINT32)mSpiCs;

  Status = TouchMapMmio (TLMM_BASE, TLMM_MAP_LEN, "TLMM");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < 4; Index++) {
    Status = TouchPinMap (Pins[Index]);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  //
  // Idle levels first, then direction: driving CS low before it is an output
  // would briefly select the controller with whatever the pad was holding.
  //
  TouchPinWrite (mSpiClk, 0);
  TouchPinWrite (mSpiMosi, 0);
  TouchPinWrite (mSpiCs, 1);

  //
  // What the bootloader left, and whether the AP still owns these pads.
  //
  // On this SoC bit 11 of GPIO_CFG set means the pad was handed to the external
  // (GPI/eGPIO) controller and bit 12 set means it was handed back. In the
  // handed-away state writes to GPIO_CFG are ignored, the pad keeps whatever
  // function it had, and the bus reads dead for a reason that has nothing to do
  // with the controller. The reference port on this board refuses to drive a pin
  // unless this check passes (gg_pin_owned in its HoujiTouchGpio.h), so the same
  // pair is printed here before anything is written.
  //
  for (Index = 0; Index < 4; Index++) {
    UINT32  Before;
    BOOLEAN Owned;

    Before = MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (Pins[Index]));
    Owned  = (((Before & (1U << 11)) == 0) || ((Before & (1U << 12)) != 0));
    TcmSay (
      "[TP] gpio%u before: cfg=%08x io=%08x ap-owned=%u\r\n",
      Pins[Index],
      Before,
      MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (Pins[Index])),
      Owned ? 1 : 0
      );
  }

  //
  // Pin parameters come from the device tree's pinctrl, not from taste:
  //
  //   mi_ts_spi_miso/mosi/clk/cs_active   drive-strength = <6>   bias-pull-down
  //   syna_tcm_rst_init                   bias-disable         output-low
  //   syna_tcm_eint_init                  bias-disable         input-enable
  //
  // Two of those were wrong here before. The SPI lines were given 2 mA where the
  // device tree asks for 6, and MISO was given a pull-up where it asks for a
  // pull-down. Neither is likely to be the whole story, but both are free to fix
  // and both are the kind of difference that turns "almost works" into "never
  // answers".
  //
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (mSpiMiso),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_PULL_DOWN
    );
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (mSpiMosi),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_PULL_DOWN | TLMM_CFG_OE
    );
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (mSpiClk),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_PULL_DOWN | TLMM_CFG_OE
    );
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (mSpiCs),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_PULL_DOWN | TLMM_CFG_OE
    );

  TcmSay (
    "[TP] GPIO SPI on %u/%u/%u/%u (miso/mosi/clk/cs), mux=0, mode 0\r\n",
    (UINT32)mSpiMiso,
    (UINT32)mSpiMosi,
    (UINT32)mSpiClk,
    (UINT32)mSpiCs
    );

  TcmSay (
    "[TP] pin cfg %08x %08x %08x %08x io %08x %08x %08x %08x\r\n",
    MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (mSpiMiso)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (mSpiMosi)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (mSpiClk)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (mSpiCs)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (mSpiMiso)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (mSpiMosi)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (mSpiClk)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (mSpiCs))
    );

  //
  // Drive each output pin high and low and read it back.
  //
  // CS is the only one of the four that has ever been observed tracking, and the
  // other three are configured identically - but "configured identically" is exactly
  // the kind of assumption that has been wrong before on this pad controller. It
  // matters because a clock that never toggles produces precisely the all-zero MISO
  // this driver keeps seeing, and no amount of protocol work would ever show it.
  //
  {
    UINT32  Outputs[3];
    UINT32  Probe;

    Outputs[0] = (UINT32)mSpiMosi;
    Outputs[1] = (UINT32)mSpiClk;
    Outputs[2] = (UINT32)mSpiCs;

    for (Probe = 0; Probe < 3; Probe++) {
      UINT32  Hi;
      UINT32  Lo;

      TouchPinWrite (Outputs[Probe], 1);
      gBS->Stall (500);
      Hi = TouchPinRead (Outputs[Probe]);
      TouchPinWrite (Outputs[Probe], 0);
      gBS->Stall (500);
      Lo = TouchPinRead (Outputs[Probe]);

      TcmSay ("[TP] pad %u: drive 1 -> %u, drive 0 -> %u (want 1 then 0)\r\n",
              Outputs[Probe], Hi, Lo);
    }

    //
    // Back to the idle levels the bus expects.
    //
    TouchPinWrite (mSpiMosi, 0);
    TouchPinWrite (mSpiClk, 0);
    TouchPinWrite (mSpiCs, 1);
  }

  return EFI_SUCCESS;
}

/**
  Change the bias on the MISO pad.

  MISO carries a pull-down in the device tree, which makes an all-zero read
  ambiguous: it is what an idle line looks like whether a controller is attached or
  not. Flipping the bias to a pull-up for a few reads turns that into a real
  question - with the pad pulled up, an all-ones read means nothing is driving the
  line at all, and an all-zeros read means something is actively holding it down,
  which would mean the controller is alive and the fault is in how the bits are
  sampled rather than in whether the chip is there.

  @param[in]  Bias  TLMM_CFG_PULL_UP, TLMM_CFG_PULL_DOWN or TLMM_CFG_PULL_DISABLE.
**/
VOID
TouchMisoBias (
  IN UINT32  Bias
  )
{
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (mSpiMiso),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | (Bias & 0x3U)
    );
}

/**
  Swap which of pins 48 and 49 carries data in and which carries data out.

  The device tree names its pin groups mi_ts_spi_miso_active and
  mi_ts_spi_mosi_active, and both this driver and the reference port took those
  names at face value: gpio48 is MISO because the group that muxes it is called
  MISO. The QUP function strings behind those names are qup1_se4_l0 and l1, and
  nothing checked which lane number carries which SPI signal - the names are
  hand-written by whoever wrote the board file.

  If they are the wrong way round, every symptom in this log follows exactly:
  commands are clocked out on the pin the controller listens to for data, the
  controller's answers come back on the pin we are driving, and the pin we sample
  is the controller's own input, which it never drives. MISO would read 0 forever
  in every mode at every speed, which is precisely what it does.

  Swapping the two pads is therefore worth one pass. MISO becomes an input and
  MOSI becomes an output, and both are reconfigured for their new roles.
**/
VOID
TouchSpiSwapData (
  IN BOOLEAN  Swap
  )
{
  if (Swap) {
    mSpiMiso = TOUCH_SPI_PIN_1;
    mSpiMosi = TOUCH_SPI_PIN_0;
  } else {
    mSpiMiso = TOUCH_SPI_PIN_0;
    mSpiMosi = TOUCH_SPI_PIN_1;
  }

  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (mSpiMiso),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_PULL_DOWN
    );
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (mSpiMosi),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_PULL_DOWN | TLMM_CFG_OE
    );

  TouchPinWrite (mSpiMosi, 1);
}

/**
  Clock one byte out and one byte in, MSB first, in whichever mode is set.

  Mode 0 - the one the device tree asks for and the one every build has used -
  changes MOSI while the clock is idle and samples MISO on the leading edge. The
  other three differ in which edge carries the data, and getting that wrong produces
  a byte stream that looks like a dead bus rather than like noise, which is exactly
  the symptom this driver has been stuck on.
**/
STATIC
UINT8
TouchSpiByte (
  IN UINT8  Send
  )
{
  UINT8   Receive;
  UINT32  Bit;
  UINT32  Leading;
  UINT32  Trailing;

  Leading  = (mCpol != 0) ? 0 : 1;
  Trailing = (mCpol != 0) ? 1 : 0;

  Receive = 0;
  for (Bit = 0; Bit < 8; Bit++) {
    UINT32  Value;

    Value = (Send & 0x80) ? 1 : 0;
    Send  = (UINT8)(Send << 1);

    if (mCpha == 0) {
      TouchPinWrite (mSpiMosi, Value);
      TouchSpiPause ();

      TouchPinWrite (mSpiClk, Leading);
      TouchSpiPause ();
      Receive = (UINT8)((Receive << 1) | (TouchPinRead (mSpiMiso) & 1));
      TouchPinWrite (mSpiClk, Trailing);
      TouchSpiPause ();
    } else {
      TouchPinWrite (mSpiClk, Leading);
      TouchSpiPause ();

      TouchPinWrite (mSpiMosi, Value);
      TouchSpiPause ();

      TouchPinWrite (mSpiClk, Trailing);
      TouchSpiPause ();
      Receive = (UINT8)((Receive << 1) | (TouchPinRead (mSpiMiso) & 1));
    }
  }

  return Receive;
}

STATIC
VOID
TouchSpiBegin (
  VOID
  )
{
  TouchPinWrite (mSpiCs, 0);
  TouchSpiPause ();
}

STATIC
VOID
TouchSpiEnd (
  VOID
  )
{
  TouchSpiPause ();
  TouchPinWrite (mSpiCs, 1);
  TouchSpiPause ();
}

/**
  Full duplex transfer. Either buffer may be NULL.

  The first transfer is timed and logged: if the bus turns out to be far too slow
  for a 20 ms poll, that number says so instead of leaving it to be guessed from
  how the UI feels.
**/
STATIC
EFI_STATUS
TouchSpiTransfer (
  IN  CONST UINT8  *TxBuffer,
  OUT UINT8        *RxBuffer,
  IN  UINT32       Length
  )
{
  UINT32  Index;
  UINT64  Start;
  UINT64  End;

  if (Length > TOUCH_SPI_MAX_XFER) {
    return EFI_INVALID_PARAMETER;
  }

  Start = GetPerformanceCounter ();

  //
  // Chip select first, then the data line inside the bit loop - which is the order
  // the reference port's gg_transfer uses and the only order that has been observed
  // to work on this board. An earlier revision of this file presented the first bit
  // before asserting chip select, which is textbook SPI but was never what the working
  // implementation does; it was a guess and has been removed.
  //
  TouchSpiBegin ();
  for (Index = 0; Index < Length; Index++) {
    UINT8  Send;
    UINT8  Receive;

    Send    = (TxBuffer != NULL) ? TxBuffer[Index] : 0xFF;
    Receive = TouchSpiByte (Send);
    if (RxBuffer != NULL) {
      RxBuffer[Index] = Receive;
    }
  }

  TouchSpiEnd ();

  //
  // Leave the data line idle high, which is where the next transaction expects to
  // find it and where the controller's own idle state is.
  //
  TouchPinWrite (mSpiMosi, 1);

  if (!mTimingLogged) {
    UINT64  Frequency;
    UINT64  Microseconds;
    UINT64  KilobitsPerSecond;

    mTimingLogged = TRUE;
    End           = GetPerformanceCounter ();
    Frequency     = GetPerformanceCounterProperties (NULL, NULL);
    if ((Frequency != 0) && (End > Start)) {
      //
      // Integer arithmetic on purpose: PrintLib has no floating point support, so
      // a %f here would print garbage or nothing at all.
      //
      Microseconds     = (End - Start) * 1000000 / Frequency;
      KilobitsPerSecond = (Microseconds != 0)
                          ? ((UINT64)Length * 8000 / Microseconds)
                          : 0;
      TcmSay (
        "[TP] spi %u bytes in %u us, about %u kbit/s\r\n",
        Length,
        (UINT32)Microseconds,
        (UINT32)KilobitsPerSecond
        );
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
TouchSpiRead (
  OUT UINT8  *RxBuffer,
  IN  UINT32 Length
  );

/**
  One raw read with no protocol interpretation, plus the number of high bits seen.

  This exists to answer one question that no amount of protocol work can: is
  anything at all driving MISO? The count makes the three cases distinguishable
  at a glance:

    ones == 0        MISO is being held low - either nothing is on the line, or
                     the controller is unpowered and its pins clamp through the
                     ESD diodes.
    ones == Length*8 nothing is driving the line high or the pad is not in input
                     mode, so the pull-up/pull-down is all that is seen.
    anything else    a real signal. Then the bytes themselves say whether it is
                     TouchCom (0xA5 ...) or noise.

  @param[out] Buffer  First bytes received, clocked out as 0xFF.
  @param[in]  Length  How many bytes to clock.
  @param[out] Ones    Number of bits set across the received bytes.
**/
EFI_STATUS
TouchSpiProbe (
  OUT UINT8   *Buffer,
  IN  UINT32  Length,
  OUT UINT32  *Ones
  )
{
  EFI_STATUS  Status;
  UINT32      Index;
  UINT32      Bit;
  UINT32      Count;

  Status = TouchSpiRead (Buffer, Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Count = 0;
  for (Index = 0; Index < Length; Index++) {
    for (Bit = 0; Bit < 8; Bit++) {
      if ((Buffer[Index] & (1U << Bit)) != 0) {
        Count++;
      }
    }
  }

  *Ones = Count;
  return EFI_SUCCESS;
}

EFI_STATUS
TouchSpiWrite (
  IN CONST UINT8  *TxBuffer,
  IN UINT32       Length
  )
{
  if ((TxBuffer == NULL) || (Length == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  return TouchSpiTransfer (TxBuffer, NULL, Length);
}

EFI_STATUS
TouchSpiRead (
  OUT UINT8  *RxBuffer,
  IN  UINT32 Length
  )
{
  if ((RxBuffer == NULL) || (Length == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  return TouchSpiTransfer (NULL, RxBuffer, Length);
}

EFI_STATUS
TouchSpiWriteRead (
  IN  CONST UINT8  *TxBuffer,
  IN  UINT32       TxLen,
  OUT UINT8        *RxBuffer,
  IN  UINT32       RxLen
  )
{
  if ((TxBuffer == NULL) || (RxBuffer == NULL) || (TxLen == 0) || (TxLen != RxLen)) {
    return EFI_INVALID_PARAMETER;
  }

  return TouchSpiTransfer (TxBuffer, RxBuffer, TxLen);
}
