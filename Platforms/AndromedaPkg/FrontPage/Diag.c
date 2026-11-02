/** @file
  Read-only diagnostics for the storage and I2C paths.

  There is no keyboard and no touch on this device, so a UEFI Shell cannot be used to
  poke around - everything here has to be reachable from the FrontPage menu and its
  output has to land in the log ring so LOGAUTO can carry it off the device.

  Nothing here writes to hardware. It reads three things:

    1. The GCD descriptor for the UFS register windows, to confirm what the firmware
       believes those ranges are.
    2. The I2C protocol handle counts, because UsbConfigDxe reports
       "usb_shared_i2c_write, not open" - so either no master exists, or one exists and
       the switch's device handle was never opened.
    3. A snapshot of the UFS host controller registers, which is the only way to see
       whether the link ever came up: UFSDxe prints nothing at all on this port.

  Copyright (c) 2024, Project Mu contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include <Protocol/BlockIo.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/HobLib.h>
#include <Library/WcdUsbLib.h>

#include "FrontPage.h"

//
// gAndromedaSfDiagHobGuid, declared locally for the same reason as the protocol
// GUIDs below: keeps the INF free of Guids entries that only this file needs.
//
STATIC CONST EFI_GUID  mSfDiagHobGuid = {
  0x7d1f4a52, 0x3c68, 0x4b19, { 0x8e, 0x02, 0x51, 0xa7, 0x6d, 0x93, 0x2c, 0xf4 }
};

//
// gEfiSecDtbGuid - PrePi puts the DTBExtnProtocol pointer XBL handed it here.
//
STATIC CONST EFI_GUID  mSecDtbHobGuid = {
  0x3a06e1e8, 0x61f6, 0x11eb, { 0xbb, 0xed, 0x4b, 0x47, 0x6e, 0x2f, 0xf6, 0xa7 }
};

//
// gEfiClockProtocolGuid - the Qualcomm clock driver's protocol.
//
// Recovered by decoding I2CDxe.depex: after the leading opcode, the second GUID
// in that file is 241AFAE6-885F-4F6C-A7EA-C28EAB79C3E5, which is the value
// AndromedaPkg/Include/Protocol/EFIClock.h declares as gEfiClockProtocolGuid.
// So I2CDxe cannot dispatch at all until ClockDxe has published it, and since
// I2CDxe does dispatch, the clock driver must be working.
//
STATIC CONST EFI_GUID  mClockProtocolGuid = {
  0x241afae6, 0x885f, 0x4f6c, { 0xa7, 0xea, 0xc2, 0x8e, 0xab, 0x79, 0xc3, 0xe5 }
};

//
// The Qualcomm I2C protocol - the one I2CDxe actually publishes.
//
// gEfiI2cMasterProtocolGuid (cd72881f-...) does not appear in I2CDxe.efi at all,
// so counting EFI_I2C_MASTER_PROTOCOL handles was measuring something the driver
// never produces, and "i2c master handles=0" was never evidence of anything.
//
// This GUID instead is present in exactly two modules out of the whole FV -
// I2CDxe and UsbConfigDxe - which is what a producer/consumer pair looks like.
// Found by intersecting the 8-byte aligned GUID tables of the two binaries and
// dropping every GUID that also appears elsewhere.
//
STATIC CONST EFI_GUID  mQcomI2cProtocolGuid = {
  0xb27ae8b1, 0x3e10, 0x4d07, { 0xab, 0x5c, 0xeb, 0x9a, 0x6d, 0xc6, 0xfa, 0x8f }
};

//
// Power Control USB (PMIC SMBCHG) - read from UsbfnDwc3Dxe .data 0x16158, which
// is the GUID its own UsbSharedGetChargerPort passes to LocateProtocol.
//
STATIC CONST EFI_GUID  mPowerControlUsbGuid = {
  0xe07df17e, 0xe79e, 0x4150, { 0x93, 0x78, 0x50, 0x62, 0x3a, 0x14, 0x99, 0x4a }
};

//
// Defined further down, but the I2C open probe below needs it.
//
STATIC
VOID
FpDiagPrint (
  IN CONST CHAR8  *Format,
  ...
  );

//
// I2C state kept for the USB heartbeat, so it can re-assert the WCD939x switch
// mode while StartEx is stuck. Set by FpDiagI2cOpen, consumed by
// FpDiagI2cReassert.
//
typedef struct {
  UINT32  BusFreqKhz;
  UINT32  SlaveAddr;
  UINT32  Cfg0;
  UINT32  Cfg1;
  UINT32  Cfg2;
  UINT32  Cfg3;
} FP_I2C_CFG_STORE;


/**
  Ask the PMIC charger protocol what kind of port is on the other end.

  UsbfnDwc3Dxe's cable-status path calls UsbSharedGetChargerPort, which is
  LocateProtocol(E07DF17E-E79E-4150-9378-50623A14994A) followed by
  (*(Proto + 0x20))(PortType, &Raw), and maps the raw answer:

    raw 1 -> 0     raw 2 -> 7     raw 3 -> 1     raw 4 -> 2     else -> 8

  8 means unknown, and UsbfnDwcCoreCableStatus treats unknown by polling 101
  times at 10 ms and then assuming SDP. If the port comes back unknown here, the
  charging side is not identifying the host, and that is a much better
  explanation for D+/D- never being routed than anything in the USB controller.

  Read-only: it queries, it does not configure.
**/
STATIC
VOID
FpDiagCharger (
  VOID
  )
{
  typedef UINT32 (EFIAPI *GET_CHARGER_PORT)(UINT32 PortType, UINT32 *Raw);

  EFI_STATUS       Status;
  UINT8            *Proto;
  GET_CHARGER_PORT Fn;
  UINT32           Raw;
  UINT32           Rc;
  UINT32           Mapped;

  Proto = NULL;
  Status = gBS->LocateProtocol ((EFI_GUID *)&mPowerControlUsbGuid, NULL, (VOID **)&Proto);
  if (EFI_ERROR (Status) || (Proto == NULL)) {
    FpDiagPrint ("[DIAG] charger: no Power Control USB protocol (%r)\r\n", Status);
    return;
  }

  Fn = (GET_CHARGER_PORT)*(UINT64 *)(Proto + 0x20);
  FpDiagPrint ("[DIAG] charger proto=0x%lx get=0x%lx\r\n",
               (UINT64)(UINTN)Proto, (UINT64)(UINTN)Fn);
  if (Fn == NULL) {
    return;
  }

  // Shennong has one active USB port. A speculative port 1 query produced an
  // Invalid Port Index warning which was mistaken for a failed power vote.
  {
    Raw    = 0xFFFFFFFF;
    Rc     = Fn (0, &Raw);

    switch (Raw) {
      case 1:  Mapped = 0; break;
      case 2:  Mapped = 7; break;
      case 3:  Mapped = 1; break;
      case 4:  Mapped = 2; break;
      default: Mapped = 8; break;
    }

    FpDiagPrint ("[DIAG] charger port %u -> rc=%u raw=0x%x port=%u%s\r\n",
                 0U, Rc, Raw, Mapped,
                 (Mapped == 8) ? " (unknown)" : "");
  }
}

