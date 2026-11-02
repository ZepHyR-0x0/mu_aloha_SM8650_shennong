/** @file

  Synaptics S3910P touch controller driver for shennong.

  Publishes EFI_ABSOLUTE_POINTER_PROTOCOL so the Front Page form browser can be
  driven by touch. The stack is:

    TLMM reset (GPIO 0xA1)  ->  GENI SPI (0xA90000)  ->  TCM messages
      ->  simplified 4-byte-per-object report  ->  Absolute Pointer

  Every stage reports its outcome to the serial console and stops on failure
  rather than publishing a pointer protocol that cannot work. One boot therefore
  says exactly how far the chain got.

**/

#include "TouchHw.h"
#include <Uefi.h>
#include <PiDxe.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Protocol/AbsolutePointer.h>

extern EFI_STATUS
TouchSpiInit (
  VOID
  );

extern VOID
TcmSay (
  IN CONST CHAR8  *Format,
  ...
  );

extern VOID
TouchPinWrite (
  IN UINT32  Pin,
  IN UINT32  Value
  );

extern UINT32
TouchPinRead (
  IN UINT32  Pin
  );

extern EFI_STATUS
TouchSpiProbe (
  OUT UINT8   *Buffer,
  IN  UINT32  Length,
  OUT UINT32  *Ones
  );

extern VOID
TouchMisoBias (
  IN UINT32  Bias
  );

extern VOID
TouchSpiSwapData (
  IN BOOLEAN  Swap
  );

extern VOID
TouchSpiSetMode (
  IN UINT32  Cpol,
  IN UINT32  Cpha,
  IN UINT32  Spin
  );

extern EFI_STATUS
TouchMapMmio (
  IN UINTN        Base,
  IN UINTN        Len,
  IN CONST CHAR8  *Name
  );

//
// EFI_CLOCK_PROTOCOL, published by ClockDxe.
//
// Only the two methods needed, in vtable order - Revision at +0x00, GetClockID at
// +0x08, EnableClock at +0x10. This is the same interface UsbfnDwc3Ref uses, and
// going through ClockDxe is what makes the RPMh vote behind the branch happen;
// writing the GCC registers by hand sets them and still leaves the block dead.
//
typedef struct {
  UINT64      Revision;
  EFI_STATUS (EFIAPI *GetClockID) (
    IN  VOID         *This,
    IN  CONST CHAR8  *ClockName,
    OUT UINTN        *ClockId
    );
  EFI_STATUS (EFIAPI *EnableClock) (
    IN VOID   *This,
    IN UINTN  ClockId
    );
} TOUCH_CLOCK_IF;

//
// GCC QUPv3 wrap1 branch register, and the bit for serial engine 4. From
// drivers/clk/qcom/gcc-sm8650.c: gcc_qupv3_wrap1_s4_clk enable_reg 0x52008,
// mask BIT(26). Bit 0 of the same register is the branch's halt/off indicator.
//
#define TOUCH_GCC_WRAP1_CBCR    0x00152008UL
#define TOUCH_GCC_WRAP1_S4_EN   (1U << 26)

/**
  Read the serial engine's state, then put it in the state the reference requires.

  This is the precondition the reference port checks before it is willing to touch a
  single pin, and this driver has never looked at it:

    ((firmware >> 8) & 0xff) == 1     the engine is configured as SPI
    !(status & 0x1001)                no command active, and bit 12 clear
    !(dma_interface & 3)              the DMA interface is not enabled
    (tx_fifo & 0xfffffff) == 0        transmit FIFO empty
    (rx_fifo & 0x1ffffff) == 0        receive FIFO empty

  Their code returns an error when any of that fails rather than fixing it, so it
  never says what a failing engine looks like - but an engine left mid-transfer with
  DMA enabled is a plausible way for the pads to stay under its control no matter
  what the pin mux says. Every measurement in this driver says the pads belong to
  the GPIO block: they drive, they read back, and the clock toggles. What has never
  been checked is whether something else is still attached to the same wires.

  So the state is printed, then cleared: outputs off, DMA mode off, any pending
  command aborted, interrupts masked.

  Android's own device tree puts this engine in "se-clk" and the touch works there
  through it, which is why the clock is enabled before this runs.
**/
STATIC
VOID
TouchSeQuiesce (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Base;
  UINT32      Firmware;
  UINT32      EngineStatus;
  UINT32      DmaMode;
  UINT32      DmaConfig;
  UINT32      TxFifo;
  UINT32      RxFifo;

  Base   = (UINTN)TOUCH_SE_BASE;
  Status = TouchMapMmio (Base, TOUCH_SE_LEN, "SE4");
  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] se4 not mapped: %r\r\n", Status);
    return;
  }

  Firmware   = MmioRead32 (Base + GENI_FW_REVISION_RO);
  EngineStatus = MmioRead32 (Base + SE_GENI_STATUS);
  DmaMode    = MmioRead32 (Base + SE_GENI_DMA_MODE_EN);
  DmaConfig  = MmioRead32 (Base + SE_DMA_GENERAL_CFG);
  TxFifo     = MmioRead32 (Base + SE_GENI_TX_FIFO_STATUS);
  RxFifo     = MmioRead32 (Base + SE_GENI_RX_FIFO_STATUS);

  TcmSay ("[TP] se4 fw=%08x status=%08x dma-mode=%08x dma-cfg=%08x\r\n",
          Firmware, EngineStatus, DmaMode, DmaConfig);
  TcmSay ("[TP] se4 tx=%08x rx=%08x output=%08x m-cmd0=%08x\r\n",
          TxFifo,
          RxFifo,
          MmioRead32 (Base + SE_GENI_OUTPUT_CTRL),
          MmioRead32 (Base + SE_GENI_M_CMD0));
  TcmSay ("[TP] se4 if-disable=%08x hw0=%08x hw1=%08x ios=%08x\r\n",
          MmioRead32 (Base + GENI_IF_DISABLE_RO),
          MmioRead32 (Base + SE_HW_PARAM_0),
          MmioRead32 (Base + SE_HW_PARAM_1),
          MmioRead32 (Base + SE_GENI_IOS));

  if ((Firmware == 0xFFFFFFFF) || (EngineStatus == 0xFFFFFFFF)) {
    TcmSay ("[TP] se4 reads all ones: not clocked or not decoded\r\n");
    return;
  }

  MmioWrite32 (Base + SE_GENI_OUTPUT_CTRL, 0);
  MmioWrite32 (Base + SE_GENI_DMA_MODE_EN, 0);
  MmioWrite32 (Base + SE_GENI_M_CMD_CTRL, 1);          // M_GENI_CMD_ABORT
  MmioWrite32 (Base + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);

  TcmSay ("[TP] se4 after quiesce: status=%08x dma-mode=%08x output=%08x tx=%08x rx=%08x\r\n",
          MmioRead32 (Base + SE_GENI_STATUS),
          MmioRead32 (Base + SE_GENI_DMA_MODE_EN),
          MmioRead32 (Base + SE_GENI_OUTPUT_CTRL),
          MmioRead32 (Base + SE_GENI_TX_FIFO_STATUS),
          MmioRead32 (Base + SE_GENI_RX_FIFO_STATUS));
}

