/** @file
  PlatformUsbfnLib for xiaomi-shennong (SM8650).

  The generic DWC3 driver in this directory knows nothing about the SoC it runs on; it
  asks for everything through this one structure. The values below come from the hardware
  rather than from a template:

    ControllerBase        0x0A600000, the DWC3 instance the rest of this port already
                          talks to. GCTL reads 0x00102001 and DSTS responds there, so the
                          address is confirmed on this board.

    MaximumSpeed          High. The USB2 path runs through the PM8550B eUSB2 repeater and
                          there is no USB3 lane wired, so advertising SuperSpeed would ask
                          the PHY for a link that does not exist.

    UtmiWidth             8 bit. GUSB2PHYCFG reads 0x00102500, and bit 3 (PHYIF) is clear,
                          which selects the 8 bit UTMI+ interface.

    Usb2FreeClockExists   TRUE. The eUSB2 PHY is fed a free running clock here, so
                          GUSB2PHYCFG.U2_FREECLK_EXISTS must stay set or frame timing and
                          suspend handling drift.

    FrameLengthAdjust     0 / 0, i.e. leave GFLADJ alone. The reference clock is 19.2 MHz
                          (CNTFRQ_EL0 reports exactly that), and the correct adjustment for
                          it is a separate measurement - better skipped than guessed.

    SuperSpeedPhyDisabled TRUE. No USB3 PHY is brought up, so GUSB3PIPECTL.SUSPHY stays
                          set and the PIPE3 soft reset is skipped.

    NumEndpoints          0, so the driver reads GHWPARAMS3 for the real count.

    VendorId / ProductId  0x05C6 / 0x9008, matching the descriptors this port already
                          presents.

  Copyright (c) 2026. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "Include/Library/PlatformUsbfnLib.h"

#define SHENNONG_DWC3_BASE      0x0A600000ULL
#define SHENNONG_USB_VID        0x05C6
#define SHENNONG_USB_PID        0x9008

//
// GCC, from the device tree: clock-controller@100000, reg = <0x00100000 0x001f4200>.
//
#define SHENNONG_GCC_BASE       0x00100000ULL

//
// Everything this file observes lives in a single 4 KB page of GCC:
//
//   0x39018 usb30_prim_master_cbcr        0x39060 usb3_prim_phy_aux_cbcr
//   0x39024 usb30_prim_sleep_cbcr         0x39064 usb3_prim_phy_com_aux_cbcr
//   0x39028 usb30_prim_mock_utmi_cbcr     0x39068 usb3_prim_phy_pipe_cbcr
//   0x3902c usb30_prim_master_cmd_rcgr    0x39088 cfg_noc_usb3_prim_axi_cbcr
//   0x39030 usb30_prim_master_cfg_rcgr    0x3908c aggre_usb3_prim_axi_cbcr
//
// Mapping the whole block failed: the device tree gives GCC a length of 0x1F4200 and
// SetMemorySpaceAttributes rejects anything that is not a multiple of 4 KB, which is what
// "RegionStart: 0x100000 or RegionLength: 0x1F4200 are not page aligned" was. Only this
// page is needed, and it is aligned by construction.
//
#define SHENNONG_GCC_OBS_BASE   0x00139000ULL
#define SHENNONG_GCC_OBS_SIZE   0x00001000ULL

//
// EFI_CLOCK_PROTOCOL, published by ClockDxe.
//
// Declared locally with only the two methods needed, in vtable order - Revision at +0x00,
// GetClockID at +0x08, EnableClock at +0x10 - because the protocol header in this tree is
// a partial reverse-engineered one. This is the same interface the touch driver uses to
// bring up a QUP serial engine, and going through ClockDxe is what makes the RPMh vote
// happen; writing the GCC branch and root-enable bits by hand sets the registers and still
// leaves the block dead.
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
} USB_CLOCK_IF;

//
// Not const: LocateProtocol in this core still takes a non-const EFI_GUID *.
//
extern EFI_STATUS
RpmhUsbPhyRailsOn (
  VOID
  );

extern EFI_STATUS
RpmhUsbRepeaterRailsOn (
  VOID
  );

STATIC EFI_GUID  mUsbClockProtocolGuid = {
  0x241AFAE6, 0x885F, 0x4F6C, { 0xA7, 0xEA, 0xC2, 0x8E, 0xAB, 0x79, 0xC3, 0xE5 }
};

//
// EFI_NPA_PROTOCOL, published by NpaDxe. Qualcomm's stock UsbConfigDxe uses
// this exact protocol to create the USB interconnect client and to vote the
// aggre1_noc USB master path before the DWC3 DMA engine is allowed to run.
//
// The two methods used here are at +0x10 and +0x90. Unlike EFI protocols that
// pass the protocol object as This, UsbConfigDxe calls both NPA entries with
// the resource path/node as x0. Mirror that ABI exactly.
//
#ifndef USB_NPA_INTERCONNECT_VOTE
#define USB_NPA_INTERCONNECT_VOTE 0
#endif

STATIC EFI_GUID  mUsbNpaProtocolGuid = {
  0x79D6C870, 0x725E, 0x489E, { 0xA0, 0xA1, 0x27, 0xE7, 0xA5, 0xD0, 0xCB, 0x35 }
};

typedef VOID (EFIAPI *USB_NPA_GET_NODE) (
  IN  CONST CHAR8  *Path,
  IN  CONST CHAR8  *NodeName,
  IN  UINT32       Size,
  IN  UINT32       Index,
  IN  VOID         *PropertyData OPTIONAL,
  OUT EFI_HANDLE   *Node
  );

typedef VOID (EFIAPI *USB_NPA_VOTE_BUS) (
  IN EFI_HANDLE   Node,
  IN UINT32       Count,
  IN CONST VOID   *Table
  );

//
// Byte-for-byte copy of UsbConfigDxe's 16-entry table at image RVA 0x12220.
// UsbConfigDxe calls NPA +0x90 as VoteBus(Node, 16, Table). Keeping the opaque
// vendor table avoids inventing an ICB vector layout or bandwidth unit.
//
STATIC CONST UINT64  mUsbNpaBusVote[16] = {
  0x0000000000000004ULL, 0x0000000000000000ULL,
  0x0000000000000000ULL, 0x0000000000000000ULL,
  0x0000000000000004ULL, 0x0000000000000000ULL,
  0x0000000000000000ULL, 0x0000000000000000ULL,
  0x0000000000000004ULL, 0x0000000017D78400ULL,
  0x0000000000000000ULL, 0x0000000000000000ULL,
  0x0000000000000004ULL, 0x0000000027EF6380ULL,
  0x0000000027EF6380ULL, 0x0000000000000000ULL
};

//
// The fifth argument to NPA GetNode is mandatory for an ICB arbiter client.
// This is the exact 16-DWORD descriptor UsbConfigDxe keeps at RVA 0x122A0
// and passes while opening usb_ss1_bus. Passing NULL reaches ICBDxe but makes
// it dereference address zero; the resulting synchronous exception was seen
// at ICBDxe +0x30A4 on the first NPA-enabled test image.
//
STATIC CONST UINT32  mUsbNpaClientConfig[16] = {
  0x00000000, 0x00000016, 0x00000020, 0x00000000,
  0x00000010, 0x00000058, 0x000000A0, 0x000000E4,
  0x00000040, 0x00000020, 0x00000030, 0x00000010,
  0x00000050, 0x000000A0, 0x00000040, 0x00000070
};

//
// The eight clocks Qualcomm's UsbConfigDxe enables before it starts a core. The list is
// read from its own binary: InitUsbClocks(8, table@0x14658) in
// reverse/raw/UsbConfigDxe.ghidra.c, with the names from the same file's string table.
//
// The CBCR offsets are from drivers/clk/qcom/gcc-pineapple.c in the kernel tree - that is
// SM8650's clock controller - and each branch's CLK_OFF bit is bit 0 of its CBCR, so the
// readback below says plainly whether the controller is being clocked.
//
STATIC CONST CHAR8 *mUsbClockName[8] = {
  "gcc_cfg_noc_usb3_prim_axi_clk",
  "gcc_aggre_usb3_prim_axi_clk",
  "gcc_usb30_prim_master_clk",
  "gcc_usb30_prim_sleep_clk",
  "gcc_usb30_prim_mock_utmi_clk",
  "gcc_usb3_prim_phy_aux_clk",
  "gcc_usb3_prim_phy_com_aux_clk",
  "gcc_usb3_prim_phy_pipe_clk"
};

//
// Offsets from SHENNONG_GCC_OBS_BASE, i.e. the CBCR offset minus 0x39000.
//
STATIC CONST UINT32  mUsbClockCbcr[8] = {
  0x088,   // 0x39088 cfg_noc_usb3_prim_axi
  0x08C,   // 0x3908c aggre_usb3_prim_axi
  0x018,   // 0x39018 usb30_prim_master
  0x024,   // 0x39024 usb30_prim_sleep
  0x028,   // 0x39028 usb30_prim_mock_utmi
  0x060,   // 0x39060 usb3_prim_phy_aux
  0x064,   // 0x39064 usb3_prim_phy_com_aux
  0x068    // 0x39068 usb3_prim_phy_pipe
};

//
// The source the master branch runs from. A branch can be enabled and still deliver
// nothing if this divider is not programmed, which is the difference between "ClockDxe
// said yes" and "the core is actually clocked".
//
#define GCC_USB30_PRIM_MASTER_CMD_RCGR   0x02C
#define GCC_USB30_PRIM_MASTER_CFG_RCGR   0x030

//
// gcc_usb30_prim_gdsc, from shennong_fdt.dts:
//
//   qcom,gdsc@139004  regulator-name = "gcc_usb30_prim_gdsc"
//   ssusb@a600000     USB3_GDSC-supply = <&gcc_usb30_prim_gdsc>
//
// A GDSC gates the USB block itself. Its branches can read enabled in GCC while
// every transfer, GEVNTADRLO included, is dropped because the power domain is
// collapsed - which is the shape of the b99 log (CSFTRST never clears, event
// buffer address reads 0). BIT(31) of the GDSCR is PWR_ON and BIT(0) is
// SW_COLLAPSE, per drivers/clk/qcom/gdsc.c.
//
//
// The offset is 0x004, not 0x9004. shennong_fdt.dts says
//
//   qcom,gdsc@139004 { reg = <0x00139004 0x00000004>; }
//
// and Gcc points at 0x139000, the window the clock branches live in. 0x139004 is
// therefore Gcc + 4. An earlier version of this file used 0x9004, which lands on
// 0x142004 - outside the single 4 KB page this driver maps - and on this platform an
// access to an unmapped page is an external abort with no message, not a wrong value.
//
// The device tree declares four bytes and nothing else, so there is no companion
// config register to read here.
//
//
// GCC_USB30_PRIM_BCR, the block-level reset for the whole USB30 controller.
//
// drivers/clk/qcom/gcc-pineapple.c maps it to 0x39000, which is GCC + 0x39000 =
// 0x139000 - the first word of the very page this driver already maps for the clock
// branches. It has never been read.
//
// If it is asserted the controller's functional logic is held in reset while its APB
// window still answers, which is exactly the shape of the measurements: DCTL accepts a
// write and reads it back, DSTS never changes, and both CSFTRST and GCTL.CORESOFTRESET
// hang. The bit semantics are the same as the PHY's: qcom_reset_assert writes the mask,
// qcom_reset_deassert writes zero.
//
//
// TCSR_USB_INT_DYN_EN_DIS, the second reg-names entry of ssusb@a600000.
//
//   reg = <0x0a600000 0x00100000  0x01fc6000 0x00000004>;
//   reg-names = "core_base", "tcsr_dyn_en_dis";
//
// drivers/usb/dwc3/dwc3-msm-core.c reads it before it will touch the controller:
//
//   Reading a value on the TCSR_USB_INT_DYN_EN_DIS register deviating
//   from 0xF means that TZ caused TCSR to disable USB Interface
//
//   if (mdwc->tcsr_dyn_en_dis) {
//           val = dwc3_msm_read_reg(mdwc->tcsr_dyn_en_dis, 0);
//           if (val != 0xF)
//                   return -EINVAL;
//   }
//
// That is a gate on the USB interface held outside the controller. A gated interface
// still answers its APB window - GSNPSID reads, DCTL accepts a write and reads back -
// while nothing inside the core advances, which is exactly what this port measures:
// DSTS identical across half a second, and two different reset engines, CSFTRST and
// GCTL.CORESOFTRESET, both failing to self-clear.
//
//
// QSCRATCH, the controller's own PHY control block at core_base + 0xF8800.
//
// The vendor driver at FUN_0000e998 in the ABL image does exactly this, and only this,
// immediately before UsbfnDwcCoreInitImpl:
//
//   if (coreIdx == 0) {
//           *(u32 *)0x0A6F8810 |= 0x10100000;      QSCRATCH_HS_PHY_CTRL
//           *(u32 *)0x0A6F8830 |= 0x01000000;      QSCRATCH_SS_PHY_CTRL
//   }
//
// and the matching detach path FUN_0000e9d0 clears the same two words. The bits are
// named in drivers/usb/dwc3/dwc3-qcom.c:
//
//   #define QSCRATCH_HS_PHY_CTRL   0x10
//   #define UTMI_OTG_VBUS_VALID    BIT(20)      0x00100000
//   #define SW_SESSVLD_SEL         BIT(28)      0x10000000
//   #define QSCRATCH_SS_PHY_CTRL   0x30
//   #define LANE0_PWR_PRESENT      BIT(24)      0x01000000
//
// so the vendor writes UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL and LANE0_PWR_PRESENT -
// bit for bit what dwc3_qcom_vbus_override_enable(qcom, true) writes for device mode.
//
// SW_SESSVLD_SEL is the point: it tells the PHY that the session is valid from
// software, overriding the OTG/VBUS comparator. Without it the PHY sees no VBUS, keeps
// its UTMI interface down, and the core has nothing to run its reset against - which
// is precisely what this port measures. Nothing here has ever set these bits.
//
//
// APPS SMMU, and the stream the USB controller uses.
//
// ssusb@a600000 declares iommus = <&apps_smmu 0x40 0x0>, so every DMA the controller
// issues is translated through stream 0x40. ShennongPkg's PlatformMemoryMapLib
// declares the block - { "SMMU", 0x15000000, 0x00200000, AddDev, MMAP_IO, ... } - so
// reading it is safe.
//
// ARM SMMU-500 layout: IDR0 at 0x0000, S2CR(n) at 0x8000 + n*0x20, CBAR(n) at
// 0xC000 + n*0x20. S2CR bits 1:0 are the type: 0 fault, 1 bypass, 2 translate. A
// stream left at fault is rejected at the SMMU, and a rejected DMA on this platform
// takes the bus down and the machine resets - which is the shape of the failure after
// the device appears on the host.
//
#define SHENNONG_SMMU_BASE               0x15000000ULL
#define SMMU_IDR0                        0x0000
#define SMMU_S2CR(n)                     (0x8000U + (n) * 0x20U)
#define SMMU_CBAR(n)                     (0xC000U + (n) * 0x20U)
#define SMMU_S2CR_TYPE_MASK              0x3U
#define SMMU_S2CR_TYPE_FAULT             0U
#define SMMU_S2CR_TYPE_BYPASS            1U
#define SMMU_S2CR_TYPE_TRANSLATE         2U
#define SHENNONG_USB_SID                 0x40U

#define SHENNONG_QSCRATCH_BASE           0x0A6F8800ULL

//
// QSCRATCH_GENERAL_CFG, and the bit the vendor toggles around it. Names from
// drivers/usb/dwc3/dwc3-qcom.c:
//
//   #define QSCRATCH_GENERAL_CFG   0x08
//   #define PIPE_UTMI_CLK_SEL      BIT(0)
//   #define PIPE3_PHYSTATUS_SW     BIT(3)
//   #define PIPE_UTMI_CLK_DIS      BIT(8)
//
// UsbConfigDxe's device-mode path pulses this register before it touches the core:
//
//   |= 0x100   PIPE_UTMI_CLK_DIS = 1    disable the UTMI pipe clock
//   stall
//   |= 0x1     PIPE_UTMI_CLK_SEL = 1
//   |= 0x8     PIPE3_PHYSTATUS_SW = 1
//   &= ~0x200  clear bit 9
//   stall 100 us
//   &= ~0x100  PIPE_UTMI_CLK_DIS = 0    and enable it again
//   QSCRATCH+0x28 |= 2
//
// Nothing in this port has ever written 0x08. If PIPE_UTMI_CLK_DIS is left set the
// core has no UTMI clock, which is exactly the state that would leave the APB window
// answering while CSFTRST and GCTL.CORESOFTRESET both hang forever.
//
#define QSCRATCH_GENERAL_CFG             0x08
#define QSCRATCH_REF_CLK_CTRL            0x28
#define QSCRATCH_PIPE_UTMI_CLK_SEL       BIT0
#define QSCRATCH_PIPE3_PHYSTATUS_SW      BIT3
#define QSCRATCH_PIPE_UTMI_CLK_DIS       BIT8
#define QSCRATCH_PIPE_RSVD9              BIT9

#define QSCRATCH_HS_PHY_CTRL             0x10
#define QSCRATCH_SS_PHY_CTRL             0x30
#define QSCRATCH_UTMI_OTG_VBUS_VALID     BIT20
#define QSCRATCH_SW_SESSVLD_SEL          BIT28
#define QSCRATCH_LANE0_PWR_PRESENT       BIT24

#define SHENNONG_TCSR_USB_DYN_EN_DIS     0x01FC6000ULL

//
// The eUSB2 PHY's reference clock enable lives in the TCSR clock controller, not in GCC.
//
// pineapple-usb.dtsi gives eusb2_phy0 two clocks: RPMH_CXO_PAD_CLK, which is the always-on
// crystal, and TCSR_USB2_CLKREF_EN. The second one is not a GCC branch - it is a single
// enable bit:
//
//   tcsrcc: clock-controller@1f40000           base 0x01F40000
//   tcsr_usb2_clkref_en.enable_reg = 0xB1118   mask BIT(0)
//   drivers/clk/qcom/tcsrcc-pineapple.c
//
// so the absolute address is 0x01FF1118, bit 0. The USB3 equivalent sits at 0x01FF1108.
//
// Without it the PHY has no reference clock. Every register in it still answers, because
// the register file runs on the AHB clock, which is why twenty builds of probes have all
// looked healthy while the analogue side had nothing to run on.
//
#define SHENNONG_TCSRCC_USB2_CLKREF      0x01FF1118ULL
#define SHENNONG_TCSRCC_USB3_CLKREF      0x01FF1108ULL
#define TCSR_USB_INT_ENABLED             0xFU

#define SHENNONG_USB30_PRIM_BCR          0x000
#define USB30_PRIM_BCR_RESET_N           BIT0

#define SHENNONG_USB_GDSC                0x004

//
// The eUSB2 PHY has its own power domain and its own reset, and neither is in the
// page the clock branches live in.
//
//   gcc_usb3_phy_gdsc        absolute 0x150018   (qcom,gdsc@150018)
//   gcc_qusb2phy_prim_bcr    absolute 0x112000   (GCC_QUSB2PHY_PRIM_BCR, id 21)
//
// The reset id is confirmed from the device tree and the binding: hsphy@88e3000
// declares resets = <&gcc 0x15>, and include/dt-bindings/clock/qcom,gcc-pineapple.h
// gives GCC_QUSB2PHY_PRIM_BCR = 21 = 0x15. Its register comes from the reset map in
// drivers/clk/qcom/gcc-pineapple.c: [GCC_QUSB2PHY_PRIM_BCR] = { 0x12000 }.
//
// Polarity, from drivers/clk/qcom/reset.c - and it is the opposite of the intuitive
// reading:
//
//   qcom_reset_assert()   regmap_update_bits(map, mask, mask)   write 1 -> in reset
//   qcom_reset_deassert() regmap_update_bits(map, mask, 0)      write 0 -> released
//
// So BIT(0) set means the PHY is being held in reset. Nothing in this port has ever
// cleared it, which is consistent with a PHY whose register block reads as repeated
// byte patterns rather than as register values.
//
//
// The eUSB2 PHY's register block and the bits the kernel driver writes.
//
// Every offset and every mask below is transcribed from
// drivers/usb/phy/phy-msm-snps-eusb2.c, which is the driver this device runs
// in Android (the node's compatible is "qcom,usb-snps-eusb2-phy" and that file is its
// implementation). Reading the PHY before running this sequence returns repeated byte
// patterns - 01010101 at 0x50, 4B4B4B4B at 0x54, 17171717 at 0x64 - because nothing has
// ever initialised it in UEFI.
//
// The detail that makes this impossible to guess at: POR is a pulse, not a level. It is
// set to bring the PHY's internal blocks out of power-on reset and cleared again a few
// writes later. A PHY left with POR set is still in power-on reset; a PHY with POR
// clear at the end is running.
//
#define PHY_REG_UTMI_CTRL0        0x3C
#define PHY_REG_UTMI_CTRL5        0x50
#define PHY_REG_COMMON0           0x54
#define PHY_REG_COMMON1           0x58
#define PHY_REG_COMMON2           0x5C
#define PHY_REG_HS_PHY_CTRL1      0x60
#define PHY_REG_HS_PHY_CTRL2      0x64
#define PHY_REG_CFG0              0x94
#define PHY_REG_REFCLK_CTRL       0xA0

#define PHY_SLEEPM                BIT0
#define PHY_POR                   BIT1
#define PHY_SIDDQ                 BIT2
#define PHY_FSEL_MASK             0x70U
#define PHY_VBUSVLDEXTSEL0        BIT4
#define PHY_PLLBTUNE              BIT5
#define PHY_VREGBYPASS            BIT0
#define PHY_VBUSVLDEXT0           BIT0
#define PHY_USB2_SUSPEND_N        BIT2
#define PHY_USB2_SUSPEND_N_SEL    BIT3
#define PHY_CMN_CTRL_OVERRIDE_EN  BIT1
#define PHY_REFCLK_SEL_MASK       0x3U
#define PHY_REFCLK_SEL_DEFAULT    0x2U

#define SHENNONG_PHY_BASE                0x088E3000ULL

//
// The eUSB2 PHY node in kalama-usb.dtsi carries two extra registers beside the
// 0x154-byte PHY block: eud_enable_reg at 0x088E2000 and eud_detect_reg at
// 0x0C276000. usb_eud_is_active() in the vendor's UsbConfigDxe reads bit 0 of the
// first and skips every PHY and repeater path when it is set.
//
#define SHENNONG_EUD_ENABLE              0x088E2000ULL
//
// 0x0C276000 (eud_detect_reg) is deliberately not used: b208 mapped it and took a
// synchronous exception at exactly that address. A successful map does not make an
// MMIO address reachable - the same trap this port has hit before.
//
#define SHENNONG_EUD_DETECT_UNUSED       0x0C276000ULL

#define SHENNONG_PHY_GDSC_PAGE           0x00150000ULL
#define SHENNONG_PHY_GDSC                0x018
#define SHENNONG_QUSB2PHY_BCR_PAGE       0x00112000ULL
#define SHENNONG_QUSB2PHY_PRIM_BCR       0x000
#define QUSB2PHY_BCR_RESET_N             BIT0
#define GDSC_PWR_ON                      BIT31
#define GDSC_SW_COLLAPSE                 BIT0
#define GDSC_HW_CONTROL                  BIT1



STATIC
VOID
UsbfnClockSay (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Buffer[192];
  VA_LIST  Marker;
  UINTN    Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer) - 1, Format, Marker);
  VA_END (Marker);

  // No SerialPortInitialize() per line - see the note in UsbfnDwc3Dxe's RefSay.
  SerialPortWrite ((UINT8 *)Buffer, Length);

}

/**
  Read the eight branch enable bits as a string, one character per clock in table order.

  '1' means the branch is enabled. These are clk_branch clocks, and gcc-pineapple.c gives
  each of them enable_reg = its CBCR with enable_mask = BIT(0), so bit 0 is the enable bit
  and it reads back the state of the vote, not a "halted" flag.

  This is the control for everything else in this file. b72 read 11111111 both before and
  after EnableClock, which - read the right way round - means the branches were already on
  and ClockDxe changed nothing.
**/
STATIC
VOID
UsbfnClockEnableMask (
  IN  UINT64  Gcc,
  OUT CHAR8   *Mask
  )
{
  UINTN  Index;

  for (Index = 0; Index < 8; Index++) {
    Mask[Index] = ((MmioRead32 ((UINTN)Gcc + mUsbClockCbcr[Index]) & BIT0) != 0) ? '1' : '0';
  }

  Mask[8] = '\0';
}

