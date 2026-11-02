/** @file

  Hardware constants for the shennong Synaptics S3910P touch controller.

  Everything here comes from the device's own device tree
  (/soc/qcom,qupv3_1_geni_se@ac0000/spi@a90000/synaptics_tcm@0) and from the
  Synaptics TCM driver source, not from guesswork. The GENI SPI register block is
  the one uncertain part and is documented as such in GeniSpi.c.

**/

#ifndef _TOUCH_HW_H_
#define _TOUCH_HW_H_

//
// The build this driver was compiled from, printed on its own first line.
//
// It exists because a log was read against the wrong image once: b101 changed the
// reset drive strength and the pasted log still showed the old value, which cost a
// whole round trip to notice. The probe driver prints a tag too, but that line is
// nowhere near the touch output and is easy to cut off when pasting. Keeping the
// same string in both places means the touch section identifies itself.
//
#define TOUCH_BUILD_TAG         "b200-wcd-route"

//
// QUPv3_1 serial engine 4, which the device tree gives as spi@a90000 with
// qcom,spi-geni and a 0x4000 window. The I2C engine this port already drives is
// the same GENI block at 0xA8C000, so the region and the clock gate family are
// known to work.
//
#define TOUCH_SE_BASE           0x000A90000UL
#define TOUCH_SE_LEN            0x4000U

//
// The I2C engine next door, on SE3. This port already drives it successfully
// through I2CDxe, which makes it the only reference in the system that is known
// to answer: reading the same offsets here and there separates "the driver is
// asking for the wrong thing" from "this particular engine is not powered".
//
#define I2C_SE_BASE             0x00A8C000UL
#define I2C_SE_LEN              0x4000U

//
// The SE's own clock, from the device tree: spi@a90000 has
//
//   clocks = <&gcc 0x2c 0x6c>        clock-names = "se-clk"
//
// so the GCC clock id is 0x6C. The I2C engine this port already drives is
// i2c@a8c000 with clock id 0x6A, one step below - useful as a reference when
// deciding whether a dead register block means "clock off" or "wrong offset".
//
#define TOUCH_SE_CLK_ID         0x6C
#define I2C_SE_CLK_ID           0x6A

//
// GCC, so the SE clock can actually be turned on.
//
// The device tree says clock id 0x6C, and that numbering is the downstream
// Qualcomm one, not mainline's. In downstream gcc-sm8650 the QUP clocks come in
// CLK / CLK_SRC pairs, and the DTB uses 0x6A for the I2C engine on SE3 and 0x6C
// for the SPI engine on SE4 - one pair apart, which pins 0x6C to
// GCC_QUPV3_WRAP1_S4_CLK.
//
// The offsets come from mainline drivers/clk/qcom/gcc-sm8650.c, where S3 and S4
// are two adjacent bits of the same branch register:
//
//   gcc_qupv3_wrap1_s3_clk  enable_reg 0x52008  mask BIT(25)  halt 0x18284
//   gcc_qupv3_wrap1_s4_clk  enable_reg 0x52008  mask BIT(26)  halt 0x183bc
//   gcc_qupv3_wrap1_s4_clk_src  cmd_rcgr 0x183c8
//
// Two neighbouring bits in one register is the kind of detail that is hard to
// get right by accident, so the S3/S4 pairing is taken as confirmation.
//
// This matters because the SE clock is NOT on by default: I2CDxe has to enable
// SE3's clock itself before its engine answers, which is the reason this driver
// cannot skip the step and expect the register dump to be anything but zero.
//
#define GCC_BASE                0x00100000UL
#define GCC_LEN                 0x00200000UL
#define GCC_QUPV3_WRAP1_CBCR    0x00052008UL
#define GCC_QUPV3_WRAP1_S4_EN   (1U << 26)
#define GCC_QUPV3_WRAP1_S3_EN   (1U << 25)
#define GCC_QUPV3_WRAP1_S4_HALT 0x000183BCUL
#define GCC_QUPV3_WRAP1_S4_RCG  0x000183C8UL
//
// RCG2 register block: CMD_RCGR(+0) holds ROOT_EN in BIT(1); CFG_RCGR(+4) the
// source select, D(+0x10) the divider. These offsets and bits come from the
// downstream gcc-pineapple.c / clk-rcg2.c the phone actually runs. An earlier
// version wrote BIT(0) to +0x10, which is the D divider - the root never turned
// on, and the branch halt bit never cleared.
//
#define GCC_RCG_CMD             0x00000000UL
#define GCC_RCG_ROOT_EN         (1U << 1)
#define GCC_RCG_CFG             0x00000004UL
#define GCC_RCG_D               0x00000010UL
//
// CFG_RCGR fields: source select in [2:0], divider in [12:8].
//
// The vendor table (ftbl_gcc_qupv3_wrap1_s0_clk_src, which this clock uses)
// spells the slowest entry out as
//
//   F(19200000, P_BI_TCXO, 1, 0, 0)
//
// - parent BI_TCXO, pre_div 1 - and pre_div is what gets written into SRC_DIV.
// One is therefore the minimum valid divider; writing zero leaves the RCG on a
// setting the table never produces, the clock does not run, and every access to
// the engine returns a bus error instead of a register value.
//
#define GCC_RCG_SRC_SEL_MASK    0x00000007UL
#define GCC_RCG_SRC_DIV_SHIFT   8
#define GCC_RCG_SRC_DIV(div)    (((UINT32)(div) << GCC_RCG_SRC_DIV_SHIFT) & 0x00001F00UL)
//
// Downstream clk-branch.c: BRANCH_CLK_OFF is BIT(31) of the halt register.
// 0x183BC bit14 is not the off bit; reading "halt=0x4000" means the clock is
// running, not stuck.
//
#define GCC_QUPV3_WRAP1_S4_CLKOFF (1U << 31)

