/** @file
  Boot log ring reader.

  The ring is captured by FrameBufferInMemorySerialPortLib into the platform's
  PStore region (falling back to the stock "UEFI Log" region). This helper
  linearizes the last bytes of the ring so the QR pages can display the boot
  log.

  Copyright (c) 2026, Project Mu port. SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef BOOT_LOG_H_
#define BOOT_LOG_H_

//
// How many log bytes the QR pages present. The ring itself is bigger; only
// this tail is turned into QR pages.
//
//
// The ring lives in the "UEFI Log" region, which is 64 KB, so a 16 KB read window
// only ever returned the tail and threw away the early boot - exactly where the UFS
// drivers run. 60 KB leaves room for the 16-byte ring header.
//
#define BOOT_LOG_MAX_BYTES  (64 * 1024)

EFI_STATUS
BootLogRead (
  OUT CHAR8  *Buffer,
  IN  UINTN  BufferSize,
  OUT UINTN  *LogLength
  );

#endif // BOOT_LOG_H_
