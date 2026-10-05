#!/usr/bin/env python3
"""Swap in the real hud.xml (touch variant kept as reference backup)."""
import zipfile, io, os, shutil

data = open('code_pbin_fixed.bin', 'rb').read()
idx = data.find(b'PK\x03\x04')
zf = zipfile.ZipFile(io.BytesIO(data[idx:]))
real = zf.read('panorama/layout/hud/hud.xml')

TARGETS = [
    'CSGO-Source-Linux-20260928/ohos/overlay/csgo/panorama/layout/hud/hud.xml',
    'hap/entry/src/main/resources/rawfile/csgo/csgo/panorama/layout/hud/hud.xml',
]
for t in TARGETS:
    if os.path.exists(t):
        bak = t + '.csno-touch.bak'
        shutil.copy2(t, bak)
        print('backed up', bak)
    with open(t, 'wb') as f:
        f.write(real)
    print('swapped', t, len(real), 'bytes')
