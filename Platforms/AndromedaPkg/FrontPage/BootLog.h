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
// The entire region is 64 KiB including its ring header. Reads never exceed
// either the actual ring capacity or this 64 KiB snapshot allocation.
//
#define BOOT_LOG_MAX_BYTES  (256 * 1024)

EFI_STATUS
BootLogRead (
  OUT CHAR8  *Buffer,
  IN  UINTN  BufferSize,
  OUT UINTN  *LogLength
  );

EFI_STATUS BootLogSave (CONST CHAR8 *Phase);
EFI_STATUS BootLogReadStored (CHAR8 *Buffer, UINTN *Length, BOOLEAN Previous);

#endif // BOOT_LOG_H_