/**
  Turn on the serial engine's clock through ClockDxe.

  This is the one step the reference port on this board takes that this driver
  never did, and it is worth doing for that reason alone: on houji the sequence is
  RPMh for the rails, then ClockDxe for the SE clock, then GPIO for the bus, and
  the probe there recorded the branch as off (80004000) before any of it.

  Whether a software-driven bus needs the SE's clock is a fair question - the bit
  banging does not go through the engine. But the engine's I/O cells sit between
  the pads and the outside world, and an unclocked engine is not the same thing as
  an absent one. It costs one protocol call to stop guessing.
**/
STATIC
VOID
TouchClockOn (
  VOID
  )
{
  EFI_GUID        Guid = { 0x241AFAE6, 0x885F, 0x4F6C,
                           { 0xA7, 0xEA, 0xC2, 0x8E, 0xAB, 0x79, 0xC3, 0xE5 } };
  TOUCH_CLOCK_IF  *Clock;
  EFI_STATUS      Status;
  UINTN           Id;
  UINT32          Before;
  UINT32          After;

  Before = 0xFFFFFFFF;
  After  = 0xFFFFFFFF;

  if (!EFI_ERROR (TouchMapMmio (0x00152000UL, 0x1000, "GCC QUP1"))) {
    Before = MmioRead32 (TOUCH_GCC_WRAP1_CBCR);
  }

  Clock = NULL;
  Status = gBS->LocateProtocol (&Guid, NULL, (VOID **)&Clock);
  if (EFI_ERROR (Status) || (Clock == NULL)) {
    TcmSay ("[TP] clock protocol not found: %r\r\n", Status);
    return;
  }

  //
  // The device tree gives this clock as id 0x6c in the downstream numbering, which
  // is what ClockDxe's table uses, so the numeric id is the reliable argument. The
  // name lookup is attempted first only because it would confirm the mapping.
  //
  Id     = 0;
  Status = Clock->GetClockID (Clock, "gcc_qupv3_wrap1_s4_clk", &Id);
  TcmSay ("[TP] clock by name -> %r, id=0x%x\r\n", Status, (UINT32)Id);
  if (EFI_ERROR (Status) || (Id == 0)) {
    Id = TOUCH_SE_CLK_ID;
  }

  Status = Clock->EnableClock (Clock, Id);
  TcmSay ("[TP] enable clock 0x%x -> %r\r\n", (UINT32)Id, Status);

  if (Before != 0xFFFFFFFF) {
    After = MmioRead32 (TOUCH_GCC_WRAP1_CBCR);
    TcmSay ("[TP] qup1 cbcr %08x -> %08x (bit26 %u -> %u)\r\n",
            Before,
            After,
            (Before & TOUCH_GCC_WRAP1_S4_EN) ? 1U : 0U,
            (After & TOUCH_GCC_WRAP1_S4_EN) ? 1U : 0U);
  }
}

extern EFI_STATUS
TcmLoadDescriptor (
  OUT UINT32  *Length
  );

extern EFI_STATUS
TcmReadMessage (
  OUT UINT8  *Code,
  OUT UINT8  *Payload,
  OUT UINT32 *Length
  );

extern EFI_STATUS
TcmCommand (
  IN  UINT8        Command,
  IN  UINT8        Expected,
  IN  CONST UINT8  *Send,
  IN  UINT32       SendLen,
  OUT UINT8        *Resp,
  OUT UINT32       *RespLen
  );

extern VOID
RpmhTouchRailsOn (
  VOID
  );

extern EFI_STATUS
RpmhTouchIovddOn (
  VOID
  );

extern VOID
RpmhCmdDbDump (
  VOID
  );

extern EFI_STATUS
TcmIdentify (
  OUT UINT8  *Payload,
  OUT UINT32 *Length
  );

extern EFI_STATUS
TcmGetGeometry (
  OUT UINT32  *MaxX,
  OUT UINT32  *MaxY,
  OUT UINT32  *MaxObjects
  );

extern EFI_STATUS
TcmSetSimpleReportConfig (
  VOID
  );

extern EFI_STATUS
TcmPollTouch (
  OUT UINT32  *X,
  OUT UINT32  *Y,
  OUT BOOLEAN *Touching,
  OUT UINT8   *Raw,
  OUT UINT32  *RawLen
  );

extern EFI_STATUS
TouchMapMmio (
  IN UINTN        Base,
  IN UINTN        Len,
  IN CONST CHAR8  *Name
  );

extern EFI_STATUS
TouchPinMap (
  IN UINT32  Pin
  );

STATIC EFI_ABSOLUTE_POINTER_PROTOCOL  mAbsPointer;
STATIC EFI_ABSOLUTE_POINTER_MODE      mAbsMode;
STATIC EFI_ABSOLUTE_POINTER_STATE     mAbsState;
STATIC EFI_EVENT                      mAbsWaitEvent;
STATIC BOOLEAN                        mStatePending;
STATIC UINT32                         mTransitionsLogged;
STATIC EFI_EVENT                      mPollTimer;
STATIC UINT32                         mMaxX = 1440;
STATIC UINT32                         mMaxY = 3200;
STATIC BOOLEAN                        mWasTouching = FALSE;
STATIC UINT32                         mRawLogged = 0;
STATIC UINT32                         mPollFails = 0;
STATIC UINT32                         mResetActiveLevel = 0;

VOID
TcmSay (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR8    Buffer[192];
  UINTN    Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  if (Length > 0) {
    SerialPortWrite ((UINT8 *)Buffer, MIN (Length, sizeof (Buffer) - 1));
  }
}

