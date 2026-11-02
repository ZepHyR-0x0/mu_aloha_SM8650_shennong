/** @file

  Minimal QUPv3 GENI SPI master for the touch controller.

  There is no SPI driver in this image to build on. SPIDxe is in the FV but it
  only publishes a three-method stub whose methods are "mov w0, #3 ; ret", and
  nothing else in the FV mentions SPI at all, so this has to be written here.

  The register map and the transfer sequence follow the upstream Linux GENI
  driver (include/linux/soc/qcom/geni-se.h, drivers/soc/qcom/qcom-geni-se.c,
  drivers/spi/spi-geni-qcom.c) rather than anything reconstructed here. An
  earlier version of this file guessed the offsets and had every one of them
  wrong; the independent confirmation that the new ones are right is the I2C
  driver this port already runs on the same silicon - reverse/src/I2CDxe.c shows
  it writing 0x604 to abort a command and polling 0x610 for completion, which is
  exactly the map used below.

**/

#include "TouchHw.h"
#include <Uefi.h>
#include <PiDxe.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC
VOID
GeniSay (
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
  Make a MMIO range accessible, reporting what the GCD actually holds.

  Three times now this port has assumed a declared range was usable and taken a
  translation fault instead. The declaration in PlatformMemoryMapLib is only an
  AddDev hint; what matters is the GCD descriptor and whether the page tables were
  ever built. So ask the GCD what it has, add the range if it is missing, then set
  the attributes - and print every answer, because "Unsupported" here has more
  than one cause and the numbers tell them apart.

  @param[in] Base  Range start.
  @param[in] Len   Range length.
  @param[in] Name  Label for the log.
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

  if (EFI_ERROR (Status)) {
    GeniSay ("[TP] %a @0x%lx: no descriptor (%r)\r\n", Name, (UINT64)Base, Status);

    Status = gDS->AddMemorySpace (
                   EfiGcdMemoryTypeMemoryMappedIo,
                   Base,
                   Len,
                   EFI_MEMORY_UC
                   );
    GeniSay ("[TP] %a: AddMemorySpace -> %r\r\n", Name, Status);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  } else {
    GeniSay (
      "[TP] %a @0x%lx type=0x%x attr=0x%lx cap=0x%lx\r\n",
      Name,
      (UINT64)Base,
      (UINT32)Desc.GcdMemoryType,
      (UINT64)Desc.Attributes,
      (UINT64)Desc.Capabilities
      );

    if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      Status = gDS->AddMemorySpace (
                     EfiGcdMemoryTypeMemoryMappedIo,
                     Base,
                     Len,
                     EFI_MEMORY_UC
                     );
      GeniSay ("[TP] %a: was non-existent, AddMemorySpace -> %r\r\n", Name, Status);

      //
      // A 4 KB window can fail if the GCD holds a partial descriptor next door -
      // which is exactly this case: I2CDxe mapped [0xA8C000, 0xA90000) for its
      // own engine, and this one starts on the very next page. Fall back to
      // declaring the whole region the platform config already describes.
      //
      if (EFI_ERROR (Status)) {
        Status = gDS->AddMemorySpace (
                       EfiGcdMemoryTypeMemoryMappedIo,
                       QUP_REGION_BASE,
                       QUP_REGION_LEN,
                       EFI_MEMORY_UC
                       );
        GeniSay ("[TP] %a: whole QUP region AddMemorySpace -> %r\r\n", Name, Status);
      }

      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  Status = gDS->SetMemorySpaceAttributes (Base, Len, EFI_MEMORY_UC);
  GeniSay ("[TP] %a: SetMemorySpaceAttributes -> %r\r\n", Name, Status);

  if (EFI_ERROR (Status)) {
    //
    // A range that is already mapped uncached does not need this call to
    // succeed. GCC is in that position: the clock driver has it mapped long
    // before this driver runs, so asking again is a formality that can fail on
    // a length mismatch while the bytes we want are perfectly reachable.
    //
    ZeroMem (&Desc, sizeof (Desc));
    if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor (Base, &Desc))) {
      if ((Desc.Attributes & EFI_MEMORY_UC) != 0) {
        GeniSay ("[TP] %a: already uncached, continuing\r\n", Name);
        return EFI_SUCCESS;
      }
    }
  }

  return Status;
}

/**
  The clock protocol, declared here rather than pulled in.

  EFI_CLOCK_PROTOCOL is published by ClockDxe, which this image contains, and
  using it is how I2CDxe turns on SE3's clock - through the RPMh votes that
  actually decide whether a QUP branch is clocked. Writing the GCC branch and
  root-enable bits by hand sets the registers and still leaves the engine dead,
  which is exactly what the last two boots showed.

  Only the two methods needed are declared, in vtable order, which is all that
  matters for reaching them: Revision at +0x00, GetClockID at +0x08, EnableClock
  at +0x10. The rest of the interface continues with DisableClock and the
  frequency calls.
**/
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
// Not const: LocateProtocol in this core still takes a non-const EFI_GUID *.
//
STATIC EFI_GUID  mClockProtocolGuid = {
  0x241AFAE6, 0x885F, 0x4F6C, { 0xA7, 0xEA, 0xC2, 0x8E, 0xAB, 0x79, 0xC3, 0xE5 }
};

/**
  Resolve a clock name and print what happened, without failing the caller.

  Names are tried in order because the driver's table is not in any file here and
  the vendor kernel uses "gcc_qupv3_wrap1_s4_clk" - the UEFI clock driver may or
  may not use the same spelling. SE3's name is resolved first as a control: if
  that one does not resolve either, the problem is the name convention rather
  than this particular clock.

  @param[in]  Clock  Protocol instance.
  @param[in]  Name   Candidate clock name.
  @param[out] Id     Clock id on success.

  @retval EFI_SUCCESS  Name resolved.
**/
STATIC
EFI_STATUS
TouchResolveClock (
  IN  TOUCH_CLOCK_IF  *Clock,
  IN  CONST CHAR8     *Name,
  OUT UINTN           *Id
  )
{
  EFI_STATUS  Status;

  Status = Clock->GetClockID (Clock, Name, Id);
  GeniSay ("[TP] clock %a -> %r", Name, Status);
  if (!EFI_ERROR (Status)) {
    GeniSay (" id=%u", *Id);
  }

  GeniSay ("\r\n");
  return Status;
}

