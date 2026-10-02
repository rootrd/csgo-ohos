#!/usr/bin/env python3
"""从 pbin 提取 mainmenu 相关布局"""
import zipfile
import io
import os

data = open('/mnt/e/csgo/pbin_dump.bin', 'rb').read()
idx = data.find(b'PK\x03\x04')
zf = zipfile.ZipFile(io.BytesIO(data[idx:]))
os.makedirs('/e/csgo/pbin_out', exist_ok=True)
count = 0
for name in zf.namelist():
    norm = name.lower().replace('\\', '/')
    if norm.endswith('mainmenu.xml') or norm.endswith('mainmenu_common.xml') or 'mainmenu' in norm:
        d = zf.read(name)
        out = '/e/csgo/pbin_out/' + norm.replace('/', '_')
        open(out, 'wb').write(d)
        print('extracted', name, len(d))
        count += 1
print(count, 'files')