/**
  Read the PMIC side of the touch rails over SPMI.

  b78 drove the AVDD switch and the controller still would not answer: MISO and IRQ
  stayed clamped low and the reset line read low even while it was being driven
  high, which is what an unpowered chip looks like. AVDD on this board is a
  regulator-fixed behind GPIO 0xA4, so the switch is only half of it - the rail
  feeding that switch is a PMIC LDO, and the device tree names both of them:

      avdd-supply  = touch_avdd_vreg   (GPIO 0xA4 switches it)
      iovdd-supply = pm_humu_l12       (RPMh LDO b12, 1.8 V)

  PmicDxeLa carries the node names /pm/ldob9/{en,mV,mode} and /pm/ldob13/... in its
  own strings, so the LDOs exist as PMIC5 peripherals. PMIC5 LDOs sit at 0x4000 plus
  0x100 per index with the enable at +0x46 and the voltage at +0x41/+0x42 - the same
  layout the eUSB2 repeater block at 0xFD00 uses, which this port already reads and
  writes successfully on SPMI slave 7.

  This is a read-only probe on purpose. It answers two questions at once: whether the
  LDO registers are reachable over SPMI at all, and whether l9/l12 are currently
  enabled. A write only becomes worth attempting once both are known.
**/
// Not static: kept out of the boot path but still compiled, so the SPMI/RPMh
// probe code stays buildable without an unused-function warning.
VOID
TouchRailsProbe (
  VOID
  )
{
  EFI_GUID  SpmiGuid = { 0xFA5F306B, 0xF47D, 0x4AC4,
                         { 0xA4, 0x7D, 0x88, 0x2F, 0x82, 0x04, 0xEC, 0x30 } };

  typedef UINT32 (EFIAPI *TOUCH_SPMI_READ)(
                          VOID *Proto, UINT32 A1, UINT32 Slave, UINT32 A3,
                          UINT32 Reg, UINT32 *Buf, UINT32 Len, UINT32 *Status);

  STATIC CONST UINT32  Registers[] = {
    0x4046,   // ldo1 enable - control: does the LDO block answer at all
    0x4846,   // l9  enable
    0x4841,   // l9  voltage low
    0x4B46,   // l12 enable
    0x4B41    // l12 voltage low
  };

  VOID            *Spmi;
  TOUCH_SPMI_READ  SpmiRead;
  UINT32           Value;
  UINT32           Status;
  UINT32           Rc;
  UINTN            Index;

  Spmi = NULL;
  if (EFI_ERROR (gBS->LocateProtocol (&SpmiGuid, NULL, &Spmi)) || (Spmi == NULL)) {
    TcmSay ("[TP] rails: spmi protocol not found\r\n");
    return;
  }

  SpmiRead = (TOUCH_SPMI_READ)*(UINT64 *)((UINT8 *)Spmi + 0x08);
  if (SpmiRead == NULL) {
    TcmSay ("[TP] rails: spmi read missing\r\n");
    return;
  }

  //
  // Full PMIC5 common register dump for every responder.
  //
  // b91 established where the LDO blocks are; this reads the rest of each one. The
  // register offsets are not guesses any more - qcom_spmi-regulator.c defines them:
  //
  //   SPMI_COMMON_REG_VOLTAGE_RANGE 0x40   SPMI_COMMON_REG_VOLTAGE_SET 0x41
  //   SPMI_COMMON_REG_MODE          0x45   SPMI_COMMON_REG_ENABLE      0x46 (bit 7)
  //
  // and the LDO base is 0x4000 + (n - 1) * 0x100, so l9 is 0x4800 and l12 is 0x4B00.
  // An enabled LDO with a zero voltage setpoint is a contradiction, so these five bytes
  // per regulator say whether the block really is what it looks like, which rails are
  // live, and whether anything sits at 1.8 V - the value IOVDD should have.
  //
  {
    STATIC CONST UINT32  Slaves[] = { 0, 1, 7 };
    UINT32               SlaveIndex;
    UINT32               Base;
    UINT32               Hit;
    UINT32               Enable;
    UINT32               Range;
    UINT32               Volt;
    UINT32               Mode;
    UINT32               Pull;
    UINT32               VoltRc;

    for (SlaveIndex = 0; SlaveIndex < ARRAY_SIZE (Slaves); SlaveIndex++) {
      for (Base = 0x4000; Base < 0x6000; Base += 0x100) {
        Enable = 0;
        Status = 0;
        Hit    = SpmiRead (Spmi, 0, Slaves[SlaveIndex], 0, Base + 0x46, &Enable, 1, &Status);
        if (Hit != 0) {
          continue;
        }

        Range = 0; Volt = 0; Mode = 0; Pull = 0;
        Status = 0; SpmiRead (Spmi, 0, Slaves[SlaveIndex], 0, Base + 0x40, &Range, 1, &Status);
        Status = 0; VoltRc = SpmiRead (Spmi, 0, Slaves[SlaveIndex], 0, Base + 0x41, &Volt, 1, &Status);
        Status = 0; SpmiRead (Spmi, 0, Slaves[SlaveIndex], 0, Base + 0x45, &Mode,  1, &Status);
        Status = 0; SpmiRead (Spmi, 0, Slaves[SlaveIndex], 0, Base + 0x48, &Pull,  1, &Status);

        //
        // The voltage read code matters: an enabled LDO cannot have a zero setpoint, so
        // if the read is failing the zero means nothing. This is the difference between
        // "the rail is at zero volts" and "the read never happened".
        //
        TcmSay ("[TP] s%u %04x rng=%02x volt=%02x(rc=%u) mode=%02x en=%02x pull=%02x%s\r\n",
                Slaves[SlaveIndex], Base,
                Range & 0xFF, Volt & 0xFF, VoltRc, Mode & 0xFF, Enable & 0xFF, Pull & 0xFF,
                ((Enable & 0x80) != 0) ? "  <- enabled" : "");
      }
    }
  }

  //
  // Map which PMIC hosts the LDO block before trusting any single address.
  //
  // b83 read 0x4846 and got a value, but a read that succeeds only proves some
  // peripheral answered on that slave - not that the address is the touch rail's LDO.
  // PMIC5 puts LDOs at 0x4000 + n*0x100 with the enable at +0x46, so the shape of the
  // responders tells us where the block actually lives and how many LDOs it has.
  //
  {
    UINT32  Slave;
    UINT32  Base;
    UINT32  Hit;
    UINT32  Value;
    CHAR8   Line[160];
    UINTN   Len;
    UINT32  Count;

    for (Slave = 0; Slave < 8; Slave++) {
      Len   = AsciiSPrint (Line, sizeof (Line), "[TP] spmi slave %u:", Slave);
      Count = 0;

      for (Base = 0x4000; Base < 0x6000; Base += 0x100) {
        Value  = 0;
        Status = 0;
        Hit    = SpmiRead (Spmi, 0, Slave, 0, Base + 0x46, &Value, 1, &Status);
        if (Hit != 0) {
          continue;
        }

        Count++;
        if (Len < sizeof (Line) - 12) {
          Len += AsciiSPrint (&Line[Len], sizeof (Line) - Len, " %04x=%02x",
                              Base, Value & 0xFF);
        }
      }

      if (Count == 0) {
        Len += AsciiSPrint (&Line[Len], sizeof (Line) - Len, " none");
      }

      Len += AsciiSPrint (&Line[Len], sizeof (Line) - Len, "\r\n");
      SerialPortWrite ((UINT8 *)Line, Len);
    }
  }

  //
  // Sweep the whole PMIC5 LDO window first. b82 probed five addresses and found 0x4800
  // alive while 0x4000 and 0x4B00 were not, which means the map is not the uniform
  // 0x4000 + n*0x100 it assumed - and l12 was never located. Sixteen reads cost nothing
  // and settle the layout.
  //
  for (Index = 0; Index < 0x10; Index++) {
    UINT32  Base;
    UINT32  Enable;
    UINT32  Volt;

    Base   = 0x4700 + Index * 0x100;
    Enable = 0;
    Volt   = 0;
    Status = 0;
    Rc     = SpmiRead (Spmi, 0, 7, 0, Base + 0x46, &Enable, 1, &Status);
    if (Rc != 0) {
      continue;
    }

    Status = 0;
    SpmiRead (Spmi, 0, 7, 0, Base + 0x41, &Volt, 1, &Status);

    TcmSay ("[TP] ldo 0x%04x en=0x%02x volt=0x%02x\r\n", Base, Enable & 0xFF, Volt & 0xFF);
  }

  for (Index = 0; Index < ARRAY_SIZE (Registers); Index++) {
    Value  = 0;
    Status = 0;
    Rc     = SpmiRead (Spmi, 0, 7, 0, Registers[Index], &Value, 1, &Status);
    TcmSay ("[TP] rails: spmi 0x%04x -> rc=%u val=0x%02x\r\n",
            Registers[Index], Rc, Value & 0xFF);
  }
}

