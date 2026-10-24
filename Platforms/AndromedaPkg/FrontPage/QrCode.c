/** @file
  QrCode.c - byte-mode QR encoder (ECC level L, versions 1-10) and SWM painter.

  Structure follows the reference algorithm closely enough to be checkable: the
  Reed-Solomon remainder and the format/version BCH codes are computed exactly as
  the specification defines them. tools/qr_selftest.py (checked into the repo root)
  mirrors this encoder in Python and verifies both by recomputing their syndromes
  and asserting the known format constant, so a regression in the maths shows up
  when that script is run rather than as an unscannable symbol on the phone.

  Versions 1-5 use a single Reed-Solomon block at level L; versions 6-9 use two
  and version 10 uses four, so the final codeword stream interleaves the per-block
  streams.

  Copyright (c) 2026, ProjectMu shennong port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "QrCode.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>

//
// Level L, versions 1-10. Block counts are per the ISO 18004 table: versions
// 6-9 have two blocks, version 10 has four (69, 69, 68, 68 data codewords).
//
//
// UINT16, not UINT8: version 10 needs 274 data codewords and 274 does not fit in a
// byte. As UINT8 it wrapped to 18, so the capacity check saw 18*8 = 144 bits and
// decided version 10 could hold 15 bytes. Every page longer than 231 bytes then had
// no version that fit and QrPaint returned EFI_BUFFER_TOO_SMALL - which is why pages
// 4 onward failed while the first few, shortened by the newline search, still worked.
//
STATIC CONST UINT16 mDataCodewords[10] = { 19, 34, 55, 80, 108, 136, 156, 194, 232, 274 };
STATIC CONST UINT8  mEccCodewords[10]  = {  7, 10, 15, 20,  26,  36,  40,  48,  60,  72 };
STATIC CONST UINT8  mBlockCount[10]    = {  1,  1,  1,  1,   1,   2,   2,   2,   2,   4 };
STATIC CONST UINT8  mBlockEcc[10]      = {  7, 10, 15, 20,  26,  18,  20,  24,  30,  18 };

//
// Alignment pattern centre coordinates per version, ISO 18004 Annex E. The 6s
// are part of the grid; patterns at the three finder corners are omitted.
//
STATIC CONST UINT8  mAlignCount[10] = { 0, 2, 2, 2, 2, 2, 3, 3, 3, 3 };
STATIC CONST UINT8  mAlignPos[10][7] = {
  {  0,  0,  0,  0,  0,  0,  0 },
  {  6, 18,  0,  0,  0,  0,  0 },
  {  6, 22,  0,  0,  0,  0,  0 },
  {  6, 26,  0,  0,  0,  0,  0 },
  {  6, 30,  0,  0,  0,  0,  0 },
  {  6, 34,  0,  0,  0,  0,  0 },
  {  6, 22, 38,  0,  0,  0,  0 },
  {  6, 24, 42,  0,  0,  0,  0 },
  {  6, 26, 46,  0,  0,  0,  0 },
  {  6, 28, 50,  0,  0,  0,  0 },
};

STATIC UINT8  mGfExp[512];
STATIC UINT8  mGfLog[256];
STATIC BOOLEAN  mGfReady = FALSE;

STATIC
VOID
GfInit (
  VOID
  )
{
  UINT32  I;
  UINT32  X;

  if (mGfReady) {
    return;
  }

  X = 1;
  for (I = 0; I < 255; I++) {
    mGfExp[I] = (UINT8)X;
    mGfLog[X] = (UINT8)I;
    X <<= 1;
    if ((X & 0x100) != 0) {
      X ^= 0x11D;             // x^8 + x^4 + x^3 + x^2 + 1
    }
  }

  for (I = 255; I < 512; I++) {
    mGfExp[I] = mGfExp[I - 255];
  }

  mGfReady = TRUE;
}

STATIC
UINT8
GfMul (
  IN UINT8  A,
  IN UINT8  B
  )
{
  if ((A == 0) || (B == 0)) {
    return 0;
  }

  return mGfExp[mGfLog[A] + mGfLog[B]];
}

/**
  Compute the ECC codewords for one block: remainder of data * x^EccCount mod g(x).
**/
STATIC
VOID
RsCompute (
  IN  CONST UINT8  *Data,
  IN  UINTN        DataLen,
  OUT UINT8        *Ecc,
  IN  UINTN        EccLen
  )
{
  UINT8   Gen[64];
  UINT8   Rem[64];
  UINTN   I;
  UINTN   J;
  UINT8   Factor;

  //
  // g(x) = (x - a^0)(x - a^1)...(x - a^(EccLen-1)), stored highest term first
  // without the leading 1.
  //
  ZeroMem (Gen, sizeof (Gen));
  Gen[EccLen - 1] = 1;
  for (I = 0; I < EccLen; I++) {
    for (J = 0; J < EccLen; J++) {
      Gen[J] = GfMul (Gen[J], mGfExp[I]);
      if (J + 1 < EccLen) {
        Gen[J] ^= Gen[J + 1];
      }
    }
  }

  ZeroMem (Rem, sizeof (Rem));
  for (I = 0; I < DataLen; I++) {
    Factor = Data[I] ^ Rem[0];
    CopyMem (&Rem[0], &Rem[1], EccLen - 1);
    Rem[EccLen - 1] = 0;
    for (J = 0; J < EccLen; J++) {
      Rem[J] ^= GfMul (Gen[J], Factor);
    }
  }

  CopyMem (Ecc, Rem, EccLen);
}

