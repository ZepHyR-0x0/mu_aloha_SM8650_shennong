#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
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

extern VOID TcmSay (IN CONST CHAR8 *Format, ...);

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
  TcmSay ("[RPMH] drv2 id=%08x config=%08x tcs=%u slots=%u\r\n",
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

      TcmSay ("[RPMH] tcs%u enable=%08x status=%08x ctl=%08x\r\n",
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
      TcmSay ("[RPMH] using idle ACTIVE tcs%u slot0\r\n", Index);
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
      MmioRead32 (RpmhTcsReg (mRpmhTcs, RPMH_REG_STATUS)) == 0) {
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
      TcmSay ("[RPMH] write %05x=%u msgid=00010108 status=%08x\r\n",
              Address, Data, CmdStatus);
      return ((CmdStatus & (CMD_STATUS_ISSUED | CMD_STATUS_COMPLETED)) ==
              (CMD_STATUS_ISSUED | CMD_STATUS_COMPLETED)) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }
    MicroSecondDelay (1);
  }

  // An uncompleted TCS may still be executing; do not erase or reuse it.
  mRpmhFailed = TRUE;
  TcmSay ("[RPMH] write %05x=%u timed out on tcs%u\r\n", Address, Data, mRpmhTcs);
  return EFI_TIMEOUT;
}

STATIC
UINT32
RpmhCmdDbLookup (
  IN CONST CHAR8 *Name
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
    if (MmioRead16 (Header) != CMDDB_VRM_SLAVE) {
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
        if (((Address >> 16) & 0xF) != CMDDB_VRM_SLAVE || (Address & 0xF) != 0) {
          return 0;
        }
        TcmSay ("[CMDDb] %a -> %05x\r\n", Name, Address);
        return Address;
      }
    }
  }

  return 0;
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

    TcmSay ("[RPMH] IOVDD ldob12 1800 mV HPM enable, attempt %u -> %r\r\n",
            Attempt, Status);
    if (!EFI_ERROR (Status)) {
      break;
    }
  }

  return Status;
}