/**
  Why the LDO is not written directly.

  b80 tried: after reading 0x4841 and 0x4846 as zero, it wrote 3100 mV to 0x4841 and
  then set the enable bit. The machine died on the write, inside SPMI.dll:

      Synchronous Exception
      ESR = 0x96000010      EC 0x25 (data abort, same EL), DFSC 0x10 (external abort)
      IMG = 0x3B54000 + 0x37F0   SPMI.dll
      CALLER = 0x3B54000 + 0x2944  SPMI.dll

  Reads of the same registers return rc=0 with sane values, and writes to the eUSB2
  repeater at 0xFD46 return rc=0 as well, so SPMI writes work in general and it is
  this address that faults. That is what a register owned by RPMh looks like from the
  outside: readable through the PMIC's SPMI window, not writable by the AP.

  So the rail has to be turned on the way the kernel does it - an RPMh VRM request -
  and that is a separate piece of work. Nothing here writes PMIC registers again
  until then.

**/
/**
  Independent check that the RPMh vote landed.

  The SPMI read of the LDO's own enable register has nothing to do with RPMh's
  completion logic, so it is the only evidence here that does not depend on this
  driver's own bookkeeping: if l9 is really on, 0x4846 should now read 0x80 where it
  read 0x00 before the vote.
**/
// Not static: kept out of the boot path but still compiled, so the SPMI/RPMh
// probe code stays buildable without an unused-function warning.
VOID
TouchRailsCheck (
  VOID
  )
{
  EFI_GUID  SpmiGuid = { 0xFA5F306B, 0xF47D, 0x4AC4,
                         { 0xA4, 0x7D, 0x88, 0x2F, 0x82, 0x04, 0xEC, 0x30 } };

  typedef UINT32 (EFIAPI *TOUCH_SPMI_READ)(
                          VOID *Proto, UINT32 A1, UINT32 Slave, UINT32 A3,
                          UINT32 Reg, UINT32 *Buf, UINT32 Len, UINT32 *Status);

  VOID             *Spmi;
  TOUCH_SPMI_READ   SpmiRead;
  UINT32            Enable;
  UINT32            Status;

  Spmi = NULL;
  if (EFI_ERROR (gBS->LocateProtocol (&SpmiGuid, NULL, &Spmi)) || (Spmi == NULL)) {
    return;
  }

  SpmiRead = (TOUCH_SPMI_READ)*(UINT64 *)((UINT8 *)Spmi + 0x08);
  if (SpmiRead == NULL) {
    return;
  }

  //
  // The four addresses that could be the touch rails, read after the vote. b94 fixed the
  // IOVDD address - it had been going to ldob1 at 0x40700 because the name match only
  // compared five characters - so l12 at 0x40900 is voted on for the first time here and
  // these lines say whether anything moved.
  //
  {
    STATIC CONST UINT32  Check[4][2] = { { 1, 0x4846 }, { 7, 0x4846 }, { 0, 0x5846 }, { 0, 0x5B46 } };
    UINTN                Index;

    for (Index = 0; Index < 4; Index++) {
      Enable = 0;
      Status = 0;
      SpmiRead (Spmi, 0, Check[Index][0], 0, Check[Index][1], &Enable, 1, &Status);
      TcmSay ("[TP] after vote: s%u %04x en=0x%02x\r\n",
              Check[Index][0], Check[Index][1], Enable & 0xFF);
    }
  }
}

/**
  Switch AVDD on through the GPIO the device tree gives it.

  touch_avdd_vreg is a regulator-fixed on TLMM 0xA4, active high. Without this the
  controller never gets 3.0 V and cannot answer anything - which is what the
  previous build showed: identify failed with MISO and IRQ both reading low
  through their pull-ups, the signature of a chip that is not powered at all.
**/
STATIC
EFI_STATUS
TouchPowerOn (
  VOID
  )
{
  EFI_STATUS  Status;

  //
  // No RPMh vote here any more, and the reason is worth writing down because a lot
  // of builds went into the alternative.
  //
  // shennong-sm8650.dtsi, synaptics_tcm@0:
  //
  //   avdd-supply  = <&touch_avdd_vreg>;   regulator-fixed, gpio <&tlmm 164 0>
  //   iovdd-supply = <&L12B>;
  //
  // AVDD is the GPIO driven just below. It is not a PMIC rail on this board. The
  // houji device tree next door says avdd-supply = <&L9B>, which is where the "vote
  // ldob9" idea came from - and houji is a different phone.
  //
  // IOVDD is L12B, and cliffs-regulators.dtsi says of it: "initial voltage of L12B
  // is set to be 2.4v/2.96v during PON ... UFS is the only client on this". UFS is
  // this firmware's root device, so L12B cannot be off. It is on by necessity.
  //
  // L12 is also not addressable over SPMI at all: it is an rpmh-vrm-regulator, so
  // the PMIC firmware owns the register and the AP only ever sees a CmdDb resource
  // address (ldob12 = 0x40900). That is why every SPMI read of 0x4846/0x4B00 came
  // back rc=0 value=0 - those addresses belong to nobody - and why writing one
  // faulted inside SPMI.dll. The register map was never wrong; there is no register.
  //
  // The IOVDD vote is back, and the reason the earlier reasoning was wrong is worth
  // recording: Android's regulator_summary does show pm_humu_l12 enabled at 1800 mV
  // in fast mode, but that is Android, where the kernel's own rpmh-regulator driver
  // votes for it at probe time from qcom,init-voltage. Nothing in UEFI does that. The
  // argument "L12 is shared with periph-1cc and vddio, and I2C works, so it must be
  // on" was inference, not measurement, and it is the one part of the power chain
  // this driver never actually established.
  //
  // If IOVDD is missing, the controller's IO ring is dead: its reset input still
  // floats (so the pull-up reads 1) while MISO never moves - which is exactly what
  // every build has measured. So the vote is issued, and the controller's answer is
  // the proof of whether it worked. There is no other way to observe an RPMh VRM.
  //
  Status = RpmhTouchIovddOn ();
  TcmSay ("[TP] IOVDD vote -> %r\r\n", Status);

  Status = TouchPinMap (TOUCH_AVDD_PIN);
  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] avdd pin page not mapped\r\n");
    return Status;
  }

  //
  // What ABL left on this pin, before anything is written to it.
  //
  TcmSay (
    "[TP] gpio%u before: cfg=%08x io=%08x\r\n",
    TOUCH_AVDD_PIN,
    MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (TOUCH_AVDD_PIN)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (TOUCH_AVDD_PIN))
    );

  //
  // 6 mA, not 2. A load-switch enable is a logic input, so drive strength should
  // not matter - but the reset line reads low even when driven at 2 mA, and one of
  // the two explanations for that is that a drive-strength field of zero is not the
  // weak setting but no setting at all. 6 mA is what the device tree asks for on
  // every other pin of this controller.
  //
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (TOUCH_AVDD_PIN),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_OE
    );
  TouchPinWrite (TOUCH_AVDD_PIN, 1);
  MicroSecondDelay (TOUCH_POWER_DELAY_MS * 1000);

  TcmSay (
    "[TP] avdd gpio%u on, cfg=%08x io=%08x, waited %u ms\r\n",
    TOUCH_AVDD_PIN,
    MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (TOUCH_AVDD_PIN)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (TOUCH_AVDD_PIN)),
    TOUCH_POWER_DELAY_MS
    );

  return EFI_SUCCESS;
}