//
// Two small windows rather than the whole 2 MB GCC block. The registers needed
// are 1.3 MB apart, and a single AddMemorySpace spanning both would collide with
// descriptors the clock driver already owns - which turns a working access into
// an EFI_ACCESS_DENIED for no benefit.
//
#define GCC_CBCR_WIN            (GCC_BASE + 0x00050000UL)
#define GCC_CBCR_WIN_LEN        0x00004000UL
#define GCC_RCG_WIN             (GCC_BASE + 0x00180000UL)
#define GCC_RCG_WIN_LEN         0x00004000UL

//
// The whole QUP register region as uefiplat.cfg declares it, used as a fallback
// when the 4 KB window next to I2CDxe's cannot be added on its own.
//
#define QUP_REGION_BASE         0x00800000UL
#define QUP_REGION_LEN          0x00300000UL

//
// SE registers.
//
// The first version of this file used 0x00/0x04/0x0C for FORCE_DEFAULT,
// OUTPUT_CTRL and STATUS and 0x40/0x60 for M_CMD0/M_IRQ_STATUS. Every one of
// those was wrong: that is a different QUP generation's layout. The offsets
// below come from include/linux/soc/qcom/geni-se.h (v6.6) and from
// drivers/soc/qcom/qcom-geni-se.c, and they are independently confirmed by the
// I2CDxe disassembly in reverse/src/I2CDxe.c, which drives the working I2C engine
// at 0xA8C000 on this very board - it writes 0x604 to abort, polls 0x610, and
// masks 0x618.
//
#define SE_GENI_FORCE_DEFAULT   0x0020
#define SE_GENI_OUTPUT_CTRL     0x0024
#define SE_GENI_CGC_CTRL        0x0028
#define SE_GENI_STATUS          0x0040
#define GENI_FW_REVISION_RO     0x0068
#define SE_GENI_CFG_SEQ_START   0x0084
#define SE_GENI_BYTE_GRAN       0x0254
#define SE_GENI_DMA_MODE_EN     0x0258
#define SE_GENI_TX_PACKING_CFG0 0x0260
#define SE_GENI_TX_PACKING_CFG1 0x0264
#define SE_GENI_RX_PACKING_CFG0 0x0284
#define SE_GENI_RX_PACKING_CFG1 0x0288
#define SE_GENI_M_CMD0          0x0600
#define SE_GENI_M_CMD_CTRL      0x0604
#define SE_GENI_M_IRQ_STATUS    0x0610
#define SE_GENI_M_IRQ_EN        0x0614
#define SE_GENI_M_IRQ_CLEAR     0x0618
#define SE_GENI_TX_FIFO         0x0700
#define SE_GENI_RX_FIFO         0x0780
#define SE_GENI_TX_FIFO_STATUS  0x0800
#define SE_GENI_RX_FIFO_STATUS  0x0804
#define SE_GENI_TX_WATERMARK    0x080C
#define SE_GENI_RX_WATERMARK    0x0810
#define GENI_IF_DISABLE_RO      0x0064
#define SE_GENI_IOS             0x0908
#define SE_GENI_S_IRQ_CLEAR     0x0648
#define SE_GSI_EVENT_EN         0x0E18
#define SE_IRQ_EN               0x0E1C
#define SE_HW_PARAM_0           0x0E24
#define SE_HW_PARAM_1           0x0E28
#define SE_DMA_GENERAL_CFG      0x0E30
#define SE_DMA_TX_IRQ_CLR       0x0C44
#define SE_DMA_RX_IRQ_CLR       0x0D44