/** Deprecated heartbeat hook; no longer modifies WCD routing. */
UINT32
FpDiagI2cReassert (VOID)
{
  return 0;
}

/**
  Read the four SE registers that say whether a serial engine has been brought up.

  GENI_FORCE_DEFAULT_REG, GENI_OUTPUT_CTRL, GENI_CGC_CTRL and GENI_STATUS at
  0x00..0x0C. Comparing the same four before and after an Open is what shows
  whether the driver actually programmed the engine or just handed out a handle.
**/
STATIC
VOID
FpDiagQup4 (
  IN CONST CHAR8  *Tag,
  IN UINTN        Base
  )
{
  EFI_STATUS                        Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR   Desc;

  ZeroMem (&Desc, sizeof (Desc));
  Status = gDS->GetMemorySpaceDescriptor (Base, &Desc);
  if (EFI_ERROR (Status) || (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    FpDiagPrint ("[DIAG] qup %a not mapped\r\n", Tag);
    return;
  }

  FpDiagPrint (
    "[DIAG] qup %a %08x %08x %08x %08x\r\n",
    Tag,
    MmioRead32 (Base + 0x00),
    MmioRead32 (Base + 0x04),
    MmioRead32 (Base + 0x08),
    MmioRead32 (Base + 0x0C)
    );
}

/**
  Call the Qualcomm I2C protocol's Open directly.

  Counting handles said the protocol exists; it does not say whether a device can
  be opened on it, and that is the step UsbConfigDxe reports as
  "usb_shared_i2c_write, not open".

  Layout, recovered from I2CDxe.efi by disassembly rather than from a header:

    0x1420  InstallMultipleProtocolInterfaces (&Handle, &Guid@0x9058,
                                               &Interface@0x90D8, NULL)
    0x9058  B27AE8B1-3E10-4D07-AB5C-EB9A6DC6FA8F
    0x90D8  UINT64 Revision = 0x00010000
    0x90E0  method 0 -> 0x28E8   Open  (slave address, &handle)
    0x90E8  method 1 -> 0x2B60   read  (7 arguments, not called)
    0x90F0  method 2 -> 0x2F8C   read  (7 arguments, not called)
    0x90F8  method 3 -> 0x2C70   write (8 arguments, not called)
    0x9100  method 4 -> 0x30E0   Close (handle)

  0x28E8 takes w0 as the slave address and x1 as the out handle, builds
  "/soc/i2c/i2c_device_config_%d" from that address, returns 34 when the node is
  missing, and otherwise initialises the serial engine on first use.

  There is deliberately no "address that does not exist" control here. One was
  tried - 0x55 - and it took a synchronous exception inside I2CDxe at
  0x4810 (ldr wzr, [x1] with x1 = core_base + 0x2000 = 0xA9A00000): XBL's
  GetNode does not fail cleanly on a missing path, it hands back a node whose
  properties are garbage, and the driver then programmes a serial engine at an
  address that does not exist. Not worth crashing the DIAG page over.

  This one does touch hardware, unlike every other probe in this file, so it only
  runs from the DIAG page and never on a normal boot.
**/
STATIC
VOID
FpDiagI2cOpen (
  VOID
  )
{
  typedef UINT32 (EFIAPI *QCOM_I2C_OPEN)(UINT32 Addr, UINT64 *Handle);
  typedef UINT32 (EFIAPI *QCOM_I2C_READ)(
                          UINT64 Handle, VOID *Config, UINT32 Reg, UINT32 Flag,
                          VOID *Buffer, UINT32 Length, UINT32 *TransferStatus,
                          UINT32 Timeout);
  typedef UINT32 (EFIAPI *QCOM_I2C_WRITE)(
                          UINT64 Handle, VOID *Config, UINT32 Reg, UINT32 Flag,
                          VOID *Buffer, UINT32 Length, UINT32 *TransferStatus,
                          UINT32 Timeout);
  typedef UINT32 (EFIAPI *QCOM_I2C_CLOSE)(UINT64 Handle);

  //
  // Mirrors REDRIVER_I2C_CTX from UsbConfigDxe starting at its +0x04, which is
  // what the driver passes as the Read/Write Config argument.
  //
  typedef struct {
    UINT32  BusFreqKhz;   // 100
    UINT32  SlaveAddr;    // 0x0E
    UINT32  Cfg0;
    UINT32  Cfg1;         // 500
    UINT32  Cfg2;
    UINT32  Cfg3;
  } FP_I2C_CFG;

  EFI_STATUS     Status;
  UINT8          *Proto;
  UINT64         Revision;
  QCOM_I2C_OPEN  Open;
  QCOM_I2C_READ  Read;
  QCOM_I2C_WRITE Write;
  QCOM_I2C_CLOSE Close;
  UINT64         Handles[1];
  UINT32         Rc;

  Proto = NULL;
  Status = gBS->LocateProtocol ((EFI_GUID *)&mQcomI2cProtocolGuid, NULL, (VOID **)&Proto);
  if (EFI_ERROR (Status) || (Proto == NULL)) {
    FpDiagPrint ("[DIAG] qcom-i2c open: no protocol (%r)\r\n", Status);
    return;
  }

  Revision = *(UINT64 *)Proto;
  Open     = (QCOM_I2C_OPEN)*(UINT64 *)(Proto + 0x08);
  Read     = (QCOM_I2C_READ)*(UINT64 *)(Proto + 0x10);
  Write    = (QCOM_I2C_WRITE)*(UINT64 *)(Proto + 0x18);
  Close    = (QCOM_I2C_CLOSE)*(UINT64 *)(Proto + 0x28);
  FpDiagPrint ("[DIAG] qcom-i2c proto=0x%lx rev=0x%lx open=0x%lx read=0x%lx write=0x%lx close=0x%lx\r\n",
               (UINT64)(UINTN)Proto, Revision,
               (UINT64)(UINTN)Open, (UINT64)(UINTN)Read,
               (UINT64)(UINTN)Write, (UINT64)(UINTN)Close);

  if (Open == NULL) {
    FpDiagPrint ("[DIAG] qcom-i2c open: method 0 is NULL\r\n");
    return;
  }

  FpDiagQup4 ("wcd pre ", 0x00A8C000);

  //
  // Only 0x0e, the WCD939x.
  //
  // Opening three addresses in a row is not stable: on one boot 0x0e, 0x0b and
  // 0x11 all returned rc=0 and the fourth call faulted, on the next boot the
  // fault moved to the third call. Same ELR (I2CDxe + 0x4810) and same FAR
  // (0xA9A00000) both times, so something inside the driver corrupts its device
  // list as entries accumulate. The first Open has succeeded on every run, so
  // that is the one worth keeping - it answers the question (can a device be
  // opened at all) without walking into the bug.
  //
  Handles[0] = 0;
  Rc = Open (0x0E, &Handles[0]);
  FpDiagPrint ("[DIAG] qcom-i2c open 0x0e -> rc=%u handle=0x%lx\r\n", Rc, Handles[0]);

  FpDiagQup4 ("wcd post", 0x00A8C000);

  //
  // Read the WCD939x back.
  //
  // This is the question the last few builds could not answer: the driver's own
  // init sequence writes five registers to this device and reports no error, but
  // the controller still sees no USB reset and the line state settles at SE0, so
  // either the writes never landed or the switch is configured and the problem
  // is further down the D+/D- path.
  //
  // The config block and the read entry point come from the UsbConfigDxe
  // reconstruction, not from a header:
  //
  //   REDRIVER_I2C_CTX: +0x04 BusFreqKhz (100), +0x08 SlaveAddr (0x0E),
  //                     +0x0C..+0x18 = {0, 500, 0, 0}, +0x20 I2cHandle
  //   Read = protocol +0x10, called as
  //          Read(handle, &cfg, reg, 1, buf, len, &transferStatus, 2500)
  //
  // The driver writes 0x000=0x04, 0x414=0x00, 0x414=0x03, 0x000=0x00,
  // 0x002=0x28, so those are the registers worth reading back.
  //
  if ((Read != NULL) && (Handles[0] != 0)) {
    FP_I2C_CFG  Cfg;
    UINTN       Index;

    ZeroMem (&Cfg, sizeof (Cfg));
    Cfg.BusFreqKhz = 100;
    Cfg.SlaveAddr  = 0x0E;
    Cfg.Cfg1       = 500;

    // Page-aware, read-only inspection of the configured routing registers.
    if (Write != NULL) {
      WCD_USB_IO Io = { Handles[0], &Cfg, (WCD_I2C_TRANSFER)Read, (WCD_I2C_TRANSFER)Write };
      STATIC CONST UINT16 Routing[] = { 0x403,0x404,0x406,0x407,0x413,0x414,0x415,0x417,0x419,0x41B,0x41C };
      for (Index = 0; Index < ARRAY_SIZE (Routing); Index++) {
        UINT8 Data;
        EFI_STATUS Status = WcdReadRegister (&Io, Routing[Index], &Data);
        FpDiagPrint ("[DIAG b200] WCD page-aware %03x=%02x -> %r\r\n", Routing[Index], Data, Status);
      }
      // No reset/replay here: diagnostics must preserve the configured route.

    }
  }

  if ((Close != NULL) && (Handles[0] != 0)) {
    Rc = Close (Handles[0]);
    FpDiagPrint ("[DIAG] qcom-i2c close 0x0e -> rc=%u\r\n", Rc);
  }
}

//
// From MdePkg.dec. Declared locally so the INF does not need protocol entries.
//
STATIC CONST EFI_GUID  mI2cMasterProtocolGuid = {
  0xcd72881f, 0x45b5, 0x4feb, { 0x98, 0xc8, 0x31, 0x3d, 0xa8, 0x11, 0x74, 0x62 }
};

STATIC CONST EFI_GUID  mI2cIoProtocolGuid = {
  0xb60a3e6b, 0x18c4, 0x46e5, { 0xa2, 0x9a, 0xc9, 0xa1, 0x06, 0x65, 0xa2, 0x8e }
};

//
// Whether UFSDxe produced anything usable. The controller registers show the device
// present and the controller enabled, so the open question is whether a block device
// was published - a zero here means UFSDxe never got that far, a non-zero one means
// the problem is partitions rather than the link.
//
STATIC CONST EFI_GUID  mBlockIoGuid = {
  0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

STATIC CONST EFI_GUID  mDiskIoGuid = {
  0xce345171, 0xba0b, 0x11d2, { 0x8e, 0x4f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

STATIC CONST EFI_GUID  mSimpleFsGuid = {
  0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

//
// Register windows, taken from this platform's own memory map. UFS_MEM and UFS_ICE
// sit in one 0x80000 window that the GCD merges with CRYPTO.
//
#define UFS_MEM_BASE  0x1D84000ULL
#define UFS_MEM_LEN   0x00030000ULL
#define UFS_ICE_BASE  0x1D88000ULL
#define UFS_ICE_LEN   0x00001000ULL

//
// UFSHCI registers worth naming. The dump prints the whole first 0x100 anyway, but
// these are the ones that answer "did the link come up".
//
#define UFSHCI_CAP    0x00   // Controller capabilities
#define UFSHCI_VER    0x08   // Version
#define UFSHCI_IS     0x20   // Interrupt status
#define UFSHCI_HCS    0x30   // Host controller status
#define UFSHCI_HCE    0x34   // Host controller enable

STATIC
VOID
FpDiagPrint (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR8    Buffer[256];
  UINTN    Length;

  VA_START (Marker, Format);
  Length = AsciiVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  if (Length > 0) {
    SerialPortWrite ((UINT8 *)Buffer, MIN (Length, sizeof (Buffer) - 1));
  }
}

STATIC
VOID
FpDiagPrintGcd (
  IN CONST CHAR8       *Name,
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Length
  )
{
  EFI_STATUS             Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;

  ZeroMem (&Desc, sizeof (Desc));
  Status = gDS->GetMemorySpaceDescriptor (Base, &Desc);
  if (EFI_ERROR (Status)) {
    FpDiagPrint ("[DIAG] gcd %a 0x%llx+0x%llx -> %r\r\n", Name, Base, Length, Status);
    return;
  }

  FpDiagPrint (
    "[DIAG] gcd %a 0x%llx+0x%llx type=0x%x attr=0x%llx\r\n",
    Name,
    Desc.BaseAddress,
    Desc.Length,
    Desc.GcdMemoryType,
    Desc.Attributes
    );
}

/**
  Count the handles carrying a protocol, for protocols that are not I2C.

  Same shape as FpDiagPrintI2c but with a neutral label, so a zero here reads as
  "nothing published it" rather than "I2C is broken".
**/
STATIC
VOID
FpDiagCountProto (
  IN CONST CHAR8     *Name,
  IN CONST EFI_GUID  *Protocol
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, (EFI_GUID *)Protocol, NULL, &Count, &Handles);
  FpDiagPrint ("[DIAG] proto %a handles=%u status=%r\r\n", Name, Count, Status);

  if (Handles != NULL) {
    FreePool (Handles);
  }
}

STATIC
VOID
FpDiagPrintI2c (
  IN CONST CHAR8     *Name,
  IN CONST EFI_GUID  *Protocol
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, (EFI_GUID *)Protocol, NULL, &Count, &Handles);

  //
  // Not found is the interesting answer, not an error to report as a failure: it means
  // nothing published the protocol at all.
  //
  FpDiagPrint ("[DIAG] i2c %a handles=%u status=%r\r\n", Name, Count, Status);

  if (Handles != NULL) {
    FreePool (Handles);
  }
}


//
// Walk every BlockIo handle: device path type, media geometry, and whether LBA 1 holds
// an "EFI PART" header. The stock partition tables in the engineering package put the
// GPT header at byte offset 0x1000, which is LBA 1 only if the device reports a 4096
// byte block - so the block size the media reports is the thing to look at first.
//
STATIC
VOID
FpDiagDumpBlockIo (
  VOID
  )
{
  EFI_STATUS              Status;
  EFI_HANDLE              *Handles;
  UINTN                   Count;
  UINTN                   Index;
  EFI_BLOCK_IO_PROTOCOL   *BlockDev;
  UINT8                   *Buf;
  UINTN                   Shown;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    FpDiagPrint ("[DIAG] blockio enumerate %r\r\n", Status);
    return;
  }

  Buf   = AllocatePool (4096);
  Shown = 0;

  for (Index = 0; (Index < Count) && (Shown < 8); Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockDev))) {
      continue;
    }

    if ((BlockDev->Media == NULL) || !BlockDev->Media->MediaPresent || BlockDev->Media->LogicalPartition) {
      continue;
    }

    FpDiagPrint (
      "[DIAG] bio%u blockSize=%u lastLba=0x%lx ro=%u\r\n",
      (UINT32)Shown,
      BlockDev->Media->BlockSize,
      BlockDev->Media->LastBlock,
      BlockDev->Media->ReadOnly ? 1 : 0
      );

    if (Buf == NULL) {
      continue;
    }

    //
    // LBA 1 with the media's own block size. On a 4096-byte device this is the GPT
    // header; on a 512-byte device it is not, which is exactly what we want to see.
    //
    Status = BlockDev->ReadBlocks (BlockDev, BlockDev->Media->MediaId, 1, BlockDev->Media->BlockSize, Buf);
    if (EFI_ERROR (Status)) {
      FpDiagPrint ("[DIAG] bio%u read lba1 %r\r\n", (UINT32)Shown, Status);
    } else {
      FpDiagPrint (
        "[DIAG] bio%u lba1: %02x %02x %02x %02x %02x %02x %02x %02x  sig=%a\r\n",
        (UINT32)Shown,
        Buf[0], Buf[1], Buf[2], Buf[3], Buf[4], Buf[5], Buf[6], Buf[7],
        (Buf[0] == 'E' && Buf[1] == 'F' && Buf[2] == 'I' && Buf[3] == ' ') ? "EFI PART" : "-"
        );
    }

    Shown++;
  }

  if (Buf != NULL) {
    FreePool (Buf);
  }

  FreePool (Handles);
  FpDiagPrint ("[DIAG] blockio total=%u\r\n", (UINT32)Count);
}