/**
  Turn on the serial engine's clock.

  I2CDxe enables its own SE clock before touching the engine, so SE clocks are
  gated coming out of reset and there is no reason to expect SE4's to be any
  different. Reading a clock-gated block is exactly the kind of access that can
  fail to complete, which is why this runs before the register dump rather than
  after it discovers everything reads zero.

  The protocol is tried first and the raw GCC pokes are kept afterwards as a
  fallback and as a readback: the branch and root-enable bits can be made to read
  back as set while the engine stays dead, so a set bit proves nothing on its own.

  @retval EFI_SUCCESS  Some path reported the clock enabled.
  @retval EFI_TIMEOUT  Nothing did.
**/
STATIC
EFI_STATUS
TouchEnableSeClock (
  VOID
  )
{
  EFI_STATUS      Status;
  UINT32          Cbcr;
  UINT32          Cmd;
  UINT32          Halt;
  UINTN           Poll;
  UINTN           ClockId;
  TOUCH_CLOCK_IF  *Clock;

  ClockId = 0;

  //
  // The architected path first. ClockDxe owns the RPMh votes that actually
  // ungate a QUP branch; the register writes below only change what the branch
  // register reports.
  //
  Clock = NULL;
  Status = gBS->LocateProtocol (&mClockProtocolGuid, NULL, (VOID **)&Clock);
  GeniSay ("[TP] clock protocol -> %r\r\n", Status);

  if (!EFI_ERROR (Status) && (Clock != NULL)) {
    //
    // SE3 first: I2C runs on it, so its name is a known-good reference for the
    // naming convention. If this one fails too, the spelling is wrong rather
    // than the clock being absent.
    //
    (VOID)TouchResolveClock (Clock, "gcc_qupv3_wrap1_s3_clk", &ClockId);

    if (!EFI_ERROR (TouchResolveClock (Clock, "gcc_qupv3_wrap1_s4_clk", &ClockId))) {
      Status = Clock->EnableClock (Clock, ClockId);
      GeniSay ("[TP] enable gcc_qupv3_wrap1_s4_clk -> %r\r\n", Status);
    } else if (!EFI_ERROR (TouchResolveClock (Clock, "gcc_qupv3_wrap1_s4_clk_src", &ClockId))) {
      Status = Clock->EnableClock (Clock, ClockId);
      GeniSay ("[TP] enable gcc_qupv3_wrap1_s4_clk_src -> %r\r\n", Status);
    } else {
      GeniSay ("[TP] no clock name resolved for SE4\r\n");
    }
  }

  Status = TouchMapMmio (GCC_CBCR_WIN, GCC_CBCR_WIN_LEN, "GCC cbcr");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = TouchMapMmio (GCC_RCG_WIN, GCC_RCG_WIN_LEN, "GCC rcg");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Cbcr = MmioRead32 (GCC_BASE + GCC_QUPV3_WRAP1_CBCR);
  Cmd  = MmioRead32 (GCC_BASE + GCC_QUPV3_WRAP1_S4_RCG + GCC_RCG_CMD);
  GeniSay (
    "[TP] GCC cbcr=%08x (s4 en=%u s3 en=%u) s4 rcg cmd=%08x\r\n",
    Cbcr,
    (Cbcr & GCC_QUPV3_WRAP1_S4_EN) != 0,
    (Cbcr & GCC_QUPV3_WRAP1_S3_EN) != 0,
    Cmd
    );

  //
  // Configure the RCG first, then root-enable it, then open the branch.
  //
  // Source 0 is P_BI_TCXO and the divider field must carry the table's pre_div,
  // which is 1 for the 19.2 MHz entry. Writing 0 there is not "no division", it
  // is a setting the table never uses, and the clock does not come out - that is
  // what left the engine answering with a bus error instead of a register value.
  //
  // The D register only matters for the fractional M/N/D modes; this is integer
  // mode, so it is left alone.
  //
  MmioWrite32 (
    GCC_BASE + GCC_QUPV3_WRAP1_S4_RCG + GCC_RCG_CFG,
    GCC_RCG_SRC_DIV (1)
    );

  Cmd = MmioRead32 (GCC_BASE + GCC_QUPV3_WRAP1_S4_RCG + GCC_RCG_CMD);
  if ((Cmd & GCC_RCG_ROOT_EN) == 0) {
    MmioWrite32 (
      GCC_BASE + GCC_QUPV3_WRAP1_S4_RCG + GCC_RCG_CMD,
      Cmd | GCC_RCG_ROOT_EN
      );
    MicroSecondDelay (100);
  }

  if ((Cbcr & GCC_QUPV3_WRAP1_S4_EN) == 0) {
    MmioWrite32 (GCC_BASE + GCC_QUPV3_WRAP1_CBCR, Cbcr | GCC_QUPV3_WRAP1_S4_EN);
    MicroSecondDelay (100);
  }

  //
  // BRANCH_CLK_OFF is BIT(31) of the halt register in the downstream driver.
  // A successful enable clears it.
  //
  Halt = MmioRead32 (GCC_BASE + GCC_QUPV3_WRAP1_S4_HALT);
  for (Poll = 0; (Poll < 100) && ((Halt & GCC_QUPV3_WRAP1_S4_CLKOFF) != 0); Poll++) {
    MicroSecondDelay (100);
    Halt = MmioRead32 (GCC_BASE + GCC_QUPV3_WRAP1_S4_HALT);
  }

  Cbcr = MmioRead32 (GCC_BASE + GCC_QUPV3_WRAP1_CBCR);
  GeniSay (
    "[TP] GCC after: cbcr=%08x s4 en=%u rcg cmd=%08x cfg=%08x halt=%08x clkoff=%u\r\n",
    Cbcr,
    (Cbcr & GCC_QUPV3_WRAP1_S4_EN) != 0,
    MmioRead32 (GCC_BASE + GCC_QUPV3_WRAP1_S4_RCG + GCC_RCG_CMD),
    MmioRead32 (GCC_BASE + GCC_QUPV3_WRAP1_S4_RCG + GCC_RCG_CFG),
    Halt,
    (Halt & GCC_QUPV3_WRAP1_S4_CLKOFF) != 0
    );

  if ((Cbcr & GCC_QUPV3_WRAP1_S4_EN) == 0) {
    return EFI_TIMEOUT;
  }

  if ((Halt & GCC_QUPV3_WRAP1_S4_CLKOFF) != 0) {
    return EFI_NOT_READY;
  }

  return EFI_SUCCESS;
}