#define SE_SPI_CPHA             0x0224
#define SE_SPI_LOOPBACK         0x022C
#define SE_SPI_CPOL             0x0230
#define SE_SPI_DEMUX_OUTPUT_INV 0x024C
#define SE_SPI_DEMUX_SEL        0x0250
#define SE_SPI_TRANS_CFG        0x025C
#define SE_SPI_WORD_LEN         0x0268
#define SE_SPI_TX_TRANS_LEN     0x026C
#define SE_SPI_RX_TRANS_LEN     0x0270
#define SE_SPI_PRE_POST_CMD_DLY 0x0274
#define SE_SPI_DELAY_COUNTERS   0x0278

//
// Protocol value in GENI_FW_REVISION_RO bits [15:8], from enum
// geni_se_protocol_type: NONE 0, SPI 1, UART 2, I2C 3, I3C 4, SPI_SLAVE 5.
// Reading this back is how the driver confirms it is looking at an SPI engine and
// not at a stale offset - the I2C driver does the same check with 0x300.
//
#define GENI_PROTO_SPI          0x01
#define GENI_PROTO_I2C          0x03
#define GENI_PROTO_MASK         0xFF00
#define GENI_PROTO_SHIFT        8

//
// GENI master command opcodes, from spi-geni-qcom.c. These are not the low byte
// of M_CMD0 - the opcode lives in bits [31:27].
//
#define SPI_TX_ONLY             0x01
#define SPI_RX_ONLY             0x02
#define SPI_TX_RX               0x07
#define SPI_CS_ASSERT           0x08
#define SPI_CS_DEASSERT         0x09

#define M_OPCODE_SHIFT          27
#define M_PARAMS_MASK           0x07FFFFFFU

//
// FRAGMENTATION is BIT(2) of the parameter field; spi-geni-qcom.c passes it on
// every transfer, so it is not optional.
//
#define GENI_M_PARAM_FRAGMENTATION 0x00000004U

//
// M_IRQ_STATUS bits, from geni-se.h M_*_EN. Only CMD_DONE is needed: this driver
// polls and never enables the line to the GIC.
//
// Written as literals rather than BIT0/BIT1 because this header is included
// before <Uefi.h> in every translation unit, and Base.h has not been seen yet.
//
#define M_CMD_DONE              0x00000001U
#define M_CMD_OVERRUN           0x00000002U
#define M_ILLEGAL_CMD           0x00000004U
#define M_CMD_FAILURE           0x00000008U
#define M_RX_FIFO_WATERMARK     0x04000000U
#define M_RX_FIFO_LAST          0x08000000U
#define M_TX_FIFO_WATERMARK     0x40000000U

#define M_GENI_CMD_ACTIVE       0x00000001U

