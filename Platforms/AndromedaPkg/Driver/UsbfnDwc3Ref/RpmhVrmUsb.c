/** @file

  RPMh VRM voter for the SoC eUSB2 PHY and PM8550B repeater supplies.

  This is the same voter the touch driver uses - it is the piece that finally turned
  PMIC L12 on after twenty builds of trying to read the rail back over SPMI, and the
  one that works. It lives here as a copy rather than as a shared library because the
  two drivers are separate binaries and this tree has no library slot for it; the
  duplication is deliberate and small, and the alternative was not having USB PHY
  power at all.

  What it is for: shennong_fdt.dts, hsphy@88e3000, declares

    vdd-supply     = <&pm_v8_l1>;   ldoi1, 880000 uV, qcom,init-voltage
    vdda12-supply  = <&pm_v8_l3>;   ldoi3, 1200000 uV, qcom,init-voltage

  Linux requests these voltages at probe time. This port now makes explicit votes
  using CmdDb addresses. Inherited bootloader votes may already be active: neither
  a missing explicit vote nor repeated-byte register reads prove a rail is off.
  Completion is a request acknowledgment, not a measured output voltage.

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>

#define RPMH_DRV_BASE           0x17A20000UL
#define RPMH_DRV_MAP_LEN        0x2000UL
#define RPMH_DRV_ID             2
#define RPMH_TCS_OFFSET         0xD00UL
#define RPMH_TCS_STRIDE         0x2A0
#define RPMH_REG_DRV_CONFIG     0x0C
#define RPMH_REG_IRQ_ENABLE     0x00
#define RPMH_REG_IRQ_STATUS     0x04
#define RPMH_REG_IRQ_CLEAR      0x08
#define RPMH_REG_CMD_WAIT       0x20
#define RPMH_REG_CONTROL        0x24
#define RPMH_REG_STATUS         0x28
#define RPMH_REG_CMD_ENABLE     0x2C
#define RPMH_REG_CMD_MSGID      0x34
#define RPMH_REG_CMD_ADDR       0x38
#define RPMH_REG_CMD_DATA       0x3C
#define RPMH_REG_CMD_STATUS     0x40

// shennong_fdt.dts:6097 orders three ACTIVE TCS before SLEEP/WAKE/FAST_PATH.
#define RPMH_ACTIVE_TCS_COUNT   3
#define TCS_AMC_MODE_ENABLE     BIT16
#define TCS_AMC_MODE_TRIGGER    BIT24
#define CMD_MSGID_LEN           8
#define CMD_MSGID_RESP_REQ      BIT8
#define CMD_MSGID_WRITE         BIT16
#define CMD_STATUS_ISSUED       BIT8
#define CMD_STATUS_COMPLETED    BIT16

#define CMDDB_BASE              0x81C60000UL
#define CMDDB_MAP_LEN           0x4000UL
#define CMDDB_MAGIC             0x0C0330DB
#define CMDDB_HDR_SIZE          144
#define CMDDB_RSC_COUNT         8
#define CMDDB_RSC_SIZE          16
#define CMDDB_ENTRY_STRIDE      24
#define CMDDB_ADDR_OFFSET       16
#define CMDDB_VRM_SLAVE         4
#define CMDDB_BCM_SLAVE         5

STATIC BOOLEAN mRpmhMapped;

//
// How many candidate TCS to skip before taking one. Used to retry the whole vote on
// a different TCS when a command does not complete, instead of giving up on the
// first one - the bootloader may have left the first ACTIVE TCS in a state this
// driver does not understand.
//
STATIC UINT32  mRpmhSkip;
STATIC BOOLEAN mRpmhFailed;
STATIC UINT32  mRpmhTcs;

STATIC
VOID
RpmhSay (
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

  if (Length > 0) {
    SerialPortWrite ((UINT8 *)Buffer, Length);
  }
}

STATIC
UINTN
RpmhTcsReg (
  IN UINT32 Tcs,
  IN UINT32 Reg
  )
{
  return RPMH_DRV_BASE + RPMH_TCS_OFFSET + RPMH_TCS_STRIDE * Tcs + Reg;
}

STATIC
EFI_STATUS
RpmhVrmInit (VOID)
{
  EFI_STATUS                      Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
  UINT32                          DrvId;
  UINT32                          Config;
  UINT32                          TcsCount;
  UINT32                          CmdCount;
  UINT32                          Index;

  if (mRpmhFailed) {
    return EFI_DEVICE_ERROR;
  }

  if (mRpmhMapped) {
    return EFI_SUCCESS;
  }

  Status = gDS->GetMemorySpaceDescriptor (RPMH_DRV_BASE, &Desc);
  if (EFI_ERROR (Status) || (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo,
                    RPMH_DRV_BASE, RPMH_DRV_MAP_LEN, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = gDS->SetMemorySpaceAttributes (RPMH_DRV_BASE, RPMH_DRV_MAP_LEN, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DrvId    = MmioRead32 (RPMH_DRV_BASE);
  Config   = MmioRead32 (RPMH_DRV_BASE + RPMH_REG_DRV_CONFIG);
  TcsCount = (Config >> (6 * RPMH_DRV_ID)) & 0x3F;
  CmdCount = (Config >> 27) & 0x1F;
  RpmhSay ("[RPMH] drv2 id=%08x config=%08x tcs=%u slots=%u\r\n",
          DrvId, Config, TcsCount, CmdCount);
  if (((DrvId >> 16) & 0xFF) != 3 || TcsCount < RPMH_ACTIVE_TCS_COUNT ||
      CmdCount == 0 || CmdCount > 16) {
    return EFI_UNSUPPORTED;
  }

  //
  // Only CMD_ENABLE and the AMC trigger bit say whether a TCS is holding work. An
  // earlier version of this loop also required STATUS to be non-zero before it would
  // use a TCS, and if STATUS reads zero on all of them - which is what an idle TCS
  // looks like - every candidate was skipped and the function returned NOT_READY
  // without sending anything at all. That is indistinguishable from a vote that was
  // sent and ignored, which is why the rail never moved and why it looked like RPMh
  // did not work.
  //
  {
    UINT32  Skipped;

    Skipped = 0;
    for (Index = 0; Index < RPMH_ACTIVE_TCS_COUNT; Index++) {
      UINT32  Enable;
      UINT32  TcsStatus;
      UINT32  Control;

      Enable    = MmioRead32 (RpmhTcsReg (Index, RPMH_REG_CMD_ENABLE));
      TcsStatus = MmioRead32 (RpmhTcsReg (Index, RPMH_REG_STATUS));
      Control   = MmioRead32 (RpmhTcsReg (Index, RPMH_REG_CONTROL));

      RpmhSay ("[RPMH] tcs%u enable=%08x status=%08x ctl=%08x\r\n",
              Index, Enable, TcsStatus, Control);

      if ((Enable != 0) || ((Control & TCS_AMC_MODE_TRIGGER) != 0)) {
        continue;
      }

      if (Skipped < mRpmhSkip) {
        Skipped++;
        continue;
      }

      mRpmhTcs    = Index;
      mRpmhMapped = TRUE;
      RpmhSay ("[RPMH] using idle ACTIVE tcs%u slot0\r\n", Index);
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_READY;
}

STATIC
EFI_STATUS
RpmhControlWrite (
  IN UINT32 Value
  )
{
  UINT32 Timeout;

  MmioWrite32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CONTROL), Value);
  for (Timeout = 0; Timeout < 1000; Timeout++) {
    if (MmioRead32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CONTROL)) == Value) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }

  mRpmhFailed = TRUE;
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
RpmhVrmWrite (
  IN UINT32 Address,
  IN UINT32 Data
  )
{
  EFI_STATUS Status;
  UINT32     Bit;
  UINT32     Control;
  UINT32     IrqEnable;
  UINT32     CmdStatus;
  UINT32     Timeout;

  Status = RpmhVrmInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (MmioRead32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CMD_ENABLE)) != 0 ||
      (MmioRead32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CONTROL)) & TCS_AMC_MODE_TRIGGER) != 0) {
    return EFI_NOT_READY;
  }

  Bit     = 1U << mRpmhTcs;
  Control = MmioRead32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CONTROL));
  Status  = RpmhControlWrite (Control & ~TCS_AMC_MODE_TRIGGER);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = RpmhControlWrite (Control & ~(TCS_AMC_MODE_TRIGGER | TCS_AMC_MODE_ENABLE));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // IRQ registers are shared at tcs_base; never acknowledge another owner's TCS.
  IrqEnable = MmioRead32 (RpmhTcsReg (0, RPMH_REG_IRQ_ENABLE));
  MmioWrite32 (RpmhTcsReg (0, RPMH_REG_IRQ_CLEAR), Bit);
  MmioWrite32 (RpmhTcsReg (0, RPMH_REG_IRQ_ENABLE), IrqEnable | Bit);
  MmioWrite32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CMD_MSGID),
               CMD_MSGID_LEN | CMD_MSGID_WRITE | CMD_MSGID_RESP_REQ);
  MmioWrite32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CMD_ADDR), Address);
  MmioWrite32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CMD_DATA), Data);
  MmioWrite32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CMD_WAIT), BIT0);
  MmioWrite32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CMD_ENABLE), BIT0);
  MemoryFence ();

  Status = RpmhControlWrite (TCS_AMC_MODE_ENABLE);
  if (!EFI_ERROR (Status)) {
    Status = RpmhControlWrite (TCS_AMC_MODE_ENABLE | TCS_AMC_MODE_TRIGGER);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Timeout = 0; Timeout < 5000; Timeout++) {
    if ((MmioRead32 (RpmhTcsReg (0, RPMH_REG_IRQ_STATUS)) & Bit) != 0) {
      CmdStatus = MmioRead32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CMD_STATUS));
      Status = RpmhControlWrite (TCS_AMC_MODE_ENABLE);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Status = RpmhControlWrite (0);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      MmioWrite32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CMD_ENABLE), 0);
      MmioWrite32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_CMD_WAIT), 0);
      MmioWrite32 (RpmhTcsReg (0, RPMH_REG_IRQ_CLEAR), Bit);
      MmioWrite32 (RpmhTcsReg (0, RPMH_REG_IRQ_ENABLE),
                   (MmioRead32 (RpmhTcsReg (0, RPMH_REG_IRQ_ENABLE)) & ~Bit) | (IrqEnable & Bit));
      RpmhSay ("[RPMH] write %05x=%u msgid=00010108 status=%08x\r\n",
              Address, Data, CmdStatus);
      return ((CmdStatus & (CMD_STATUS_ISSUED | CMD_STATUS_COMPLETED)) ==
              (CMD_STATUS_ISSUED | CMD_STATUS_COMPLETED)) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }
    MicroSecondDelay (1);
  }

  // An uncompleted TCS may still be executing; do not erase or reuse it.
  mRpmhFailed = TRUE;
  RpmhSay ("[RPMH] write %05x=%u timed out on tcs%u\r\n", Address, Data, mRpmhTcs);
  return EFI_TIMEOUT;
}

STATIC
UINT32
RpmhCmdDbLookupResource (
  IN CONST CHAR8 *Name,
  IN UINT32 Slave,
  IN UINT32 AlignmentMask
  )
{
  EFI_STATUS                      Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
  UINT8                           Query[8];
  UINT32                          Index;
  UINT32                          Resource;
  UINT32                          Count;
  UINT32                          Offset;
  UINT32                          Entry;
  UINT32                          Address;
  UINTN                           Header;
  UINTN                           Record;
  BOOLEAN                         Match;

  ZeroMem (Query, sizeof (Query));
  for (Index = 0; Index < sizeof (Query) && Name[Index] != '\0'; Index++) {
    Query[Index] = (UINT8)Name[Index];
  }
  if (Name[Index] != '\0') {
    return 0;
  }

  Status = gDS->GetMemorySpaceDescriptor (CMDDB_BASE, &Desc);
  if (EFI_ERROR (Status) || Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo,
                    CMDDB_BASE, CMDDB_MAP_LEN, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      return 0;
    }
  }
  Status = gDS->SetMemorySpaceAttributes (CMDDB_BASE, CMDDB_MAP_LEN, EFI_MEMORY_UC);
  if (EFI_ERROR (Status) || MmioRead32 (CMDDB_BASE + 4) != CMDDB_MAGIC) {
    return 0;
  }

  for (Resource = 0; Resource < CMDDB_RSC_COUNT; Resource++) {
    Header = CMDDB_BASE + 8 + Resource * CMDDB_RSC_SIZE;
    if (MmioRead16 (Header) != Slave) {
      continue;
    }
    Count  = MmioRead16 (Header + 6);
    Offset = CMDDB_HDR_SIZE + MmioRead16 (Header + 2);
    if (Offset > CMDDB_MAP_LEN || Count > (CMDDB_MAP_LEN - Offset) / CMDDB_ENTRY_STRIDE) {
      return 0;
    }
    for (Entry = 0; Entry < Count; Entry++) {
      Record = CMDDB_BASE + Offset + Entry * CMDDB_ENTRY_STRIDE;
      Match  = TRUE;
      for (Index = 0; Index < sizeof (Query); Index++) {
        if (MmioRead8 (Record + Index) != Query[Index]) {
          Match = FALSE;
          break;
        }
      }
      if (Match) {
        Address = MmioRead32 (Record + CMDDB_ADDR_OFFSET);
        if (((Address >> 16) & 0xF) != Slave || (Address & AlignmentMask) != 0) {
          return 0;
        }
        RpmhSay ("[CMDDb] %a -> %05x\r\n", Name, Address);
        if (Slave == CMDDB_BCM_SLAVE) {
          UINT32 AuxLength = MmioRead16 (Record + 20);
          UINT32 AuxOffset = CMDDB_HDR_SIZE + MmioRead16 (Header + 4) + MmioRead16 (Record + 22);
          if (AuxLength >= 8 && AuxOffset <= CMDDB_MAP_LEN - 8) {
            RpmhSay ("[NOC b200] %a unit=%u width=%u vcd=%u\r\n", Name,
                     MmioRead32 (CMDDB_BASE + AuxOffset),
                     MmioRead16 (CMDDB_BASE + AuxOffset + 4),
                     MmioRead8 (CMDDB_BASE + AuxOffset + 6));
          }
        }
        return Address;
      }
    }
  }

  return 0;
}

STATIC UINT32
RpmhCmdDbLookup (IN CONST CHAR8 *Name)
{
  return RpmhCmdDbLookupResource (Name, CMDDB_VRM_SLAVE, 0xF);
}

VOID
RpmhUsbNocReport (VOID)
{
  UINT32 Address = RpmhCmdDbLookupResource ("SN2", CMDDB_BCM_SLAVE, 3);
  RpmhSay ("[NOC b200] USB A1NOC SN2=%05x; boot observation only\r\n", Address);
}

// pineapple.c: USB3_0 -> A1NOC_SNOC -> qnm_aggre1_noc, covered by BCM SN2.
// UFS uses A2NOC/SN3, so its working DMA does not prove the USB branch is voted.
// One ACTIVE-only minimum vote uses the exact BCM_TCS_CMD format from tcs.h.
// This tests branch liveness; it is not a throughput/bandwidth configuration.
EFI_STATUS
RpmhUsbNocOn (VOID)
{
  UINT32 Address;
  EFI_STATUS Status;

  Address = RpmhCmdDbLookupResource ("SN2", CMDDB_BCM_SLAVE, 3);
  if (Address == 0) {
    RpmhSay ("[NOC b200] SN2 absent from BCM CmdDb; no vote sent\r\n");
    return EFI_NOT_FOUND;
  }
  mRpmhMapped = FALSE;
  mRpmhSkip = 0;
  // COMMIT bit30, VALID bit29, X=1 at [27:14], Y=1 at [13:0].
  Status = RpmhVrmWrite (Address, BIT30 | BIT29 | BIT14 | BIT0);
  RpmhSay ("[NOC b200] SN2 addr=%05x vote=60004001 -> %r\r\n", Address, Status);
  return Status;
}

EFI_STATUS
RpmhTouchIovddOn (VOID)
{
  EFI_STATUS Status;
  UINT32     L12;
  UINT32     Attempt;

  L12 = RpmhCmdDbLookup ("ldob12");
  if (L12 == 0) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_DEVICE_ERROR;
  for (Attempt = 0; Attempt < RPMH_ACTIVE_TCS_COUNT; Attempt++) {
    mRpmhMapped = FALSE;
    mRpmhSkip   = Attempt;

    Status = RpmhVrmWrite (L12, 1800);
    if (!EFI_ERROR (Status)) {
      Status = RpmhVrmWrite (L12 + 8, 7);
    }
    if (!EFI_ERROR (Status)) {
      Status = RpmhVrmWrite (L12 + 4, 1);
    }

    RpmhSay ("[RPMH] IOVDD ldob12 1800 mV HPM enable, attempt %u -> %r\r\n",
            Attempt, Status);
    if (!EFI_ERROR (Status)) {
      break;
    }
    // Retry only before submission when no TCS was available. A command error
    // or timeout must survive unchanged; an in-flight command cannot be reused.
    if (Status != EFI_NOT_READY || mRpmhFailed) {
      break;
    }
  }

  return Status;
}

/**
  Vote on one VRM rail: voltage, then mode, then enable.

  Offsets are the rpmh-regulator ones: REG_VRM_VOLTAGE at 0, REG_ENABLE at 4,
  REG_VRM_MODE at 8, and voltage is carried in millivolts. Mode 7 is the control
  value for HPM on a PMIC5 LDO, which is what the device tree asks for with
  qcom,init-mode = <RPMH_REGULATOR_MODE_HPM>.

  @param[in]  Name      CmdDb resource name, e.g. "ldoi1".
  @param[in]  Millivolts Voltage to request.
  @param[out] Address   CmdDb address, or 0 when the name was not found.

  @retval EFI_SUCCESS  All three commands completed.
**/
STATIC
EFI_STATUS
RpmhRailOn (
  IN  CONST CHAR8  *Name,
  IN  UINT32       Millivolts,
  OUT UINT32       *Address
  )
{
  EFI_STATUS  Status;
  UINT32      Base;
  UINT32      Attempt;

  *Address = 0;
  Base     = RpmhCmdDbLookup (Name);
  if (Base == 0) {
    RpmhSay ("[RPMH] %a not in CmdDb\r\n", Name);
    return EFI_NOT_FOUND;
  }

  *Address = Base;
  Status   = EFI_DEVICE_ERROR;

  for (Attempt = 0; Attempt < RPMH_ACTIVE_TCS_COUNT; Attempt++) {
    mRpmhMapped = FALSE;
    mRpmhSkip   = Attempt;

    Status = RpmhVrmWrite (Base, Millivolts);
    if (!EFI_ERROR (Status)) {
      Status = RpmhVrmWrite (Base + 8, 7);
    }

    if (!EFI_ERROR (Status)) {
      Status = RpmhVrmWrite (Base + 4, 1);
    }

    if (!EFI_ERROR (Status)) {
      break;
    }
    if (Status != EFI_NOT_READY || mRpmhFailed) {
      break;
    }
  }

  RpmhSay ("[RPMH] %a %u mV HPM enable -> %r (attempt %u)\r\n",
           Name, Millivolts, Status, Attempt);
  return Status;
}