/**
  Make the single page that holds one pin accessible, and say what the GCD said.

  Touching TLMM beyond its first page reset the SoC, which is a translation
  fault with no exception handler worth the name. The reason a whole-range call
  was not enough is that EDK2 returns EFI_SUCCESS early when the requested
  attributes already equal the descriptor's - and PlatformMemoryMapLib's AddDev
  already stamps these ranges uncached. The descriptor therefore claims the
  whole of TLMM is reachable while the page tables only cover what somebody
  actually mapped, and the attribute call quietly does nothing.

  So the check is per page, and when the attributes already match a different
  value is written first to force a real change, then the wanted value. That is
  the only way to make the core build page tables for a range it believes is
  already correct.

  @param[in] Pin  Pin whose page must be reachable.

  @retval EFI_SUCCESS  The page is mapped uncached and safe to touch.
**/
EFI_STATUS
TouchPinMap (
  IN UINT32  Pin
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  UINTN                            Addr;
  UINTN                            Page;

  Addr = TLMM_BASE + TLMM_GPIO_CFG (Pin);
  Page = Addr & ~(UINTN)(TLMM_PAGE - 1);

  ZeroMem (&Desc, sizeof (Desc));
  Status = gDS->GetMemorySpaceDescriptor (Page, &Desc);

  if (EFI_ERROR (Status) || (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (
                   EfiGcdMemoryTypeMemoryMappedIo,
                   Page,
                   TLMM_PAGE,
                   EFI_MEMORY_UC
                   );
    GeniSay ("[TP] gpio%u page 0x%lx add -> %r\r\n", Pin, (UINT64)Page, Status);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  } else if ((Desc.Attributes & EFI_MEMORY_UC) != 0) {
    //
    // Already uncached per the GCD. Writing the same value back would be a
    // no-op, so go via write-combining to force the page tables to be rebuilt.
    //
    (VOID)gDS->SetMemorySpaceAttributes (Page, TLMM_PAGE, EFI_MEMORY_WC);
  }

  Status = gDS->SetMemorySpaceAttributes (Page, TLMM_PAGE, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    GeniSay ("[TP] gpio%u page 0x%lx attr -> %r\r\n", Pin, (UINT64)Page, Status);
  }

  return Status;
}

/**
  Report what the pin controller currently has for the six touch pins.

  Read-only on purpose. Writing a mux needs the TLMM function number for
  "qup1_se4", which is not in any file this port has, so the useful thing to
  learn first is whether the pins are already muxed - if the function field is
  non-zero, someone before us (ABL, or the previous Android boot surviving a warm
  reset) already did it and the number is right there in the dump.

  Also prints the first words of the TLMM block. If those read as 0 or all ones
  while the SE reads sanely, the base address is wrong; if both are dead, the
  problem is above both of them.
**/
STATIC
VOID
TouchDumpPins (
  VOID
  )
{
  STATIC CONST UINT32  Pins[] = {
    TOUCH_SPI_PIN_0,
    TOUCH_SPI_PIN_1,
    TOUCH_SPI_PIN_2,
    TOUCH_SPI_PIN_3,
    TOUCH_RESET_PIN,
    TOUCH_IRQ_PIN
  };
  UINTN   Index;
  UINT32  Pin;
  UINT32  Cfg;

  GeniSay (
    "[TP] TLMM base 0x%lx: %08x %08x %08x %08x\r\n",
    (UINT64)TLMM_BASE,
    MmioRead32 (TLMM_BASE + 0x00),
    MmioRead32 (TLMM_BASE + 0x04),
    MmioRead32 (TLMM_BASE + 0x08),
    MmioRead32 (TLMM_BASE + 0x0C)
    );

  for (Index = 0; Index < ARRAY_SIZE (Pins); Index++) {
    Pin = Pins[Index];

    //
    // Print before touching. A fault here resets the machine, and a log line
    // naming the address is the only way to know which page was at fault.
    //
    GeniSay ("[TP]   gpio%u @0x%lx\r\n", Pin, (UINT64)(TLMM_BASE + TLMM_GPIO_CFG (Pin)));

    if (EFI_ERROR (TouchPinMap (Pin))) {
      continue;
    }

    Cfg = MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (Pin));
    GeniSay (
      "[TP]     cfg=%08x mux=%u drive=%u pull=%u io=%08x\r\n",
      Cfg,
      (Cfg >> 2) & 0xF,
      (Cfg >> 6) & 0x7,
      Cfg & 0x3,
      MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (Pin))
      );
  }
}