//
// TLMM (SoC pin controller). GPIO 0xA1 resets the controller and 0xA2 is its
// interrupt line, both straight from the device tree:
//
//   synaptics,reset-gpio = <0xc5 0xa1 0x00>     pin 0xA1
//   synaptics,irq-gpio   = <0xc5 0xa2 0x2008>   pin 0xA2, falling edge
//
//
// TLMM. The device tree names it pinctrl@f000000, which is 0x0F000000 - an
// earlier version of this file had 0x00F00000 and was wrong by a factor of 16.
//
// Two things about the pineapple layout are not the generic msm layout, and both
// were wrong here before:
//
//   #define REG_BASE 0x100000          <- pins do NOT start at offset 0
//   #define REG_SIZE 0x1000
//   ctl_reg = REG_BASE + REG_SIZE * id
//   mux_bit = 2   pull_bit = 0   drv_bit = 6   oe_bit = 9
//
// Pins therefore live at 0x100000 + pin * 0x1000. Computing pin * 0x1000 alone
// put gpio48 at 0x0F030000, which is in the hole between the block's own
// registers and the first pin tile - reading it is a bus error, and on this
// platform a bus error is a reset with no message. That is exactly what happened.
//
// The field layout comes from the PINGROUP macro in pinctrl-pineapple.c and the
// write in msm_pinmux_set_mux(), which does "val |= i << g->mux_bit" where i is
// the index of the function in that pin's funcs[] array. For gpio48 the array is
// { gpio, qup1_se4_l0, ibi_i3c, ... }, so qup1_se4_l0 is index 1 and lands in
// bits [5:2] as 0x4 - not as 0x1 in the low bits, which is GPIO plus a pull-down.
//
#define TLMM_BASE               0x0F000000UL
#define TLMM_LEN                0x00400000UL
#define TLMM_PAGE               0x1000UL
#define TLMM_GPIO_BASE          0x00100000UL
#define TLMM_GPIO_CFG(pin)      (TLMM_GPIO_BASE + (pin) * TLMM_PAGE + 0x0UL)
#define TLMM_GPIO_IN_OUT(pin)   (TLMM_GPIO_BASE + (pin) * TLMM_PAGE + 0x4UL)
#define TLMM_GPIO_INTR_CFG(pin) (TLMM_GPIO_BASE + (pin) * TLMM_PAGE + 0x8UL)
#define TLMM_GPIO_INTR_STATUS(pin) (TLMM_GPIO_BASE + (pin) * TLMM_PAGE + 0xCUL)

#define TLMM_CFG_MUX(func)      ((UINT32)(func) << 2)
#define TLMM_CFG_FUNC_GPIO      TLMM_CFG_MUX (0)
#define TLMM_CFG_FUNC_QUP1_SE4  TLMM_CFG_MUX (1)

#define TLMM_CFG_PULL_DISABLE   (0U << 0)
#define TLMM_CFG_PULL_DOWN      (1U << 0)
#define TLMM_CFG_PULL_UP        (3U << 0)

#define TLMM_CFG_DRV_2MA        (0U << 6)
#define TLMM_CFG_DRV_6MA        (2U << 6)
#define TLMM_CFG_DRV_8MA        (3U << 6)
//
// Without this the pin stays an input and writes to GPIO_IN_OUT go nowhere -
// the reset line would never move and the whole bring-up would look like a dead
// controller rather than a pin that was never driven.
//
#define TLMM_CFG_OE             (1U << 9)

//
// GPIO_IN_OUT bit positions for this SoC. pinctrl-pineapple.c declares
//
//   .oe_bit = 9, .in_bit = 0, .out_bit = 1
//
// so the output level is bit 1 and the input level is bit 0 - the opposite of what
// code written for bit 0 assumes. The first reset sequence here wrote 1 to bit 0,
// which is the input field: the output bit stayed 0 and the controller was held in
// reset for the entire boot.
//
#define TLMM_IO_OUT_MASK        (1U << 1)
#define TLMM_IO_IN_MASK         (1U << 0)

//
// One mapping length shared by every caller. Pins go up to 210, so the last one
// sits at 0x100000 + 210 * 0x1000 = 0x1D2000; 2 MB covers the lot with room to
// spare and keeps the range inside the 4 MB the device tree gives the block.
//
#define TLMM_MAP_LEN            0x00200000UL