/**
  Read the branch halt bits. BIT(31) is CLK_OFF: '1' here with '1' in the enable
  mask means the vote is set but the branch is not delivering a clock, which
  points at the parent or the GDSC rather than at this driver.
**/
STATIC
VOID
UsbfnClockHaltMask (
  IN  UINT64  Gcc,
  OUT CHAR8   *Mask
  )
{
  UINTN  Index;

  for (Index = 0; Index < 8; Index++) {
    Mask[Index] = ((MmioRead32 ((UINTN)Gcc + mUsbClockCbcr[Index]) & BIT31) != 0) ? '1' : '0';
  }

  Mask[8] = '\0';
}

/**
  Make sure the USB power domain is actually powered.

  Nothing in this port has ever enabled it: the driver was started directly,
  bypassing the UsbConfigDxe path that would consume USB3_GDSC-supply. If
  PWR_ON reads 0 the block is collapsed, and no amount of clock voting or core
  reset work can succeed. Only touch the register when it is collapsed and not
  under hardware control, and verify PWR_ON afterwards.
**/
STATIC
VOID
UsbfnEnsureUsbGdsc (
  IN UINT64  Gcc
  )
{
  UINT32 Value;
  UINT32 Timeout;

  Value = MmioRead32 ((UINTN)Gcc + SHENNONG_USB_GDSC);
  UsbfnClockSay ("[GDSC] usb30_prim=%08x pwr_on=%u collapse=%u hw_ctl=%u\n",
                 Value,
                 (Value >> 31) & 1,
                 Value & 1,
                 (Value >> 1) & 1);

  if (((Value & GDSC_PWR_ON) != 0) || ((Value & GDSC_HW_CONTROL) != 0)) {
    return;
  }

  MmioWrite32 ((UINTN)Gcc + SHENNONG_USB_GDSC, Value & ~GDSC_SW_COLLAPSE);
  for (Timeout = 0; Timeout < 1000; Timeout++) {
    if ((MmioRead32 ((UINTN)Gcc + SHENNONG_USB_GDSC) & GDSC_PWR_ON) != 0) {
      break;
    }
    MicroSecondDelay (1);
  }

  UsbfnClockSay ("[GDSC] after enable: %08x pwr_on=%u waited=%u us\n",
                 MmioRead32 ((UINTN)Gcc + SHENNONG_USB_GDSC),
                 (MmioRead32 ((UINTN)Gcc + SHENNONG_USB_GDSC) >> 31) & 1,
                 Timeout);
}

STATIC
VOID
UsbfnProbePhy (
  VOID
  );