/**
  Mux the four SPI pins to their qup1_se4 functions.

  The device tree active config puts gpio48..51 on qup1_se4_l0..l3 with 6 mA
  drive and a pull-down on every line. Function 1 is the qup1_se4_l<N> mux for
  all four pins on pineapple, so a single function value applies to the whole
  set. Without this the SE clocks its signals into GPIO inputs and the touch
  controller never hears a word.
**/
STATIC
VOID
TouchMuxSpiPins (
  VOID
  )
{
  STATIC CONST UINT32  Pins[] = {
    TOUCH_SPI_PIN_0,
    TOUCH_SPI_PIN_1,
    TOUCH_SPI_PIN_2,
    TOUCH_SPI_PIN_3
  };
  UINTN   Index;
  UINT32  Config;

  Config = TLMM_CFG_FUNC_QUP1_SE4 | TLMM_CFG_DRV_6MA | TLMM_CFG_PULL_DOWN;

  for (Index = 0; Index < ARRAY_SIZE (Pins); Index++) {
    GeniSay ("[TP] mux gpio%u @0x%lx\r\n", Pins[Index], (UINT64)(TLMM_BASE + TLMM_GPIO_CFG (Pins[Index])));

    if (EFI_ERROR (TouchPinMap (Pins[Index]))) {
      continue;
    }

    MmioWrite32 (TLMM_BASE + TLMM_GPIO_CFG (Pins[Index]), Config);
    GeniSay (
      "[TP]   cfg=%08x io=%08x\r\n",
      MmioRead32 (TLMM_BASE + TLMM_GPIO_CFG (Pins[Index])),
      MmioRead32 (TLMM_BASE + TLMM_GPIO_IN_OUT (Pins[Index]))
      );
  }
}

/**
  Read the FIFO depths out of the hardware parameter registers.

  TX depth is bits [21:16], or [23:16] on QUP 3.10 and later where it grew to 256
  entries. The version is in SE_HW_PARAM_0 bits [31:28] major / [27:16] minor, so
  pick the mask the same way the kernel does instead of assuming.
**/
STATIC
UINT32
GeniFifoDepth (
  IN BOOLEAN  Tx
  )
{
  UINT32  Version;
  UINT32  Param;
  UINT32  Major;
  UINT32  Minor;
  UINT32  Mask;

  Version = MmioRead32 (TOUCH_SE_BASE + SE_HW_PARAM_0);
  Major   = (Version >> 28) & 0xF;
  Minor   = (Version >> 16) & 0xFFF;

  Param = MmioRead32 (TOUCH_SE_BASE + (Tx ? SE_HW_PARAM_0 : SE_HW_PARAM_1));

  if ((Major > 3) || ((Major == 3) && (Minor >= 10))) {
    Mask = 0x00FF0000U;
  } else {
    Mask = 0x003F0000U;
  }

  return (Param & Mask) >> 16;
}

/**
  Program the 8-bit, MSB-first packing that lets one byte occupy one FIFO word.

  geni_se_config_packing() with bpw = 8 and pack_words = 1 produces a single
  vector: start bit 7, direction MSB-to-LSB, length 7, stop bit set - which is
  0xFF - and writes 0 to SE_GENI_BYTE_GRAN because bpw / 16 is zero. With that in
  place a FIFO write carries exactly one byte, so no shifting is needed here.
**/
STATIC
VOID
GeniSetupPacking (
  VOID
  )
{
  //
  // start = 7 (<<5), dir = 1 (<<4), len = 8-1 (<<1), stop = 1
  //
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_TX_PACKING_CFG0, 0x000000FF);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_TX_PACKING_CFG1, 0x00000000);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_RX_PACKING_CFG0, 0x000000FF);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_RX_PACKING_CFG1, 0x00000000);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_BYTE_GRAN, 0x00000000);
}

/**
  One register to read for the log.
**/
typedef struct {
  UINT32       Offset;
  CONST CHAR8  *Name;
} GENI_REG_DUMP;

/**
  Read a named list of registers.

  This used to walk a raw address range, which was a mistake: the GENI block does
  not decode every offset, and reading an unimplemented one is a synchronous
  external abort - EC 0x25, ISS 0x10, FAR 0xA9000C - not a zero. That killed the
  boot four bytes into the first dump. Only offsets that exist in geni-se.h or
  spi-geni-qcom.c are read now.

  @param[in] Label  Heading for the group.
  @param[in] Regs   Registers to read.
  @param[in] Count  How many.
**/
STATIC
VOID
GeniDumpRegs (
  IN CONST CHAR8         *Label,
  IN CONST GENI_REG_DUMP *Regs,
  IN UINTN               Count
  )
{
  UINTN  Index;

  GeniSay ("[TP] %a\r\n", Label);
  for (Index = 0; Index < Count; Index++) {
    GeniSay (
      "[TP]   %03x %-14a = %08x\r\n",
      Regs[Index].Offset,
      Regs[Index].Name,
      MmioRead32 (TOUCH_SE_BASE + Regs[Index].Offset)
      );
  }
}

STATIC CONST GENI_REG_DUMP  mCoreRegs[] = {
  { SE_GENI_FORCE_DEFAULT, "FORCE_DEFAULT" },
  { SE_GENI_OUTPUT_CTRL,   "OUTPUT_CTRL"   },
  { SE_GENI_CGC_CTRL,      "CGC_CTRL"      },
  { SE_GENI_STATUS,        "GENI_STATUS"   },
  { 0x060,                 "CLK_CTRL_RO"   },
  { 0x064,                 "IF_DISABLE_RO" },
  { GENI_FW_REVISION_RO,   "FW_REVISION"   },
  { 0x06C,                 "FW_S_REV"      },
  { 0x07C,                 "CLK_SEL"       },
  { 0x084,                 "CFG_SEQ_START" },
  { SE_GENI_BYTE_GRAN,     "BYTE_GRAN"     },
  { SE_GENI_DMA_MODE_EN,   "DMA_MODE_EN"   }
};

STATIC CONST GENI_REG_DUMP  mCmdRegs[] = {
  { SE_GENI_M_CMD0,         "M_CMD0"       },
  { SE_GENI_M_CMD_CTRL,     "M_CMD_CTRL"   },
  { SE_GENI_M_IRQ_STATUS,   "M_IRQ_STATUS" },
  { SE_GENI_M_IRQ_EN,       "M_IRQ_EN"     },
  { SE_GENI_M_IRQ_CLEAR,    "M_IRQ_CLEAR"  },
  { SE_GENI_TX_FIFO_STATUS, "TX_FIFO_STAT" },
  { SE_GENI_RX_FIFO_STATUS, "RX_FIFO_STAT" },
  { SE_GENI_IOS,            "IOS"          },
  { SE_HW_PARAM_0,          "HW_PARAM_0"   },
  { SE_HW_PARAM_1,          "HW_PARAM_1"   }
};