//
// The touch pins, from qts,trusted-touch-vm-gpio-list in the device tree:
//
//   <0xc5 0x30 0x0> <0xc5 0x31 0x0> <0xc5 0x32 0x0> <0xc5 0x33 0x0>
//   <0xc5 0xa1 0x0> <0xc5 0xa2 0x2008>
//
// The first four are the SE4 SPI lines - that list exists so the trusted-touch
// VM can program the pins itself, which makes it a reliable statement about
// which pins carry SPI. The remaining two are reset and interrupt.
//
#define TOUCH_SPI_PIN_0         0x30U
#define TOUCH_SPI_PIN_1         0x31U
#define TOUCH_SPI_PIN_2         0x32U
#define TOUCH_SPI_PIN_3         0x33U
#define TOUCH_RESET_PIN         0xA1U
#define TOUCH_IRQ_PIN           0xA2U

//
// AVDD is not a PMIC rail on this board. The device tree declares it as a
// regulator-fixed with a GPIO enable:
//
//   touch_avdd_vreg {
//       compatible = "regulator-fixed";
//       enable-active-high;
//       gpio = <0xc5 0xa4 0x0>;        // TLMM 0xA4, active high
//   };
//
// Nothing in UEFI ever drove that pin, so the controller has been unpowered the
// whole time - which is also why its MISO and IRQ lines read low through the
// pull-ups: an unpowered chip clamps them through its ESD diodes. IOVDD is
// pm_humu_l12, which is shared with periph-1cc and vddio, so it is already up.
//
// synaptics,power-delay-ms = 50 comes from the same node.
//
#define TOUCH_AVDD_PIN          0xA4U
#define TOUCH_POWER_DELAY_MS    50

//
// Reset timing, from the device tree:
//   synaptics,reset-active-ms = 10
//   synaptics,reset-delay-ms  = 200
//   synaptics,power-delay-ms  = 50
//
#define TOUCH_RESET_ACTIVE_MS   10
#define TOUCH_RESET_DELAY_MS    200

//
// Synaptics TCM protocol, from the driver source (synaptics_tcm_core.h,
// synaptics_tcm_touch.c). The transport is trivial:
//
//   write : [command][payload...]                      no framing at all
//   read  : send 0xFF x N, receive [0xA5][code][len lo][len hi][payload...]
//
#define TCM_MARKER              0xA5
#define TCM_PADDING             0x5A
#define TCM_HEADER_SIZE         4
//
// The TCM SPI transport is full-duplex: a read clocks out two dummy 0xFF bytes
// on MOSI, and the [0xA5][code][len lo][len hi] header starts two bytes into
// the received stream (synaptics_tcm_spi.c: TCM_SPI_READ_DUMMY_BYTES = 2). A
// pure RX_ONLY transfer leaves MOSI at its idle level and the controller never
// produces a response.
//
#define TCM_SPI_READ_DUMMY_BYTES 2

//
// Bytes a write adds in front of the caller's payload: the command byte plus the
// two payload-length bytes. syna_tcm_write_message() sends
// [command][len lo][len hi][payload] and treats the two length bytes as part of
// the payload, so an empty command is three bytes wide.
//
#define TCM_WRITE_OVERHEAD      3

#define TCM_CMD_IDENTIFY                0x02
#define TCM_CMD_RESET                   0x04
#define TCM_CMD_GET_APPLICATION_INFO    0x20
#define TCM_CMD_GET_TOUCH_REPORT_CONFIG 0x25
#define TCM_CMD_SET_TOUCH_REPORT_CONFIG 0x26

#define TCM_REPORT_IDENTIFY     0x10
#define TCM_REPORT_TOUCH        0x11
#define TCM_REPORT_APPLICATION_INFO 0x20
#define TCM_REPORT_TOUCH_REPORT_CONFIG 0x25

#define TCM_STATUS_IDLE         0x00
#define TCM_STATUS_OK           0x01
#define TCM_STATUS_BUSY         0x02
#define TCM_STATUS_CONTINUED    0x03
#define TCM_STATUS_ERROR        0x0F
#define TCM_STATUS_INVALID      0xFF

