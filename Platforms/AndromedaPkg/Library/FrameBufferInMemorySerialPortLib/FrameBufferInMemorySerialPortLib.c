/** @file

  Copyright (c) 2022-2024 DuoWoA authors

  SPDX-License-Identifier: MIT

**/
#include <PiDxe.h>

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/SerialPortLib.h>

#include <Resources/FbColor.h>
#include <Resources/font5x12.h>

#include <Library/FrameBufferSerialPortLib.h>
#include <Library/BootLogRing.h>

ARM_MEMORY_REGION_DESCRIPTOR_EX DisplayMemoryRegion;
ARM_MEMORY_REGION_DESCRIPTOR_EX PStore;

FBCON_POSITION* p_Position = NULL;
FBCON_POSITION m_MaxPosition;
FBCON_COLOR    m_Color;
BOOLEAN        m_Initialized = FALSE;

UINTN gWidth = FixedPcdGet32(PcdMipiFrameBufferWidth);
// Reserve half screen for output
UINTN gHeight = FixedPcdGet32(PcdMipiFrameBufferHeight);
UINTN gBpp    = FixedPcdGet32(PcdMipiFrameBufferPixelBpp);

// Module-used internal routine
void FbConPutCharWithFactor(char c, int type, unsigned scale_factor);

void FbConDrawglyph(
    char *pixels, unsigned stride, unsigned bpp, unsigned *glyph,
    unsigned scale_factor);

void FbConReset(void);
void FbConScrollUp(void);
void FbConFlush(void);

RETURN_STATUS
EFIAPI
SerialPortInitialize(VOID)
{
  EFI_STATUS Status;

  // Prevent dup initialization
  if (m_Initialized)
    return RETURN_SUCCESS;

  Status = LocateMemoryMapAreaByName("Display Reserved", &DisplayMemoryRegion);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  p_Position = (FBCON_POSITION*)(DisplayMemoryRegion.Address + (FixedPcdGet32(PcdMipiFrameBufferWidth) * FixedPcdGet32(PcdMipiFrameBufferHeight) * FixedPcdGet32(PcdMipiFrameBufferPixelBpp) / 8));

  // Reset console
  FbConReset();

  // Set flag
  m_Initialized = TRUE;

  //
  // Locate the log ring. Platforms that reserve a dedicated "PStore" region
  // use it; ports like shennong keep the boot log in the stock Qualcomm
  // "UEFI Log" region and have no PStore entry. Fall back to the known address
  // so the ring exists even if the lookup is unavailable in this phase.
  //
  Status = LocateMemoryMapAreaByName("PStore", &PStore);
  if (EFI_ERROR(Status)) {
    Status = LocateMemoryMapAreaByName("UEFI Log", &PStore);
  }

  if (EFI_ERROR(Status) || (PStore.Address == 0) ||
      (PStore.Length <= sizeof(BOOT_LOG_RING_HEADER))) {
    PStore.Address = BOOT_LOG_RING_BASE;
    PStore.Length  = BOOT_LOG_RING_SIZE;
    Status         = EFI_SUCCESS;
  }

  return Status;
}

void ResetFb(void)
{
  // Clear current screen.
  char *Pixels  = (void *)DisplayMemoryRegion.Address;
  UINTN BgColor = FB_BGRA8888_BLACK;

  // Set to black color.
  for (UINTN i = 0; i < gWidth; i++) {
    for (UINTN j = 0; j < gHeight; j++) {
      BgColor = FB_BGRA8888_BLACK;
      // Set pixel bit
      for (UINTN p = 0; p < (gBpp / 8); p++) {
        *Pixels = (unsigned char)BgColor;
        BgColor = BgColor >> 8;
        Pixels++;
      }
    }
  }
}

// Rounded-corner safe margins (pixels). Xiaomi 14 corner radius ~90px.
#define FBCON_MARGIN_X 64
#define FBCON_MARGIN_Y 96

void FbConReset(void)
{
  // Calc max position (leave margin so rounded corners don't clip text).
  m_MaxPosition.x = (gWidth - 2 * FBCON_MARGIN_X) / (FONT_WIDTH + 1);
  m_MaxPosition.y = (gHeight - 2 * FBCON_MARGIN_Y - 1) / FONT_HEIGHT;

  // Reset color.
  m_Color.Foreground = FB_BGRA8888_WHITE;
  m_Color.Background = FB_BGRA8888_BLACK;
}