//
// Find the ELF images among the partitions.
//
// TzDxeLA.efi loads cmnlib32 and cmnlib64 first ("Early Storage"), then Keymaster, and
// our boot dies in ValidateElfHeader with "the Early Storage is not valid!" - so the
// cmnlib load is what fails. Android partitions are ext4/f2fs/raw, so anything whose
// first block starts with 0x7f 'E' 'L' 'F' is a firmware image worth looking at.
//
// Read-only: one block per partition.
//
STATIC
VOID
FpDiagScanPartitions (
  VOID
  )
{
  EFI_STATUS              Status;
  EFI_HANDLE              *Handles;
  UINTN                   Count;
  UINTN                   Index;
  EFI_BLOCK_IO_PROTOCOL   *Part;
  UINT8                   *Buf;
  UINTN                   Printed;
  UINT64                  PhOff;
  UINT64                  PhEnd;
  UINT64                  MediaSize;
  UINT16                  PhEnt;
  UINT16                  PhNum;
  UINT32                  Class;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  Buf     = AllocatePool (4096);
  Printed = 0;

  for (Index = 0; (Index < Count) && (Printed < 12); Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Part))) {
      continue;
    }

    if ((Part->Media == NULL) || !Part->Media->MediaPresent || (Buf == NULL)) {
      continue;
    }

    if (EFI_ERROR (Part->ReadBlocks (Part, Part->Media->MediaId, 0, Part->Media->BlockSize, Buf))) {
      continue;
    }

    if ((Buf[0] != 0x7F) || (Buf[1] != 'E') || (Buf[2] != 'L') || (Buf[3] != 'F')) {
      continue;
    }

    //
    // Branch on e_ident[EI_CLASS]: real Qualcomm images are a mix. abl.elf and
    // aop.mbn are ELF32 (e_phoff at +0x1C, = 0x34), devcfg.mbn is ELF64
    // (e_phoff at +0x20, = 0x40). Reading the ELF64 offsets on an ELF32 file
    // produces nonsense such as phoff=0x500000000.
    //
    // Section headers are stripped in every shipping image (e_shoff = 0,
    // e_shnum = 0), so report the program header table - that is what a loader
    // maps, and its end offset is what "Bad Buffer Size" is about.
    //
    if (Buf[4] == 2) {
      PhOff = *(UINT64 *)(Buf + 0x20);
      PhEnt = *(UINT16 *)(Buf + 0x36);
      PhNum = *(UINT16 *)(Buf + 0x38);
      Class = 64;
    } else {
      PhOff = (UINT64)*(UINT32 *)(Buf + 0x1C);
      PhEnt = *(UINT16 *)(Buf + 0x2A);
      PhNum = *(UINT16 *)(Buf + 0x2C);
      Class = 32;
    }

    MediaSize = (Part->Media->LastBlock + 1) * Part->Media->BlockSize;
    PhEnd     = PhOff + ((UINT64)PhNum * PhEnt);

    FpDiagPrint (
      "[DIAG] elf#%u class=%u bytes=%lu phoff=0x%lx phnum=%u phend=0x%lx %a\r\n",
      (UINT32)Printed,
      (UINT32)Class,
      MediaSize,
      PhOff,
      (UINT32)PhNum,
      PhEnd,
      (PhEnd > MediaSize) ? "OVERRUN" : "ok"
      );

    Printed++;
  }

  FpDiagPrint ("[DIAG] elf partitions found=%u\r\n", (UINT32)Printed);

  if (Buf != NULL) {
    FreePool (Buf);
  }

  FreePool (Handles);
}