/**
  BCH(15,5) format information, masked with 0x5412 as the specification requires.
**/
STATIC
UINT16
FormatBits (
  IN UINT8  EccLevelBits,
  IN UINT8  Mask
  )
{
  UINT32  Data;
  UINT32  Rem;
  UINTN   I;

  Data = ((UINT32)EccLevelBits << 3) | Mask;
  Rem  = Data;
  for (I = 0; I < 10; I++) {
    Rem = (Rem << 1) ^ ((Rem >> 9) * 0x537);
  }

  return (UINT16)(((Data << 10) | Rem) ^ 0x5412);
}

STATIC
VOID
SetFunc (
  IN OUT UINT8    *Mod,
  IN OUT BOOLEAN  *IsFunc,
  IN UINTN        Size,
  IN UINTN        Row,
  IN UINTN        Col,
  IN BOOLEAN      Dark
  )
{
  Mod[Row * Size + Col]    = Dark ? 1 : 0;
  IsFunc[Row * Size + Col] = TRUE;
}

STATIC
VOID
DrawFinder (
  IN OUT UINT8    *Mod,
  IN OUT BOOLEAN  *IsFunc,
  IN UINTN        Size,
  IN UINTN        Row,
  IN UINTN        Col
  )
{
  INTN  Dr;
  INTN  Dc;
  INTN  R;
  INTN  C;
  INTN  Dist;

  for (Dr = -1; Dr <= 7; Dr++) {
    for (Dc = -1; Dc <= 7; Dc++) {
      R = (INTN)Row + Dr;
      C = (INTN)Col + Dc;
      if ((R < 0) || (C < 0) || (R >= (INTN)Size) || (C >= (INTN)Size)) {
        continue;
      }

      if ((Dr >= 0) && (Dr <= 6) && (Dc >= 0) && (Dc <= 6)) {
        Dist = ABS (Dr - 3) > ABS (Dc - 3) ? ABS (Dr - 3) : ABS (Dc - 3);
        SetFunc (Mod, IsFunc, Size, (UINTN)R, (UINTN)C, (BOOLEAN)(Dist != 2));
      } else {
        // Separator: the ring of light around the finder.
        SetFunc (Mod, IsFunc, Size, (UINTN)R, (UINTN)C, FALSE);
      }
    }
  }
}

STATIC
VOID
DrawAlignment (
  IN OUT UINT8    *Mod,
  IN OUT BOOLEAN  *IsFunc,
  IN UINTN        Size,
  IN UINTN        Row,
  IN UINTN        Col
  )
{
  INTN  Dr;
  INTN  Dc;
  INTN  R;
  INTN  C;
  INTN  Dist;

  for (Dr = -2; Dr <= 2; Dr++) {
    for (Dc = -2; Dc <= 2; Dc++) {
      R = (INTN)Row + Dr;
      C = (INTN)Col + Dc;
      if ((R < 0) || (C < 0) || (R >= (INTN)Size) || (C >= (INTN)Size)) {
        continue;
      }

      Dist = ABS (Dr) > ABS (Dc) ? ABS (Dr) : ABS (Dc);
      SetFunc (Mod, IsFunc, Size, (UINTN)R, (UINTN)C, (BOOLEAN)(Dist != 1));
    }
  }
}