//
// Report descriptor opcodes. Only the ones needed for the simplified layout are
// named; the full enum runs to 0x1E with the tuning group at 0x80.
//
#define TOUCH_END                       0x00
#define TOUCH_FOREACH_ACTIVE_OBJECT     0x01
#define TOUCH_FOREACH_OBJECT            0x02
#define TOUCH_FOREACH_END               0x03
#define TOUCH_PAD_TO_NEXT_BYTE          0x04
#define TOUCH_OBJECT_N_INDEX            0x06
#define TOUCH_OBJECT_N_CLASSIFICATION   0x07
#define TOUCH_OBJECT_N_X_POSITION       0x08
#define TOUCH_OBJECT_N_Y_POSITION       0x09
#define TOUCH_OBJECT_N_Z                0x0A
#define TOUCH_NUM_OF_ACTIVE_OBJECTS     0x17

//
// The layout this driver asks for, mirroring touch_set_report_config() in the
// Linux driver. Each active object costs exactly 4 bytes:
//
//   bits  0.. 3  object index
//   bits  4.. 7  classification
//   bits  8..19  X, 12 bits
//   bits 20..31  Y, 12 bits
//
// Asking for this layout is what removes the need for a descriptor interpreter:
// the positions are known because we chose them.
//
#define TCM_REPORT_OBJECT_BYTES 4

//
// Descriptor tokens, as used by the interpreter that decodes the controller's own
// report descriptor. The names below are the ones the reference port uses and they are
// confirmed by the descriptor this device returns:
//
//   10 08 1B 30 16 04 1E 04 12 10 20 10 21 10 22 04 23 04 25 10
//   18 08                          <- active object count, 8 bits
//   01 06 04 07 04 08 10 09 10 ...  <- per object: index 4, class 4, X 16, Y 16
//   03                             <- end of loop
//   00                             <- end of descriptor
//
// Note 0x18, not 0x17: TOUCH_NUM_OF_ACTIVE_OBJECTS above is 0x17, which does not
// appear in this descriptor at all. The reference port's parser keys on 0x18 and the
// bytes above say it is right.
//
#define TCM_DESC_FOREACH_ACTIVE 0x01
#define TCM_DESC_FOREACH_OBJECT 0x02
#define TCM_DESC_FOREACH_END    0x03
#define TCM_DESC_PAD            0x04
#define TCM_DESC_INDEX          0x06
#define TCM_DESC_CLASS          0x07
#define TCM_DESC_X              0x08
#define TCM_DESC_Y              0x09
#define TCM_DESC_NUM_ACTIVE     0x18

//
// Object classifications. Only these two are contacts; the descriptor's class field
// also carries hover, stylus and palm codes, which must not be reported as touches.
//
#define TCM_OBJECT_FINGER       1
#define TCM_OBJECT_GLOVE        2

#define TCM_MAX_CONTACTS        10
#define TCM_DESCRIPTOR_MAX      128

//
// Polling and buffer sizes.
//
#define TCM_READ_LENGTH         256
#define TCM_READ_ATTEMPTS       16
#define TCM_POLL_ITERATIONS     400
#define TCM_POLL_DELAY_US       50

//
// How long to wait for a command's answer. The reference port on this board polls
// 100 times at 10 ms before giving up, and the controller does take tens of
// milliseconds to answer IDENTIFY straight after reset - four polls, which is what
// this driver used before, is under a millisecond and cannot see it.
//
#define TCM_COMMAND_ATTEMPTS    100

//
// A command gets three write attempts, each with this many read polls. Three is not
// superstition: a command frame is three bytes on a software clocked bus, and one
// missed frame looks exactly like a controller that ignored the command.
//
#define TCM_CMD_POLLS           40

//
// Bytes read in the single-transaction path: header, room for a 128-byte payload, and
// the five-byte terminator. Enough for the largest reply this driver asks for - the
// application info is 56 bytes - and small enough that one read costs about 3 ms.
//
#define TCM_BLOCK_READ          (TCM_HEADER_SIZE + 128 + 7)

//
// Largest single SPI transfer. The TCM read is TCM_READ_LENGTH plus the dummy
// bytes the transport clocks out first, so the limit has to be above the sum and
// not equal to TCM_READ_LENGTH: a guard of 256 against a 258-byte request made
// every read fail with EFI_INVALID_PARAMETER before it touched the hardware.
//
#define TOUCH_SPI_MAX_XFER      512

#endif // _TOUCH_HW_H_