/**
  Power the eUSB2 PHY's two rails.

  hsphy@88e3000 declares vdd-supply = pm_v8_l1 (ldoi1, 880 mV) and vdda12-supply =
  pm_v8_l3 (ldoi3, 1200 mV). Both are qcom,rpmh-vrm-regulator with qcom,init-voltage,
  matching the voltage requests of the Linux regulator driver. A missing rail can
  prevent the PHY clock/reset sequence, but these votes alone do not establish the
  cause of a reset or enumeration failure.

  @retval EFI_SUCCESS  Both rails were voted on.
**/
EFI_STATUS
RpmhUsbPhyRailsOn (VOID)
{
  EFI_STATUS  Status;
  EFI_STATUS  Second;
  UINT32      L1;
  UINT32      L3;

  L1     = 0;
  L3     = 0;
  Status = RpmhRailOn ("ldoi1", 880, &L1);
  Second = RpmhRailOn ("ldoi3", 1200, &L3);

  RpmhSay ("[RPMH] PHY rails: ldoi1=%05x ldoi3=%05x -> %r / %r\r\n",
           L1, L3, Status, Second);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Second;
}

/**
  Request the PM8550B repeater supplies declared by the device's DT.

  vdd18 phandle 0x126 -> pm_humu_l15 / ldob15 / 1800000 uV.
  vdd3  phandle 0x38B -> pm_humu_l5  / ldob5  / 3104000 uV.
  CmdDb supplies the VRM addresses; these are not guessed SPMI LDO offsets.
  A completion acknowledges the request, not a measured analog output voltage.
**/
EFI_STATUS
RpmhUsbRepeaterRailsOn (VOID)
{
  EFI_STATUS Status;
  UINT32     Address;

  Status = RpmhRailOn ("ldob15", 1800, &Address);
  RpmhSay ("[RPMH] repeater-b212 vdd18 ldob15=%05x -> %r\r\n", Address, Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = RpmhRailOn ("ldob5", 3104, &Address);
  RpmhSay ("[RPMH] repeater-b212 vdd3 ldob5=%05x -> %r\r\n", Address, Status);
  return Status;
}