STATIC CONST GENI_REG_DUMP  mSpiRegs[] = {
  { SE_SPI_CPHA,             "CPHA"          },
  { SE_SPI_LOOPBACK,         "LOOPBACK"      },
  { SE_SPI_CPOL,             "CPOL"          },
  { SE_SPI_DEMUX_OUTPUT_INV, "DEMUX_OUT_INV" },
  { SE_SPI_DEMUX_SEL,        "DEMUX_SEL"     },
  { SE_SPI_TRANS_CFG,        "TRANS_CFG"     },
  { SE_SPI_WORD_LEN,         "WORD_LEN"      },
  { SE_SPI_TX_TRANS_LEN,     "TX_TRANS_LEN"  },
  { SE_SPI_RX_TRANS_LEN,     "RX_TRANS_LEN"  },
  { SE_SPI_DELAY_COUNTERS,   "DELAY_COUNTERS"}
};

/**
  Read the same registers from the I2C engine as a control.

  SE4 aborts on every register access while SE3, one engine down, is driven
  successfully by I2CDxe. Without a control there is no way to tell whether the
  driver is reading the wrong addresses or whether this one engine has no clock,
  and the difference decides the whole next step.

  The address is printed before each read, because a failed access here is a
  synchronous external abort that resets the SoC - the last line on screen is the
  only record of how far it got.

  @param[in] Label  Heading, so the two blocks cannot be confused in the log.
  @param[in] Base   Engine to read.
**/
STATIC
VOID
GeniProbeEngine (
  IN CONST CHAR8  *Label,
  IN UINTN        Base
  )
{
  STATIC CONST UINT32  Offsets[] = { 0x020, 0x024, 0x040, 0x064, 0x068, 0x258 };
  UINTN                Index;

  GeniSay ("[TP] probe %a @0x%lx\r\n", Label, (UINT64)Base);

  for (Index = 0; Index < ARRAY_SIZE (Offsets); Index++) {
    GeniSay ("[TP]   %a +0x%03x = ", Label, Offsets[Index]);
    GeniSay ("%08x\r\n", MmioRead32 (Base + Offsets[Index]));
  }
}

