#!/usr/bin/env python3
"""解析 panorama code.pbin（PAN1 + 512 签名 + zip blob + 版本字节）列出文件名"""
import struct
import sys
import zipfile
import io

data = open(sys.argv[1], 'rb').read()
print('size', len(data), 'magic', data[:4])

# pbin 的 zip blob 是原样 zip 存储（非 deflate 包裹）：跳过头部签名区找 PK 头
idx = data.find(b'PK\x03\x04')
print('local header at', idx)
if idx < 0:
    sys.exit(1)

zf = zipfile.ZipFile(io.BytesIO(data[idx:]))
names = zf.namelist()
print(len(names), 'files in pbin zip')
hits = [n for n in names if 'hud' in n.lower() or 'layout' in n.lower()]
for h in hits[:30]:
    print(' ', h)