/**
  BCH(18,6) version information, unmasked as the specification stores it.

  The generator is x^12 + x^11 + x^10 + x^9 + x^8 + x^5 + x^2 + 1 (0x1F25). The
  commonly published (rem >> 11) * 0x1F25 trick is invalid here because the
  initial value (version << 12) is wider than the field, so this uses the plain
  shift-and-conditional-XOR division instead.
**/
STATIC
UINT32
VersionBits (
  IN UINTN  Version
  )
{
  UINT32  Rem;
  UINTN   I;

  Rem = (UINT32)Version;
  for (I = 0; I < 12; I++) {
    Rem <<= 1;
    if ((Rem & 0x1000) != 0) {
      Rem ^= 0x1F25;
    }
  }

  return ((UINT32)Version << 12) | Rem;
}

/**
  Draw the 18-bit version information blocks (versions 7 and up): one above the
  bottom-left finder, one left of the top-right finder. Both blocks are 3 rows by
  6 columns, transposes of each other; transposing either one leaves the bottom-left
  finder trampled and the symbol undecodable.
**/
STATIC
VOID
DrawVersionInfo (
  IN OUT UINT8    *Mod,
  IN OUT BOOLEAN  *IsFunc,
  IN UINTN        Size,
  IN UINTN        Version
  )
{
  UINT32  Info;
  UINTN   I;

  Info = VersionBits (Version);
  for (I = 0; I < 18; I++) {
    SetFunc (Mod, IsFunc, Size, I / 3, Size - 11 + I % 3, (BOOLEAN)((Info >> I) & 1));
    SetFunc (Mod, IsFunc, Size, Size - 11 + I % 3, I / 3, (BOOLEAN)((Info >> I) & 1));
  }
}

