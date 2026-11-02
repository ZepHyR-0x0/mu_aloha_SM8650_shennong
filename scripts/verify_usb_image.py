#!/usr/bin/env python3
"""Verify USB owners in the FV actually embedded in an Android boot image.

Parse FFS headers, not string hits or exact standalone-FFS matches. GenFv
changes FFS state according to FV erase polarity. No phone/device operations.
"""
from pathlib import Path
import argparse
import gzip
import hashlib
import json
import lzma
import struct
from uuid import UUID

NATIVE = {
    'UsbConfigDxe': 'CD823A4D-7DEC-4531-AE5D-4134FA4127B8',
    'UsbfnDwc3Dxe': '94F8A6A7-DC34-4101-88C1-99179CCEAE83',
    'UsbDeviceDxe': '3299A266-15F0-4346-8318-716336736D3E',
    'UsbStockReplayDxe': '1638DFB0-62CC-4C10-81E7-99857D06DE44',
}
DIRECT = {
    'UsbfnDwc3Ref': '7C4A1E92-5B3D-4F86-9A17-2E8D6C0B4F51',
    'UsbDevStartDxe': '3C81E6B2-57A4-4F19-8D60-C24B9E7A5F38',
}
LZMA_GUID = 'EE4E5898-3914-4259-9D6E-DC7BD79403CF'
COMPETING = {
    'XhciDxe': '5F0937CD-73F9-445E-B320-BB6FD5D3CBDE',
    'UsbMsdDxe': '9FB4B4A7-42C0-4BCD-8540-9BCC6711F83E',
}

def sections(body):
    off = 0
    while off + 4 <= len(body):
        size = int.from_bytes(body[off:off+3], 'little')
        header = 4
        if size == 0xffffff:
            size = struct.unpack_from('<I', body, off+4)[0]
            header = 8
        assert header <= size <= len(body)-off, f'Invalid section at {off:X}'
        yield body[off+3], body[off:off+size], header
        off = (off + size + 3) & ~3

