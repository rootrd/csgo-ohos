#!/usr/bin/env python3
"""重打 code.pbin：替换 CS2 时代面板类型的布局文件
PAN1 格式：'PAN'+版本字节 + 512B 签名区 + zip 数据 + 1B 版本尾
签名校验已被 DEVELOPMENT_ONLY 绕过，重打后可直接使用"""
import zipfile
import io
import os
import re
import sys

src = sys.argv[1] if len(sys.argv) > 1 else '/mnt/e/csgo/pbin_dump.bin'
dst = sys.argv[2] if len(sys.argv) > 2 else '/mnt/e/csgo/code_pbin_fixed.bin'

data = open(src, 'rb').read()
idx = data.find(b'PK\x03\x04')
head = data[:idx]          # PAN1 + 512 签名
tail = data[-1:]           # 版本尾字节
zipdata = data[idx:-1]

zin = zipfile.ZipFile(io.BytesIO(zipdata))
buf = io.BytesIO()
zout = zipfile.ZipFile(buf, 'w', zipfile.ZIP_STORED)

patched = []
skipped_types = re.compile(
    rb'<(CCSGOTabletPanoLayer|CSGOSurvivalBuyMenu|CSGORadialMenuBase)\b[^>]*>(?:.*?</\1>)?',
    re.S)

for name in zin.namelist():
    d = zin.read(name)
    norm = name.replace('\\', '/')
    if norm.endswith('.xml'):
        orig = d
        # 删除未知面板类型的整个元素（自闭合或成对）
        d2 = skipped_types.sub(b'', d)
        if d2 != orig:
            patched.append(norm)
        d = d2
    zout.writestr(name, d)

zout.close()
newzip = buf.getvalue()

out = head + newzip + tail
with open(dst, 'wb') as f:
    f.write(out)

print('patched files:', len(patched))
for p in patched:
    print(' ', p)
print('written', dst, os.path.getsize(dst), 'bytes')