/**
  Map one page of MMIO and mark it uncached.

  @param[in]  Base  Physical base, page aligned.
  @param[in]  Name  Label for the log.

  @retval EFI_SUCCESS  The page is accessible.
**/
STATIC
EFI_STATUS
UsbfnMapPage (
  IN UINT64        Base,
  IN CONST CHAR8   *Name
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;

  // Callers pass register addresses as well as page bases. GCD attributes and
  // AddMemorySpace require an aligned page, e.g. 01FF1118 -> 01FF1000.
  Base &= ~0xFFFULL;

  ZeroMem (&Desc, sizeof (Desc));
  Status = gDS->GetMemorySpaceDescriptor (Base, &Desc);
  if (EFI_ERROR (Status) || (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, 0x1000, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      UsbfnClockSay ("[PHY] %a add -> %r\n", Name, Status);
      return Status;
    }
  }

  Status = gDS->SetMemorySpaceAttributes (Base, 0x1000, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    EFI_STATUS QueryStatus;
    QueryStatus = gDS->GetMemorySpaceDescriptor (Base, &Desc);
    if (!EFI_ERROR (QueryStatus) && Desc.GcdMemoryType != EfiGcdMemoryTypeNonExistent &&
        (Desc.Attributes & EFI_MEMORY_UC) != 0 && Desc.BaseAddress <= Base &&
        Desc.Length >= Base - Desc.BaseAddress + 0x1000) {
      UsbfnClockSay ("[MAP b196] %a page=%lx attr=%r; existing UC verified\n", Name, Base, Status);
      return EFI_SUCCESS;
    }
    UsbfnClockSay ("[MAP b196] %a page=%lx attr=%r; access refused\n", Name, Base, Status);
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Reset and initialise the eUSB2 PHY.

  This is a transcription of qcom_snps_hsphy_init() in
  drivers/phy/qualcomm/phy-qcom-snps-femto-v2.c, which is what this device runs in
  Android. Nothing in this port has ever done it, and the PHY read back as repeated
  byte patterns precisely because of that - an uninitialised PHY has no reason to
  return register values.

  Two details matter and neither is guessable:

    - the reset is asserted and then deasserted. Checking that BIT(0) was already
      clear, which is what this driver did before, skips the pulse entirely.
    - POR is set and then cleared. It is a pulse that takes the PHY's internal blocks
      out of power-on reset, not a level to leave enabled.

  If the PHY now reads as registers instead of patterns, the core's suspend clock
  exists and CSFTRST has something to complete against.
**/
STATIC
VOID
UsbfnPhyDumpSequence (
  IN CONST CHAR8 *Tag
  )
{
  STATIC CONST UINT16 Offsets[] = {
    0x03C, 0x050, 0x054, 0x058, 0x05C, 0x060, 0x064, 0x068,
    0x06C, 0x070, 0x078, 0x07C, 0x094, 0x098, 0x0B8, 0x130
  };
  UINTN  Index;

  UsbfnClockSay ("[PHY] %a:", Tag);
  for (Index = 0; Index < ARRAY_SIZE (Offsets); Index++) {
    UsbfnClockSay (" %03x=%08x", Offsets[Index],
                   MmioRead32 ((UINTN)SHENNONG_PHY_BASE + Offsets[Index]));
  }
  UsbfnClockSay ("\n");
}

/**
  Dump the whole eUSB2 PHY register file, 0x000..0x150.

  The sixteen offsets the vendor programs all read back correct, so the interesting
  value is one we have never looked at: a PLL lock bit, or a block that reads zero
  because nothing ever wrote it. Twenty-one words per line keeps this to four lines.
**/
STATIC
VOID
UsbfnPhyDumpAll (
  IN CONST CHAR8 *Tag
  )
{
  UINTN  Offset;
  UINTN  Column;

  for (Offset = 0; Offset <= 0x150; Offset += 21 * 4) {
    UsbfnClockSay ("[PHY] %a %03x:", Tag, Offset);
    for (Column = 0; Column < 21 && (Offset + Column * 4) <= 0x150; Column++) {
      UsbfnClockSay (" %08x", MmioRead32 ((UINTN)SHENNONG_PHY_BASE + Offset + Column * 4));
    }
    UsbfnClockSay ("\n");
  }
}

//
// The PM8550B eUSB2 repeater's tuning block, from qcom,param-override-seq in
// shennong_fdt.dts line 11189. Value first, register second - the order the vendor's
// eusb2_repeater_update_seq() applies them in.
//
// Apply during powered platform preparation, before PHY initialization. The b218-b222
// report used UINT32[12] for a 12-byte SPMI read; its "000302..." output did not show
// lost tuning. Do not reapply this table from PHY completion or a running-link report.
//
STATIC CONST UINT8   mRepeaterTuneValues[12] = {
  0x00, 0x0F, 0x04, 0x07, 0x03, 0x01, 0x03, 0x05, 0x02, 0x00, 0x03, 0x02
};
STATIC CONST UINT16  mRepeaterTuneRegs[12] = {
  0xFD50, 0xFD51, 0xFD52, 0xFD53, 0xFD54, 0xFD55,
  0xFD56, 0xFD57, 0xFD58, 0xFD59, 0xFD5A, 0xFD5B
};

// SPMIDxe copies Len bytes with LDRB/STRB; Done is the byte count, not status.
typedef UINT32 (EFIAPI *USB_SPMI_READ) (
  VOID *, UINT32, UINT32, UINT32, UINT32, UINT8 *, UINT32, UINT32 *);
typedef UINT32 (EFIAPI *USB_SPMI_WRITE) (
  VOID *, UINT32, UINT32, UINT32, UINT32, UINT8 *, UINT32);

STATIC
BOOLEAN
UsbfnRepeaterRead (
  IN VOID           *Protocol,
  IN USB_SPMI_READ    Read,
  IN CONST CHAR8    *Tag,
  IN UINT32          Address,
  OUT UINT8         *Buffer,
  IN UINT32          Length
  )
{
  UINT32 Rc;
  UINT32 Done;

  Done = 0;
  ZeroMem (Buffer, Length);
  Rc = Read (Protocol, 0, 7, 0, Address, Buffer, Length, &Done);
  if ((Rc != 0) || (Done != Length)) {
    UsbfnClockSay ("[RPTR b223] %a %04x unavailable rc=%u bytes=%u/%u\n",
                   Tag, Address, Rc, Done, Length);
    return FALSE;
  }
  return TRUE;
}

STATIC
BOOLEAN
UsbfnRepeaterApplyTune (
  IN CONST CHAR8  *Tag
  )
{
  EFI_GUID  SpmiGuid = {
    0xFA5F306B, 0xF47D, 0x4AC4, { 0xA4, 0x7D, 0x88, 0x2F, 0x82, 0x04, 0xEC, 0x30 }
  };
  VOID        *Protocol;
  USB_SPMI_READ  Read;
  USB_SPMI_WRITE Write;
  EFI_STATUS   Status;
  UINT32       St;
  UINT8        Value;
  UINT8        Verify;
  UINTN        Idx;

  Protocol = NULL;
  Status   = gBS->LocateProtocol (&SpmiGuid, NULL, &Protocol);
  if (EFI_ERROR (Status) || (Protocol == NULL)) {
    UsbfnClockSay ("[RPTR] %a tune: no spmi -> %r\n", Tag, Status);
    return FALSE;
  }

  Read  = (USB_SPMI_READ)*(UINT64 *)((UINT8 *)Protocol + 0x08);
  Write = (USB_SPMI_WRITE)*(UINT64 *)((UINT8 *)Protocol + 0x10);
  if ((Read == NULL) || (Write == NULL)) {
    UsbfnClockSay ("[RPTR] %a tune: no entry points\n", Tag);
    return FALSE;
  }

  if (!UsbfnRepeaterRead (Protocol, Read, Tag, 0xFD46, &Value, 1)) {
    return FALSE;
  }
  UsbfnClockSay ("[RPTR] %a tune: enable=%a\n", Tag, (Value & 0x80) ? "ON" : "OFF");

  for (Idx = 0; Idx < 12; Idx++) {
    Value  = mRepeaterTuneValues[Idx];
    St     = Write (Protocol, 0, 7, 0, mRepeaterTuneRegs[Idx], &Value, 1);
    Verify = 0;
    if (St != 0) {
      UsbfnClockSay ("[RPTR b223] %a %04x write failed rc=%u\n", Tag, mRepeaterTuneRegs[Idx], St);
      return FALSE;
    }
    if (!UsbfnRepeaterRead (Protocol, Read, Tag, mRepeaterTuneRegs[Idx], &Verify, 1)) {
      return FALSE;
    }
    UsbfnClockSay ("[RPTR] %a %04x=%02x read=%02x\n",
                   Tag, mRepeaterTuneRegs[Idx],
                   (UINT32)mRepeaterTuneValues[Idx], (UINT32)(Verify & 0xFF));
  }

  return TRUE;
}

STATIC
VOID
UsbfnInitPhy (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Value;

  UsbfnPlatformEudReport ();

  Status = UsbfnMapPage (SHENNONG_PHY_BASE, "eusb2-phy");
  if (EFI_ERROR (Status)) {
    return;
  }

  UsbfnProbePhy ();
  UsbfnPhyDumpSequence ("before");

  Status = UsbfnMapPage (SHENNONG_QUSB2PHY_BCR_PAGE, "qusb2phy-bcr");
  if (!EFI_ERROR (Status)) {
    Value = MmioRead32 ((UINTN)SHENNONG_QUSB2PHY_BCR_PAGE + SHENNONG_QUSB2PHY_PRIM_BCR);
    UsbfnClockSay ("[PHY] bcr before=%08x in_reset=%u\n",
                   Value, (Value & QUSB2PHY_BCR_RESET_N) ? 1 : 0);

    MmioWrite32 ((UINTN)SHENNONG_QUSB2PHY_BCR_PAGE + SHENNONG_QUSB2PHY_PRIM_BCR,
                 Value | QUSB2PHY_BCR_RESET_N);
    MicroSecondDelay (150);

    MmioWrite32 ((UINTN)SHENNONG_QUSB2PHY_BCR_PAGE + SHENNONG_QUSB2PHY_PRIM_BCR,
                 MmioRead32 ((UINTN)SHENNONG_QUSB2PHY_BCR_PAGE + SHENNONG_QUSB2PHY_PRIM_BCR)
                 & ~QUSB2PHY_BCR_RESET_N);
    MicroSecondDelay (100);

    Value = MmioRead32 ((UINTN)SHENNONG_QUSB2PHY_BCR_PAGE + SHENNONG_QUSB2PHY_PRIM_BCR);
    UsbfnClockSay ("[PHY] bcr after=%08x in_reset=%u\n",
                   Value, (Value & QUSB2PHY_BCR_RESET_N) ? 1 : 0);
  }

  //
  // The vendor's table, transcribed from FUN_0000e25c in UsbConfigDxe - the module that
  // actually brings this PHY up in ABL.
  //
  // Every value is written TWICE. That is not redundancy in the transcription: the
  // disassembly issues each store twice, back to back, and 0x7C three times. Either the
  // register file is two-deep and needs two writes to latch, or the second write covers a
  // pipeline stall. Either way, a single write is not what the vendor does, so it is not
  // what this port should do. The previous revision wrote each value once and also skipped
  // 0x54=9 near the start and the 0x64=0x1F pair near the end - the latter is the only
  // VBUS-valid pulse in the whole sequence.
  //
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x094, 2);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x050, 2);
  gBS->Stall (100);

  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x054, 9);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x054, 9);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x130, 4);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x098, 0);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x0B8, 1);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x054, 0x49);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x05C, 200);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x060, 0);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x060, 0);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x058, 2);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x068, 0x21);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x068, 0x21);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x06C, 0x50);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x070, 0);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x06C, 0x50);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x064, 0x17);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x064, 0x17);

  //
  // Stock MTP (type 8) also calls E25C. In the extracted post_ddr_lanai.dts,
  // /soc/usb0/hs_phy_cfg/table contains only FFFFFFFF sentinel addresses, which
  // E4A4 skips. No additional HS board writes are established by that table.
  //
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x07C, 200);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x078, 0x18);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x07C, 200);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x078, 0x18);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x07C, 200);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x064, 0x1F);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x064, 0x1F);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x03C, 1);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x054, 0x4B);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x054, 0x4B);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x050, 0);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x064, 0x17);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + 0x094, 0);
  gBS->Stall (200);

  UsbfnPhyDumpSequence ("after");
  UsbfnPhyDumpAll ("all");

  UsbfnClockSay ("[PHY] init sequence done: 50=%08x 54=%08x 64=%08x 94=%08x\n",
                 MmioRead32 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_UTMI_CTRL5),
                 MmioRead32 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_COMMON0),
                 MmioRead32 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_HS_PHY_CTRL2),
                 MmioRead32 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_CFG0));

  //
  // Is this block readable at all, or only writable?
  //
  // Every read so far has returned four identical bytes - 4B4B4B4B, 17171717, 09090909,
  // 07070707 - which is either how this register file presents a byte-wide space to a
  // 32-bit access, or the signature of a block whose read path is not clocked. Those
  // two cases need opposite responses, so measure rather than argue:
  //
  //   three reads of one register, back to back   stable -> the space is real
  //   the four bytes of one word, read separately  identical -> byte-replicated,
  //                                                different -> a real 32-bit register
  // Do not test writability by changing COMMON0: PHY_ENABLE is bit0, so the old
  // 0x5A probe disabled the PHY after POR had already been released.
  //
  {
    UINT32  First;
    UINT32  Second;
    UINT32  Third;

    First  = MmioRead32 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_COMMON0);
    Second = MmioRead32 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_COMMON0);
    Third  = MmioRead32 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_COMMON0);
    UsbfnClockSay ("[PHY] 54 read x3: %08x %08x %08x\n", First, Second, Third);

    //
    // Byte lanes, read as bytes.
    //
    // The first version of this line used MmioRead32 at +1, +2 and +3 and took a
    // synchronous exception at FAR=0x88E3057: an unaligned 32-bit access to Device
    // memory is an alignment fault on this core, not a read of the byte at that
    // address. MmioRead8 is the only correct way to ask the question.
    //
    UsbfnClockSay ("[PHY] 54..57 byte lanes: %02x %02x %02x %02x\n",
                   MmioRead8 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_COMMON0 + 0),
                   MmioRead8 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_COMMON0 + 1),
                   MmioRead8 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_COMMON0 + 2),
                   MmioRead8 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_COMMON0 + 3));

    UsbfnClockSay ("[PHY] common0-b212 observation only; no enable toggle\n");
  }

  UsbfnProbePhy ();

}

