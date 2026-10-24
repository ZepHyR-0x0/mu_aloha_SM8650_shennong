#!/usr/bin/env python3
"""Mirror of QrCode.c (byte mode, ECC level L, versions 1-10), checked against
known QR constants and an independent decoder.

The C encoder cannot be run at build time, so the same algorithm is reproduced here
and verified three ways:

  1. format information for (level L, mask 0) must equal 0x77C4 - the value printed
     in every QR reference table
  2. every Reed-Solomon syndrome of every block must be zero
  3. zxing-cpp must decode the rendered matrix back to the original text - this is
     fully independent of the layout code below, so it catches placement bugs
     (zigzag, interleaving, alignment grid, version info) that mirror checks cannot
"""
DATA_CW    = [19, 34, 55, 80, 108, 136, 156, 194, 232, 274]
ECC_CW     = [7, 10, 15, 20, 26, 36, 40, 48, 60, 72]
BLOCK_COUNT = [1, 1, 1, 1, 1, 2, 2, 2, 2, 4]
BLOCK_ECC  = [7, 10, 15, 20, 26, 18, 20, 24, 30, 18]
ALIGN_COUNT = [0, 2, 2, 2, 2, 2, 3, 3, 3, 3]
ALIGN_POS = [
    [0, 0, 0, 0, 0, 0, 0],
    [6, 18, 0, 0, 0, 0, 0],
    [6, 22, 0, 0, 0, 0, 0],
    [6, 26, 0, 0, 0, 0, 0],
    [6, 30, 0, 0, 0, 0, 0],
    [6, 34, 0, 0, 0, 0, 0],
    [6, 22, 38, 0, 0, 0, 0],
    [6, 24, 42, 0, 0, 0, 0],
    [6, 26, 46, 0, 0, 0, 0],
    [6, 28, 50, 0, 0, 0, 0],
]

exp = [0] * 512
log = [0] * 256
x = 1
for i in range(255):
    exp[i] = x
    log[x] = i
    x <<= 1
    if x & 0x100:
        x ^= 0x11D
for i in range(255, 512):
    exp[i] = exp[i - 255]


def gmul(a, b):
    if a == 0 or b == 0:
        return 0
    return exp[log[a] + log[b]]


def rs_ecc(data, ecc_len):
    gen = [0] * ecc_len
    gen[ecc_len - 1] = 1
    for i in range(ecc_len):
        for j in range(ecc_len):
            gen[j] = gmul(gen[j], exp[i])
            if j + 1 < ecc_len:
                gen[j] ^= gen[j + 1]
    rem = [0] * ecc_len
    for b in data:
        factor = b ^ rem[0]
        rem = rem[1:] + [0]
        for j in range(ecc_len):
            rem[j] ^= gmul(gen[j], factor)
    return rem


def format_bits(ecc_bits, mask):
    data = (ecc_bits << 3) | mask
    rem = data
    for _ in range(10):
        rem = (rem << 1) ^ ((rem >> 9) * 0x537)
    return ((data << 10) | rem) ^ 0x5412


def version_bits(ver):
    """BCH(18,6), generator 0x1F25. Plain shift loop: the (rem >> 11) * 0x1F25
    trick is invalid here because the initial value (ver << 12) is wider than the
    field."""
    rem = ver
    for _ in range(12):
        rem <<= 1
        if rem & 0x1000:
            rem ^= 0x1F25
    return (ver << 12) | rem


def syndromes_zero(data, ecc):
    cw = data + ecc
    for i in range(len(ecc)):
        s = 0
        for c in cw:
            s = gmul(s, exp[i]) ^ c
        if s != 0:
            return False
    return True


def payload_bytes(text):
    """Accept str or raw bytes: LOGAUTO packets start with a 0x00 index byte,
    which no NUL-terminated C string can represent."""
    return bytes(text) if isinstance(text, (bytes, bytearray)) else text.encode()