STATIC
VOID
FpDiagDumpUfs (
  VOID
  )
{
  UINT32  Offset;
  UINT32  Value;

  FpDiagPrint ("[DIAG] ufs registers @0x%llx\r\n", UFS_MEM_BASE);

  for (Offset = 0; Offset < 0x100; Offset += 0x10) {
    FpDiagPrint (
      "[DIAG]   %03x: %08x %08x %08x %08x\r\n",
      Offset,
      MmioRead32 (UFS_MEM_BASE + Offset + 0x0),
      MmioRead32 (UFS_MEM_BASE + Offset + 0x4),
      MmioRead32 (UFS_MEM_BASE + Offset + 0x8),
      MmioRead32 (UFS_MEM_BASE + Offset + 0xC)
      );
  }

  Value = MmioRead32 (UFS_MEM_BASE + UFSHCI_HCS);

  //
  // HCS bit0 is Device Present. Reading bit16 here was wrong and made a healthy
  // controller look like it had no device attached.
  //
  FpDiagPrint (
    "[DIAG] ufs HCS=0x%08x dp=%u utrlrdy=%u utmrlrdy=%u\r\n",
    Value,
    (Value >> 0) & 0x1,
    (Value >> 1) & 0x1,
    (Value >> 2) & 0x1
    );

  Value = MmioRead32 (UFS_MEM_BASE + UFSHCI_HCE);
  FpDiagPrint ("[DIAG] ufs HCE=0x%08x\r\n", Value);

  Value = MmioRead32 (UFS_MEM_BASE + UFSHCI_IS);
  FpDiagPrint ("[DIAG] ufs IS =0x%08x\r\n", Value);
}