/**
  Read one byte of the eUSB2 PHY register file.

  The file is byte-wide: a 32-bit read returns the byte replicated four times, so every
  access here goes through MmioRead8.
**/
UINT8
UsbfnPhyReadByte (
  IN UINT32  Offset
  )
{
  return MmioRead8 ((UINTN)SHENNONG_PHY_BASE + Offset);
}

/**
  Read-modify-write one byte of the eUSB2 PHY register file.
**/
VOID
UsbfnPhyUpdateByte (
  IN UINT32  Offset,
  IN UINT8   AndMask,
  IN UINT8   OrMask
  )
{
  UINT8  Value;

  Value = MmioRead8 ((UINTN)SHENNONG_PHY_BASE + Offset);
  Value = (UINT8)((Value & AndMask) | OrMask);
  MmioWrite32 ((UINTN)SHENNONG_PHY_BASE + Offset, Value);
}

/** Read-only eUSB2 state, using phy-msm-snps-eusb2.c (not femto-v2). */
VOID
UsbfnPhyStateReport (VOID)
{
  UINT8 C0 = UsbfnPhyReadByte (0x3C);
  UINT8 C5 = UsbfnPhyReadByte (0x50);
  UINT8 Cm0 = UsbfnPhyReadByte (0x54);
  UINT8 DivLo = UsbfnPhyReadByte (0x5C);
  UINT8 DivHi = UsbfnPhyReadByte (0x60);
  UINT8 C2 = UsbfnPhyReadByte (0x64);
  UINT8 Cfg = UsbfnPhyReadByte (0x94);

  UsbfnClockSay ("[PHY] por=%u sleepm=%u opmode=%u enable=%u siddq=%u reten=%u\n",
                 (C5 >> 1) & 1, C0 & 1, (C0 >> 3) & 3,
                 Cm0 & 1, (Cm0 >> 2) & 1, (Cm0 >> 3) & 1);
  UsbfnClockSay ("[PHY] fsel=%u pll_fb=%03x pll_ref=%u (0=19.2,4=38.4 MHz)\n",
                 (Cm0 >> 4) & 7, ((DivHi & 0xF) << 8) | DivLo, (DivHi >> 4) & 0xF);
  UsbfnClockSay ("[PHY] vbus_sel=%u vbus_valid=%u suspend_n=%u susp_sel=%u cmn_ovr=%u data_ovr=%u\n",
                 (C2 >> 4) & 1, C2 & 1, (C2 >> 2) & 1, (C2 >> 3) & 1,
                 (Cfg >> 1) & 1, Cfg & 1);
}

/**
  Prepare the powered repeater before PHY POR release.

  Match the vendor order: release repeater reset if needed, apply device tuning,
  clear host-only E8/ED bit6, then wait for RPTR_OK. This is initialization;
  running-link snapshots must never write these registers or toggle EN_CTL1.
**/
VOID
UsbfnRepeaterPrepare (
  VOID
  )
{
  EFI_GUID SpmiGuid = {
    0xFA5F306B, 0xF47D, 0x4AC4, { 0xA4, 0x7D, 0x88, 0x2F, 0x82, 0x04, 0xEC, 0x30 }
  };
  STATIC CONST UINT32 ForceRegs[] = { 0xFDE8, 0xFDED };
  VOID           *Protocol;
  USB_SPMI_READ   Read;
  USB_SPMI_WRITE  Write;
  EFI_STATUS      Status;
  UINT32          Rc;
  UINT8           Value;
  UINT8           Back;
  UINTN           Idx;

  Protocol = NULL;
  Status = gBS->LocateProtocol (&SpmiGuid, NULL, &Protocol);
  if (EFI_ERROR (Status) || (Protocol == NULL)) {
    UsbfnClockSay ("[RPTR b223] prepare no spmi -> %r\n", Status);
    return;
  }
  Read = (USB_SPMI_READ)*(UINT64 *)((UINT8 *)Protocol + 0x08);
  Write = (USB_SPMI_WRITE)*(UINT64 *)((UINT8 *)Protocol + 0x10);
  if ((Read == NULL) || (Write == NULL)) {
    UsbfnClockSay ("[RPTR b223] prepare missing entry\n");
    return;
  }
  if (!UsbfnRepeaterRead (Protocol, Read, "prepare", 0xFD46, &Value, 1)) {
    return;
  }
  if ((Value & BIT7) == 0) {
    Value |= BIT7;
    Rc = Write (Protocol, 0, 7, 0, 0xFD46, &Value, 1);
    if (Rc != 0) {
      UsbfnClockSay ("[RPTR b223] prepare enable failed rc=%u\n", Rc);
      return;
    }
  }
  if (!UsbfnRepeaterApplyTune ("prepare")) {
    return;
  }
  for (Idx = 0; Idx < ARRAY_SIZE (ForceRegs); Idx++) {
    if (!UsbfnRepeaterRead (Protocol, Read, "prepare", ForceRegs[Idx], &Value, 1)) {
      return;
    }
    if ((Value & BIT6) != 0) {
      Value &= (UINT8)~BIT6;
      Rc = Write (Protocol, 0, 7, 0, ForceRegs[Idx], &Value, 1);
      if (Rc != 0) {
        UsbfnClockSay ("[RPTR b223] prepare %04x clear failed rc=%u\n", ForceRegs[Idx], Rc);
        return;
      }
    }
    if (!UsbfnRepeaterRead (Protocol, Read, "prepare-verify", ForceRegs[Idx], &Back, 1)) {
      return;
    }
    if ((Back & BIT6) != 0) {
      UsbfnClockSay ("[RPTR b223] prepare %04x host force still set=%02x\n", ForceRegs[Idx], (UINT32)Back);
      return;
    }
  }
  // Vendor usleep_range(10, 20) is microseconds, not milliseconds.
  for (Idx = 0; Idx < 5; Idx++) {
    if (!UsbfnRepeaterRead (Protocol, Read, "prepare-ok", 0xFD08, &Value, 1)) {
      return;
    }
    if ((Value & BIT7) != 0) {
      UsbfnClockSay ("[RPTR b223] prepare complete status=%02x\n", (UINT32)Value);
      return;
    }
    gBS->Stall (20);
  }
  UsbfnClockSay ("[RPTR b223] prepare RPTR_OK timeout status=%02x\n", (UINT32)Value);
}

/** Read-only snapshot after RunStop; SPMI buffers contain consecutive bytes. */
VOID
UsbfnRepeaterReport (
  VOID
  )
{
  EFI_GUID SpmiGuid = {
    0xFA5F306B, 0xF47D, 0x4AC4, { 0xA4, 0x7D, 0x88, 0x2F, 0x82, 0x04, 0xEC, 0x30 }
  };
  STATIC CONST UINT32 Registers[] = {
    0xFD46, 0xFD08, 0xFD40, 0xFD41, 0xFD09, 0xFD0A,
    0xFDE8, 0xFDED, 0xFD00, 0xFD42
  };
  VOID          *Protocol;
  USB_SPMI_READ  Read;
  EFI_STATUS     Status;
  UINT8          Values[ARRAY_SIZE (Registers)];
  UINT8          Tune[12];
  BOOLEAN        Complete;
  UINT32         Last;
  UINT32         Now;
  UINTN          Step;
  UINTN          Idx;

  Protocol = NULL;
  Status = gBS->LocateProtocol (&SpmiGuid, NULL, &Protocol);
  if (EFI_ERROR (Status) || (Protocol == NULL)) {
    UsbfnClockSay ("[RPTR b223] snapshot no spmi -> %r\n", Status);
    return;
  }
  Read = (USB_SPMI_READ)*(UINT64 *)((UINT8 *)Protocol + 0x08);
  if (Read == NULL) {
    UsbfnClockSay ("[RPTR b223] snapshot no read entry\n");
    return;
  }
  Complete = TRUE;
  for (Idx = 0; Idx < ARRAY_SIZE (Registers); Idx++) {
    if (!UsbfnRepeaterRead (Protocol, Read, "snapshot", Registers[Idx], &Values[Idx], 1)) {
      Complete = FALSE;
    }
  }
  if (Complete) {
    UsbfnClockSay ("[RPTR b223] snapshot enable=%02x status=%02x mode=%02x hiz=%02x infra=%02x dropout=%02x\n",
                   (UINT32)Values[0], (UINT32)Values[1], (UINT32)Values[2],
                   (UINT32)Values[3], (UINT32)Values[4], (UINT32)Values[5]);
    UsbfnClockSay ("[RPTR b223] snapshot force_e8=%02x force_ed=%02x rev=%02x autoresume=%02x\n",
                   (UINT32)Values[6], (UINT32)Values[7], (UINT32)Values[8], (UINT32)Values[9]);
  } else {
    UsbfnClockSay ("[RPTR b223] snapshot incomplete; no decoded state\n");
  }
  // A 12-byte transfer into UINT32[12] printed 000302... in b218-b222.
  // Only publish tuning if both the return code and the transferred length match.
  if (UsbfnRepeaterRead (Protocol, Read, "tune", 0xFD50, Tune, sizeof (Tune))) {
    UsbfnClockSay ("[RPTR b223] tune bytes=12/12:");
    for (Idx = 0; Idx < ARRAY_SIZE (Tune); Idx++) {
      UsbfnClockSay (" %02x", (UINT32)Tune[Idx]);
    }
    UsbfnClockSay ("\n");
  }

  // Bounded observation only. DSTS is C70C; no link-state writes or reset here.
  Last = MmioRead32 ((UINTN)SHENNONG_DWC3_BASE + 0xC70C);
  UsbfnClockSay ("[RPTR b223] link watch start DSTS=%08x link=%u\n", Last, (Last >> 18) & 0xF);
  for (Step = 0; Step < 60; Step++) {
    gBS->Stall (5000);
    Now = MmioRead32 ((UINTN)SHENNONG_DWC3_BASE + 0xC70C);
    if (((Now >> 18) & 0xF) != ((Last >> 18) & 0xF)) {
      UsbfnClockSay ("[RPTR b223] link t=%ums DSTS=%08x link=%u\n",
                     (UINT32)((Step + 1) * 5), Now, (Now >> 18) & 0xF);
    }
    Last = Now;
  }
  UsbfnClockSay ("[RPTR b223] link watch end DSTS=%08x link=%u\n", Last, (Last >> 18) & 0xF);
}

/**
  Power and un-reset the eUSB2 PHY.

  Three things are needed and only the first was ever done in this port:

    1. ldoi1 and ldoi3, the PHY's two supplies, voted over RPMh.
    2. gcc_usb3_phy_gdsc, the PHY's own power domain - a different domain from
       gcc_usb30_prim_gdsc, which the core uses.
    3. gcc_qusb2phy_prim_bcr, the PHY's reset - which reads 1 while the PHY is held.

  Nothing here is speculation about what a PHY wants: each address and each bit is
  read from shennong_fdt.dts, the GCC binding and the kernel's reset driver, and the
  result is verified by reading the PHY back afterwards.
**/
/**
  Assert VBUS override and lane power in QSCRATCH, as the vendor driver does.

  This is the device-mode VBUS override: UTMI_OTG_VBUS_VALID and SW_SESSVLD_SEL in
  QSCRATCH_HS_PHY_CTRL, and LANE0_PWR_PRESENT in QSCRATCH_SS_PHY_CTRL. The ABL image
  sets exactly these bits immediately before it initialises the core, and clears them
  on detach.

  @retval EFI_SUCCESS  The bits read back set.
**/
STATIC
EFI_STATUS
UsbfnEnsureVbusOverride (
  VOID
  )
{
  UINT32      HsBefore;
  UINT32      SsBefore;
  UINT32      HsAfter;
  UINT32      SsAfter;

  //
  // No mapping call here on purpose. ShennongPkg's PlatformMemoryMapLib declares
  //
  //   { "USB", 0x0A600000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE }
  //
  // so 0x0A6F8800 is already added and already uncached. Asking the GCD to set
  // attributes on a sub-range of a declared region returns EFI_UNSUPPORTED while the
  // memory stays perfectly accessible, and the previous revision treated that refusal
  // as "cannot touch QSCRATCH" and skipped the write entirely.
  //
  HsBefore = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_HS_PHY_CTRL);
  SsBefore = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_SS_PHY_CTRL);
  UsbfnClockSay ("[QSCR] hs_phy_ctrl=%08x ss_phy_ctrl=%08x\n", HsBefore, SsBefore);

  MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_HS_PHY_CTRL,
               HsBefore | QSCRATCH_UTMI_OTG_VBUS_VALID | QSCRATCH_SW_SESSVLD_SEL);
  MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_SS_PHY_CTRL,
               SsBefore | QSCRATCH_LANE0_PWR_PRESENT);

  HsAfter = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_HS_PHY_CTRL);
  SsAfter = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_SS_PHY_CTRL);
  UsbfnClockSay ("[QSCR] after: hs=%08x (vbus=%u sessvld=%u) ss=%08x (lane0=%u)\n",
                 HsAfter,
                 (HsAfter & QSCRATCH_UTMI_OTG_VBUS_VALID) ? 1U : 0U,
                 (HsAfter & QSCRATCH_SW_SESSVLD_SEL) ? 1U : 0U,
                 SsAfter,
                 (SsAfter & QSCRATCH_LANE0_PWR_PRESENT) ? 1U : 0U);

  if (((HsAfter & (QSCRATCH_UTMI_OTG_VBUS_VALID | QSCRATCH_SW_SESSVLD_SEL)) ==
       (QSCRATCH_UTMI_OTG_VBUS_VALID | QSCRATCH_SW_SESSVLD_SEL)) &&
      ((SsAfter & QSCRATCH_LANE0_PWR_PRESENT) != 0))
  {
    return EFI_SUCCESS;
  }

  return EFI_DEVICE_ERROR;
}