void FbConPutCharWithFactor(char c, int type, unsigned scale_factor)
{
  char *Pixels;

paint:

  if ((unsigned char)c > 127)
    return;

  if ((unsigned char)c < 32) {
    if (c == '\n') {
      goto newline;
    }
    else if (c == '\r') {
      p_Position->x = 0;
      return;
    }
    else {
      return;
    }
  }

  // Save some space
  if (p_Position->x == 0 && (unsigned char)c == ' ' &&
      type != FBCON_SUBTITLE_MSG && type != FBCON_TITLE_MSG)
    return;

  BOOLEAN intstate = ArmGetInterruptState();
  if (intstate)
    ArmDisableInterrupts();

  Pixels = (void *)DisplayMemoryRegion.Address;
  Pixels += FBCON_MARGIN_Y * ((gBpp / 8) * gWidth);
  Pixels += FBCON_MARGIN_X * (gBpp / 8);
  Pixels += p_Position->y * ((gBpp / 8) * FONT_HEIGHT * gWidth);
  Pixels += p_Position->x * scale_factor * ((gBpp / 8) * (FONT_WIDTH + 1));

  FbConDrawglyph(
      Pixels, gWidth, (gBpp / 8), font5x12 + (c - 32) * 2, scale_factor);

  p_Position->x++;

  if (p_Position->x >= (int)(m_MaxPosition.x / scale_factor))
    goto newline;

  if (intstate)
    ArmEnableInterrupts();
  return;

newline:
  p_Position->y += scale_factor;
  p_Position->x = 0;
  if (p_Position->y >= m_MaxPosition.y - scale_factor) {
    ResetFb();
    FbConFlush();
    p_Position->y = 0;

    if (intstate)
      ArmEnableInterrupts();
    goto paint;
  }
  else {
    FbConFlush();
    if (intstate)
      ArmEnableInterrupts();
  }
}

void FbConDrawglyph(
    char *pixels, unsigned stride, unsigned bpp, unsigned *glyph,
    unsigned scale_factor)
{
  char *       bg_pixels = pixels;
  unsigned     x, y, i, j, k;
  unsigned     data, temp;
  unsigned int fg_color = m_Color.Foreground;
  unsigned int bg_color = m_Color.Background;
  stride -= FONT_WIDTH * scale_factor;

  for (y = 0; y < FONT_HEIGHT / 2; ++y) {
    for (i = 0; i < scale_factor; i++) {
      for (x = 0; x < FONT_WIDTH; ++x) {
        for (j = 0; j < scale_factor; j++) {
          bg_color = m_Color.Background;
          for (k = 0; k < bpp; k++) {
            *bg_pixels = (unsigned char)bg_color;
            bg_color   = bg_color >> 8;
            bg_pixels++;
          }
        }
      }
      bg_pixels += (stride * bpp);
    }
  }

  for (y = 0; y < FONT_HEIGHT / 2; ++y) {
    for (i = 0; i < scale_factor; i++) {
      for (x = 0; x < FONT_WIDTH; ++x) {
        for (j = 0; j < scale_factor; j++) {
          bg_color = m_Color.Background;
          for (k = 0; k < bpp; k++) {
            *bg_pixels = (unsigned char)bg_color;
            bg_color   = bg_color >> 8;
            bg_pixels++;
          }
        }
      }
      bg_pixels += (stride * bpp);
    }
  }

  data = glyph[0];
  for (y = 0; y < FONT_HEIGHT / 2; ++y) {
    temp = data;
    for (i = 0; i < scale_factor; i++) {
      data = temp;
      for (x = 0; x < FONT_WIDTH; ++x) {
        if (data & 1) {
          for (j = 0; j < scale_factor; j++) {
            fg_color = m_Color.Foreground;
            for (k = 0; k < bpp; k++) {
              *pixels  = (unsigned char)fg_color;
              fg_color = fg_color >> 8;
              pixels++;
            }
          }
        }
        else {
          for (j = 0; j < scale_factor; j++) {
            pixels = pixels + bpp;
          }
        }
        data >>= 1;
      }
      pixels += (stride * bpp);
    }
  }

  data = glyph[1];
  for (y = 0; y < FONT_HEIGHT / 2; ++y) {
    temp = data;
    for (i = 0; i < scale_factor; i++) {
      data = temp;
      for (x = 0; x < FONT_WIDTH; ++x) {
        if (data & 1) {
          for (j = 0; j < scale_factor; j++) {
            fg_color = m_Color.Foreground;
            for (k = 0; k < bpp; k++) {
              *pixels  = (unsigned char)fg_color;
              fg_color = fg_color >> 8;
              pixels++;
            }
          }
        }
        else {
          for (j = 0; j < scale_factor; j++) {
            pixels = pixels + bpp;
          }
        }
        data >>= 1;
      }
      pixels += (stride * bpp);
    }
  }
}

/* TODO: Take stride into account */
void FbConScrollUp(void)
{
  unsigned short *dst   = (void *)DisplayMemoryRegion.Address;
  unsigned short *src   = dst + (gWidth * FONT_HEIGHT);
  unsigned        count = gWidth * (gHeight - FONT_HEIGHT);

  while (count--) {
    *dst++ = *src++;
  }

  count = gWidth * FONT_HEIGHT;
  while (count--) {
    *dst++ = m_Color.Background;
  }

  FbConFlush();
}

void FbConFlush(void)
{
  unsigned total_x, total_y;
  unsigned bytes_per_bpp;

  total_x       = gWidth;
  total_y       = gHeight;
  bytes_per_bpp = (gBpp / 8);

  WriteBackInvalidateDataCacheRange(
      (void *)DisplayMemoryRegion.Address,
      (total_x * total_y * bytes_per_bpp));
}