/**
  Re-print the SecProtocolFinderLib trace that PrePi captured.

  RamPartitionDxe already dumps this early in DXE, but "early in DXE" is at the
  head of a 64 KiB ring - hundreds of QC driver prints follow it, so on a busy
  boot it can be evicted before LOGAUTO reads the ring. The HOB is still there,
  so print it again here, at the tail.
**/
STATIC
VOID
FpDiagDumpSf (
  VOID
  )
{
  VOID   *Hob;
  UINT8  *Text;

  Hob = GetFirstGuidHob ((EFI_GUID *)&mSfDiagHobGuid);
  if (Hob == NULL) {
    FpDiagPrint ("[DIAG] sf: no diag HOB\r\n");
    return;
  }

  Text = (UINT8 *)GET_GUID_HOB_DATA (Hob);
  FpDiagPrint ("[DIAG] sf ---- begin ----\r\n");
  SerialPortWrite (Text, AsciiStrLen ((CHAR8 *)Text));
  FpDiagPrint ("[DIAG] sf ---- end ----\r\n");
}

//
// Which device tree do the Qualcomm drivers actually read?
//
// I2CDxe and DALTLMM both take their configuration from DTBExtnProtocol, and
// both fail on this board: I2CDxe publishes zero I2C masters (so UsbConfigDxe
// reports "usb_shared_i2c_write, not open" and the WCD939x USB switch is never
// programmed) and DALTLMM reports "Failed to enable GPIO status : 0x7". Neither
// prints anything about the tree itself.
//
// DTBExtnProtocol is not a DXE driver - PrePi finds it inside XBL's own image
// (0xA703F0E8 on this board) and hands it over in gEfiSecDtbGuid. So the tree it
// wraps is whatever XBL built, not the complete shennong tree that ships in the
// FV. If XBL's tree is minimal, that is the whole explanation.
//
// This looks for an FDT in the two places it could be and reports whether the
// nodes I2CDxe needs are in it. Pure reads.
//
#define FD_RESERVED_BASE  0xA7000000UL
#define FD_RESERVED_LEN   0x00100000U
#define XBL_DT_BASE       0x81A00000UL
#define XBL_DT_LEN        0x00040000U