//
// HAL IOMMU, the block that gives the USB master an address translation domain.
//
// The vendor UsbConfigDxe is omitted from the current FV. This source owner
// creates/configures/attaches a persistent domain before starting the core and
// validates the exact USB SID mapping before the manual DMA test. Missing or
// conflicting mappings remain errors; interface publication alone is insufficient.
//
// The GUID and the call sequence are read out of UsbConfigDxe itself: the GUID sits at
// .data 0x141d8, and the four calls are, in order,
//
//   LocateProtocol(&guid, NULL, &proto)
//   proto->GetInterface()                 vtable + 0x08
//   iface->[0x00]  CreateDomain(iface)
//   iface->[0x40]  ConfigureUsbDomain(iface, 0)
//   iface->[0x10]  Attach(iface, "USB0", 0x3000000, 0)
//
// with "USB0" being the literal at .text 0x10d5e that the vendor passes as the name.
//
typedef EFI_STATUS (EFIAPI *HAL_IOMMU_CREATE_DOMAIN) (IN VOID *Iface);
typedef EFI_STATUS (EFIAPI *HAL_IOMMU_CONFIGURE_USB) (IN VOID *Iface, IN UINT32 Flag);
typedef EFI_STATUS (EFIAPI *HAL_IOMMU_ATTACH) (
  IN VOID   *Iface,
  IN CHAR8  *Name,
  IN UINT32  Size,
  IN UINT32  Flag
  );
// HALIOMMU +0x14D0 only stores the output pointer; it does not return a status.
typedef VOID (EFIAPI *HAL_IOMMU_GET_IFACE) (OUT VOID **IfaceTable);

STATIC UINT64   mUsbIommuDomain[8];
STATIC BOOLEAN  mUsbIommuReady;

/**
  Create, configure and attach the USB IOMMU domain, as UsbConfigDxe would.

  @retval EFI_SUCCESS  The domain is attached and the USB master can translate.
**/
STATIC
EFI_STATUS
UsbfnEnsureIommuDomain (
  VOID
  )
{
  EFI_STATUS          Status;
  VOID                *Proto;
  VOID                *Iface;
  UINT64              *Vtbl;
  HAL_IOMMU_GET_IFACE GetIface;
  //
  // Caller-owned domain object, not the function table.
  //
  // CreateDomain at 0x15e8 allocates 64 bytes, zeroes them, and then executes
  // "str x20, [x19]" - it stores the new object into the caller's structure at +0x00.
  // ConfigureUsbDomain at 0x2098 then zeroes +0x08 .. +0x20 of that same structure. The
  // table at 0xb0d0 must never be passed as this pointer: doing so overwrote table entry
  // [0] with a heap address, and the next call through the table jumped to garbage -
  // which is the external abort inside HALIOMMU at +0x272C with a wild FAR.
  //
  UINT64              *Domain = mUsbIommuDomain;
  //
  // The bytes at UsbConfigDxe .data 0x141d8 are b4 d3 b6 54 33 5d 91 4f ...  Data1 is
  // little endian, so that is 0x54B6D3B4. Reading it as big endian gives 0xB654D3B4 and
  // LocateProtocol then returns NotFound, which is exactly what the first attempt did.
  // The correct GUID appears six times in FVMAIN.Fv; the wrong one appears zero times.
  //
  STATIC EFI_GUID     HalIommuGuid = {
    0x54B6D3B4, 0x5D33, 0x4F91, { 0x86, 0x00, 0x6C, 0x41, 0xD5, 0xDE, 0xB1, 0x9A }
  };

  if (mUsbIommuReady) {
    return EFI_SUCCESS;
  }

  Proto = NULL;
  Status = gBS->LocateProtocol (&HalIommuGuid, NULL, &Proto);
  UsbfnClockSay ("[IOMMU] protocol -> %r handle=0x%lx\n", Status, (UINT64)(UINTN)Proto);
  if (EFI_ERROR (Status) || (Proto == NULL)) {
    return EFI_DEVICE_ERROR;
  }

  //
  // The vendor calls GetInterface with no visible argument and then uses a structure in
  // its own .data, so the callee fills a caller-owned buffer. Pass one; if it comes back
  // empty, try the other calling convention before giving up.
  //
  //
  // The protocol is { Revision, Function... } and its second entry is
  //
  //   GetInterface(OUT VOID **Iface)   { *Iface = <table>; }
  //
  // which is three instructions in HALIOMMUDxe at 0x14d0 - adrp 0xb000, add 0xd0, str
  // [x0]. So it takes one argument and hands back a pointer, it does not fill a caller
  // buffer. The table it returns lives at 0xb0d0 and its entries are, in order,
  // [0] CreateDomain, [2] Attach, [8] ConfigureUsbDomain - the same offsets UsbConfigDxe
  // uses.
  //
  GetIface = (HAL_IOMMU_GET_IFACE)((UINT64 *)Proto)[1];
  Iface    = NULL;
  GetIface (&Iface);
  UsbfnClockSay ("[IOMMU] get interface table=0x%lx\n", (UINT64)(UINTN)Iface);
  if (Iface == NULL) {
    return EFI_DEVICE_ERROR;
  }

  Vtbl = (UINT64 *)Iface;
  UsbfnClockSay ("[IOMMU] table create=0x%lx attach=0x%lx usbcfg=0x%lx\n",
                 Vtbl[0], Vtbl[2], Vtbl[8]);
  if ((Vtbl[0] == 0) || (Vtbl[2] == 0) || (Vtbl[8] == 0)) {
    return EFI_DEVICE_ERROR;
  }

  ZeroMem (Domain, sizeof (mUsbIommuDomain));

  //
  // All three calls take the caller's structure, and the structure is what works: with
  // this exact sequence the capture shows every step returning Success.
  //
  // It is tempting to pass Domain[0] - the object CreateDomain allocated - to the later
  // two calls, since that is the "domain" in the ordinary sense. Attach rejects it:
  //
  //   Attach at 0x16e8:  ldr w8, [x0] ; cmp w8, #1 ; b.ne <continue> ; return 2
  //
  // and CreateDomain sets object[0] = 1, so the object is precisely what Attach refuses.
  //
  Status = ((HAL_IOMMU_CREATE_DOMAIN)Vtbl[0]) (Domain);
  UsbfnClockSay ("[IOMMU] create domain -> %r object=0x%lx\n",
                 Status, Domain[0]);
  if ((Status != 0) || (Domain[0] == 0)) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Arg 0 means bypass. The decompiled ConfigureUsbDomain zeroes This+0x08..+0x1C and then
  // stores 0xE0 at This+0x20 for arg 0, or 0x0A9F00E0 for arg 1, before calling a helper
  // that programs the context bank from those fields:
  //
  //   +0x20 -> SCTLR, +0x08 -> TTBR0, +0x10 -> TTBR1, +0x18 -> TCR, +0x1C -> MAIR
  //
  // 0xE0 leaves SCTLR.M clear, so translation is off and the stream passes through. The
  // 0x0A9F00E0 form sets M with TTBR0 and TTBR1 both zero, which faults every access. Arg 0
  // is the one that makes sense for a firmware that does not maintain translation tables.
  //
  Status = ((HAL_IOMMU_CONFIGURE_USB)Vtbl[8]) (Domain, 0);
  UsbfnClockSay ("[IOMMU] configure usb domain -> %r\n", Status);
  if (Status != 0) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Attach is neither of the two things tried so far.
  //
  // It runs "ldr x8, [x27, #0x30]!" and then "ldr x8, [x8]" - so This+0x30 has to be a
  // pointer to a structure, not a code pointer. Passing the interface table put a function
  // address there and the dereference took a translation fault at +0x181C. Passing the
  // caller structure put a zero there, which took the branch at +0x183c and registered
  // nothing while returning success.
  //
  // Dump what the domain actually contains before deciding what to hand it.
  //
  {
    UINT64  Obj;
    UINT64  *Words;
    UINTN   Index;

    Obj = Domain[0];
    UsbfnClockSay ("[IOMMU] domain struct: %lx %lx %lx %lx %lx %lx %lx %lx\n",
                   Domain[0], Domain[1], Domain[2], Domain[3],
                   Domain[4], Domain[5], Domain[6], Domain[7]);

    if (Obj != 0) {
      Words = (UINT64 *)(UINTN)Obj;
      for (Index = 0; Index < 8; Index += 4) {
        UsbfnClockSay ("[IOMMU] object+%02x: %lx %lx %lx %lx\n",
                       (UINT32)(Index * 8),
                       Words[Index], Words[Index + 1],
                       Words[Index + 2], Words[Index + 3]);
      }
    }
  }

  //
  // Back to the id UsbConfigDxe passes.
  //
  // 0x40 was tried because the device tree says iommus = <&apps_smmu 0x40 0x0>. It came back
  // as 1, EFI_WARN_DELETE_FAILURE, and the decompiled Attach only ever returns 0, 2, 4, 0x13,
  // 0x14 or 0x15 - so 0x40 is not an id this interface recognises and the call went somewhere
  // ghidra did not recover cleanly. 0x3000000 returns 0, and its low sixteen bits select id 0,
  // which is what the vendor passes.
  //
  Status = ((HAL_IOMMU_ATTACH)Vtbl[2]) (Domain, "USB0", 0x3000000, 0);
  UsbfnClockSay ("[IOMMU] attach USB0 -> %r\n", Status);
  if (Status != 0) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Configure a second time, now that Attach has filled the list.
  //
  // ConfigureUsbDomain does two things. It sets This[0] = 0 and This+0x20 = 0xE0, and then
  // it calls a helper that walks the list at This+0x30 - the list of attached streams - and
  // programs each one's context bank:
  //
  //   for (node = *(This + 0x30); node; node = node->next)
  //     if (node->sid && node->cb != -1 && *This == 0)
  //       SCTLR = This+0x20, TTBR0 = This+0x08, TTBR1 = This+0x10, TCR = This+0x18, ...
  //
  // Called before Attach, that loop has nothing to walk and does nothing. Attach does
  // program the bank it binds, using the same fields, so the first call is not wasted - but
  // it only covers the one node it just created. Running Configure again afterwards covers
  // every node on the list with the values it just wrote, which is what the function is
  // shaped to do.
  //
  Status = ((HAL_IOMMU_CONFIGURE_USB)Vtbl[8]) (Domain, 0);
  UsbfnClockSay ("[IOMMU] configure after attach -> %r\n", Status);
  UsbfnClockSay ("[IOMMU] persistent domain=0x%lx streams=0x%lx sctlr=%08x rc=%u\n",
                 (UINT64)(UINTN)Domain, Domain[6], (UINT32)Domain[4], (UINT32)Status);
  mUsbIommuReady = (BOOLEAN)(Status == 0 && Domain[6] != 0);
  return mUsbIommuReady ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

// Read the bank which HALIOMMU actually attached, using its own node layout and
// its +0x229C register calculation. The old 15008800 probe was not S2CR(SID40):
// GR0 SMR/S2CR are slot-indexed at +800/+C00, not indexed by SID at +8800.
VOID
UsbfnPlatformDmaReport (VOID)
{
  UINT64 *Node;
  UINT32 Base;
  UINT32 Bank;
  UINT32 Idr0;
  UINT32 Idr1;
  UINT32 Context;
  UINT32 Slots;
  UINT32 Index;
  UINT32 Matches = 0;
  EFI_STATUS Status;

  if (!mUsbIommuReady || mUsbIommuDomain[6] == 0) {
    UsbfnClockSay ("[SMMU b200] no attached USB domain\n");
    return;
  }
  Node = (UINT64 *)(UINTN)mUsbIommuDomain[6];
  Base = (UINT32)Node[0];
  Bank = *(UINT32 *)((UINT8 *)Node + 0xC);
  UsbfnClockSay ("[SMMU b200] HAL node=%lx base=%08x cb=%u clients=%lx\n",
                 (UINT64)(UINTN)Node, Base, Bank, Node[2]);
  if (Base != 0x15000000 || Bank >= 96) {
    UsbfnClockSay ("[SMMU b200] unexpected IORT base/bank; MMIO skipped\n");
    return;
  }
  Status = UsbfnMapPage (Base, "usb-smmu-gr0");
  if (EFI_ERROR (Status)) return;
  Idr0 = MmioRead32 (Base + 0x20);
  Idr1 = MmioRead32 (Base + 0x24);
  Slots = Idr0 & 0xFF;
  UsbfnClockSay ("[SMMU b200] IDR0=%08x IDR1=%08x sCR0=%08x GFSR=%08x slots=%u\n",
                 Idr0, Idr1, MmioRead32 (Base), MmioRead32 (Base + 0x48), Slots);
  if (Slots > 128) return;

  for (Index = 0; Index < Slots; Index++) {
    UINT32 Smr = MmioRead32 (Base + 0x800 + Index * 4);
    UINT32 Mask = (Smr >> 16) & 0x7FFF;
    if ((Smr & BIT31) == 0 || (((Smr & 0x7FFF) ^ 0x40) & ~Mask) != 0) continue;
    {
      UINT32 S2cr = MmioRead32 (Base + 0xC00 + Index * 4);
      UsbfnClockSay ("[SMMU b200] SID40 slot=%u SMR=%08x S2CR=%08x type=%u cb=%u\n",
                     Index, Smr, S2cr, (S2cr >> 16) & 3, S2cr & 0xFF);
      Matches++;
    }
  }
  UsbfnClockSay ("[SMMU b200] SID40 matching slots=%u (one expected)\n", Matches);
  Context = ((2U << ((Idr1 >> 28) & 7)) * ((Idr1 & BIT31) ? 0x10000U : 0x1000U)) + Bank * 0x1000;
  // IDR1=60000047 places CB10 at +8A000. The Linux DT maps a 1MB base
  // window; IORT Span=80000 only covered GR0 and must not bound the CB half.
  if (Context >= 0x100000 || Context < 0x2000 || Bank >= (Idr1 & 0xFF)) return;
  Status = UsbfnMapPage (Base + Context, "usb-smmu-cb");
  if (EFI_ERROR (Status)) return;
  UsbfnClockSay ("[SMMU b200] cb@%08x SCTLR=%08x FSR=%08x FAR=%08x%08x\n",
                 Base + Context, MmioRead32 (Base + Context),
                 MmioRead32 (Base + Context + 0x58), MmioRead32 (Base + Context + 0x64),
                 MmioRead32 (Base + Context + 0x60));
}

EFI_STATUS
UsbfnEnsureUsbStream (VOID)
{
  UINT64 *Node;
  UINT32 Base;
  UINT32 Bank;
  UINT32 Idr0;
  UINT32 Idr1;
  UINT32 Slots;
  UINT32 Context;
  UINT32 OwnSlot;
  UINT32 Index;
  UINT32 Count = 0;
  EFI_STATUS Status;

  if (!mUsbIommuReady || mUsbIommuDomain[6] == 0) return EFI_NOT_READY;
  Node = (UINT64 *)(UINTN)mUsbIommuDomain[6];
  Base = (UINT32)Node[0];
  Bank = *(UINT32 *)((UINT8 *)Node + 0xC);
  if (Base != 0x15000000) return EFI_UNSUPPORTED;
  Status = UsbfnMapPage (Base, "usb-smmu-gr0");
  if (EFI_ERROR (Status)) return Status;
  Idr0 = MmioRead32 (Base + 0x20);
  Idr1 = MmioRead32 (Base + 0x24);
  Slots = Idr0 & 0xFF;
  Context = ((2U << ((Idr1 >> 28) & 7)) * ((Idr1 & BIT31) ? 0x10000U : 0x1000U)) + Bank * 0x1000;
  if (Slots == 0 || Slots > 128 || Bank >= (Idr1 & 0xFF) || Context >= 0x100000) return EFI_UNSUPPORTED;
  Status = UsbfnMapPage (Base + Context, "usb-smmu-cb");
  if (EFI_ERROR (Status)) return Status;
  if ((MmioRead32 (Base + Context) & BIT0) != 0) {
    UsbfnClockSay ("[SMMU b200] owned CB%u has translation enabled; stream unchanged\n", Bank);
    return EFI_DEVICE_ERROR;
  }
  if (MmioRead32 (SHENNONG_DWC3_BASE + 0xC708) != 0) return EFI_NOT_READY;

  OwnSlot = Slots;
  // Inspect the entire table before changing anything. A masked/shared mapping
  // may cover other devices; only exact SID40 mappings can be retired here.
  for (Index = 0; Index < Slots; Index++) {
    UINT32 Smr = MmioRead32 (Base + 0x800 + Index * 4);
    UINT32 Mask = (Smr >> 16) & 0x7FFF;
    UINT32 S2cr;
    if ((Smr & BIT31) == 0 || (((Smr & 0x7FFF) ^ 0x40) & ~Mask) != 0) continue;
    if (Mask != 0 || (Smr & 0xFFFF) != 0x40) return EFI_UNSUPPORTED;
    S2cr = MmioRead32 (Base + 0xC00 + Index * 4);
    if (((S2cr >> 16) & 3) == 0 && (S2cr & 0xFF) == Bank && OwnSlot == Slots) OwnSlot = Index;
  }
  if (OwnSlot == Slots) return EFI_NOT_FOUND;
  for (Index = 0; Index < Slots; Index++) {
    UINT32 Smr = MmioRead32 (Base + 0x800 + Index * 4);
    if (Index == OwnSlot || (Smr & BIT31) == 0 || (Smr & 0x7FFFFFFF) != 0x40) continue;
    MmioWrite32 (Base + 0x800 + Index * 4, Smr & ~BIT31);
    UsbfnClockSay ("[SMMU b200] retired duplicate SID40 slot=%u, kept slot=%u CB%u\n", Index, OwnSlot, Bank);
  }
  MemoryFence ();
  for (Index = 0; Index < Slots; Index++) {
    UINT32 Smr = MmioRead32 (Base + 0x800 + Index * 4);
    if ((Smr & BIT31) != 0 && (Smr & 0x7FFFFFFF) == 0x40) Count++;
  }
  UsbfnClockSay ("[SMMU b200] SID40 exact matches=%u SCTLR=%08x\n", Count, MmioRead32 (Base + Context));
  return Count == 1 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

// Read-only comparison of the owned USB context bank. Identical sticky FSR
// values cannot exclude a repeated fault; preserve them instead of clearing a
// shared/global fault register or resuming an unknown stalled transaction.
EFI_STATUS
UsbfnPlatformDmaSnapshot (IN CONST CHAR8 *Phase, IN BOOLEAN Baseline)
{
  STATIC BOOLEAN HaveBaseline;
  STATIC UINT32 BaselineContext;
  STATIC UINT32 BaselineFsr;
  STATIC UINT64 BaselineFar;
  UINT64 *Node;
  UINT32 Base;
  UINT32 Bank;
  UINT32 Idr0;
  UINT32 Idr1;
  UINT32 Context;
  UINT32 Slots;
  UINT32 Index;
  UINT32 Matches = 0;
  UINT32 Fsr;
  UINT64 Far;
  EFI_STATUS Status;

  if (!mUsbIommuReady || mUsbIommuDomain[6] == 0) return EFI_NOT_READY;
  Node = (UINT64 *)(UINTN)mUsbIommuDomain[6];
  Base = (UINT32)Node[0];
  Bank = *(UINT32 *)((UINT8 *)Node + 0xC);
  if (Base != 0x15000000 || Bank >= 96) return EFI_UNSUPPORTED;
  Status = UsbfnMapPage (Base, "usb-smmu-gr0");
  if (EFI_ERROR (Status)) return Status;
  Idr0 = MmioRead32 (Base + 0x20);
  Idr1 = MmioRead32 (Base + 0x24);
  Slots = Idr0 & 0xFF;
  Context = ((2U << ((Idr1 >> 28) & 7)) * ((Idr1 & BIT31) ? 0x10000U : 0x1000U)) + Bank * 0x1000;
  if (Slots == 0 || Slots > 128 || Bank >= (Idr1 & 0xFF) ||
      Context < 0x2000 || Context >= 0x100000) return EFI_UNSUPPORTED;
  for (Index = 0; Index < Slots; Index++) {
    UINT32 Smr = MmioRead32 (Base + 0x800 + Index * 4);
    UINT32 Mask = (Smr >> 16) & 0x7FFF;
    UINT32 S2cr;
    if ((Smr & BIT31) == 0 || (((Smr & 0x7FFF) ^ 0x40) & ~Mask) != 0) continue;
    S2cr = MmioRead32 (Base + 0xC00 + Index * 4);
    if (Mask != 0 || (Smr & 0xFFFF) != 0x40 ||
        ((S2cr >> 16) & 3) != 0 || (S2cr & 0xFF) != Bank) return EFI_UNSUPPORTED;
    Matches++;
  }
  if (Matches != 1) return EFI_NOT_READY;
  Status = UsbfnMapPage (Base + Context, "usb-smmu-cb");
  if (EFI_ERROR (Status)) return Status;
  if ((MmioRead32 (Base + Context) & BIT0) != 0) return EFI_DEVICE_ERROR;
  Fsr = MmioRead32 (Base + Context + 0x58);
  Far = LShiftU64 ((UINT64)MmioRead32 (Base + Context + 0x64), 32) |
        MmioRead32 (Base + Context + 0x60);
  if (Baseline) {
    HaveBaseline = TRUE;
    BaselineContext = Base + Context;
    BaselineFsr = Fsr;
    BaselineFar = Far;
  }
  // Standard ARM SMMUv2 bits: MULTI/SS and faults [8:1]. Qualcomm's
  // observed bit10 (0x400) is outside this mask; do not call it historical.
  UsbfnClockSay ("[DMA b213] %a CB%u FSR=%08x known=%08x other=%08x FAR=%016lx SYNR0=%08x\n",
                 Phase, Bank, Fsr, Fsr & 0xC00001FEU, Fsr & ~0xC00001FEU,
                 Far, MmioRead32 (Base + Context + 0x68));
  if (!Baseline && HaveBaseline && BaselineContext == Base + Context) {
    UsbfnClockSay ("[DMA b213] delta fsr=%08x far-changed=%u; equal sticky faults cannot exclude recurrence\n",
                   Fsr ^ BaselineFsr, Far != BaselineFar);
  }
  // SS means a transaction is stalled. Do not attempt to resume it here.
  return Baseline && (Fsr & 0x40000000U) != 0 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

/**
  Raise or drop the QSCRATCH VBUS override, without touching anything else.

  @param[in] Enable  TRUE to assert UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL and
                     LANE0_PWR_PRESENT, FALSE to clear them.

  @retval EFI_SUCCESS  The bits read back in the requested state.
**/
EFI_STATUS
EFIAPI
UsbfnPlatformPhyVbusOverride (
  IN BOOLEAN  Enable
  )
{
  UINT32  Hs;
  UINT32  Ss;

  Hs = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_HS_PHY_CTRL);
  Ss = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_SS_PHY_CTRL);

  if (Enable) {
    MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_HS_PHY_CTRL,
                 Hs | QSCRATCH_UTMI_OTG_VBUS_VALID | QSCRATCH_SW_SESSVLD_SEL);
    MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_SS_PHY_CTRL,
                 Ss | QSCRATCH_LANE0_PWR_PRESENT);
  } else {
    MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_HS_PHY_CTRL,
                 Hs & ~(QSCRATCH_UTMI_OTG_VBUS_VALID | QSCRATCH_SW_SESSVLD_SEL));
    MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_SS_PHY_CTRL,
                 Ss & ~QSCRATCH_LANE0_PWR_PRESENT);
  }

  Hs = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_HS_PHY_CTRL);
  Ss = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_SS_PHY_CTRL);
  UsbfnClockSay ("[QSCR] vbus override %a -> hs=%08x ss=%08x\n",
                 Enable ? "on" : "off", Hs, Ss);

  return EFI_SUCCESS;
}

