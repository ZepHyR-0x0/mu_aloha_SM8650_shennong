/** @file
  Shared layout of the boot log ring in the platform's "UEFI Log" region.

  FrameBufferInMemorySerialPortLib fills the ring, the Front Page QR viewer
  drains it. Layout:

    <RingSize bytes of text><BOOT_LOG_RING_HEADER>

  Only this serial library may write there. Another producer with a different
  header (SecProtocolFinderLib used to keep its own 'SFLG' log at offset 0) is
  read by BootLogRead as log text - its magic came out as the first QR page on
  the device.

  Copyright (c) 2026, Project Mu port. SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef BOOT_LOG_RING_H_
#define BOOT_LOG_RING_H_

//
// 'BLOG'. Marks a header the serial library has taken ownership of; a zeroed
// or foreign region is re-initialized on the first write.
//
#define BOOT_LOG_RING_MAGIC  0x474F4C42U

//
// Fallback location when the memory map lookup fails: "UEFI Log" in
// ShennongPkg/Device/xiaomi-shennong/Library/PlatformMemoryMapLib. Both the
// writer and the reader use it so they can never disagree on the address.
//
#define BOOT_LOG_RING_BASE   0x81CE4000UL
#define BOOT_LOG_RING_SIZE   0x00010000UL

#pragma pack(1)
typedef struct {
  UINT32  Magic;      // BOOT_LOG_RING_MAGIC
  UINT32  Reserved;
  UINT64  Written;    // total bytes ever stored
  UINT64  Offset;     // next write position; oldest byte once wrapped
} BOOT_LOG_RING_HEADER;
#pragma pack()

#endif // BOOT_LOG_RING_H_