//
// Set by FpDiagScanFdtWindow to the first FDT it found, consumed by
// FpDiagParseFdt.
//
STATIC UINTN  mFdtFound = 0;

STATIC
UINT32
FdtBe32 (
  IN volatile UINT8  *P
  )
{
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) |
         ((UINT32)P[2] << 8) | (UINT32)P[3];
}

/**
  Walk the structure block of an FDT and report the bus nodes.

  String matching on the raw blob cannot tell an enabled node from a disabled
  one, and cannot tell whether a controller has any children - which is exactly
  what I2CDxe keys off. This walks the real token stream instead and prints, for
  every node whose name starts with i2c/qup/spmi, its full path, its status
  property, and how many children it has.
**/
STATIC
VOID
FpDiagParseFdt (
  IN UINTN  Fdt
  )
{
  volatile UINT8  *B;
  UINT32          TotalSize;
  UINT32          OffStruct;
  UINT32          OffStrings;
  UINT32          Pos;
  UINT32          Tok;
  CHAR8           Path[256];
  UINTN           PathLen;
  UINTN           PathStack[24];
  BOOLEAN         WatchStack[24];
  UINTN           ChildStack[24];
  CHAR8           StatusStack[24][16];
  UINTN           Depth;
  UINT32          BusNodes;
  UINT32          BusEnabled;

  if (Fdt == 0) {
    return;
  }

  B          = (volatile UINT8 *)Fdt;
  TotalSize  = FdtBe32 (B + 4);
  OffStruct  = FdtBe32 (B + 8);
  OffStrings = FdtBe32 (B + 12);

  if ((TotalSize > 0x200000) || (OffStruct >= TotalSize) || (OffStrings >= TotalSize)) {
    FpDiagPrint ("[DIAG] fdt parse: implausible header\r\n");
    return;
  }

  FpDiagPrint ("[DIAG] fdt parse @0x%lx size=0x%x\r\n", (UINT64)Fdt, TotalSize);

  Path[0]    = '\0';
  PathLen    = 0;
  Depth      = 0;
  Pos        = OffStruct;
  BusNodes   = 0;
  BusEnabled = 0;

  while (Pos + 4 <= TotalSize) {
    Tok  = FdtBe32 (B + Pos);
    Pos += 4;

    if (Tok == 1) {
      //
      // FDT_BEGIN_NODE. The name follows the token, NUL terminated, padded to
      // a 4-byte boundary. The root node's name is empty.
      //
      UINT32 NamePos = Pos;
      UINT32 NameEnd = Pos;
      CHAR8  Name[48];
      UINTN  NameLen;
      BOOLEAN Watch;

      while ((NameEnd < TotalSize) && (B[NameEnd] != 0)) {
        NameEnd++;
      }

      NameLen = 0;
      while ((NamePos < NameEnd) && (NameLen + 1 < sizeof (Name))) {
        Name[NameLen++] = (CHAR8)B[NamePos++];
      }
      Name[NameLen] = '\0';

      //
      // Match on the node name, not the whole path: once inside /soc/i2c@900000
      // every descendant's path also contains "i2c", and matching on the path
      // would watch the children too and lose the parent's status.
      //
      Watch = (AsciiStrnCmp (Name, "i2c", 3) == 0) ||
              (AsciiStrnCmp (Name, "qup", 3) == 0) ||
              (AsciiStrnCmp (Name, "spmi", 4) == 0) ||
              (AsciiStrnCmp (Name, "pmic", 4) == 0);

      if (Depth < 24) {
        PathStack[Depth]        = PathLen;
        WatchStack[Depth]       = Watch;
        ChildStack[Depth]       = 0;
        StatusStack[Depth][0]   = '\0';
      }

      if ((Depth > 0) && (Depth - 1 < 24) && WatchStack[Depth - 1]) {
        ChildStack[Depth - 1]++;
      }

      if ((NameLen > 0) && (PathLen + NameLen + 2 < sizeof (Path))) {
        Path[PathLen++] = '/';
        for (UINTN k = 0; k < NameLen; k++) {
          Path[PathLen++] = Name[k];
        }
        Path[PathLen] = '\0';
      }

      Depth++;
      if (Watch) {
        BusNodes++;
      }

      Pos = (NameEnd + 4) & ~3u;
    } else if (Tok == 2) {
      //
      // FDT_END_NODE. Print on the way out, when the status property has been
      // seen whatever order it came in.
      //
      Depth--;
      if ((Depth < 24) && WatchStack[Depth]) {
        FpDiagPrint (
          "[DIAG] fdt node %a status=%a children=%u\r\n",
          Path,
          (StatusStack[Depth][0] != '\0') ? StatusStack[Depth] : "?",
          ChildStack[Depth]
          );
        if ((StatusStack[Depth][0] == '\0') || (AsciiStrCmp (StatusStack[Depth], "okay") == 0)) {
          BusEnabled++;
        }
      }

      if (Depth < 24) {
        PathLen = PathStack[Depth];
        Path[PathLen] = '\0';
      }
    } else if (Tok == 3) {
      //
      // FDT_PROP: u32 length, u32 name offset into the strings block, then the
      // value padded to a 4-byte boundary.
      //
      UINT32 Len;
      UINT32 NameOff;
      UINT32 NamePos;
      CHAR8  PropName[32];
      UINTN  Copy;

      Len     = FdtBe32 (B + Pos);
      NameOff = FdtBe32 (B + Pos + 4);
      Pos    += 8;

      NamePos = OffStrings + NameOff;
      Copy    = 0;
      while ((NamePos < TotalSize) && (B[NamePos] != 0) && (Copy + 1 < sizeof (PropName))) {
        PropName[Copy++] = (CHAR8)B[NamePos++];
      }
      PropName[Copy] = '\0';

      if ((Depth > 0) && (Depth - 1 < 24) && WatchStack[Depth - 1] &&
          (Len > 0) && (Len < sizeof (StatusStack[0])) &&
          (AsciiStrCmp (PropName, "status") == 0)) {
        for (UINT32 k = 0; (k < Len) && (k + 1 < sizeof (StatusStack[0])); k++) {
          StatusStack[Depth - 1][k] = (CHAR8)B[Pos + k];
        }
        StatusStack[Depth - 1][Len] = '\0';
      }

      Pos = (Pos + Len + 3) & ~3u;
    } else if (Tok == 4) {
      continue;
    } else {
      break;
    }
  }

  FpDiagPrint ("[DIAG] fdt parse: bus nodes=%u enabled=%u\r\n", BusNodes, BusEnabled);
}