//
// The ring lives in the shared "UEFI Log" region: <text bytes><header>, see
// BootLogRing.h. Nothing else may write into that region - a second producer
// with its own header is read back as log text.
//
static void mem_putchar(UINT8 c)
{
  UINT8 *base;
  UINTN  size;
  UINTN  pos;
  BOOT_LOG_RING_HEADER *hdr;

  if ((PStore.Address == 0) ||
      (PStore.Length <= sizeof(BOOT_LOG_RING_HEADER))) {
    return;
  }

  base = (UINT8 *)PStore.Address;
  size = PStore.Length - sizeof(BOOT_LOG_RING_HEADER);
  hdr  = (BOOT_LOG_RING_HEADER *)(base + size);

  //
  // The region is zeroed at boot, so an unknown header means "first write of
  // this boot" (or foreign data); take ownership and start from the beginning.
  //
  if ((hdr->Magic != BOOT_LOG_RING_MAGIC) || (hdr->Offset >= size)) {
    hdr->Magic    = BOOT_LOG_RING_MAGIC;
    hdr->Reserved = 0;
    hdr->Written  = 0;
    hdr->Offset   = 0;
  }

  pos               = (UINTN)hdr->Offset;
  base[pos]         = c;
  hdr->Offset       = (pos + 1) % size;
  hdr->Written++;

  //
  // Flush the byte's line and the header only. Flushing all 64 KiB per
  // character made startup crawl.
  //
  WriteBackInvalidateDataCacheRange(&base[pos], 1);
  WriteBackInvalidateDataCacheRange(hdr, sizeof(*hdr));
}

UINTN
EFIAPI
SerialPortWrite(IN UINT8 *Buffer, IN UINTN NumberOfBytes)
{
  UINT8 *CONST Final          = &Buffer[NumberOfBytes];
  UINTN        InterruptState = ArmGetInterruptState();

  if (InterruptState)
    ArmDisableInterrupts();

  for (UINTN i = 0; i < NumberOfBytes; i++) {
    mem_putchar(Buffer[i]);
  }

  while (Buffer < Final) {
    FbConPutCharWithFactor(*Buffer++, FBCON_COMMON_MSG, SCALE_FACTOR);
  }

  if (InterruptState)
    ArmEnableInterrupts();
  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortWriteCritical(IN UINT8 *Buffer, IN UINTN NumberOfBytes)
{
  UINT8 *CONST Final             = &Buffer[NumberOfBytes];
  UINTN        CurrentForeground = m_Color.Foreground;
  UINTN        InterruptState    = ArmGetInterruptState();

  if (InterruptState)
    ArmDisableInterrupts();

  for (UINTN i = 0; i < NumberOfBytes; i++) {
    mem_putchar(Buffer[i]);
  }

  m_Color.Foreground = FB_BGRA8888_YELLOW;

  while (Buffer < Final) {
    FbConPutCharWithFactor(*Buffer++, FBCON_COMMON_MSG, SCALE_FACTOR);
  }

  m_Color.Foreground = CurrentForeground;

  if (InterruptState)
    ArmEnableInterrupts();
  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead(OUT UINT8 *Buffer, IN UINTN NumberOfBytes) { return 0; }

BOOLEAN
EFIAPI
SerialPortPoll(VOID) { return FALSE; }

RETURN_STATUS
EFIAPI
SerialPortSetControl(IN UINT32 Control) { return RETURN_UNSUPPORTED; }

RETURN_STATUS
EFIAPI
SerialPortGetControl(OUT UINT32 *Control) { return RETURN_UNSUPPORTED; }

RETURN_STATUS
EFIAPI
SerialPortSetAttributes(
    IN OUT UINT64 *BaudRate, IN OUT UINT32 *ReceiveFifoDepth,
    IN OUT UINT32 *Timeout, IN OUT EFI_PARITY_TYPE *Parity,
    IN OUT UINT8 *DataBits, IN OUT EFI_STOP_BITS_TYPE *StopBits)
{
  //
  // PORT: report success instead of RETURN_UNSUPPORTED.
  //
  // This console has no baud rate / framing, but prebuilt Qualcomm drivers ask
  // the ShLib "SerialPort Lib" for SetAttributes and treat any error as fatal
  // for their entry point (SimpleTextInOutSerial returns EFI_UNSUPPORTED from
  // EfiEntry, which trips ASSERT_EFI_ERROR in its AutoGen.c). Report the
  // canonical 115200 8N1 values and succeed; nothing is actually changed.
  //
  if (BaudRate != NULL) {
    *BaudRate = 115200;
  }

  if (ReceiveFifoDepth != NULL) {
    *ReceiveFifoDepth = 0;
  }

  if (Timeout != NULL) {
    *Timeout = 0;
  }

  if (Parity != NULL) {
    *Parity = NoParity;
  }

  if (DataBits != NULL) {
    *DataBits = 8;
  }

  if (StopBits != NULL) {
    *StopBits = OneStopBit;
  }

  return RETURN_SUCCESS;
}

UINTN SerialPortFlush(VOID) { return 0; }

VOID EnableSynchronousSerialPortIO(VOID)
{
  // Already synchronous
}