def files(fv):
    assert fv[40:44] == b'_FVH', 'Not an FV'
    length = struct.unpack_from('<Q', fv, 32)[0]
    hlen = struct.unpack_from('<H', fv, 48)[0]
    assert length == len(fv) and hlen % 2 == 0
    assert sum(struct.unpack('<' + 'H'*(hlen//2), fv[:hlen])) & 0xffff == 0, 'FV header checksum'
    erase = bool(struct.unpack_from('<I', fv, 44)[0] & 0x800)
    off = (hlen + 7) & ~7
    while off + 24 <= length and fv[off:off+24] != bytes((255 if erase else 0,))*24:
        size = int.from_bytes(fv[off+20:off+23], 'little')
        header = 24
        if size == 0xffffff:
            header = 32
            size = struct.unpack_from('<Q', fv, off+24)[0]
        assert header <= size <= length-off, f'Invalid FFS at {off:X}'
        raw = fv[off:off+size]
        effective = raw[23] ^ (255 if erase else 0)
        assert (sum(raw[:header])-raw[17]-raw[23]) & 255 == 0, f'FFS header checksum {off:X}'
        assert effective & 4 and not effective & 0x30, f'Invalid FFS state {off:X}'
        guid = str(UUID(bytes_le=raw[:16])).upper()
        yield guid, dict(offset=off, size=size, type=raw[18], stored_state=raw[23], effective_state=effective,
                        body=raw[header:])
        off = (off + size + 7) & ~7

def embedded_fv(image):
    assert image[:8] == b'ANDROID!' and struct.unpack_from('<I', image, 40)[0] == 3
    count = struct.unpack_from('<I', image, 8)[0]
    payload = gzip.decompress(image[4096:4096+count])
    start = payload.find(b'_FVH')-40
    assert start >= 0
    compact_len = struct.unpack_from('<Q', payload, start+32)[0]
    compact = payload[start:start+compact_len]
    candidates = []
    for _, item in files(compact):
        for kind, section, header in sections(item['body']):
            if kind == 2 and str(UUID(bytes_le=section[header:header+16])).upper() == LZMA_GUID:
                dataoff, attr = struct.unpack_from('<HH', section, header+16)
                assert dataoff >= header+20 and attr & 1
                plain = lzma.decompress(section[dataoff:], format=lzma.FORMAT_ALONE)
                fvstart = plain.find(b'_FVH')-40
                assert fvstart >= 0
                size = struct.unpack_from('<Q', plain, fvstart+32)[0]
                candidates.append(plain[fvstart:fvstart+size])
    assert len(candidates) == 1, 'Expected one LZMA-encapsulated FVMAIN'
    return payload, candidates[0]

def verify(image_path, stack, build_fv=None, build_fd=None, stock_root=None, output=None):
    image = image_path.read_bytes()
    payload, fv = embedded_fv(image)
    if build_fv is not None:
        assert fv == build_fv.read_bytes(), 'Final image contains another build FV'
    if build_fd is not None:
        assert payload.endswith(build_fd.read_bytes()), 'Final payload does not contain the build FD'
    entries = list(files(fv))
    by_guid = dict(entries)
    assert len(entries) == len(by_guid), 'Duplicate FFS GUID'
    required, excluded = (NATIVE, DIRECT) if stack == 'stock' else (DIRECT, NATIVE)
    modules = {}
    if output:
        output.mkdir(parents=True, exist_ok=True)
        (output/'FVMAIN-from-image.Fv').write_bytes(fv)
    for name, guid in required.items():
        assert guid in by_guid, f'Missing USB owner in final image: {name} ({guid})'
        item = by_guid[guid]
        assert item['type'] == 7, f'{name} is not a DXE driver'
        groups = list(sections(item['body']))
        pes = [s[h:] for kind,s,h in groups if kind == 0x10]
        depex = [s[h:] for kind,s,h in groups if kind == 0x13]
        assert len(pes) == len(depex) == 1 and pes[0][:2] == b'MZ'
        if name in NATIVE and name != 'UsbStockReplayDxe' and stock_root:
            assert pes[0] == (stock_root/name/(name+'.efi')).read_bytes(), f'{name} PE changed'
            assert depex[0] == (stock_root/name/(name+'.depex')).read_bytes(), f'{name} DEPEX differs'
        modules[name] = dict(guid=guid, **{k:v for k,v in item.items() if k != 'body'},
                             pe_bytes=len(pes[0]), pe_sha256=hashlib.sha256(pes[0]).hexdigest(),
                             depex_hex=depex[0].hex())
        if output:
            (output/(name+'.efi')).write_bytes(pes[0])
            (output/(name+'.depex')).write_bytes(depex[0])
    for name, guid in excluded.items():
        assert guid not in by_guid, f'Competing owner present: {name}'
    for name, guid in COMPETING.items():
        assert guid not in by_guid, f'Competing/storage owner present: {name}'
    summary = dict(stack=stack, image=str(image_path), bytes=len(image), image_sha256=hashlib.sha256(image).hexdigest(),
                   fv_sha256=hashlib.sha256(fv).hexdigest(), ffs_count=len(entries), modules=modules,
                   build_fv_matches=(build_fv is not None), build_fd_matches=(build_fd is not None),
                   limit='Packaging and binary identity do not establish DXE dispatch, protocol publication, or USB enumeration.')
    if output:
        (output/'verify.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
    return summary

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('image', type=Path)
    p.add_argument('--stack', choices=('stock','direct'), required=True)
    p.add_argument('--fv', type=Path)
    p.add_argument('--fd', type=Path)
    p.add_argument('--stock-root', type=Path)
    p.add_argument('--extract', type=Path)
    a = p.parse_args()
    print(json.dumps(verify(a.image,a.stack,a.fv,a.fd,a.stock_root,a.extract),indent=2))

if __name__ == '__main__':
    main()