/**
  Bring the serial engine up and configure it for SPI mode 0.

  Prints the register block before and after, plus the protocol code the hardware
  reports, so that "clock off", "wrong offset" and "wrong engine" are three
  distinguishable outcomes instead of one silent failure.
**/
EFI_STATUS
GeniSpiInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Proto;
  UINT32      Value;
  UINT32      TxDepth;

  //
  // Clock first. A failure here is reported but not fatal: the register dump
  // below is what proves whether the engine answers, and seeing a dead dump is
  // more informative than returning early with no evidence.
  //
  if (EFI_ERROR (TouchEnableSeClock ())) {
    GeniSay ("[TP] SE clock did not enable - dump below is expected to read 0\r\n");
  }

  //
  // Control first, then the pins, and only then the engine under test.
  //
  // SE4 aborts on every access, and that abort resets the SoC: everything after
  // it in this function is lost. So the two things that do not depend on it -
  // proving the read method against the working I2C engine, and getting the pins
  // muxed - both run before anything touches 0xA90000.
  //
  Status = TouchMapMmio (I2C_SE_BASE, I2C_SE_LEN, "SE3");
  if (EFI_ERROR (Status)) {
    GeniSay ("[TP] control engine not mapped: %r\r\n", Status);
  } else {
    GeniProbeEngine ("ctl", I2C_SE_BASE);
  }

  Status = TouchMapMmio (TLMM_BASE, TLMM_MAP_LEN, "TLMM");
  if (EFI_ERROR (Status)) {
    GeniSay ("[TP] TLMM not mapped, skipping pin dump\r\n");
  } else {
    TouchDumpPins ();
    TouchMuxSpiPins ();
  }

  //
  // Now the engine under test. Reading 0xA9000C took a translation fault at level
  // 3 on the first attempt because the GCD did not hold the QUP range at all.
  // HWIO was tried as an alternative and is gone again: its BaseMap methods
  // return EFI_PROTOCOL_ERROR and one of them hangs the boot outright.
  //
  Status = TouchMapMmio (TOUCH_SE_BASE, TOUCH_SE_LEN, "SE");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  GeniProbeEngine ("spi", TOUCH_SE_BASE);
  GeniDumpRegs ("SE regs before:", mCoreRegs, ARRAY_SIZE (mCoreRegs));

  //
  // A dead engine reads as all zero or all ones. Neither is a state this block
  // can legitimately be in after reset, so treat it as "clock is off" and say so
  // rather than programming registers that will not take.
  //
  //
  // Is the engine alive? Ask FW_REVISION, not GENI_STATUS.
  //
  // GENI_STATUS reads 0 on a healthy idle engine, and SE3 - the I2C engine this
  // port already drives - reports 0 there too. Testing it for zero is what has
  // been stopping this driver: even with the clock running and the base address
  // right, it declared the engine dead before programming a single register.
  //
  // FW_REVISION is the honest test. An engine that is unclocked, unmapped, or
  // absent reads 0 or 0xFFFFFFFF; a real one reports its firmware version. This
  // device reports 0x102.
  //
  Value = MmioRead32 (TOUCH_SE_BASE + GENI_FW_REVISION_RO);
  if ((Value == 0x00000000) || (Value == 0xFFFFFFFF)) {
    GeniSay ("[TP] SE looks dead (FW_REVISION=%08x) - clock likely off\r\n", Value);
    return EFI_DEVICE_ERROR;
  }

  //
  // Which protocol is this engine wired for? SPI must read 1. If it reads 3 the
  // base address landed on the I2C engine; anything else means the offset is
  // wrong. This one number replaces a lot of guessing.
  //
  Proto = MmioRead32 (TOUCH_SE_BASE + GENI_FW_REVISION_RO);
  GeniSay (
    "[TP] fw_rev=%08x proto=%u (want %u for SPI)\r\n",
    Proto,
    (Proto & GENI_PROTO_MASK) >> GENI_PROTO_SHIFT,
    GENI_PROTO_SPI
    );

  //
  // Pins are handled at the very end of this function, not here.
  //
  // Touching a TLMM page whose page tables were never built takes a translation
  // fault, and on this platform that is a reset, not a message - the whole log
  // is lost with it. Everything above is the evidence that actually matters, so
  // it has to be on the console before anything can reboot the SoC.
  //

  //
  // geni_se_io_init(): clock gating on, outputs on, then force defaults.
  //
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_CGC_CTRL, 0x0000007F);
  MmioWrite32 (TOUCH_SE_BASE + SE_DMA_GENERAL_CFG, 0x0000000F);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_OUTPUT_CTRL, 0x0000007F);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_FORCE_DEFAULT, 0x00000001);
  MicroSecondDelay (10);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_FORCE_DEFAULT, 0x00000000);
  MicroSecondDelay (10);

  //
  // geni_se_irq_clear(): drop every pending event, and leave the line to the GIC
  // masked. Completion is polled in M_IRQ_STATUS, which latches regardless.
  //
  MmioWrite32 (TOUCH_SE_BASE + SE_GSI_EVENT_EN, 0);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFF);
  MmioWrite32 (TOUCH_SE_BASE + SE_DMA_TX_IRQ_CLR, 0xFFFFFFFF);
  MmioWrite32 (TOUCH_SE_BASE + SE_DMA_RX_IRQ_CLR, 0xFFFFFFFF);
  MmioWrite32 (TOUCH_SE_BASE + SE_IRQ_EN, 0);

  //
  // Leave DMA_MODE_EN exactly as the hardware set it, and say what that was.
  //
  // This used to force 0 - FIFO mode - on the reasoning that FIFO needs no
  // descriptors and no IOMMU. The engine disagrees: GENI_IF_DISABLE_RO (0x64) bit 0
  // is FIFO_IF_DISABLE, and it reads 1 here. SE3 at 0xA8C000, the I2C engine this
  // port drives successfully, reads 0 there and runs in FIFO mode.
  //
  // The symptom matches. With FIFO forced, the first command never reports done:
  // CS_ASSERT is issued, CS_DEASSERT comes back as M_CMD_OVERRUN because a command
  // is still active, and SPI_WRITE then waits forever with GENI_STATUS bit 0 set.
  //
  // So this is the experiment, not a fix: leave the mode alone and see whether CS
  // completes. If it does, FIFO was the blocker and the data path needs a TRE ring.
  // If it still stalls, the mode was not the cause and this line goes back.
  //
  GeniSay (
    "[TP] DMA_MODE_EN=%08x FIFO_IF_DISABLE=%08x - leaving as is\r\n",
    MmioRead32 (TOUCH_SE_BASE + SE_GENI_DMA_MODE_EN),
    MmioRead32 (TOUCH_SE_BASE + 0x064)
    );
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_EN, M_CMD_DONE);

  GeniSetupPacking ();

  //
  // SPI mode 0: CPOL = 0, CPHA = 0, no loopback, chip select 0. WORD_LEN is
  // (bits_per_word - 4), not (bits_per_word - 1).
  //
  MmioWrite32 (TOUCH_SE_BASE + SE_SPI_LOOPBACK, 0);
  MmioWrite32 (TOUCH_SE_BASE + SE_SPI_DEMUX_SEL, 0);
  MmioWrite32 (TOUCH_SE_BASE + SE_SPI_CPHA, 0);
  MmioWrite32 (TOUCH_SE_BASE + SE_SPI_CPOL, 0);
  MmioWrite32 (TOUCH_SE_BASE + SE_SPI_DEMUX_OUTPUT_INV, 0);
  MmioWrite32 (TOUCH_SE_BASE + SE_SPI_WORD_LEN, 8 - 4);

  TxDepth = GeniFifoDepth (TRUE);
  GeniSay (
    "[TP] SE up, tx fifo depth=%u rx fifo depth=%u\r\n",
    TxDepth,
    GeniFifoDepth (FALSE)
    );

  GeniDumpRegs ("SE regs after:", mCoreRegs, ARRAY_SIZE (mCoreRegs));
  GeniDumpRegs ("CMD regs:", mCmdRegs, ARRAY_SIZE (mCmdRegs));
  GeniDumpRegs ("SPI regs:", mSpiRegs, ARRAY_SIZE (mSpiRegs));

  return EFI_SUCCESS;
}

/**
  Wait for the command in M_CMD0 to complete.

  M_IRQ_STATUS bit 0 is M_CMD_DONE. The other error bits are reported because
  they say why a transfer failed, and "failure" and "overrun" mean different
  things: the first is the controller rejecting the command, the second is the
  FIFO not keeping up.

  @param[out] LastIrq  Final value of M_IRQ_STATUS.
  @param[out] Elapsed   Loop count consumed, for the log.

  @retval TRUE   Command completed.
  @retval FALSE  Timed out.
**/
STATIC
BOOLEAN
GeniWaitDone (
  OUT UINT32  *LastIrq,
  OUT UINT32  *Elapsed
  )
{
  UINT32  Iteration;
  UINT32  Irq;

  for (Iteration = 0; Iteration < TCM_POLL_ITERATIONS; Iteration++) {
    Irq = MmioRead32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_STATUS);
    if ((Irq & (M_CMD_DONE | M_CMD_FAILURE | M_ILLEGAL_CMD | M_CMD_OVERRUN)) != 0) {
      *LastIrq = Irq;
      *Elapsed = Iteration;
      return (Irq & M_CMD_DONE) != 0;
    }
    MicroSecondDelay (TCM_POLL_DELAY_US);
  }

  *LastIrq = MmioRead32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_STATUS);
  *Elapsed = Iteration;
  return FALSE;
}