/**
  Does the controller have power? Three measurements that vary the suspect.

  b99 printed the one result that made every earlier power argument untenable: the
  reset line, driven high with its output enabled at 2 mA, read 0 on its input path -
  and the same pin read 0 with a pull-up on it. A pin whose only other connection is a
  CMOS input on a powered chip reads 1 in both cases. So either something holds that
  line down hard, or the output driver was never on.

  Rather than argue, this function changes the two things that could be responsible and
  watches what moves: drive strength, and the AVDD switch. The AVDD sweep is the one
  that matters - see the comment at the loop for how to read its three lines.
**/
STATIC
VOID
TouchLineProbe (
  VOID
  )
{
  UINT32  High;
  UINT32  Low;
  UINT32  Phase;
  UINT32  Avdd;
  UINT32  Rst[3];

  //
  // Three measurements that together answer the one question left: does the
  // controller have power?
  //
  // Every previous probe rested on a single reading of a single pin and could be
  // explained away. These cannot, because they vary the thing under suspicion.
  //
  // b99's log is what makes this necessary: the reset line, driven high with the
  // output enabled, still read 0 on its input path, and a pull-up on it read 0 too.
  // A pin whose only connection is a CMOS input on a powered chip reads 1 in both
  // of those cases. Something is holding it down hard - an unpowered chip clamps
  // through its ESD diodes - or the output driver was never on in the first place.
  //
  // 1. Drive-strength ladder on the reset line. If 8 mA can lift the read to 1,
  //    the thing holding it down is a resistor and the pad was simply under-driven.
  //    If no strength lifts it, it is a clamp.
  //
  TcmSay (
    "[TP] reset ladder: cfg=%08x io=%08x before\r\n",
    MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (TOUCH_RESET_PIN)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (TOUCH_RESET_PIN))
    );

  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (TOUCH_RESET_PIN),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_2MA | TLMM_CFG_OE
    );
  TouchPinWrite (TOUCH_RESET_PIN, 1);
  gBS->Stall (2000);
  High = TouchPinRead (TOUCH_RESET_PIN);

  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (TOUCH_RESET_PIN),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_OE
    );
  TouchPinWrite (TOUCH_RESET_PIN, 1);
  gBS->Stall (2000);
  Low = TouchPinRead (TOUCH_RESET_PIN);

  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (TOUCH_RESET_PIN),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_8MA | TLMM_CFG_OE
    );
  TouchPinWrite (TOUCH_RESET_PIN, 1);
  gBS->Stall (2000);

  TcmSay (
    "[TP] reset high at 2/6/8 mA -> %u %u %u (want 1 1 1)\r\n",
    High,
    Low,
    TouchPinRead (TOUCH_RESET_PIN)
    );

  //
  // 2. Can any pad here be driven at all? The interrupt line is driven as an output
  //    and read back. If that tracks, the pad path is fine and the reset line's low
  //    reading is external to this driver rather than a configuration mistake.
  //
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (TOUCH_IRQ_PIN),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_OE
    );
  TouchPinWrite (TOUCH_IRQ_PIN, 1);
  gBS->Stall (1000);
  High = TouchPinRead (TOUCH_IRQ_PIN);
  TouchPinWrite (TOUCH_IRQ_PIN, 0);
  gBS->Stall (1000);
  Low = TouchPinRead (TOUCH_IRQ_PIN);

  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (TOUCH_IRQ_PIN),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_2MA | TLMM_CFG_PULL_UP
    );
  TcmSay (
    "[TP] irq pad self-test: drive 1 -> %u, drive 0 -> %u (want 1 then 0)\r\n",
    High,
    Low
    );

  //
  // 3. Sweep AVDD and watch the reset line each time.
  //
  // This is the measurement that settles it. The reset pin is configured as an
  // input with a pull-up - it drives nothing - and read once with the AVDD switch
  // on, once with it off, and once on again:
  //
  //   1 1 1   the chip is powered either way: GPIO 164 is not its switch, or the
  //           rail it gates was already up
  //   1 0 1   GPIO 164 is the switch and this polarity is right
  //   0 1 0   GPIO 164 is the switch and enable-active-high is inverted on this
  //           board, which would mean the chip has been off in every build so far
  //   0 0 0   the pin is clamped regardless: whatever feeds the switch is dead, or
  //           the switch is not on GPIO 164
  //
  // The same three pins are read every time, so the line to compare is the first
  // number.
  //
  for (Phase = 0; Phase < 3; Phase++) {
    Avdd = (Phase == 1) ? 0 : 1;
    TouchPinWrite (TOUCH_AVDD_PIN, Avdd);
    gBS->Stall (60000);

    MmioWrite32 (
      TLMM_BASE + TLMM_GPIO_CFG (TOUCH_RESET_PIN),
      TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_2MA | TLMM_CFG_PULL_UP
      );
    gBS->Stall (2000);

    Rst[Phase] = TouchPinRead (TOUCH_RESET_PIN);
    TcmSay (
      "[TP] avdd=%u: rst-pullup=%u irq=%u miso=%u\r\n",
      Avdd,
      Rst[Phase],
      TouchPinRead (TOUCH_IRQ_PIN),
      TouchPinRead (TOUCH_SPI_PIN_0)
      );
  }

  //
  // If the pin follows AVDD the switch is doing its job, so leave it in whichever
  // state produced a high reading rather than in whichever state this driver assumed.
  // Without this the sweep would answer the question and then throw the answer away:
  // an inverted switch would be left off for the rest of the boot.
  //
  if ((Rst[1] == 1) && (Rst[0] == 0)) {
    TouchPinWrite (TOUCH_AVDD_PIN, 0);
    TcmSay ("[TP] avdd: pin follows the switch inverted, leaving it LOW\r\n");
  } else {
    TouchPinWrite (TOUCH_AVDD_PIN, 1);
    TcmSay ("[TP] avdd: leaving it HIGH (sweep %u %u %u)\r\n", Rst[0], Rst[1], Rst[2]);
  }

  //
  // Reset line back to an output for TouchReset, at 6 mA - see the note in
  // TouchPowerOn about why 2 mA is no longer trusted.
  //
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (TOUCH_RESET_PIN),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_OE
    );
}