STATIC
VOID
FpDiagScanFdtWindow (
  IN CONST CHAR8  *Name,
  IN UINTN        Base,
  IN UINTN        Length
  )
{
  volatile UINT8  *P;
  UINTN           Off;
  UINTN           FdtOff;
  UINT32          FdtSize;
  UINT32          I2cNodes;
  UINT32          SpmiNodes;
  UINT32          ControllerCompat;
  BOOLEAN         HasSocI2cPath;

  P                = (volatile UINT8 *)Base;
  FdtOff           = (UINTN)-1;
  FdtSize          = 0;
  I2cNodes         = 0;
  SpmiNodes        = 0;
  ControllerCompat = 0;
  HasSocI2cPath    = FALSE;

  for (Off = 0; Off + 32 < Length; Off++) {
    if ((P[Off] == 0xD0) && (P[Off + 1] == 0x0D) &&
        (P[Off + 2] == 0xFE) && (P[Off + 3] == 0xED) && (FdtOff == (UINTN)-1)) {
      FdtOff  = Off;
      FdtSize = ((UINT32)P[Off + 4] << 24) | ((UINT32)P[Off + 5] << 16) |
                ((UINT32)P[Off + 6] << 8) | (UINT32)P[Off + 7];
    }

    if ((P[Off] == 'i') && (P[Off + 1] == '2') && (P[Off + 2] == 'c') && (P[Off + 3] == '@')) {
      I2cNodes++;
    }

    if ((P[Off] == 's') && (P[Off + 1] == 'p') && (P[Off + 2] == 'm') && (P[Off + 3] == 'i')) {
      SpmiNodes++;
    }

    if ((P[Off] == 'q') && (P[Off + 1] == 'c') && (P[Off + 2] == 'o') && (P[Off + 3] == 'm') &&
        (P[Off + 4] == ',') && (P[Off + 5] == 'i') && (P[Off + 6] == '2') && (P[Off + 7] == 'c')) {
      ControllerCompat++;
    }

    if ((P[Off] == '/') && (P[Off + 1] == 's') && (P[Off + 2] == 'o') && (P[Off + 3] == 'c') &&
        (P[Off + 4] == '/') && (P[Off + 5] == 'i') && (P[Off + 6] == '2') && (P[Off + 7] == 'c')) {
      HasSocI2cPath = TRUE;
    }
  }

  if (FdtOff == (UINTN)-1) {
    FpDiagPrint ("[DIAG] fdt %a: no FDT magic in 0x%lx+0x%lx\r\n",
                 Name, (UINT64)Base, (UINT64)Length);
  } else {
    FpDiagPrint ("[DIAG] fdt %a: @0x%lx size=0x%x\r\n",
                 Name, (UINT64)(Base + FdtOff), FdtSize);
    if (mFdtFound == 0) {
      mFdtFound = Base + FdtOff;
    }
  }

  FpDiagPrint ("[DIAG] fdt %a: i2c@=%u spmi=%u qcom-i2c=%u /soc/i2c=%a\r\n",
               Name, I2cNodes, SpmiNodes, ControllerCompat,
               HasSocI2cPath ? "yes" : "no");
}

STATIC
VOID
FpDiagDumpDtb (
  VOID
  )
{
  VOID    *Hob;
  UINT64  DtbExtn;
  UINT32  Word;
  UINTN   Index;

  FpDiagScanFdtWindow ("FDRes", FD_RESERVED_BASE, FD_RESERVED_LEN);
  FpDiagScanFdtWindow ("XblDt", XBL_DT_BASE, XBL_DT_LEN);

  //
  // The DTBExtnProtocol pointer itself. Its layout is not in any header we
  // have, so dump the first words and look for one that points at an FDT.
  //
  Hob = GetFirstGuidHob ((EFI_GUID *)&mSecDtbHobGuid);
  if (Hob == NULL) {
    FpDiagPrint ("[DIAG] dtbextn: no HOB\r\n");
    return;
  }

  DtbExtn = *(UINT64 *)GET_GUID_HOB_DATA (Hob);
  FpDiagPrint ("[DIAG] dtbextn=0x%lx\r\n", DtbExtn);
  if (DtbExtn == 0) {
    return;
  }

  for (Index = 0; Index < 8; Index++) {
    Word = MmioRead32 ((UINTN)(DtbExtn + Index * 4));
    FpDiagPrint ("[DIAG]   +%02x = %08x\r\n", (UINT32)(Index * 4), Word);
  }

  //
  // Keep going past the first eight words. The three pointers at +08/+10/+18
  // all land inside XBL's code range, so they are functions; if the struct also
  // carries a data pointer it will be further in, and the interesting one is
  // whichever points into the XBL DT window - that is the tree the Qualcomm
  // drivers actually read, and finding it is what says whether patching the
  // stub at 0x81A02800 would even be seen.
  //
  for (Index = 8; Index < 24; Index++) {
    UINT64 Slot = DtbExtn + Index * 4;
    UINT64 Value;

    Word  = MmioRead32 ((UINTN)Slot);
    Value = (UINT64)Word;
    FpDiagPrint ("[DIAG]   +%02x = %08x", (UINT32)(Index * 4), Word);
    if ((Value >= XBL_DT_BASE) && (Value < (XBL_DT_BASE + XBL_DT_LEN))) {
      FpDiagPrint ("  <- XBL DT +0x%lx\r\n", Value - XBL_DT_BASE);
    } else if ((Value >= FD_RESERVED_BASE) && (Value < (FD_RESERVED_BASE + FD_RESERVED_LEN))) {
      FpDiagPrint ("  <- FD Reserved +0x%lx\r\n", Value - FD_RESERVED_BASE);
    } else {
      FpDiagPrint ("\r\n");
    }
  }

  //
  // Now walk the tree properly. The raw string counts above can only say the
  // names are present; they cannot say whether a node is enabled, or whether it
  // has any children. I2CDxe needs both.
  //
  FpDiagParseFdt (mFdtFound);
}

