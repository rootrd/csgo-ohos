#!/usr/bin/env python3
"""从 pbin 提取 mainmenu 布局与全部 layout XML（供审查）"""
import zipfile
import io
import os

data = open('/mnt/e/csgo/pbin_dump.bin', 'rb').read()
idx = data.find(b'PK\x03\x04')
zf = zipfile.ZipFile(io.BytesIO(data[idx:]))
os.makedirs('/tmp/pbin_out', exist_ok=True)
count = 0
for name in zf.namelist():
    norm = name.lower().replace('\\', '/')
    d = zf.read(name)
    out = '/tmp/pbin_out/' + norm.replace('/', '_')
    open(out, 'wb').write(d)
    count += 1
print(count, 'files extracted to /tmp/pbin_out')
for n in sorted(os.listdir('/tmp/pbin_out')):
    if 'mainmenu' in n and n.endswith('.xml'):
        print('XML:', n)