/**
  Drive the reset line and wait out the timing the device tree asks for.

  TLMM is declared in PlatformMemoryMapLib but, like CORE_TOP_CSR, AddDev only
  puts it in the GCD - so ask for the mapping first and give up if it is refused,
  rather than taking a translation fault on a page nobody ever mapped.
**/
STATIC
EFI_STATUS
TouchReset (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Config;
  UINT32      IrqInReset;
  UINT32      IrqAfter;

  //
  // TouchSpiInit already mapped TLMM; this call is here so the function is
  // self-contained if the order ever changes. Same length on purpose - asking
  // SetMemorySpaceAttributes to cover 16 MB would extend past what was added and
  // fail, which is how the first version of this function quietly died.
  //
  Status = TouchMapMmio (TLMM_BASE, TLMM_MAP_LEN, "TLMM");
  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] TLMM not accessible: %r\r\n", Status);
    return Status;
  }

  //
  // Reset pin: plain GPIO output, no pulls. The device tree gives it flags 0,
  // and the vendor driver drives 0 for reset-active-ms then 1 - so 0 is asserted.
  //
  // Both pins get their own page mapped first: touching a TLMM page whose page
  // tables were never built resets the SoC, and a whole-range mapping call is
  // not enough because the GCD already believes these ranges are uncached.
  //
  if (EFI_ERROR (TouchPinMap (TOUCH_RESET_PIN))) {
    TcmSay ("[TP] reset pin page not mapped\r\n");
    return EFI_DEVICE_ERROR;
  }

  if (EFI_ERROR (TouchPinMap (TOUCH_IRQ_PIN))) {
    TcmSay ("[TP] irq pin page not mapped\r\n");
    return EFI_DEVICE_ERROR;
  }

  //
  // Power detector, before the pin is driven.
  //
  // The reset line is configured as an input with a pull-up for a moment: an unpowered
  // controller clamps its pins low through the ESD diodes and the pull-up loses, while a
  // powered one leaves the pin high impedance and the pull-up wins. This needs no
  // knowledge of any PMIC register, which is exactly why it is here - every register
  // based answer so far has depended on a map that may be wrong.
  //
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (TOUCH_RESET_PIN),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_2MA | TLMM_CFG_PULL_UP
    );
  gBS->Stall (2000);
  TcmSay ("[TP] power probe: rst input+pullup reads %u (1 = chip has power)\r\n",
          TouchPinRead (TOUCH_RESET_PIN));

  //
  // 6 mA, and this is the bug that cost every build up to b100.
  //
  // b100's ladder drove this same pin high at three strengths and read the input
  // path back each time:
  //
  //   [TP] reset high at 2/6/8 mA -> 0 1 1
  //
  // At 2 mA the line stays low; at 6 mA and 8 mA it goes high. Whatever holds it
  // down - an external pull-down, or the controller's own reset input - is stronger
  // than a 2 mA driver. A drive-strength field of zero is therefore not a weak
  // setting on this pad, it is effectively no output at all.
  //
  // This line used to write TLMM_CFG_DRV_2MA, so the sequence below asserted reset
  // and then never released it: the release write went into a pin that could not
  // pull its own line up. Every "the controller never spoke" result in this log is
  // downstream of that one constant.
  //
  Config = TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_6MA | TLMM_CFG_OE;
  MmioWrite32 (TLMM_BASE + TLMM_GPIO_CFG (TOUCH_RESET_PIN), Config);

  //
  // The interrupt line is configured as an input with a pull-up before the pulse,
  // because it is the one signal that says whether the controller actually booted -
  // and it says so without depending on the SPI transport at all.
  //
  // Held in reset, the controller tri-states its outputs, so the pull-up wins and
  // the line reads 1. Released and running, it asserts attention, which on this part
  // is active low, so the line reads 0.
  //
  //   1 then 0   the chip came out of reset and is asking to be read
  //   1 then 1   it is out of reset but has nothing to say
  //   0 then 0   it is clamped: no power, or reset never moved
  //
  // That last case is what every build up to b100 produced, for the reason below.
  //
  MmioWrite32 (
    TLMM_BASE + TLMM_GPIO_CFG (TOUCH_IRQ_PIN),
    TLMM_CFG_FUNC_GPIO | TLMM_CFG_DRV_2MA | TLMM_CFG_PULL_UP
    );

  //
  // Assert low for 10 ms, release, then wait 200 ms - the device tree's
  // synaptics,reset-active-ms and synaptics,reset-delay-ms, which is also exactly
  // what the reference port on this board does.
  //
  // mResetActiveLevel is 0. It used to be flipped on retries because the polarity
  // was in doubt; b100's ladder settled the polarity question differently - the
  // line was never going high at all, because it was driven at 2 mA.
  //
  //
  // Bus idle levels before the pulse, exactly as the reference port leaves them:
  // MOSI driven high, clock low, chip select high, reset high. MOSI at 0 was this
  // driver's own choice and nothing ever justified it - the reference sets it to 1
  // on the line before it even configures the pins.
  //
  TouchPinWrite (TOUCH_SPI_PIN_1, 1);
  TouchPinWrite (TOUCH_SPI_PIN_2, 0);
  TouchPinWrite (TOUCH_SPI_PIN_3, 1);

  TouchPinWrite (TOUCH_RESET_PIN, mResetActiveLevel);
  MicroSecondDelay (TOUCH_RESET_ACTIVE_MS * 1000);
  IrqInReset = TouchPinRead (TOUCH_IRQ_PIN);

  TouchPinWrite (TOUCH_RESET_PIN, mResetActiveLevel ^ 1);
  MicroSecondDelay (TOUCH_RESET_DELAY_MS * 1000);
  IrqAfter = TouchPinRead (TOUCH_IRQ_PIN);

  TcmSay (
    "[TP] irq: %u in reset, %u after release (1 then 0 = controller booted)\r\n",
    IrqInReset,
    IrqAfter
    );

  TcmSay (
    "[TP] reset done, gpio%u cfg=%08x io=%08x, irq gpio%u level=%u, miso=%u\r\n",
    TOUCH_RESET_PIN,
    MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (TOUCH_RESET_PIN)),
    MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (TOUCH_RESET_PIN)),
    TOUCH_IRQ_PIN,
    TouchPinRead (TOUCH_IRQ_PIN),
    TouchPinRead (TOUCH_SPI_PIN_0)
    );

  //
  // The interrupt pin was already set up before the pulse, because the booted-or-not
  // reading depends on it; it stays an input with a pull-up. This driver polls and
  // never enables the line to the GIC, so nothing else is needed here.
  //
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TouchGetState (
  IN  EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  OUT EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  if ((This == NULL) || (State == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mStatePending) {
    // A pending release is data even though no finger remains on the panel.
    // Consume every transition once; never suppress it based on button state.
    return EFI_NOT_READY;
  }

  CopyMem (State, &mAbsState, sizeof (*State));

  //
  // The consumer has collected the state, so the wait event has nothing left to
  // report until the next change.
  //
  mStatePending = FALSE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TouchResetProtocol (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  ZeroMem (&mAbsState, sizeof (mAbsState));
  mWasTouching = FALSE;
  mStatePending = FALSE;
  return EFI_SUCCESS;
}

/**
  Notify function for the wait event the absolute pointer publishes.

  EFI_ABSOLUTE_POINTER_PROTOCOL.WaitForInput is an EVT_NOTIFY_WAIT event, not a plain
  one: the firmware calls this function when a consumer waits, and the driver signals
  the event only if there is a state the consumer has not collected yet. Signalling a
  plain manual-reset event instead - which is what this driver did - leaves the event
  permanently set after the first touch and never reports anything again.
**/
STATIC
VOID
EFIAPI
TouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (VOID)Context;

  if (mStatePending) {
    gBS->SignalEvent (Event);
  }
}

STATIC
VOID
EFIAPI
TouchPollTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32   X;
  UINT32   Y;
  UINT32   RawLen;
  UINT8    Raw[TCM_READ_LENGTH];
  BOOLEAN  Touching;
  UINTN    Index;

  if (EFI_ERROR (TcmPollTouch (&X, &Y, &Touching, Raw, &RawLen))) {
    //
    // This used to cancel the timer after eight failures, which is 160 ms. A
    // controller with nothing to report is idle, not dead, and the poll that
    // followed a moment later would have reported a real touch - except the timer
    // no longer existed. Never cancel: a touch panel is polled for as long as the
    // firmware runs, and a genuinely dead link costs one short transfer per period.
    //
    if (mPollFails < 8) {
      mPollFails++;
      if (mPollFails == 8) {
        TcmSay ("[TP] no report in 8 polls; still polling\r\n");
      }
    }

    return;
  }

  mPollFails = 0;

  //
  // Log the first few raw payloads. If the report layout turns out not to be what
  // the descriptor asked for, these bytes are the evidence.
  //
  if ((mRawLogged < 4) && (RawLen > 0)) {
    CHAR8  Line[160];
    UINTN  Len;
    UINTN  Offset;

    Len = AsciiSPrint (Line, sizeof (Line), "[TP] raw(%u):", RawLen);
    for (Offset = 0; (Offset < RawLen) && (Offset < 12); Offset++) {
      Len += AsciiSPrint (&Line[Len], sizeof (Line) - Len, " %02x", Raw[Offset]);
    }
    AsciiSPrint (&Line[Len], sizeof (Line) - Len, "\r\n");
    SerialPortWrite ((UINT8 *)Line, AsciiStrLen (Line));
    mRawLogged++;
  }

  //
  // Report on change, not on every poll.
  //
  // This is the difference between a pointer and a firehose. The previous version set
  // the pending flag unconditionally while a finger was down, so at a 20 ms period it
  // produced fifty events a second whether or not anything had moved. The consumer -
  // FrontPage through MouseAbsolutePointerDxe - could not drain them: the screen shows
  // "Pointer: event: queue overflow", the on-screen keyboard repeats the character
  // under the finger, and the UI stops responding because it never finishes one
  // iteration.
  //
  // A pointer reports three things and only when they change: where it is, that a
  // button went down, and that it came back up. Holding still produces nothing.
  //
  if (Touching) {
    // Preserve a queued release until SWM has consumed it, including quick taps.
    if (mStatePending && mAbsState.ActiveButtons == 0) return;
    if ((X > mMaxX) || (Y > mMaxY)) {
      return;
    }

    if (!mWasTouching ||
        (X != mAbsState.CurrentX) ||
        (Y != mAbsState.CurrentY))
    {
      mAbsState.CurrentX      = X;
      mAbsState.CurrentY      = Y;
      mAbsState.CurrentZ      = 1;
      mAbsState.ActiveButtons = 1;
      mWasTouching            = TRUE;
      mStatePending           = TRUE;

      if (mTransitionsLogged < 8) {
        mTransitionsLogged++;
        TcmSay ("[TP] press/move %u,%u (max %u,%u)\r\n", X, Y, mMaxX, mMaxY);
      }
    }
  } else if (mWasTouching) {
    //
    // Hold the press one poll if nobody has collected it yet.
    //
    // A tap shorter than the consumer's own loop would otherwise be delivered as a
    // release with no press in front of it, and a release that was never preceded by a
    // press is not a click - which is exactly what a tap on FrontPage did: the pointer
    // moved and nothing was activated. Deferring the release until the press has been
    // read turns every tap into press-then-release, whatever the timing.
    //
    if (mStatePending) {
      return;
    }

    mAbsState.ActiveButtons = 0;
    mAbsState.CurrentZ      = 0;
    mWasTouching            = FALSE;
    mStatePending           = TRUE;

    if (mTransitionsLogged < 8) {
      mTransitionsLogged++;
      TcmSay ("[TP] release at %u,%u\r\n", mAbsState.CurrentX, mAbsState.CurrentY);
    }
  }

  (VOID)Index;
}