STATIC
VOID
GeniIssueCommand (
  IN UINT32  Opcode
  )
{
  MmioWrite32 (
    TOUCH_SE_BASE + SE_GENI_M_CMD0,
    (Opcode << M_OPCODE_SHIFT) | GENI_M_PARAM_FRAGMENTATION
    );
}

/**
  Drive the GENI chip select. spi_geni_set_cs() issues these as standalone
  commands with an empty parameter field, so a transfer without them never
  reaches the controller - the CS line simply stays deasserted.
**/
STATIC
EFI_STATUS
GeniCsControl (
  IN UINT32  Opcode
  )
{
  UINT32   Irq;
  UINT32   Elapsed;

  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_CMD0, (Opcode << M_OPCODE_SHIFT));

  if (!GeniWaitDone (&Irq, &Elapsed)) {
    GeniSay ("[TP] cs op %u failed: irq=%08x status=%08x\r\n", Opcode, Irq, MmioRead32 (TOUCH_SE_BASE + SE_GENI_STATUS));
    MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    return EFI_DEVICE_ERROR;
  }

  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  return EFI_SUCCESS;
}

/**
  Shift bytes out, ignoring whatever comes back.

  Order matters and is not obvious: the command is issued first and the FIFO
  filled afterwards. That is what spi_geni_transfer_one() does - the sequencer
  stalls on an empty TX FIFO until data arrives, so filling first would work too,
  but issuing first is what the hardware is documented against.

  @param[in] TxBuffer  Bytes to send.
  @param[in] Length    Byte count.

  @retval EFI_SUCCESS       Transfer completed.
  @retval EFI_DEVICE_ERROR  The command never reported done.
**/
EFI_STATUS
GeniSpiWrite (
  IN CONST UINT8  *TxBuffer,
  IN UINT32       Length
  )
{
  UINT32   Index;
  UINT32   Irq;
  UINT32   Elapsed;
  UINT32   Depth;
  BOOLEAN  Done;

  if ((Length == 0) || (Length > TOUCH_SPI_MAX_XFER)) {
    return EFI_INVALID_PARAMETER;
  }

  if (EFI_ERROR (GeniCsControl (SPI_CS_ASSERT))) {
    return EFI_DEVICE_ERROR;
  }

  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  MmioWrite32 (TOUCH_SE_BASE + SE_SPI_TX_TRANS_LEN, Length);
  GeniIssueCommand (SPI_TX_ONLY);

  Depth = GeniFifoDepth (TRUE);
  if (Depth == 0) {
    //
    // A zero depth would make the wait below spin forever, so fall back to the
    // smallest depth this IP is built with rather than trusting a register that
    // may not have been read correctly.
    //
    Depth = 16;
  }

  for (Index = 0; Index < Length; Index++) {
    UINT32  Spins;

    //
    // Do not overrun the FIFO: TX_FIFO_STATUS bits [27:0] are the current word
    // count, and a write past the depth is dropped silently. The spin cap keeps
    // a stalled engine from hanging the boot inside a loop that never exits.
    //
    for (Spins = 0; Spins < 10000; Spins++) {
      if ((MmioRead32 (TOUCH_SE_BASE + SE_GENI_TX_FIFO_STATUS) & 0x0FFFFFFF) < Depth) {
        break;
      }
      MicroSecondDelay (TCM_POLL_DELAY_US);
    }

    MmioWrite32 (TOUCH_SE_BASE + SE_GENI_TX_FIFO, TxBuffer[Index]);
  }

  Done = GeniWaitDone (&Irq, &Elapsed);
  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  (VOID)GeniCsControl (SPI_CS_DEASSERT);

  if (!Done) {
    GeniSay (
      "[TP] tx %u failed: irq=%08x after %u polls, status=%08x cmd0=%08x\r\n",
      Length,
      Irq,
      Elapsed,
      MmioRead32 (TOUCH_SE_BASE + SE_GENI_STATUS),
      MmioRead32 (TOUCH_SE_BASE + SE_GENI_M_CMD0)
      );
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Shift bytes in. The controller ignores what it sends on MOSI during a read, so
  there is no dummy pattern to choose.

  @param[out] RxBuffer  Bytes received.
  @param[in]  Length    Byte count.

  @retval EFI_SUCCESS       Transfer completed.
  @retval EFI_DEVICE_ERROR  The command never reported done.
**/
EFI_STATUS
GeniSpiRead (
  OUT UINT8  *RxBuffer,
  IN  UINT32 Length
  )
{
  UINT32   Index;
  UINT32   Irq;
  UINT32   Elapsed;
  BOOLEAN  Done;

  if ((Length == 0) || (Length > TOUCH_SPI_MAX_XFER)) {
    return EFI_INVALID_PARAMETER;
  }

  if (EFI_ERROR (GeniCsControl (SPI_CS_ASSERT))) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Drain anything stale so the receive side starts clean.
  //
  while ((MmioRead32 (TOUCH_SE_BASE + SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFF) != 0) {
    (VOID)MmioRead32 (TOUCH_SE_BASE + SE_GENI_RX_FIFO);
  }

  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  MmioWrite32 (TOUCH_SE_BASE + SE_SPI_RX_TRANS_LEN, Length);
  GeniIssueCommand (SPI_RX_ONLY);

  Done = GeniWaitDone (&Irq, &Elapsed);
  if (!Done) {
    GeniSay (
      "[TP] rx %u failed: irq=%08x after %u polls, status=%08x cmd0=%08x\r\n",
      Length,
      Irq,
      Elapsed,
      MmioRead32 (TOUCH_SE_BASE + SE_GENI_STATUS),
      MmioRead32 (TOUCH_SE_BASE + SE_GENI_M_CMD0)
      );
    MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    (VOID)GeniCsControl (SPI_CS_DEASSERT);
    return EFI_DEVICE_ERROR;
  }

  for (Index = 0; Index < Length; Index++) {
    RxBuffer[Index] = (UINT8)MmioRead32 (TOUCH_SE_BASE + SE_GENI_RX_FIFO);
  }

  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  (VOID)GeniCsControl (SPI_CS_DEASSERT);
  return EFI_SUCCESS;
}

/**
  Move whatever the receive FIFO currently holds into the caller's buffer.

  Needed during a transfer, not only after it: the RX FIFO is shallow (tens of
  entries, not the 258 bytes a TCM read asks for), and once it fills the
  sequencer stops making progress, so waiting for M_CMD_DONE without draining
  would simply time out. Bytes beyond RxLen are read and discarded so they cannot
  corrupt the next transfer.

  @param[out]    RxBuffer  Destination, may be filled partially.
  @param[in,out] Got       Bytes stored so far; advanced by this call.
  @param[in]     RxLen     Capacity of RxBuffer.
**/
STATIC
VOID
GeniDrainRx (
  OUT UINT8       *RxBuffer,
  IN OUT UINT32   *Got,
  IN     UINT32   RxLen
  )
{
  UINT32  Words;

  Words = MmioRead32 (TOUCH_SE_BASE + SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFF;
  while (Words-- != 0) {
    UINT8  Byte;

    Byte = (UINT8)MmioRead32 (TOUCH_SE_BASE + SE_GENI_RX_FIFO);
    if (*Got < RxLen) {
      RxBuffer[(*Got)++] = Byte;
    }
  }
}

/**
  Full-duplex transfer: clock out TxLen bytes on MOSI while receiving RxLen
  bytes on MISO. This is the only transfer shape the TCM read can use - the
  controller answers a stream of 0xFF requests with its response, and a
  RX_ONLY command leaves MOSI at the idle level, which is an all-zero request
  the controller does not answer.

  @param[in]  TxBuffer  Bytes to send (usually 0xFF dummies).
  @param[in]  TxLen     Byte count to send.
  @param[out] RxBuffer  Bytes received.
  @param[in]  RxLen     Byte count to receive.

  @retval EFI_SUCCESS       Transfer completed.
  @retval EFI_DEVICE_ERROR  The command never reported done.
**/
EFI_STATUS
GeniSpiWriteRead (
  IN  CONST UINT8  *TxBuffer,
  IN  UINT32       TxLen,
  OUT UINT8        *RxBuffer,
  IN  UINT32       RxLen
  )
{
  UINT32   Index;
  UINT32   Irq;
  UINT32   Depth;
  UINT32   Got;
  UINT32   Poll;
  BOOLEAN  Done;

  if ((TxLen == 0) || (TxLen > TOUCH_SPI_MAX_XFER) ||
      (RxLen == 0) || (RxLen > TOUCH_SPI_MAX_XFER)) {
    GeniSay ("[TP] txrx bad length tx=%u rx=%u\r\n", TxLen, RxLen);
    return EFI_INVALID_PARAMETER;
  }

  if (EFI_ERROR (GeniCsControl (SPI_CS_ASSERT))) {
    return EFI_DEVICE_ERROR;
  }

  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  MmioWrite32 (TOUCH_SE_BASE + SE_SPI_TX_TRANS_LEN, TxLen);
  MmioWrite32 (TOUCH_SE_BASE + SE_SPI_RX_TRANS_LEN, RxLen);
  GeniIssueCommand (SPI_TX_RX);

  Depth = GeniFifoDepth (TRUE);
  if (Depth == 0) {
    Depth = 16;
  }

  Got = 0;

  for (Index = 0; Index < TxLen; Index++) {
    UINT32  Spins;

    for (Spins = 0; Spins < 10000; Spins++) {
      if ((MmioRead32 (TOUCH_SE_BASE + SE_GENI_TX_FIFO_STATUS) & 0x0FFFFFFF) < Depth) {
        break;
      }
      GeniDrainRx (RxBuffer, &Got, RxLen);
      MicroSecondDelay (TCM_POLL_DELAY_US);
    }

    MmioWrite32 (TOUCH_SE_BASE + SE_GENI_TX_FIFO, TxBuffer[Index]);
  }

  //
  // Keep draining while waiting, otherwise a full RX FIFO stalls the sequencer
  // and the command never completes.
  //
  Irq = 0;
  for (Poll = 0; Poll < TCM_POLL_ITERATIONS; Poll++) {
    GeniDrainRx (RxBuffer, &Got, RxLen);

    Irq = MmioRead32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_STATUS);
    if ((Irq & (M_CMD_DONE | M_CMD_FAILURE | M_ILLEGAL_CMD | M_CMD_OVERRUN)) != 0) {
      break;
    }

    MicroSecondDelay (TCM_POLL_DELAY_US);
  }

  Done = (Irq & M_CMD_DONE) != 0;

  //
  // Whatever arrived after the done flag, plus anything left over, comes out now
  // so the next transfer starts on a clean FIFO.
  //
  GeniDrainRx (RxBuffer, &Got, RxLen);
  while ((MmioRead32 (TOUCH_SE_BASE + SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFF) != 0) {
    (VOID)MmioRead32 (TOUCH_SE_BASE + SE_GENI_RX_FIFO);
  }

  MmioWrite32 (TOUCH_SE_BASE + SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  (VOID)GeniCsControl (SPI_CS_DEASSERT);

  if (!Done) {
    GeniSay (
      "[TP] txrx failed: irq=%08x after %u polls, got=%u/%u status=%08x cmd0=%08x\r\n",
      Irq,
      Poll,
      Got,
      RxLen,
      MmioRead32 (TOUCH_SE_BASE + SE_GENI_STATUS),
      MmioRead32 (TOUCH_SE_BASE + SE_GENI_M_CMD0)
      );
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}
