/** @file
  QrCode.h - generate a QR symbol and paint it through the Simple Window Manager.

  Scope is deliberately narrow: byte mode, error correction level L, versions 1-10.
  Versions 1-5 use one Reed-Solomon block each; versions 6-9 use two and version 10
  uses four, interleaved. That covers 17 to 271 bytes, which is what a screen of log
  text or a short status string needs, and it avoids the larger interleaving tables
  that make a full encoder large.

  A standard decoder (cv2.QRCodeDetector and friends) reads the result as UTF-8
  text, so no ECI or structured-append handling is required.

  Copyright (c) 2026, ProjectMu shennong port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef QR_CODE_H_
#define QR_CODE_H_

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleWindowManager.h>

//
// Largest symbol this encoder produces: version 10 is 57x57 modules and holds
// 271 data bytes at error correction level L.
//
#define QR_MAX_MODULES  57
#define QR_MAX_BYTES    271

/**
  Encode a raw byte string into a module matrix.

  @param[in]  Data        Bytes to encode, 1..271 bytes. Embedded NUL bytes are
                          data, which is what LOGAUTO needs for its 0x00 start.
  @param[in]  Length      Number of bytes in Data.
  @param[out] Matrix      QR_MAX_MODULES * QR_MAX_MODULES bytes, 1 = dark.
                          Row-major, Matrix[Row * Size + Column].
  @param[out] Size        Modules per side: 21 + 4 * (Version - 1), 21..57.
  @param[out] Version     QR version 1..10.

  @retval EFI_SUCCESS            Encoded.
  @retval EFI_INVALID_PARAMETER  Null argument or zero length.
  @retval EFI_BUFFER_TOO_SMALL   Data longer than version 10 allows.
**/
EFI_STATUS
QrEncodeBytes (
  IN  CONST UINT8  *Data,
  IN  UINTN        Length,
  OUT UINT8        *Matrix,
  OUT UINTN        *Size,
  OUT UINTN        *Version
  );

/**
  Encode a NUL-terminated string. Wrapper over QrEncodeBytes for text pages.
**/
EFI_STATUS
QrEncode (
  IN  CONST CHAR8  *Text,
  OUT UINT8        *Matrix,
  OUT UINTN        *Size,
  OUT UINTN        *Version
  );

/**
  Draw a raw byte payload as a white field with the symbol centred, with a quiet
  zone, using the window manager's BltWindow so the rendering engine sees the
  surface being painted.

  @param[in] Swm          Simple Window Manager whose BltWindow performs the draw.
  @param[in] ImageHandle  Handle of the image this draw belongs to.
  @param[in] AreaX        Left edge of the content area in pixels.
  @param[in] AreaY        Top edge of the content area in pixels.
  @param[in] AreaWidth    Width of the content area in pixels.
  @param[in] AreaHeight   Height of the content area in pixels.
  @param[in] Data         Bytes to encode.
  @param[in] Length       Number of bytes in Data.
  @param[in] Scale        Pixels per module; clamped so the symbol fits the area.

  @retval EFI_SUCCESS           Drawn.
  @retval EFI_INVALID_PARAMETER Null Swm, empty payload or zero area.
  @retval other                 QrEncodeBytes failure or a Blt error; nothing may
                                be drawn.
**/
EFI_STATUS
QrPaintBytes (
  IN MS_SIMPLE_WINDOW_MANAGER_PROTOCOL  *Swm,
  IN EFI_HANDLE                         ImageHandle,
  IN UINTN                              AreaX,
  IN UINTN                              AreaY,
  IN UINTN                              AreaWidth,
  IN UINTN                              AreaHeight,
  IN CONST UINT8                        *Data,
  IN UINTN                              Length,
  IN UINTN                              Scale
  );

/**
  Draw a NUL-terminated string. Wrapper over QrPaintBytes.
**/
EFI_STATUS
QrPaint (
  IN MS_SIMPLE_WINDOW_MANAGER_PROTOCOL  *Swm,
  IN EFI_HANDLE                         ImageHandle,
  IN UINTN                              AreaX,
  IN UINTN                              AreaY,
  IN UINTN                              AreaWidth,
  IN UINTN                              AreaHeight,
  IN CONST CHAR8                        *Text,
  IN UINTN                              Scale
  );

#endif // QR_CODE_H_
