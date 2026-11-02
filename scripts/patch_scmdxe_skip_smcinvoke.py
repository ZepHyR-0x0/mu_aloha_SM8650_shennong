#!/usr/bin/env python3
"""Retired patch: verify the original SCM without modifying it.

b317 restores the native init-error branch. Publishing a half-initialized SCM
protocol would hide transport failure and allow calls through invalid objects.
"""
from pathlib import Path
import hashlib
root = Path(__file__).resolve().parents[1]
image = root / 'Platforms/ShennongPkg/Device/xiaomi-shennong/Binaries/QcomPkg/Drivers/TzDxe/ScmDxeCompat.efi'
original = root.parent / 'extract/uefi_extract/ScmDxeCompat.efi'
def main():
    data = image.read_bytes()
    assert data == original.read_bytes(), 'SCM differs from extracted original; no changes made'
    assert data[0x1510:0x1514] == bytes.fromhex('800300b5'), 'Native init error branch missing'
    print('Retired patch; original SCM verified, SHA256=' + hashlib.sha256(data).hexdigest())
    return 0
if __name__ == '__main__':
    raise SystemExit(main())