//
// The QUPv3 serial engine the WCD939x sits on.
//
// Taken from the real device tree, not from the stub: /sys/firmware/fdt on the
// phone has
//
//   /soc/qcom,qupv3_1_geni_se@ac0000/i2c@a8c000
//       wcd939x_i2c@e   reg=0x0e   compatible=qcom,wcd939x-i2c
//
// The stub in the FV (and the copy XBL carries) instead declares a single
// container "i2c@900000" whose reg points at nothing in particular, but its
// per-device children do carry core_base_addr 0x00A00000 plus core_offset
// 0x0008C000 for device 0x0e, which lands exactly on 0xA8C000. So the stub's
// numbers are right even though its node name is not.
//
// These registers are read-only here. GENI_FORCE_DEFAULT_REG and the CFG
// registers come out of reset as 0; if I2CDxe had ever programmed this SE they
// would not be.
//
#define QUP_SE_WCD939X    0x00A8C000UL
#define QUP_SE_NFC        0x00A80000UL

STATIC
VOID
FpDiagDumpQupSe (
  IN CONST CHAR8  *Name,
  IN UINTN        Base
  )
{
  EFI_STATUS             Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  UINT32  Off;
  UINT32  Value;

  //
  // Do not read blind. If the GCD has no descriptor covering this address the
  // page is not mapped, and an MmioRead32 there takes a data abort that would
  // take the whole DIAG page with it.
  //
  ZeroMem (&Desc, sizeof (Desc));
  Status = gDS->GetMemorySpaceDescriptor (Base, &Desc);
  if (EFI_ERROR (Status) || (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    FpDiagPrint ("[DIAG] qup %a @0x%lx not mapped (%r)\r\n", Name, (UINT64)Base, Status);
    return;
  }

  for (Off = 0; Off < 0x40; Off += 0x10) {
    FpDiagPrint (
      "[DIAG] qup %a +%02x: %08x %08x %08x %08x\r\n",
      Name,
      Off,
      MmioRead32 (Base + Off + 0x0),
      MmioRead32 (Base + Off + 0x4),
      MmioRead32 (Base + Off + 0x8),
      MmioRead32 (Base + Off + 0xC)
      );
  }

  //
  // GENI_STATUS bit0 is the "SE is in reset" flag on QUPv3. Reading it back as
  // 1 means nobody ever brought the serial engine out of reset.
  //
  Value = MmioRead32 (Base + 0x0C);
  FpDiagPrint ("[DIAG] qup %a GENI_STATUS=%08x reset=%d\r\n", Name, Value, Value & 1);
}

/**
  Dump read-only diagnostics to the log ring.

  Output goes through SerialPortWrite, which is what puts it in the ring, so the
  LOGAUTO page can carry it off the device.
**/
VOID
FpDiagDump (
  VOID
  )
{
  SerialPortInitialize ();

  FpDiagPrint ("\r\n[DIAG] ---- begin ----\r\n");

  FpDiagDumpSf ();

  FpDiagDumpDtb ();

  FpDiagPrintGcd ("UFS_MEM", UFS_MEM_BASE, UFS_MEM_LEN);
  FpDiagPrintGcd ("UFS_ICE", UFS_ICE_BASE, UFS_ICE_LEN);

  //
  // I2CDxe's depex requires the clock protocol, so a zero here would mean the
  // clock driver never published and I2CDxe could not have run at all. Seeing a
  // non-zero value alongside "i2c master handles=0" narrows the I2C failure to
  // I2CDxe's own device-tree handling.
  //
  FpDiagCountProto ("clock", &mClockProtocolGuid);

  //
  // This is the number that matters. The master protocol below is an EDK2
  // protocol that I2CDxe never installs, so it reads zero on every boot and
  // says nothing; qcom-i2c is the one UsbConfigDxe actually opens.
  //
  FpDiagCountProto ("qcom-i2c", &mQcomI2cProtocolGuid);

  //
  // What kind of port does the charging side think it is? Unknown here means the
  // USB stack is on its "assume SDP" fallback, which would explain the D+/D-
  // pair never being routed far better than anything in the controller.
  //
  FpDiagCharger ();

  //
  // Now actually try to open a device on it. The count above only says the
  // protocol exists; this says whether the serial engine behind it comes up,
  // which is what "usb_shared_i2c_write, not open" is about.
  //
  FpDiagI2cOpen ();

  FpDiagPrintI2c ("master", &mI2cMasterProtocolGuid);
  FpDiagPrintI2c ("io", &mI2cIoProtocolGuid);

  //
  // Whether anything ever touched the serial engines the stub points at. All
  // zero plus GENI_STATUS bit0 set means I2CDxe never got as far as programming
  // a QUP, so the failure is upstream of the register writes.
  //
  FpDiagDumpQupSe ("wcd939x", QUP_SE_WCD939X);
  FpDiagDumpQupSe ("nfc", QUP_SE_NFC);

  FpDiagPrintI2c ("blockio", &mBlockIoGuid);
  FpDiagPrintI2c ("diskio", &mDiskIoGuid);
  FpDiagPrintI2c ("fs", &mSimpleFsGuid);

  FpDiagDumpBlockIo ();

  FpDiagScanPartitions ();

  FpDiagDumpUfs ();

  FpDiagPrint ("[DIAG] ---- end ----\r\n");
}