/**
  Pulse QSCRATCH_GENERAL_CFG and enable the UTMI pipe clock.

  Transcription of the device-mode block in UsbConfigDxe. The order is the vendor's:
  disable the pipe clock, select UTMI, switch the PHY status source to software, drop
  bit 9, wait 100 us, then re-enable the pipe clock and set bit 1 of QSCRATCH+0x28.

  @retval EFI_SUCCESS  PIPE_UTMI_CLK_DIS reads clear afterwards.
**/
STATIC
EFI_STATUS
UsbfnEnsurePipeClock (
  VOID
  )
{
  UINT32  Before;
  UINT32  After;
  UINT32  Reg;

  Before = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG);
  UsbfnClockSay ("[QSCR] general_cfg=%08x (utmi_clk_dis=%u sel=%u status_sw=%u)\n",
                 Before,
                 (Before & QSCRATCH_PIPE_UTMI_CLK_DIS) ? 1U : 0U,
                 (Before & QSCRATCH_PIPE_UTMI_CLK_SEL) ? 1U : 0U,
                 (Before & QSCRATCH_PIPE3_PHYSTATUS_SW) ? 1U : 0U);

  MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG,
               Before | QSCRATCH_PIPE_UTMI_CLK_DIS);

  //
  // Linux waits usleep_range(100, 1000) here and again before re-enabling the clock
  // (dwc3_qcom_select_utmi_clk, the code path behind qcom,select-utmi-as-pipe-clk).
  // This used to wait 10 us, which is one to two orders of magnitude short of what the
  // reference on sibling hardware allows for the PIPE clock to actually stop before it
  // is re-selected. A switch that is too fast can leave the core on the clock it was
  // trying to leave.
  //
  gBS->Stall (1000);

  Reg = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG);
  MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG,
               Reg | QSCRATCH_PIPE_UTMI_CLK_SEL);

  Reg = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG);
  MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG,
               Reg | QSCRATCH_PIPE3_PHYSTATUS_SW);

  Reg = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG);
  MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG,
               Reg & ~QSCRATCH_PIPE_RSVD9);

  gBS->Stall (1000);

  Reg = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG);
  MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG,
               Reg & ~QSCRATCH_PIPE_UTMI_CLK_DIS);

  Reg = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_REF_CLK_CTRL);
  MmioWrite32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_REF_CLK_CTRL, Reg | 2);

  After = MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_GENERAL_CFG);
  UsbfnClockSay ("[QSCR] general_cfg after=%08x (utmi_clk_dis=%u) ref_clk_ctrl=%08x\n",
                 After,
                 (After & QSCRATCH_PIPE_UTMI_CLK_DIS) ? 1U : 0U,
                 MmioRead32 ((UINTN)SHENNONG_QSCRATCH_BASE + QSCRATCH_REF_CLK_CTRL));

  return ((After & QSCRATCH_PIPE_UTMI_CLK_DIS) == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/**
  Read, and if necessary re-assert, the USB interface enable in TCSR.

  The read is logged before anything is written, so even if the write is refused by
  the security unit the captured log still carries the answer.

  @retval EFI_SUCCESS  The register reads 0xF, before or after the write.
**/
STATIC
EFI_STATUS
UsbfnEnsureUsbIntEnabled (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Value;

  Status = UsbfnMapPage (SHENNONG_TCSR_USB_DYN_EN_DIS, "tcsr-usb");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Value = MmioRead32 (SHENNONG_TCSR_USB_DYN_EN_DIS);
  UsbfnClockSay ("[TCSR] usb_int_dyn_en_dis=%08x enabled=%u (want f)\n",
                 Value, (Value == TCSR_USB_INT_ENABLED) ? 1U : 0U);

  if (Value == TCSR_USB_INT_ENABLED) {
    return EFI_SUCCESS;
  }

  MmioWrite32 (SHENNONG_TCSR_USB_DYN_EN_DIS, TCSR_USB_INT_ENABLED);
  MicroSecondDelay (100);
  Value = MmioRead32 (SHENNONG_TCSR_USB_DYN_EN_DIS);
  UsbfnClockSay ("[TCSR] after write %x: %08x enabled=%u\n",
                 (UINT32)TCSR_USB_INT_ENABLED, Value,
                 (Value == TCSR_USB_INT_ENABLED) ? 1U : 0U);

  return (Value == TCSR_USB_INT_ENABLED) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/**
  Make sure the USB30 controller is not held in block-level reset.

  Nothing in this port has ever looked at GCC_USB30_PRIM_BCR. The measurements say the
  controller's register window works - a value written to DCTL reads back - while
  nothing inside the core ever advances: DSTS is identical at 0, 1 ms, 50 ms and
  500 ms, and two independent reset engines, DCTL.CSFTRST and GCTL.CORESOFTRESET, both
  fail to self-clear. A block reset would produce precisely that.

  @param[in]  Gcc  Mapped GCC base.

  @retval EFI_SUCCESS  The block is out of reset, or was never in it.
**/
STATIC
EFI_STATUS
UsbfnEnsureUsbBlockReset (
  IN UINT64  Gcc
  )
{
  UINT32  Bcr;
  UINT32  Next;

  Bcr  = MmioRead32 ((UINTN)Gcc + SHENNONG_USB30_PRIM_BCR);
  Next = MmioRead32 ((UINTN)Gcc + SHENNONG_USB30_PRIM_BCR + 4);
  UsbfnClockSay ("[RST] usb30_prim_bcr=%08x in_reset=%u next=%08x\n",
                 Bcr, (Bcr & USB30_PRIM_BCR_RESET_N) ? 1 : 0, Next);

  if ((Bcr & USB30_PRIM_BCR_RESET_N) == 0) {
    return EFI_SUCCESS;
  }

  MmioWrite32 ((UINTN)Gcc + SHENNONG_USB30_PRIM_BCR, Bcr & ~USB30_PRIM_BCR_RESET_N);
  MicroSecondDelay (1000);
  UsbfnClockSay ("[RST] usb30_prim_bcr after deassert=%08x\n",
                 MmioRead32 ((UINTN)Gcc + SHENNONG_USB30_PRIM_BCR));

  return EFI_SUCCESS;
}

STATIC
VOID
UsbfnEnsurePhyPower (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Value;
  UINT32      Timeout;

  //
  // 1. The PHY's power domain.
  //
  Status = UsbfnMapPage (SHENNONG_PHY_GDSC_PAGE, "phy-gdsc");
  if (!EFI_ERROR (Status)) {
    Value = MmioRead32 ((UINTN)SHENNONG_PHY_GDSC_PAGE + SHENNONG_PHY_GDSC);
    UsbfnClockSay ("[GDSC] usb3_phy=%08x pwr_on=%u collapse=%u hw_ctl=%u\n",
                   Value, (Value >> 31) & 1, Value & 1, (Value >> 1) & 1);

    if (((Value & GDSC_PWR_ON) == 0) && ((Value & GDSC_HW_CONTROL) == 0)) {
      MmioWrite32 ((UINTN)SHENNONG_PHY_GDSC_PAGE + SHENNONG_PHY_GDSC,
                   Value & ~GDSC_SW_COLLAPSE);
      for (Timeout = 0; Timeout < 1000; Timeout++) {
        if ((MmioRead32 ((UINTN)SHENNONG_PHY_GDSC_PAGE + SHENNONG_PHY_GDSC) & GDSC_PWR_ON) != 0) {
          break;
        }

        MicroSecondDelay (1);
      }

      UsbfnClockSay ("[GDSC] usb3_phy after enable: %08x pwr_on=%u waited=%u us\n",
                     MmioRead32 ((UINTN)SHENNONG_PHY_GDSC_PAGE + SHENNONG_PHY_GDSC),
                     (MmioRead32 ((UINTN)SHENNONG_PHY_GDSC_PAGE + SHENNONG_PHY_GDSC) >> 31) & 1,
                     Timeout);
    }
  }

  //
  // 2. Reset the PHY and run its init sequence. Doing nothing here was the whole
  //    reason the PHY read as garbage: it had never been initialised.
  //
  //
  // 3. The PHY's reference clock, before the PHY is asked to do anything.
  //
  {
    UINT32  ClkRef;

    Status = UsbfnMapPage (SHENNONG_TCSRCC_USB2_CLKREF, "tcsrcc-usbclkref");
    if (!EFI_ERROR (Status)) {
      ClkRef = MmioRead32 (SHENNONG_TCSRCC_USB2_CLKREF);
      UsbfnClockSay ("[CLK] usb2 refclk was %08x, enabled=%a\n",
                     ClkRef, (ClkRef & 1) ? "YES" : "NO");

      if ((ClkRef & 1) == 0) {
        MmioWrite32 (SHENNONG_TCSRCC_USB2_CLKREF, ClkRef | 1);
        gBS->Stall (2000);
        ClkRef = MmioRead32 (SHENNONG_TCSRCC_USB2_CLKREF);
        UsbfnClockSay ("[CLK] usb2 refclk now %08x, enabled=%a\n",
                       ClkRef, (ClkRef & 1) ? "YES" : "NO");
      }

      ClkRef = MmioRead32 (SHENNONG_TCSRCC_USB3_CLKREF);
      UsbfnClockSay ("[CLK] usb3 refclk was %08x, enabled=%a\n",
                     ClkRef, (ClkRef & 1) ? "YES" : "NO");
      if ((ClkRef & 1) == 0) {
        MmioWrite32 (SHENNONG_TCSRCC_USB3_CLKREF, ClkRef | 1);
        gBS->Stall (2000);
        UsbfnClockSay ("[CLK] usb3 refclk now %08x, enabled=%a\n",
                       MmioRead32 (SHENNONG_TCSRCC_USB3_CLKREF),
                       (MmioRead32 (SHENNONG_TCSRCC_USB3_CLKREF) & 1) ? "YES" : "NO");
      }
    } else {
      UsbfnClockSay ("[CLK] tcsrcc map -> %r\n", Status);
    }
  }

  UsbfnInitPhy ();
}

/**
  Vote the USB-to-DDR interconnect through the same NPA client used by the
  stock Qualcomm USB configuration driver.

  The Linux SM8650 device tree describes the path as
  aggre1_noc MASTER_USB3_0 -> mc_virt SLAVE_EBI1 and the Qualcomm DWC3 glue
  driver votes it before controller DMA. UsbConfigDxe expresses the same vote
  through /icb/arbiter, client usb_ss1_bus. Without it, the APB register path
  remains alive while the first TRB fetch can wait forever on the data path.
**/
STATIC
EFI_STATUS
UsbfnVoteUsbInterconnect (
  VOID
  )
{
  EFI_STATUS        Status;
  VOID              *Npa;
  EFI_HANDLE        Node;
  USB_NPA_GET_NODE  GetNode;
  USB_NPA_VOTE_BUS  VoteBus;

  Npa  = NULL;
  Node = NULL;
  Status = gBS->LocateProtocol (&mUsbNpaProtocolGuid, NULL, &Npa);
  if (EFI_ERROR (Status) || (Npa == NULL)) {
    UsbfnClockSay ("[NPA] protocol -> %r\n", Status);
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  GetNode = *(USB_NPA_GET_NODE *)((UINT8 *)Npa + 0x10);
  VoteBus = *(USB_NPA_VOTE_BUS *)((UINT8 *)Npa + 0x90);
  if ((GetNode == NULL) || (VoteBus == NULL)) {
    UsbfnClockSay ("[NPA] invalid vtable get=0x%lx vote=0x%lx\n",
                   (UINT64)(UINTN)GetNode,
                   (UINT64)(UINTN)VoteBus);
    return EFI_UNSUPPORTED;
  }

  UsbfnClockSay ("[NPA] opening /icb/arbiter usb_ss1_bus\n");
  GetNode ("/icb/arbiter", "usb_ss1_bus", 0x400, 0x10,
           (VOID *)mUsbNpaClientConfig, &Node);
  UsbfnClockSay ("[NPA] usb_ss1_bus node=0x%lx\n", (UINT64)(UINTN)Node);
  if (Node == NULL) {
    return EFI_NOT_FOUND;
  }

  VoteBus (Node, 16, mUsbNpaBusVote);
  UsbfnClockSay ("[NPA] usb_ss1_bus vote issued (16 entries)\n");
  return EFI_SUCCESS;
}

/**
  True if any bit of the mask says a branch is still disabled.
**/
STATIC
BOOLEAN
UsbfnClockAnyDisabled (
  IN CONST CHAR8  *Mask
  )
{
  UINTN  Index;

  for (Index = 0; Index < 8; Index++) {
    if (Mask[Index] == '0') {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Turn on the DWC3 clocks before the core is touched.

  Why this exists: DCTL.CSFTRST latched but never self-cleared through a 500 ms poll, both
  with and without the PHY soft reset, while the register interface itself answered fine
  (GSNPSID, GCTL and DSTS all read sensible values). That split - CSR works, core reset
  hangs - is the signature of a controller whose bus clock is running and whose master
  clock is not, because CSFTRST completes in the core clock domain.

  On this port the clocks are normally enabled by UsbConfigDxe's InitUsbClocks, on the path
  that UsbDevStartDxe bypasses by driving UsbDevice->Start directly. The boot log also
  shows that module failing on the way up: the QcomUsbConfig connect returns Not Found, and
  UsbConfigLib_pmic_npa_create_sync_client - the RPMh client it would vote through - is
  printing a failure of its own.
**/
STATIC
VOID
UsbfnBringUpUsbClocks (
  VOID
  )
{
  EFI_STATUS    Status;
  USB_CLOCK_IF  *Clock;
  UINT64        Gcc;
  CHAR8         Before[12];
  CHAR8         After[12];
  CHAR8         HaltBefore[12];
  CHAR8         HaltAfter[12];
  UINTN         ClockId;
  UINTN         Index;
  UINTN         Failures;

  //
  // The PHY's own rails first, before any clock or core work.
  //
  // The GDSC readback in this same log says the USB core's power domain is up and all
  // eight branches are enabled with no halt bits - and CSFTRST still never cleared.
  // What was not powered is the PHY: hsphy@88e3000 takes vdd from ldoi1 and vdda12
  // from ldoi3, both rpmh-vrm-regulators whose only voter is the Linux kernel at probe
  // time. Its register block reads as repeated byte patterns, which is what an
  // unpowered block looks like, and an unpowered PHY produces no suspend clock for the
  // core to reset against.
  //
  //
  // Order matters and this one is taken from the vendor driver's cable-attach path,
  // which runs FUN_0000e998 (the QSCRATCH override) and only then
  // UsbfnDwcCoreInitImpl. The PHY has to see a valid session before it is initialised,
  // otherwise its own init runs with no VBUS and it comes up dark.
  //
  //   1. the TCSR gate, which is outside the controller entirely
  //   2. the QSCRATCH VBUS override, so the PHY sees a cable
  //   3. the PHY's rails, power domain, reset and init sequence
  //   4. the core's block reset, power domain and clocks
  //   5. the core's own soft reset
  //
  //
  // No SMMU probe here. Reading S2CR(0x40) at 0x15008800 takes a synchronous external
  // abort - ESR 0x96000010, DFSC 0x10 - which means the block does not answer at all:
  // unpowered, or held off by the security unit. Being listed in the platform memory
  // map only means the range is described, not that it can be touched, and this is the
  // second time that distinction has cost a boot.
  //
  //
  // Before anything programs an address into the controller. If UsbConfigDxe had come up
  // it would already have done this; it does not, so the USB master would otherwise have
  // no translation for the very first TRB fetch.
  //
  Status = UsbfnEnsureIommuDomain ();
  UsbfnClockSay ("[IOMMU] usb domain ready -> %r\n", Status);

  //
  // Turn on the root of the master clock source.
  //
  // The eight branch gates all read back as enabled, and the branch status says it is not
  // halted, but that only describes the branch. gcc_usb30_prim_master_clk_src is above them
  // and its CMD_RCGR reads 0x00000000 - ROOT_EN, bit 0, is clear, so the source is off. A
  // branch hanging off a stopped source is still a stopped clock; it just does not report
  // itself as halted.
  //
  // That fits the failure exactly. Register access to the controller goes through cfg_noc,
  // which is alive, so DEPCFG and every other CSR operation complete. The datapath and the
  // command engine need the master clock, so the first command that has to actually run -
  // STARTTRANSFER - never starts, and the APB write waits behind it forever.
  //
  {
    UINT32 RcgCmd;

    //
    // Bit positions from drivers/clk/qcom/clk-rcg2.c:
    //
    //   CMD_UPDATE  BIT(0)
    //   CMD_ROOT_EN BIT(1)
    //
    // The first attempt set bit 0, which is UPDATE, not ROOT_EN - the register read back
    // unchanged and nothing started. ROOT_EN is bit 1.
    //
    RcgCmd = MmioRead32 (SHENNONG_GCC_BASE + 0x3902C);
    UsbfnClockSay ("[CLK] master RCG before=0x%08x\n", RcgCmd);
    if ((RcgCmd & 0x2) == 0) {
      MmioWrite32 (SHENNONG_GCC_BASE + 0x3902C, RcgCmd | 0x2);
      gBS->Stall (200);
      UsbfnClockSay ("[CLK] master RCG after=0x%08x (ROOT_EN bit1 set)\n",
                     MmioRead32 (SHENNONG_GCC_BASE + 0x3902C));
    }
  }

  Status = UsbfnEnsureUsbIntEnabled ();
  UsbfnClockSay ("[TCSR] usb interface gate -> %r\n", Status);

  Status = UsbfnEnsureVbusOverride ();
  UsbfnClockSay ("[QSCR] vbus override -> %r\n", Status);

  Status = UsbfnEnsurePipeClock ();
  UsbfnClockSay ("[QSCR] utmi pipe clock -> %r\n", Status);

  Status = RpmhUsbPhyRailsOn ();
  UsbfnClockSay ("[RPMH] usb phy rails -> %r\n", Status);

  // The repeater has separate supplies from the SoC PHY. Resolve their VRM
  // addresses by name in CmdDb; values come from shennong's regulator phandles.
  Status = RpmhUsbRepeaterRailsOn ();
  UsbfnClockSay ("[RPMH] repeater rails-b212 -> %r\n", Status);
  if (!EFI_ERROR (Status)) {
    // Linux initializes the powered repeater before releasing the PHY's POR.
    // UsbDevStartDxe selected peripheral mode/HIZ; prepare tuning and device-force
    // state here after the supply votes. No additional reset in the link report.
    UsbfnRepeaterPrepare ();
  }

  //
  // The override stays asserted. Withdrawing it here was tried and it broke the core
  // reset outright: CSFTRST timed out with hs=00000000 ss=00000000, which is the same
  // state that hung every reset before the override was found in b127. The session has
  // to be valid for the core to run at all, so the link comes up early and stays up.
  //
  UsbfnEnsurePhyPower ();

  Gcc = SHENNONG_GCC_OBS_BASE;

  Status = gDS->SetMemorySpaceAttributes (
                  (EFI_PHYSICAL_ADDRESS)Gcc,
                  SHENNONG_GCC_OBS_SIZE,
                  EFI_MEMORY_UC
                  );
  if (EFI_ERROR (Status)) {
    //
    // Not fatal: the readback is a diagnostic, the clock enable is the fix.
    //
    UsbfnClockSay ("[CLK] map GCC -> %r\n", Status);
    Gcc = 0;
  }

  if (Gcc != 0) {
    UsbfnEnsureUsbBlockReset (Gcc);
    UsbfnEnsureUsbGdsc (Gcc);
    UsbfnClockEnableMask (Gcc, Before);
    UsbfnClockHaltMask (Gcc, HaltBefore);
    UsbfnClockSay ("[CLK] en before=%a halt=%a src=0x%08x/0x%08x\n",
                   Before,
                   HaltBefore,
                   MmioRead32 ((UINTN)Gcc + GCC_USB30_PRIM_MASTER_CMD_RCGR),
                   MmioRead32 ((UINTN)Gcc + GCC_USB30_PRIM_MASTER_CFG_RCGR));
  }

  Status = gBS->LocateProtocol (&mUsbClockProtocolGuid, NULL, (VOID **)&Clock);
  if (EFI_ERROR (Status) || (Clock == NULL)) {
    UsbfnClockSay ("[CLK] ClockProtocol -> %r\n", Status);
    return;
  }

  Failures = 0;
  for (Index = 0; Index < 8; Index++) {
    ClockId = 0;
    Status  = Clock->GetClockID (Clock, mUsbClockName[Index], &ClockId);
    if (EFI_ERROR (Status)) {
      UsbfnClockSay ("[CLK] id %a -> %r\n", mUsbClockName[Index], Status);
      Failures++;
      continue;
    }

    Status = Clock->EnableClock (Clock, ClockId);
    if (EFI_ERROR (Status)) {
      UsbfnClockSay ("[CLK] enable %a -> %r\n", mUsbClockName[Index], Status);
      Failures++;
    }
  }

  //
  // A GCC branch vote is not an interconnect bandwidth vote. The stock USB
  // driver performs this separate NPA transaction after enabling its clocks;
  // this port bypasses that driver, so reproduce the transaction here before
  // the DWC3 command engine can fetch its first TRB.
  //
#if USB_NPA_INTERCONNECT_VOTE
  Status = UsbfnVoteUsbInterconnect ();
  UsbfnClockSay ("[NPA] usb interconnect ready -> %r\n", Status);
#else
  //
  // Off, and not because it is hard to reach.
  //
  // The vote is a bandwidth request, not an enable. In Linux the equivalent icc_set_bw()
  // sets QoS on a path that is already usable; without it transfers still happen at the
  // default arbitration. Other masters in this same firmware - UFS, display - reach DDR
  // with no vote at all.
  //
  // It also crashes. With the vendor table and the vendor client descriptor in place,
  // ICBDxe faults at +0x3BE0 dereferencing 0x74B4B550 - a wild pointer that is in no
  // loaded image - reached through NpaDxe. So on this port the ICB vote path does not
  // hand back a node this code can use, and running it at boot turns a USB question into
  // a machine that never reaches the GUI.
  //
  // Kept, disabled, with the table realigned to the vendor bytes, in case the ICB
  // configuration turns out to be fixable later.
  //
  UsbfnClockSay ("[NPA] interconnect vote disabled at build time\n");

  //
  // Reference it so -Wunused-function stays quiet while the switch is off; the code is
  // kept compiled so it does not rot.
  //
  (VOID)UsbfnVoteUsbInterconnect;
#endif

  if (Gcc != 0) {
    UsbfnClockEnableMask (Gcc, After);
    UsbfnClockHaltMask (Gcc, HaltAfter);
    UsbfnClockSay ("[CLK] enable fail=%u en after=%a halt=%a src=0x%08x/0x%08x\n",
                   (UINT32)Failures,
                   After,
                   HaltAfter,
                   MmioRead32 ((UINTN)Gcc + GCC_USB30_PRIM_MASTER_CMD_RCGR),
                   MmioRead32 ((UINTN)Gcc + GCC_USB30_PRIM_MASTER_CFG_RCGR));

    //
    // Only if a branch is actually off. b72 showed all eight already on, so this normally
    // does nothing; it stays because the stock ClockDxe is in the tree again and its
    // result is not yet known. A clk_branch enables through enable_mask = BIT(0) of its
    // CBCR, which is the same write the kernel makes - what it cannot create is the RPMh
    // vote behind it, so the readback is the answer, not the write.
    //
    if (UsbfnClockAnyDisabled (After)) {
      for (Index = 0; Index < 8; Index++) {
        MmioWrite32 ((UINTN)Gcc + mUsbClockCbcr[Index],
                     MmioRead32 ((UINTN)Gcc + mUsbClockCbcr[Index]) | BIT0);
      }

      gBS->Stall (1000);
      UsbfnClockEnableMask (Gcc, After);
      UsbfnClockHaltMask (Gcc, HaltAfter);
      UsbfnClockSay ("[CLK] direct write en=%a halt=%a\n", After, HaltAfter);
    }
  } else {
    UsbfnClockSay ("[CLK] enable fail=%u (no readback)\n", (UINT32)Failures);
  }
}

EFI_STATUS
UsbfnPlatformGetData (
  OUT USBFN_PLATFORM_DATA  *PlatformData
  )
{
  if (PlatformData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (PlatformData, sizeof (*PlatformData));

  PlatformData->ControllerBase          = SHENNONG_DWC3_BASE;
  PlatformData->MaximumSpeed            = UsbBusSpeedHigh;
  PlatformData->UtmiWidth               = UsbfnUtmiWidth8Bit;
  PlatformData->Usb2FreeClockExists     = TRUE;
  PlatformData->FrameLengthAdjustMask   = 0;
  PlatformData->FrameLengthAdjustValue  = 0;
  PlatformData->SuperSpeedPhyDisabled   = TRUE;
  PlatformData->NumEndpoints            = 0;
  PlatformData->VendorId                = SHENNONG_USB_VID;
  PlatformData->ProductId               = SHENNONG_USB_PID;

  return EFI_SUCCESS;
}

/**
  Read a few words of the eUSB2 PHY block.

  The controller's core reset completes in a clock domain fed by this PHY, and the only
  thing this port has ever verified about it is that UsbConfigDxe said it was fine. If
  the PHY block is unpowered its registers read as zero or all-ones, which would explain
  a CSFTRST that latches and never clears while the bus-side CSR keeps working.

  Guarded like every other MMIO in this port: the range is declared and marked uncached
  first, and nothing is read if that fails.
**/
STATIC
VOID
UsbfnProbePhy (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  UINT64                           Base = 0x088E3000ULL;

  ZeroMem (&Desc, sizeof (Desc));
  Status = gDS->GetMemorySpaceDescriptor ((EFI_PHYSICAL_ADDRESS)Base, &Desc);
  if (EFI_ERROR (Status) || (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, (EFI_PHYSICAL_ADDRESS)Base, 0x1000, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      UsbfnClockSay ("[PHY] add -> %r\n", Status);
      return;
    }
  }

  Status = gDS->SetMemorySpaceAttributes ((EFI_PHYSICAL_ADDRESS)Base, 0x1000, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    UsbfnClockSay ("[PHY] attr -> %r\n", Status);
    return;
  }

  UsbfnClockSay ("[PHY] eusb2 @0x%lx: %08x %08x %08x %08x | %08x %08x %08x %08x\n",
                 Base,
                 MmioRead32 ((UINTN)Base + 0x00),
                 MmioRead32 ((UINTN)Base + 0x04),
                 MmioRead32 ((UINTN)Base + 0x08),
                 MmioRead32 ((UINTN)Base + 0x0C),
                 MmioRead32 ((UINTN)Base + 0x10),
                 MmioRead32 ((UINTN)Base + 0x40),
                 MmioRead32 ((UINTN)Base + 0x44),
                 MmioRead32 ((UINTN)Base + 0x48));

  //
  // POR (BIT1 of 0x50) and PHY_ENABLE (BIT0 of 0x54) are what
  // drivers/usb/phy/phy-msm-snps-eusb2.c waits on after powering the PHY.
  // 0x64 carries USB2_SUSPEND_N; these four are the whole "is the PHY alive"
  // question that the earlier probe did not ask.
  //
  UsbfnClockSay ("[PHY] ctrl: 50=%08x 54=%08x 64=%08x 94=%08x\n",
                 MmioRead32 ((UINTN)Base + 0x50),
                 MmioRead32 ((UINTN)Base + 0x54),
                 MmioRead32 ((UINTN)Base + 0x64),
                 MmioRead32 ((UINTN)Base + 0x94));
}

EFI_STATUS
UsbfnPlatformPhyInit (
  VOID
  )
{
  //
  // The eUSB2 PHY on this board is brought up by UsbConfigDxe and UsbDevStartDxe before
  // this driver runs: the repeater in PM8550B is enabled over SPMI and the PHY reports
  // ssusb_phy_init_success in the boot log. Nothing is left for the DWC3 driver to do
  // about the PHY itself.
  //
  // The clocks are a different matter: they are normally enabled by UsbConfigDxe's
  // InitUsbClocks, on the path this port bypasses, so they are enabled here instead.
  //
  //
  // The PHY probe that used to run here moved into UsbfnBringUpUsbClocks, after the
  // rails, the PHY's power domain and its reset have been dealt with. Reading it first
  // only ever produced a pre-power snapshot.
  //
  UsbfnBringUpUsbClocks ();
  UsbfnPlatformDmaReport ();
  {
    extern VOID RpmhUsbNocReport (VOID);
    RpmhUsbNocReport ();
  }

  return EFI_SUCCESS;
}

EFI_STATUS
UsbfnPlatformPhyExit (
  VOID
  )
{
  return EFI_SUCCESS;
}

BOOLEAN
UsbfnPlatformIsVbusPresent (
  VOID
  )
{
  //
  // Report present. The Type-C state machine in UsbConfigDxe already gates on a real
  // attach before this driver is started, and returning FALSE here would make the DWC3
  // driver tear the link down immediately after setting it up.
  //
  return TRUE;
}

/**
  Report and, if set, clear the Embedded USB Debugger enable bit.

  Only 0x088E2000 is touched. The companion eud_detect register at 0x0C276000 is NOT
  read: b208 mapped it and took a synchronous exception at FAR=0x0C276000 (EC=0x25,
  ISS=0x10), because a successful page map does not make the address reachable. That
  register adds nothing - bit 0 of the enable register already answers the question.
**/
VOID
EFIAPI
UsbfnPlatformEudReport (
  VOID
  )
{
  UINT32  Enable;

  if (EFI_ERROR (UsbfnMapPage (SHENNONG_EUD_ENABLE, "eud-enable"))) {
    UsbfnClockSay ("[EUD] 0x088E2000 not mappable\n");
    return;
  }

  Enable = MmioRead32 ((UINTN)SHENNONG_EUD_ENABLE);
  UsbfnClockSay ("[EUD] enable=%08x active=%u\n", Enable, Enable & 1);

  if ((Enable & 1) != 0) {
    MmioWrite32 ((UINTN)SHENNONG_EUD_ENABLE, Enable & ~1u);
    gBS->Stall (1000);
    UsbfnClockSay ("[EUD] cleared bit0 -> %08x\n",
                   MmioRead32 ((UINTN)SHENNONG_EUD_ENABLE));
  }
}

/**
  Report the PHY handoff state without changing UTMI ownership or SLEEPM.
**/
VOID
EFIAPI
UsbfnPlatformPhyAttachReport (
  VOID
  )
{
  UINT32  Ctrl0;
  UINT32  Cfg0;

  if (EFI_ERROR (UsbfnMapPage (SHENNONG_PHY_BASE, "eusb2-phy-attach"))) {
    return;
  }

  // Stock UsbConfigDxe @ e3a0 writes SLEEPM=1, as does
  // msm_eusb2_phy_init(). Clearing it is not a verified wake operation.
  // The old probe also left CFG0.bit1 and TERMSEL forced during USB TEST.
  Ctrl0 = MmioRead32 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_UTMI_CTRL0);
  Cfg0  = MmioRead32 ((UINTN)SHENNONG_PHY_BASE + PHY_REG_CFG0);
  UsbfnClockSay ("[PHY] handoff-b211 read-only 3C=%08x sleepm=%u termsel=%u 94=%08x\n",
                 Ctrl0, Ctrl0 & 1, (Ctrl0 >> 5) & 1, Cfg0);
}