EFI_STATUS
EFIAPI
SynapticsTouchEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  //
  // TCM_READ_LENGTH, not 64: TcmReadMessage copies up to the payload length it finds
  // in the header, and that field is 16 bits wide - a 64-byte buffer here would have
  // been written past the end by any report longer than 61 bytes.
  //
  UINT8       Identify[TCM_READ_LENGTH];
  UINT32      IdentifyLen;
  UINT32      MaxObjects;
  UINTN       Attempt;
  EFI_HANDLE  Handle;

  TcmSay ("[TP] SynapticsTouchDxe entry, build %a\r\n", TOUCH_BUILD_TAG);

  //
  // Stage 0: the serial engine's clock, then make sure the engine itself is not
  // still holding the bus.
  //
  TouchClockOn ();
  TouchSeQuiesce ();

  //
  // Stage 1: serial engine.
  //
  Status = TouchSpiInit ();
  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] stage 1 GPIO SPI failed: %r\r\n", Status);
    return EFI_SUCCESS;   // do not fail the boot
  }

  //
  // Stage 2: power and reset the controller.
  //
  Status = TouchPowerOn ();
  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] stage 2 power failed: %r\r\n", Status);
    return EFI_SUCCESS;
  }

  TouchLineProbe ();

  Status = TouchReset ();
  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] stage 2 reset failed: %r\r\n", Status);
    return EFI_SUCCESS;
  }

  //
  // Stage 3: listen.
  //
  // The controller announces itself with an unsolicited IDENTIFY report after reset,
  // so the host listens first and only asks if the report that arrives is something
  // else. b108 finally got this far: with IOVDD voted on, the first read returned
  //
  //   A5 10 1C 00 | 01 01 53 33 30 20 41 30 42 30 2D 31 35 2E ...
  //
  // which is code 0x10, length 0x1c, mode 1, and the string "S3910 A0B0-15.2" - the
  // same identification the reference port reports.
  //
  // Nothing destructive may run before this read. Every earlier version opened with
  // an eight-byte raw dump, which takes the first eight bytes of the frame and
  // abandons the other twenty; the stream then stays misaligned and every later read
  // parses payload as a header. That is exactly what b108's log shows after its own
  // sweep: A5 03 39 31 30 20 41 30 is not a header, it is "910 A0" out of the middle
  // of the identification.
  //
  {
    UINT8    Code;
    BOOLEAN  Got;

    Got = FALSE;
    for (Attempt = 0; Attempt < TCM_COMMAND_ATTEMPTS; Attempt++) {
      Code        = 0;
      IdentifyLen = 0;
      ZeroMem (Identify, sizeof (Identify));

      Status = TcmReadMessage (&Code, Identify, &IdentifyLen);
      if (!EFI_ERROR (Status)) {
        TcmSay ("[TP] report 0x%02x len=%u on read %u\r\n",
                Code, IdentifyLen, (UINT32)Attempt);
        Got = TRUE;
        break;
      }

      gBS->Stall (10000);
    }

    if (!Got) {
      //
      // Only now is a raw dump worth anything, and only as a record: it is
      // destructive, so the controller is reset afterwards to resynchronise.
      //
      UINT8   Raw[8];
      UINT32  Ones;

      ZeroMem (Raw, sizeof (Raw));
      Ones = 0;
      if (!EFI_ERROR (TouchSpiProbe (Raw, sizeof (Raw), &Ones))) {
        TcmSay (
          "[TP] silent; raw %02x %02x %02x %02x %02x %02x %02x %02x ones=%u/64\r\n",
          Raw[0], Raw[1], Raw[2], Raw[3], Raw[4], Raw[5], Raw[6], Raw[7], Ones
          );
      }

      TcmSay ("[TP] stage 3: controller never spoke in %u reads\r\n",
              (UINT32)TCM_COMMAND_ATTEMPTS);
      return EFI_SUCCESS;
    }

    //
    // Some firmware announces a reset notice before it is ready for commands, so ask
    // explicitly unless the report that arrived already was IDENTIFY.
    //
    if (Code != TCM_REPORT_IDENTIFY) {
      IdentifyLen = 0;
      ZeroMem (Identify, sizeof (Identify));
      Status = TcmCommand (
                 TCM_CMD_IDENTIFY,
                 TCM_REPORT_IDENTIFY,
                 NULL,
                 0,
                 Identify,
                 &IdentifyLen
                 );
      if (EFI_ERROR (Status)) {
        TcmSay ("[TP] identify after report 0x%02x failed: %r\r\n", Code, Status);
        return EFI_SUCCESS;
      }
    }
  }

  //
  // The payload is [mode][?]["S3910"][...] with the firmware build at offset 18.
  // The reference port checks the same two things: mode == 1 and the "S3910" string.
  // The first 24 bytes go out as hex as well, because "len=32 mode=0 build=0" would
  // otherwise be the only evidence for a payload nobody has seen yet.
  //
  {
    CHAR8  Line[160];
    UINTN  Len;
    UINTN  Offset;
    UINTN  Limit;

    Len    = AsciiSPrint (Line, sizeof (Line), "[TP] identify payload:");
    Limit  = (IdentifyLen < 24) ? IdentifyLen : 24;
    for (Offset = 0; Offset < Limit; Offset++) {
      Len += AsciiSPrint (&Line[Len], sizeof (Line) - Len, " %02x", Identify[Offset]);
    }

    AsciiSPrint (&Line[Len], sizeof (Line) - Len, "\r\n");
    SerialPortWrite ((UINT8 *)Line, AsciiStrLen (Line));
  }

  TcmSay (
    "[TP] identify: len=%u mode=%u build=%u\r\n",
    IdentifyLen,
    (IdentifyLen > 1) ? (UINT32)Identify[1] : 0,
    (IdentifyLen > 21)
      ? (UINT32)(Identify[18] | ((UINT32)Identify[19] << 8) |
                 ((UINT32)Identify[20] << 16) | ((UINT32)Identify[21] << 24))
      : 0
    );

  //
  // Prove the command path separately from the report path.
  //
  // The unsolicited IDENTIFY proves the controller can talk. A command IDENTIFY
  // proves it can be talked to, which is a different direction and the one everything
  // else depends on. b109 read the report and then failed on both GET_APPLICATION_INFO
  // and SET_TOUCH_REPORT_CONFIG with "no answer in 100 polls", and those two failures
  // cannot be told apart from a write path that never reaches the chip.
  //
  // Asking the controller to repeat something it has already said is the cheapest way
  // to separate them: same command, same expected reply, one direction only.
  //
  {
    UINT8   CmdIdentify[TCM_READ_LENGTH];
    UINT32  CmdLen;

    CmdLen = 0;
    ZeroMem (CmdIdentify, sizeof (CmdIdentify));
    Status = TcmCommand (
               TCM_CMD_IDENTIFY,
               TCM_REPORT_IDENTIFY,
               NULL,
               0,
               CmdIdentify,
               &CmdLen
               );
    TcmSay ("[TP] command path: CMD_IDENTIFY -> %r, len=%u\r\n", Status, CmdLen);

    if (!EFI_ERROR (Status) && (CmdLen > 21)) {
      TcmSay ("[TP] command identify: mode=%u build=%u\r\n",
              (UINT32)CmdIdentify[1],
              (UINT32)(CmdIdentify[18] | ((UINT32)CmdIdentify[19] << 8) |
                       ((UINT32)CmdIdentify[20] << 16) | ((UINT32)CmdIdentify[21] << 24)));
    }
  }

  //
  // Stage 4: geometry and report layout, then publish the pointer.
  //
  MaxObjects = 10;
  Status     = TcmGetGeometry (&mMaxX, &mMaxY, &MaxObjects);
  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] geometry failed, keeping defaults %u x %u\r\n", mMaxX, mMaxY);
  }

  //
  // The command sequence the reference port runs after identification, in its order
  // and with its arguments - read from native/touch/HoujiTouchDriver.inc, htRun():
  //
  //   gw_command(&bus, 6, {0xc0}, 1)    stop the THP report stream
  //   gw_command(&bus, 6, {0x23}, 1)    stop the raw frame stream
  //   gw_command(&bus, 0x25, ...)       read the controller's native descriptor
  //   gw_command(&bus, 5, {0x11}, 1)    enable touch reports
  //
  // That is a different route to a working report layout than the one this driver
  // has been taking. The kernel driver writes its own simplified descriptor with
  // SET_TOUCH_REPORT_CONFIG (0x26); the reference port found that unnecessary and
  // reads the controller's own descriptor instead, which is why it never sends 0x26.
  // Following the implementation that is known to work on this hardware is the point.
  //
  // The first two are logged but not treated as fatal: they only silence optional
  // streams, and the reference ignores their result too.
  //
  {
    UINT8   Resp[TCM_READ_LENGTH];
    UINT32  RespLen;
    UINT8   Arg;

    ZeroMem (Resp, sizeof (Resp));
    RespLen = 0;
    Arg     = 0xC0;
    Status  = TcmCommand (0x06, TCM_STATUS_OK, &Arg, 1, Resp, &RespLen);
    TcmSay ("[TP] stop THP stream 0x06/0xc0 -> %r\r\n", Status);

    RespLen = 0;
    Arg     = 0x23;
    Status  = TcmCommand (0x06, TCM_STATUS_OK, &Arg, 1, Resp, &RespLen);
    TcmSay ("[TP] stop raw stream 0x06/0x23 -> %r\r\n", Status);

    RespLen = 0;
    Status  = TcmLoadDescriptor (&RespLen);
    TcmSay ("[TP] get native descriptor 0x25 -> %r, len=%u\r\n", Status, RespLen);

    RespLen = 0;
    Arg     = TCM_REPORT_TOUCH;
    Status  = TcmCommand (0x05, TCM_STATUS_OK, &Arg, 1, Resp, &RespLen);
    TcmSay ("[TP] enable touch reports 0x05/0x11 -> %r\r\n", Status);
  }

  //
  // Only if the controller would not hand over its own descriptor does this driver
  // fall back to writing the simplified one, which is the kernel driver's route.
  //
  if (EFI_ERROR (Status)) {
    Status = TcmSetSimpleReportConfig ();
    TcmSay ("[TP] fallback to simplified report config -> %r\r\n", Status);
    if (EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }
  }

  mAbsMode.AbsoluteMinX = 0;
  mAbsMode.AbsoluteMinY = 0;
  mAbsMode.AbsoluteMaxX = mMaxX;
  mAbsMode.AbsoluteMaxY = mMaxY;
  mAbsMode.Attributes   = 0;

  mAbsPointer.Reset   = TouchResetProtocol;
  mAbsPointer.GetState = TouchGetState;
  mAbsPointer.Mode    = &mAbsMode;

  //
  // EVT_NOTIFY_WAIT with a notify function, which is the shape the protocol expects
  // and the shape the other implementation in usb-touch-isolated uses. A plain
  // manual-reset event is signalled once and stays signalled, so after the first
  // contact every later wait returns immediately and the state is never collected
  // again.
  //
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_CALLBACK,
                  TouchWaitForInput,
                  NULL,
                  &mAbsWaitEvent
                  );

  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] wait event failed: %r\r\n", Status);
    return EFI_SUCCESS;
  }
  mAbsPointer.WaitForInput = mAbsWaitEvent;

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TouchPollTimer, NULL, &mPollTimer);
  if (EFI_ERROR (Status)) {
    TcmSay ("[TP] poll timer failed: %r\r\n", Status);
    return EFI_SUCCESS;
  }
  gBS->SetTimer (mPollTimer, TimerPeriodic, 200000);   // 20 ms

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gEfiAbsolutePointerProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mAbsPointer
                  );
  TcmSay ("[TP] absolute pointer installed: %r\r\n", Status);

  return EFI_SUCCESS;
}
