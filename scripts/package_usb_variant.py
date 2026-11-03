#!/usr/bin/env python3
"""Snapshot a completed WSL build and verify its exact Android RAM payload."""
from pathlib import Path
import argparse
import gzip
import hashlib
import json
import os
import shutil
import struct

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('stack', choices=('stock',))
p.add_argument('output', type=Path)
p.add_argument('--build', type=int, required=True)
default_source = os.environ.get('MU_WORK') or str(Path.home() / 'mubuild-shennong')
p.add_argument('--source', type=Path, default=Path(default_source))
p.add_argument('--build-type', choices=('DEBUG', 'RELEASE'), default='DEBUG')
a = p.parse_args()
source = a.source
root = Path(__file__).resolve().parents[1]
out = a.output / a.stack
out.mkdir(parents=True, exist_ok=True)
build = source / f'Build/ShennongPkg/{a.build_type}_CLANGPDB'
names = {'stock': f'xiaomi-shennong-b{a.build}-stock-chain.img'}
image_name = names[a.stack]
image = source / 'Build/ShennongPkg/xiaomi-shennong.img'
data = image.read_bytes()
assert data[:8] == b'ANDROID!' and struct.unpack_from('<I', data, 40)[0] == 3
kernel_size = struct.unpack_from('<I', data, 8)[0]
payload = gzip.decompress(data[4096:4096+kernel_size])
fd = build / 'FV/SM8650_EFI.fd'
shim = source / 'BootShim/BootShim.bin'
assert payload == shim.read_bytes() + fd.read_bytes(), 'Image does not contain this build'
from verify_usb_image import verify
verify(image, a.stack, build/'FV/FVMAIN.Fv', fd,
       source/'Platforms/ShennongPkg/Device/xiaomi-shennong/Binaries/QcomPkg/Drivers', out/'verified')
for src, name in ((image, image_name), (fd, fd.name),
                  (build / 'FV/FVMAIN.Fv', 'FVMAIN.Fv'), (shim, shim.name),
                  (source / 'build.log', 'build.log')):
    shutil.copy2(src, out / name)
shutil.copy2(image, root / 'Build/ShennongPkg' / image_name)
for module in ('UsbStockReplayDxe', 'SimpleWindowManagerDxe', 'ArmCpuDxe'):
    maps = [path for path in (build / 'AARCH64').rglob(module + '.map') if path.parent.name == 'OUTPUT']
    if len(maps) == 1:
        shutil.copy2(maps[0], out / (module + '.map'))
summary = dict(build=a.build, stack=a.stack, image=image_name, bytes=len(data),
               sha256=hashlib.sha256(data).hexdigest(), kernel_gzip_bytes=kernel_size,
               payload_bytes=len(payload), fd_sha256=hashlib.sha256(fd.read_bytes()).hexdigest(),
               boot_header_version=3, exact_bootshim_fd_payload=True)
(out / 'image-verify.json').write_text(json.dumps(summary, indent=2))
print(json.dumps(summary, indent=2))
