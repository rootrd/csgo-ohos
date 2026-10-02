#!/usr/bin/env python3
"""全量提取 pbin 内容 + 扫描未注册面板类型"""
import zipfile
import io
import os
import re
import sys

data = open('/mnt/e/csgo/pbin_dump.bin', 'rb').read()
idx = data.find(b'PK\x03\x04')
zf = zipfile.ZipFile(io.BytesIO(data[idx:]))
outdir = sys.argv[1] if len(sys.argv) > 1 else '/tmp/pbin_all'
os.makedirs(outdir, exist_ok=True)

count = 0
bad_types = set()
for name in zf.namelist():
    norm = name.replace('\\', '/')
    d = zf.read(name)
    dest = os.path.join(outdir, norm.replace('/', '_'))
    with open(dest, 'wb') as f:
        f.write(d)
    count += 1
    if norm.endswith('.xml'):
        # 扫描大写开头的自定义面板类型
        for m in re.finditer(rb'<([A-Z][A-Za-z0-9_]+)[\s/>]', d):
            t = m.group(1).decode()
            if t not in ('Panel', 'Root', 'Styles', 'Scripts', 'Style', 'Include',
                         'Label', 'Image', 'Button', 'RadioButton', 'Movie',
                         'Frame', 'TextEntry', 'ToggleButton', 'CheckBox',
                         'Slider', 'DropDownMenu', 'Carousel', 'ImageCarousel',
                         'HTMLEmbed', 'Loader', 'Panel2D'):
                bad_types.add(t)

print(count, 'files extracted to', outdir)
print('custom panel types used:')
for t in sorted(bad_types):
    print(' ', t)