STATIC
EFI_STATUS
QrEncodeData (
  IN  CONST UINT8  *Data,
  IN  UINTN        Length,
  OUT UINT8        *Matrix,
  OUT UINTN        *Size,
  OUT UINTN        *Version
  )
{
  UINTN   Len;
  UINTN   Ver;
  UINTN   DataLen;
  UINTN   EccLen;
  UINTN   NumBlocks;
  UINTN   ShortLen;
  UINTN   NumLong;
  UINTN   NumShort;
  UINTN   BlkEcc;
  UINTN   BlkOff;
  UINTN   BlkLen;
  UINTN   W;
  UINTN   Cap;
  BOOLEAN Found;
  UINT8   Bits[280];
  UINTN   BitCount;
  UINT8   CodewordData[274];
  UINT8   Ecc[80];
  UINT8   Codewords[346];
  UINTN   I;
  UINTN   A;
  UINTN   B;
  UINTN   Row;
  UINTN   Col;
  INTN    Right;
  UINTN   Side;
  BOOLEAN  IsFunc[QR_MAX_MODULES * QR_MAX_MODULES];
  UINTN   Idx;
  BOOLEAN  Upward;
  UINTN   Mod;
  UINT16  Fmt;

  if ((Data == NULL) || (Matrix == NULL) || (Size == NULL) || (Version == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  GfInit ();

  Len = Length;
  if (Len == 0) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Pick the smallest version that holds mode + length + payload. Versions 1-9
  // carry the byte count in 8 bits; versions 10 and up use 16.
  //
  Ver   = 0;
  Found = FALSE;
  for (I = 0; I < ARRAY_SIZE (mDataCodewords); I++) {
    Cap = mDataCodewords[I] * 8;
    if ((4 + ((I >= 9) ? 16 : 8) + Len * 8) <= Cap) {
      Ver   = I;
      Found = TRUE;
      break;
    }
  }

  if (!Found) {
    return EFI_BUFFER_TOO_SMALL;
  }

  DataLen = mDataCodewords[Ver];
  EccLen  = mEccCodewords[Ver];
  Side    = 17 + 4 * (Ver + 1);

  //
  // Bit stream: mode 0100, character count, payload, terminator, pad.
  //
  ZeroMem (Bits, sizeof (Bits));
  BitCount = 0;
#define PUSH_BITS(value, count)                        \
  do {                                                 \
    UINTN  _i;                                         \
    for (_i = 0; _i < (UINTN)(count); _i++) {          \
      Bits[BitCount >> 3] |= (UINT8)((((value) >> ((count) - 1 - _i)) & 1) << (7 - (BitCount & 7))); \
      BitCount++;                                      \
    }                                                  \
  } while (FALSE)

  PUSH_BITS (0x4, 4);
  PUSH_BITS (Len, (Ver >= 9) ? 16 : 8);
  for (I = 0; I < Len; I++) {
    PUSH_BITS (Data[I], 8);
  }

  //
  // Terminator, then pad to a codeword boundary, then the 0xEC/0x11 pad pattern.
  //
  {
    UINTN  Term = DataLen * 8 - BitCount;
    if (Term > 4) {
      Term = 4;
    }

    PUSH_BITS (0, Term);
  }

  ZeroMem (CodewordData, sizeof (CodewordData));
  CopyMem (CodewordData, Bits, BitCount >> 3);
  if ((BitCount & 7) != 0) {
    CodewordData[BitCount >> 3] = Bits[BitCount >> 3] & (UINT8)(0xFF << (8 - (BitCount & 7)));
  }

  for (I = (BitCount + 7) >> 3; I < DataLen; I++) {
    CodewordData[I] = ((I - ((BitCount + 7) >> 3)) & 1) ? 0x11 : 0xEC;
  }
#undef PUSH_BITS

  NumBlocks = mBlockCount[Ver];
  BlkEcc    = mBlockEcc[Ver];
  ShortLen  = DataLen / NumBlocks;
  NumLong   = DataLen % NumBlocks;
  NumShort  = NumBlocks - NumLong;

  for (B = 0; B < NumBlocks; B++) {
    BlkOff = B * ShortLen + ((B > NumShort) ? (B - NumShort) : 0);
    BlkLen = ShortLen + ((B >= NumShort) ? 1 : 0);
    RsCompute (CodewordData + BlkOff, BlkLen, Ecc + B * BlkEcc, BlkEcc);
  }

  //
  // Interleave one codeword from each block per column, short blocks first as
  // ISO 18004 lays them out. The extra codeword of the long blocks sits in the
  // last column, so only the long blocks emit there; the ECC follows,
  // interleaved the same way.
  //
  W = 0;
  for (I = 0; I < ShortLen; I++) {
    for (B = 0; B < NumBlocks; B++) {
      Codewords[W++] = CodewordData[B * ShortLen + ((B > NumShort) ? (B - NumShort) : 0) + I];
    }
  }

  for (B = NumShort; B < NumBlocks; B++) {
    Codewords[W++] = CodewordData[(B + 1) * ShortLen + (B - NumShort)];
  }

  for (I = 0; I < BlkEcc; I++) {
    for (B = 0; B < NumBlocks; B++) {
      Codewords[W++] = Ecc[B * BlkEcc + I];
    }
  }

  //
  // Lay out function patterns, then the data in the standard zigzag.
  //
  ZeroMem (Matrix, QR_MAX_MODULES * QR_MAX_MODULES);
  ZeroMem (IsFunc, sizeof (IsFunc));

  DrawFinder (Matrix, IsFunc, Side, 0, 0);
  DrawFinder (Matrix, IsFunc, Side, 0, Side - 7);
  DrawFinder (Matrix, IsFunc, Side, Side - 7, 0);

  for (I = 8; I < Side - 8; I++) {
    SetFunc (Matrix, IsFunc, Side, 6, I, (BOOLEAN)((I & 1) == 0));
    SetFunc (Matrix, IsFunc, Side, I, 6, (BOOLEAN)((I & 1) == 0));
  }

  //
  // Alignment patterns: the full grid minus the three finder corners (the spec
  // omits those). Version 1's grid is empty.
  //
  for (A = 0; A < mAlignCount[Ver]; A++) {
    for (B = 0; B < mAlignCount[Ver]; B++) {
      if (((A == 0) && (B == 0)) ||
          ((A == 0) && (B == mAlignCount[Ver] - 1)) ||
          ((A == mAlignCount[Ver] - 1) && (B == 0)))
      {
        continue;
      }

      DrawAlignment (Matrix, IsFunc, Side, mAlignPos[Ver][A], mAlignPos[Ver][B]);
    }
  }

  //
  // Version information is mandatory for versions 7 and up.
  //
  if (Ver >= 6) {
    DrawVersionInfo (Matrix, IsFunc, Side, Ver + 1);
  }

  //
  // Reserve the format areas now so data placement skips them; real bits go in
  // after masking.
  //
  for (I = 0; I <= 8; I++) {
    //
    // Row/column 6 is the timing pattern and must not be trampled: the format
    // strip skips it on both axes. Overwriting it here produced a symbol with a
    // broken timing line, which no decoder will lock onto.
    //
    if (I == 6) {
      continue;
    }

    SetFunc (Matrix, IsFunc, Side, 8, I, FALSE);
    SetFunc (Matrix, IsFunc, Side, I, 8, FALSE);
  }

  for (I = 0; I < 8; I++) {
    SetFunc (Matrix, IsFunc, Side, 8, Side - 1 - I, FALSE);
    SetFunc (Matrix, IsFunc, Side, Side - 1 - I, 8, FALSE);
  }

  SetFunc (Matrix, IsFunc, Side, Side - 8, 8, TRUE);

  //
  // Data placement: two-column strips, right to left. Strip direction is given by
  // the specification's formula (bottom-to-top when (right + 1) & 2 == 0, i.e.
  // right == 0 or 3 mod 4); column 6 (timing) is skipped by shifting the strip
  // boundary to 5. The original loop toggled direction per column and wrote every
  // column twice, which scrambled the codeword stream - the symbol rendered but
  // no scanner could decode it.
  //
  Idx = 0;
  for (Right = (INTN)Side - 1; Right >= 1; Right -= 2) {
    if (Right == 6) {
      Right = 5;
    }

    Upward = ((((UINTN)Right + 1) & 2) == 0);
    for (I = 0; I < Side; I++) {
      Row = Upward ? (Side - 1 - I) : I;
      for (B = 0; B < 2; B++) {
        Col = (UINTN)Right - B;
        if (IsFunc[Row * Side + Col]) {
          continue;
        }

        Mod = 0;
        if (Idx < (DataLen + EccLen) * 8) {
          Mod = (Codewords[Idx >> 3] >> (7 - (Idx & 7))) & 1;
          Idx++;
        }

        // Mask pattern 0: invert where (row + column) is even.
        if (((Row + Col) & 1) == 0) {
          Mod ^= 1;
        }

        Matrix[Row * Side + Col] = (UINT8)Mod;
      }
    }
  }

  //
  // Format information: level L is 0b01, mask 0.
  //
  // First copy: bit 0..5 run along row 8 (col 0..5), bit 6 at (8,7), bit 7 at
  // (8,8), bit 8 at (7,8), bits 9..14 up column 8. The original code had the two
  // axes of this copy swapped, which left the row-8 strip reading a different
  // 15-bit value than the column-8 strip.
  //
  Fmt = FormatBits (0x1, 0);

  for (I = 0; I <= 5; I++) {
    Matrix[8 * Side + I] = (UINT8)((Fmt >> I) & 1);
  }

  Matrix[8 * Side + 7] = (UINT8)((Fmt >> 6) & 1);
  Matrix[8 * Side + 8] = (UINT8)((Fmt >> 7) & 1);
  Matrix[7 * Side + 8] = (UINT8)((Fmt >> 8) & 1);

  for (I = 9; I < 15; I++) {
    Matrix[(14 - I) * Side + 8] = (UINT8)((Fmt >> I) & 1);
  }

  for (I = 0; I < 8; I++) {
    Matrix[8 * Side + (Side - 1 - I)] = (UINT8)((Fmt >> I) & 1);
  }

  for (I = 8; I < 15; I++) {
    Matrix[(Side - 15 + I) * Side + 8] = (UINT8)((Fmt >> I) & 1);
  }

  Matrix[(Side - 8) * Side + 8] = 1;

  *Size    = Side;
  *Version = Ver + 1;

  return EFI_SUCCESS;
}

EFI_STATUS
QrEncodeBytes (
  IN  CONST UINT8  *Data,
  IN  UINTN        Length,
  OUT UINT8        *Matrix,
  OUT UINTN        *Size,
  OUT UINTN        *Version
  )
{
  return QrEncodeData (Data, Length, Matrix, Size, Version);
}

EFI_STATUS
QrEncode (
  IN  CONST CHAR8  *Text,
  OUT UINT8        *Matrix,
  OUT UINTN        *Size,
  OUT UINTN        *Version
  )
{
  if (Text == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return QrEncodeData ((CONST UINT8 *)Text, AsciiStrLen (Text), Matrix, Size, Version);
}

//
// Paint colours. EfiBltVideoFill takes the fill colour from the BltBuffer
// parameter, and FrameBufferBltLib dereferences it unconditionally, so a NULL
// there faults; always pass one of these.
//
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mQrWhite = { 255, 255, 255, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mQrBlack = { 0, 0, 0, 0 };

STATIC
EFI_STATUS
FillRect (
  IN MS_SIMPLE_WINDOW_MANAGER_PROTOCOL    *Swm,
  IN EFI_HANDLE                           ImageHandle,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL        *Color,
  IN UINTN                                X,
  IN UINTN                                Y,
  IN UINTN                                W,
  IN UINTN                                H
  )
{
  return Swm->BltWindow (
                Swm,
                ImageHandle,
                Color,
                EfiBltVideoFill,
                0,
                0,
                X,
                Y,
                W,
                H,
                0
                );
}

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
  )
{
  EFI_STATUS  Status;
  UINT8       Matrix[QR_MAX_MODULES * QR_MAX_MODULES];
  UINTN       Size;
  UINTN       Version;
  UINTN       Quiet;
  UINTN       Total;
  UINTN       OriginX;
  UINTN       OriginY;
  UINTN       Row;
  UINTN       Col;
  UINTN       MaxScale;
  UINTN       Short;

  if ((Swm == NULL) || (AreaWidth == 0) || (AreaHeight == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = QrEncodeBytes (Data, Length, Matrix, &Size, &Version);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Quiet = 4;

  //
  // Everything is relative to the caller's area, not to the screen.
  //
  // BltWindow draws into the image's own window, so a symbol laid out against the
  // full screen overflows it as soon as a larger QR version is needed - which is
  // exactly what happened on page 4: BltWindow answered EFI_BUFFER_TOO_SMALL and
  // the page was dropped. Sizing against the content area keeps every page inside
  // the window regardless of version.
  //
  Short = (AreaWidth < AreaHeight) ? AreaWidth : AreaHeight;

  MaxScale = Short / (Size + 2 * Quiet);
  if (MaxScale == 0) {
    MaxScale = 1;
  }

  if ((Scale == 0) || (Scale > MaxScale)) {
    Scale = MaxScale;
  }

  Total   = (Size + 2 * Quiet) * Scale;
  OriginX = AreaX + ((AreaWidth > Total) ? (AreaWidth - Total) / 2 : 0);
  OriginY = AreaY + ((AreaHeight > Total) ? (AreaHeight - Total) / 2 : 0);

  //
  // White field including the quiet zone, then the dark modules.
  //
  Status = FillRect (Swm, ImageHandle, &mQrWhite, OriginX, OriginY, Total, Total);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Row = 0; Row < Size; Row++) {
    for (Col = 0; Col < Size; Col++) {
      if (Matrix[Row * Size + Col] == 0) {
        continue;
      }

      Status = FillRect (
                 Swm,
                 ImageHandle,
                 &mQrBlack,
                 OriginX + (Col + Quiet) * Scale,
                 OriginY + (Row + Quiet) * Scale,
                 Scale,
                 Scale
                 );
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  return EFI_SUCCESS;
}

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
  )
{
  if (Text == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return QrPaintBytes (
           Swm,
           ImageHandle,
           AreaX,
           AreaY,
           AreaWidth,
           AreaHeight,
           (CONST UINT8 *)Text,
           AsciiStrLen (Text),
           Scale
           );
}