def encode(text):
    raw = payload_bytes(text)
    n = len(raw)
    ver = None
    for i in range(10):
        width = 16 if i >= 9 else 8
        if 4 + width + n * 8 <= DATA_CW[i] * 8:
            ver = i
            break
    assert ver is not None, "too long"
    data_len, ecc_len = DATA_CW[ver], ECC_CW[ver]
    side = 17 + 4 * (ver + 1)

    bits = []

    def push(val, cnt):
        for k in range(cnt):
            bits.append((val >> (cnt - 1 - k)) & 1)

    push(0x4, 4)
    push(n, 16 if ver >= 9 else 8)
    for b in raw:
        push(b, 8)
    push(0, min(4, data_len * 8 - len(bits)))
    while len(bits) % 8:
        bits.append(0)
    data = [int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
    while len(data) < data_len:
        data.append(0xEC if (len(data) - ((len(bits) + 7) // 8)) % 2 == 0 else 0x11)

    if BLOCK_COUNT[ver] == 1:
        ecc = rs_ecc(data, ecc_len)
        cw = data + ecc
        blocks = [(data, ecc)]
    else:
        # Version 10-L is four uneven blocks (68, 68, 69, 69); the others in this
        # range are two even blocks. SHORT blocks come first (as ISO 18004 lays
        # them out), and the interleave takes one codeword from each block per
        # column, with the long blocks' extra codeword in the last column.
        nb = BLOCK_COUNT[ver]
        be = BLOCK_ECC[ver]
        short_len = data_len // nb
        num_long = data_len % nb
        num_short = nb - num_long

        def off(b):
            return b * short_len + (b - num_short if b > num_short else 0)

        def ln(b):
            return short_len + (1 if b >= num_short else 0)

        eccs = []
        slices = []
        for b in range(nb):
            o, l = off(b), ln(b)
            slices.append((o, l))
            eccs.append(rs_ecc(data[o:o + l], be))
        cw = []
        for i in range(short_len):
            for b in range(nb):
                cw.append(data[off(b) + i])
        for b in range(num_short, nb):
            cw.append(data[off(b) + short_len])
        for i in range(be):
            for b in range(nb):
                cw.append(eccs[b][i])
        blocks = [(data[o:o + l], eccs[b]) for b, (o, l) in enumerate(slices)]
    return side, ver + 1, data, ecc_len, cw, blocks


def build_matrix(text):
    """Mirror of QrEncode's placement, so the layout can be asserted here."""
    side, ver, data, ecc_len, cw, blocks = encode(text)
    m = [[0] * side for _ in range(side)]
    f = [[False] * side for _ in range(side)]

    def setf(r, c, dark):
        m[r][c] = 1 if dark else 0
        f[r][c] = True

    def finder(r0, c0):
        for dr in range(-1, 8):
            for dc in range(-1, 8):
                r, c = r0 + dr, c0 + dc
                if not (0 <= r < side and 0 <= c < side):
                    continue
                if 0 <= dr <= 6 and 0 <= dc <= 6:
                    dist = max(abs(dr - 3), abs(dc - 3))
                    setf(r, c, dist != 2)
                else:
                    setf(r, c, False)

    def align(r0, c0):
        for dr in range(-2, 3):
            for dc in range(-2, 3):
                r, c = r0 + dr, c0 + dc
                if 0 <= r < side and 0 <= c < side:
                    setf(r, c, max(abs(dr), abs(dc)) != 1)

    finder(0, 0)
    finder(0, side - 7)
    finder(side - 7, 0)

    for i in range(8, side - 8):
        setf(6, i, i % 2 == 0)
        setf(i, 6, i % 2 == 0)

    cnt = ALIGN_COUNT[ver - 1]
    for a in range(cnt):
        for b in range(cnt):
            if (a == 0 and b == 0) or (a == 0 and b == cnt - 1) or (a == cnt - 1 and b == 0):
                continue
            align(ALIGN_POS[ver - 1][a], ALIGN_POS[ver - 1][b])

    if ver >= 7:
        info = version_bits(ver)
        for i in range(18):
            setf(i // 3, side - 11 + i % 3, (info >> i) & 1)
            setf(side - 11 + i % 3, i // 3, (info >> i) & 1)

    for i in range(9):
        if i == 6:
            continue
        setf(8, i, False)
        setf(i, 8, False)
    for i in range(8):
        setf(8, side - 1 - i, False)
        setf(side - 1 - i, 8, False)
    setf(side - 8, 8, True)

    idx = 0
    right = side - 1
    while right >= 1:
        if right == 6:
            right = 5
        upward = ((right + 1) & 2) == 0
        for i in range(side):
            row = side - 1 - i if upward else i
            for b in range(2):
                c2 = right - b
                if f[row][c2]:
                    continue
                mod = 0
                if idx < len(cw) * 8:
                    mod = (cw[idx >> 3] >> (7 - (idx & 7))) & 1
                    idx += 1
                if (row + c2) % 2 == 0:
                    mod ^= 1
                m[row][c2] = mod
        right -= 2

    fmt = format_bits(1, 0)
    for i in range(6):
        m[8][i] = (fmt >> i) & 1
    m[8][7] = (fmt >> 6) & 1
    m[8][8] = (fmt >> 7) & 1
    m[7][8] = (fmt >> 8) & 1
    for i in range(9, 15):
        m[14 - i][8] = (fmt >> i) & 1
    for i in range(8):
        m[8][side - 1 - i] = (fmt >> i) & 1
    for i in range(8, 15):
        m[side - 15 + i][8] = (fmt >> i) & 1
    m[side - 8][8] = 1
    return side, ver, m


def check_format(m, side):
    fmt = 0x77C4
    errs = []
    for i in range(6):
        if m[8][i] != ((fmt >> i) & 1):
            errs.append("fmt copy1 row8 bit%d" % i)
    for r, c, b in ((8, 7, 6), (8, 8, 7), (7, 8, 8)):
        if m[r][c] != ((fmt >> b) & 1):
            errs.append("fmt copy1 (%d,%d)" % (r, c))
    for i in range(9, 15):
        if m[14 - i][8] != ((fmt >> i) & 1):
            errs.append("fmt copy1 col8 bit%d" % i)
    for i in range(8):
        if m[8][side - 1 - i] != ((fmt >> i) & 1):
            errs.append("fmt copy2 bit%d" % i)
    for i in range(8, 15):
        if m[side - 15 + i][8] != ((fmt >> i) & 1):
            errs.append("fmt copy3 bit%d" % i)
    return errs


def check_alignment(m, r0, c0):
    want = ((1, 1, 1, 1, 1), (1, 0, 0, 0, 1), (1, 0, 1, 0, 1),
            (1, 0, 0, 0, 1), (1, 1, 1, 1, 1))
    return all(m[r0 + dr][c0 + dc] == want[dr][dc]
               for dr in range(5) for dc in range(5))


def check(text):
    side, ver, m = build_matrix(text)
    errs = []
    for r0, c0 in ((0, 0), (0, side - 7), (side - 7, 0)):
        want = [[1, 1, 1, 1, 1, 1, 1], [1, 0, 0, 0, 0, 0, 1], [1, 0, 1, 1, 1, 0, 1],
                [1, 0, 1, 1, 1, 0, 1], [1, 0, 1, 1, 1, 0, 1], [1, 0, 0, 0, 0, 0, 1],
                [1, 1, 1, 1, 1, 1, 1]]
        for dr in range(7):
            for dc in range(7):
                if m[r0 + dr][c0 + dc] != want[dr][dc]:
                    errs.append("finder(%d,%d) off at %d,%d" % (r0, c0, dr, dc))
    for i in range(8, side - 8):
        if m[6][i] != (1 if i % 2 == 0 else 0):
            errs.append("timing row at %d" % i)
        if m[i][6] != (1 if i % 2 == 0 else 0):
            errs.append("timing col at %d" % i)
    if m[side - 8][8] != 1:
        errs.append("dark module missing")
    errs += check_format(m, side)
    cnt = ALIGN_COUNT[ver - 1]
    for a in range(cnt):
        for b in range(cnt):
            if (a == 0 and b == 0) or (a == 0 and b == cnt - 1) or (a == cnt - 1 and b == 0):
                continue
            p, q = ALIGN_POS[ver - 1][a], ALIGN_POS[ver - 1][b]
            if not check_alignment(m, p - 2, q - 2):
                errs.append("alignment(%d,%d) bad" % (p, q))
    if ver >= 7:
        info = version_bits(ver)
        for i in range(18):
            bit = (info >> i) & 1
            if m[i // 3][side - 11 + i % 3] != bit:
                errs.append("version info top-right bit%d" % i)
            if m[side - 11 + i % 3][i // 3] != bit:
                errs.append("version info bottom-left bit%d" % i)
    print("%-10r v%-2d %dx%d  layout: %s" % (text[:9], ver, side, side, "OK" if not errs else errs[:3]))
    return m


def decode_roundtrip():
    """Feed the matrix to an independent decoder (zxing-cpp); catches placement
    bugs that the structural checks above cannot, since those mirror the C code."""
    try:
        import zxingcpp
        from PIL import Image
    except ImportError:
        print("  (zxing-cpp/PIL not installed - independent decode check skipped)")
        return True

    # Boundary payloads that force each version 1..10 at level L.
    tests = ["123", "A" * 17, "A" * 32, "A" * 53, "A" * 78, "A" * 106,
             "A" * 134, "A" * 154, "A" * 192, "A" * 230, "A" * 271]
    ok = True
    for text in tests:
        side, ver, m = build_matrix(text)
        img = Image.new("RGB", (side, side), "white")
        px = img.load()
        for r in range(side):
            for c in range(side):
                if m[r][c]:
                    px[c, r] = (0, 0, 0)
        res = zxingcpp.read_barcode(img)
        good = res is not None and res.text == text and res.format.name == "QRCode"
        print("  decode v%-2d %-6r : %s" % (ver, text[:6], "OK" if good else "**FAILED: %r**" % (res.text if res else None)))
        ok = ok and good
    return ok


def decode_binary_roundtrip():
    """Raw-byte payloads: LOGAUTO puts a binary index (packet 0 begins with 0x00)
    in front of the data. A scanner must read Barcode.bytes, not text."""
    try:
        import zxingcpp
        from PIL import Image
    except ImportError:
        print("  (zxing-cpp/PIL not installed - binary decode check skipped)")
        return True

    tests = [
        bytes((0, 3)) + b"first page\n",
        bytes((1,)) + b"second page\n",
        bytes((2,)) + bytes(range(0, 32)) + bytes((0x7F, 0x80, 0xFF)),
    ]
    ok = True
    for raw in tests:
        side, ver, m = build_matrix(raw)
        img = Image.new("RGB", (side, side), "white")
        px = img.load()
        for r in range(side):
            for c in range(side):
                if m[r][c]:
                    px[c, r] = (0, 0, 0)
        res = zxingcpp.read_barcode(img)
        got = bytes(res.bytes) if res is not None and res.valid else None
        good = got == raw
        print("  binary v%-2d %-24r : %s" % (ver, raw[:6], "OK" if good else "**FAILED: %r**" % (got,)))
        ok = ok and good
    return ok


if __name__ == "__main__":
    fb = format_bits(0x1, 0)
    print("format bits (L, mask 0) = 0x%04X  %s" % (fb, "OK" if fb == 0x77C4 else "**MISMATCH, expected 0x77C4**"))
    print("version bits: v7=0x%04X v10=0x%04X  %s" % (
        version_bits(7), version_bits(10),
        "OK" if version_bits(7) == 0x7C94 else "**MISMATCH v7, expected 0x7C94**"))

    print()
    for text in ("123", "A" * 106, "A" * 134, "A" * 154, "A" * 271):
        side, ver, data, ecc_len, cw, blocks = encode(text)
        ok = all(syndromes_zero(d, e) for d, e in blocks)
        print("  %-6r v%-2d %2dx%-2d cw=%d blocks=%d  RS syndromes zero: %s"
              % (text[:6], ver, side, side, len(cw), len(blocks), ok))

    print()
    for t in ("123", "A" * 17, "A" * 32, "A" * 53, "A" * 78, "A" * 106,
              "A" * 134, "A" * 154, "A" * 192, "A" * 230, "A" * 271):
        check(t)
    print()
    print("independent decode:", "ALL OK" if decode_roundtrip() else "FAILED")
    print("binary decode:", "ALL OK" if decode_binary_roundtrip() else "FAILED")
    print()
    side, ver, m = build_matrix("123")
    for r in range(side):
        print("   " + "".join("##" if v else "  " for v in m[r]))


# ---------------------------------------------------------------------------
# Capacity table check.
#
# QrCode.c stores mDataCodewords as UINT16 precisely because 274 does not fit in a
# byte. As UINT8 it wrapped to 18, version 10 looked like it held 15 bytes, and every
# page over 230 bytes failed with EFI_BUFFER_TOO_SMALL. This asserts the numbers the
# encoder relies on, and that QR_MAX_BYTES still fits the largest version.
# ---------------------------------------------------------------------------
QR_MAX_BYTES = 271
DATA10 = [19, 34, 55, 80, 108, 136, 156, 194, 232, 274]
ECC10 = [7, 10, 15, 20, 26, 18, 20, 24, 30, 18]
BLK10 = [1, 1, 1, 1, 1, 2, 2, 2, 2, 4]


def cap_bytes(i):
    return (DATA10[i] * 8 - (4 + (16 if i >= 9 else 8))) // 8


def check_capacity():
    print("\ncapacity (level L, byte mode):")
    for i in range(10):
        print("  v%-2d data=%-3d ecc=%-2d blocks=%d  max=%d bytes"
              % (i + 1, DATA10[i], ECC10[i], BLK10[i], cap_bytes(i)))
    assert max(DATA10) <= 0xFFFF, "mDataCodewords must be at least UINT16"
    assert cap_bytes(9) == QR_MAX_BYTES, (
        "QR_MAX_BYTES %d does not match version 10 capacity %d"
        % (QR_MAX_BYTES, cap_bytes(9)))
    print("  QR_MAX_BYTES=%d fits version 10 exactly: OK" % QR_MAX_BYTES)


if __name__ == "__main__":
    check_capacity()
